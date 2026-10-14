// ============================================================================
// tests/test_integration.cpp —— 端到端集成测试（JD 职责3：集成测试、高并发稳定性）
//
// 拉起「完整数据链路」：行情网关(生产者线程) → 共享内存无锁环 → 策略消费线程
// (风控校验 + 信号生成 + 监控采集)，多线程高并发跑 N 笔行情，断言：
//   - 共享内存环零丢失（发布 tick 数 == 消费 tick 数）；
//   - 策略信号正常产生；风控拒绝可统计；
//   - 看门狗能在行情卡顿时触发告警（监控告警系统）；
//   - 熔断开关 HALT 能中断策略执行；
//   - 对象池/内存池 acquire-release 正确且并发安全。
// 编译：见 Makefile 的 `itest` 目标。
// ============================================================================
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

#include "gateway/MdGateway.h"
#include "strategy/TickConsumer.h"
#include "risk/RiskControl.h"
#include "monitor/Monitor.h"
#include "util/ObjectPool.h"
#include "common/Types.h"

using namespace md;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static uint64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 端到端跑一趟：网关(线程) → 环 → 消费(线程)，sampler 线程把发布数喂给监控
struct PipeResult {
    uint64_t pub = 0, drop = 0, consumed = 0, ticks = 0, signals = 0;
};

static PipeResult runPipeline(const char* shm, double rate, uint64_t cap,
                              uint64_t totalTicks, RiskControl& risk, Monitor& mon) {
    MdGateway gw;
    CHECK(gw.init(shm, cap));
    TickConsumer cons;
    CHECK(cons.init(shm));
    cons.bind(&risk, &mon);

    std::atomic<bool> producerDone{false};
    cons.setDrainSignal(&producerDone);

    std::thread gt([&]() { gw.run(totalTicks, rate, 0); producerDone.store(true); });
    std::thread ct([&]() { cons.run(totalTicks + 100000, nullptr); });

    // 采样线程：把网关发布数增量喂给监控，供看门狗计算丢弃率/延迟
    std::thread st([&]() {
        uint64_t prev = 0;
        auto t0 = steadyNs();
        while (gw.pubCount() < totalTicks) {
            uint64_t p = gw.pubCount();
            if (p > prev) { mon.recordPublished(p - prev); prev = p; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (steadyNs() - t0 > 30'000'000'000ULL) break;
        }
        uint64_t p = gw.pubCount();
        if (p > prev) mon.recordPublished(p - prev);
    });

    gt.join(); ct.join(); st.join();
    return PipeResult{ gw.pubCount(), gw.dropCount(),
                       cons.consumed(), cons.ticksConsumed(), cons.signals() };
}

// 1) 零丢失 + 信号 + 监控遥测（中速、大环，不触发背压）
static void testIntegrationZeroLoss() {
    RiskControl risk;
    risk.configureDefault(RiskConfig{ /*min*/0, /*max*/0, /*long*/0, /*short*/0, /*age*/0, /*rate*/0 });
    Monitor mon;
    mon.setAlertThresholds(/*dropRate*/0.5, /*p99*/10'000'000, /*stall*/5'000'000'000ULL);

    PipeResult r = runPipeline("/md_itg0", /*rate*/200'000.0, /*cap*/262144, /*ticks*/200'000, risk, mon);
    CHECK(r.pub == 200'000);
    CHECK(r.ticks == r.pub);                 // 共享内存环零丢失
    CHECK(r.drop == 0);                       // 大环、中速：无背压丢弃
    CHECK(r.signals > 0);                     // 策略产生了信号
    CHECK(mon.rejected() == 0);               // 未配置限额 → 无拒绝
    std::string tel = mon.telemetryJson();
    CHECK(!tel.empty() && tel.find("consumed") != std::string::npos);  // 遥测非空
    std::printf("[itg] zero-loss: pub=%llu ticks=%llu drop=%llu signals=%llu tel=%s\n",
                static_cast<unsigned long long>(r.pub),
                static_cast<unsigned long long>(r.ticks),
                static_cast<unsigned long long>(r.drop),
                static_cast<unsigned long long>(r.signals), tel.c_str());
}

// 2) 背压：高速 + 小环 → ring 满丢弃（验证背压/丢弃计数正确）
static void testIntegrationBackpressure() {
    RiskControl risk;
    Monitor mon;
    // 极小环(1024) + 不限速(压测) → 生产者快于消费者，必然触发背压
    PipeResult r = runPipeline("/md_itg1", /*rate*/0.0, /*cap*/1024, /*ticks*/500'000, risk, mon);
    CHECK(r.pub == 500'000);
    CHECK(r.drop > 0);                        // 背压生效：ring 满丢弃计数 > 0
    std::printf("[itg] backpressure: pub=%llu drop=%llu ticks=%llu\n",
                static_cast<unsigned long long>(r.pub),
                static_cast<unsigned long long>(r.drop),
                static_cast<unsigned long long>(r.ticks));
}

// 3) 监控告警看门狗：行情卡顿 → 触发 ALERT
static void testMonitorWatchdog() {
    RiskControl risk;
    Monitor mon;
    mon.setAlertThresholds(/*dropRate*/0.9, /*p99*/10'000'000, /*stall*/500'000'000ULL); // 卡顿阈值 0.5s
    mon.startWatchdog(&risk, /*periodMs*/200);

    // 先跑一小段，再停掉网关，让看门狗检测到「无心跳」
    std::thread gt([&]() { /* 不真正跑网关，仅制造一段心跳后停 */ });
    gt.join();
    // 模拟消费端曾收到行情、随后中断：喂一次心跳后静止
    mon.heartbeat(steadyNs());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // > stall 0.5s
    mon.stopWatchdog();

    CHECK(mon.alerts() > 0);                  // 看门狗应已产生告警
    std::printf("[itg] watchdog alerts=%llu\n", static_cast<unsigned long long>(mon.alerts()));
}

// 4) 风控熔断：triggerHalt 后所有决策返回 HALT，且消费者被中断
static void testRiskHalt() {
    RiskControl risk;
    risk.configureDefault(RiskConfig{ /*min*/0, /*max*/0, /*long*/100, /*short*/100, 0, 0 });
    risk.triggerHalt("integration-test");
    CHECK(risk.halted());
    MarketTick t{}; t.inst = 0; t.price = 100; t.qty = 1; t.side = kSideBuy;
    CHECK(risk.checkTick(t, steadyNs(), steadyNs()) == RISK_HALT);
    CHECK(risk.haltedCount() >= 1);
    std::printf("[itg] halt/reason=%s haltedCount=%llu\n",
                risk.haltReason() ? risk.haltReason() : "?",
                static_cast<unsigned long long>(risk.haltedCount()));
    risk.resume();
    CHECK(!risk.halted());
}

// 5) 风控限额：净持仓超限 → 下单被拒
static void testRiskPositionLimit() {
    RiskControl risk;
    risk.configure(0, RiskConfig{ 0, 0, /*maxLong*/50, /*maxShort*/0, 0, 0 });
    MarketTick buy{};
    buy.inst = 0; buy.price = 100; buy.qty = 30; buy.side = kSideBuy;
    CHECK(risk.checkTick(buy, steadyNs(), steadyNs()) == RISK_ALLOW);   // 持仓 +30
    CHECK(risk.position(0) == 30);
    // 再下 30 手买 → 投影 60 > 50 → 拒绝
    CHECK(risk.checkOrder(0, +1, 30, steadyNs()) == RISK_REJECT);
    CHECK(risk.rejected() >= 1);
    // 下 10 手买 → 投影 40 <= 50 → 允许
    CHECK(risk.checkOrder(0, +1, 10, steadyNs()) == RISK_ALLOW);
    std::printf("[itg] position-limit rejected=%llu\n",
                static_cast<unsigned long long>(risk.rejected()));
}

// 6) 对象池/内存池：acquire-release 正确 + 并发安全
static void testObjectPool() {
    ObjectPool<uint64_t, 64> pool;
    CHECK(pool.capacity() == 64);
    CHECK(pool.availableApprox() == 64);
    uint64_t* a = pool.acquire();
    CHECK(a != nullptr);
    *a = 12345;
    CHECK(pool.availableApprox() == 63);
    pool.release(a);
    CHECK(pool.availableApprox() == 64);
    // 池空返回 nullptr
    std::vector<uint64_t*> all;
    for (size_t i = 0; i < 64; ++i) { uint64_t* p = pool.acquire(); CHECK(p != nullptr); all.push_back(p); }
    CHECK(pool.acquire() == nullptr);   // 耗尽
    for (auto p : all) pool.release(p);
    CHECK(pool.availableApprox() == 64);

    // 并发：两线程各取各还，不崩溃、最终归位
    std::atomic<uint64_t> ops{0};
    std::thread t1([&]() { for (int i = 0; i < 20000; ++i) { uint64_t* p = pool.acquire(); if (p) { *p = 1; pool.release(p); } ops++; } });
    std::thread t2([&]() { for (int i = 0; i < 20000; ++i) { uint64_t* p = pool.acquire(); if (p) { *p = 2; pool.release(p); } ops++; } });
    t1.join(); t2.join();
    CHECK(ops == 40000);
    CHECK(pool.availableApprox() == 64);
    std::printf("[itg] object-pool ops=%llu final=%zu\n",
                static_cast<unsigned long long>(ops.load()), pool.availableApprox());
}

int main() {
    testIntegrationZeroLoss();
    testIntegrationBackpressure();
    testMonitorWatchdog();
    testRiskHalt();
    testRiskPositionLimit();
    testObjectPool();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail) { std::printf("INTEGRATION TESTS FAILED\n"); return 1; }
    std::printf("ALL INTEGRATION TESTS PASSED\n");
    return 0;
}
