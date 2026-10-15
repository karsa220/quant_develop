// ============================================================================
// tests/test_feed.cpp —— 真实 CTP / 飞马 feed 接入：单元 + 集成测试
//
// 覆盖：
//   1) 转换工具：price unit round-trip（±1 tick）、symbol→inst、交易所时间解析；
//   2) 集成：CTP 仿真后端「登录→订阅→行情→风控」跑通，零未知合约、零丢失；
//   3) 集成：飞马 仿真后端同样跑通。
// 注：默认走仿真后端（无需 SDK/账号）。接真机时定义 HAS_CTP/HAS_FEMAS 即可让
// RealXxxMdApi 参与编译（本机无 SDK，故 CI/本地仅验证仿真路径）。
// ============================================================================
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "feed/MarketFeed.h"
#include "feed/CtpMdFeed.h"
#include "feed/FemasMdFeed.h"
#include "feed/FuturesFeed.h"
#include "risk/RiskControl.h"

using namespace md;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static Nanos nowNs() {
    return (Nanos)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// 1) 转换工具单测
// ---------------------------------------------------------------------------
static void test_convert_utils() {
    for (const char* sym : {"IF", "IC", "TF", "RB", "AU", "CU"}) {
        double yuan = 3999.4;
        PriceTicks t = priceToTicks(yuan, sym);
        double back = ticksToPrice(t, sym);
        CHECK(std::fabs(back - yuan) <= symbolPriceUnit(sym) + 1e-9);
    }
    CHECK(symbolToInst("IF") == 0);
    CHECK(symbolToInst("CU") == 5);
    CHECK(symbolToInst("ZZ9999") == FuturesFeed::kNumInstruments);  // 未知合约

    Nanos ns = parseExchangeTimeNs("20261015", "09:30:00", 123);
    CHECK(ns > 0);
    // 2026-10-15 09:30:00 UTC 应落在合理范围（> 2020-01-01）
    CHECK(ns > (Nanos)1577836800ULL * 1'000'000'000ULL);

    Nanos ns2 = parseExchangeTimeNs("", "", -1);   // 非法 → 兜底（>0）
    CHECK(ns2 > 0);
}

// ---------------------------------------------------------------------------
// 2) CTP 仿真接入集成测试
// ---------------------------------------------------------------------------
static void test_ctp_sim_feed() {
    RiskControl risk;
    risk.configureDefault(RiskConfig{ .minPrice = 0, .maxPrice = 0, .maxLong = 0,
                                      .maxShort = 0, .maxAgeNs = 0, .orderRatePerSec = 0 });
    std::atomic<uint64_t> recv{0}, allow{0}, rej{0}, badInst{0};

    CtpMdFeed feed;
    feed.setOnMarketData([&](const MarketTick& t, Nanos srcTs) {
        if (t.inst >= FuturesFeed::kNumInstruments) { badInst.fetch_add(1); return; }
        auto d = risk.checkTick(t, nowNs(), srcTs);
        recv.fetch_add(1, std::memory_order_relaxed);
        if (d == RISK_ALLOW) allow.fetch_add(1, std::memory_order_relaxed);
        else                 rej.fetch_add(1, std::memory_order_relaxed);
    });

    FeedConfig cfg;
    cfg.instruments = {"IF", "IC", "TF", "RB", "AU", "CU"};
    cfg.simulation  = true;
    CHECK(feed.start(cfg));
    CHECK(std::string(feed.backend()) == "ctp-sim");

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    feed.stop();

    CHECK(recv.load() > 0);                 // 收到行情
    CHECK(badInst.load() == 0);            // 无未知合约
    CHECK(allow.load() + rej.load() == recv.load());   // 每笔都过风控
    std::printf("[feed-test] ctp-sim  recv=%llu allow=%llu reject=%llu\n",
                (unsigned long long)recv.load(), (unsigned long long)allow.load(),
                (unsigned long long)rej.load());
}

// ---------------------------------------------------------------------------
// 3) 飞马 仿真接入集成测试
// ---------------------------------------------------------------------------
static void test_femas_sim_feed() {
    RiskControl risk;
    risk.configureDefault(RiskConfig{ .minPrice = 0, .maxPrice = 0, .maxLong = 0,
                                      .maxShort = 0, .maxAgeNs = 0, .orderRatePerSec = 0 });
    std::atomic<uint64_t> recv{0}, allow{0}, rej{0}, badInst{0};

    FemasMdFeed feed;
    feed.setOnMarketData([&](const MarketTick& t, Nanos srcTs) {
        if (t.inst >= FuturesFeed::kNumInstruments) { badInst.fetch_add(1); return; }
        auto d = risk.checkTick(t, nowNs(), srcTs);
        recv.fetch_add(1, std::memory_order_relaxed);
        if (d == RISK_ALLOW) allow.fetch_add(1, std::memory_order_relaxed);
        else                 rej.fetch_add(1, std::memory_order_relaxed);
    });

    FeedConfig cfg;
    cfg.instruments = {"IF", "IC", "TF", "RB", "AU", "CU"};
    cfg.simulation  = true;
    CHECK(feed.start(cfg));
    CHECK(std::string(feed.backend()) == "femas-sim");

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    feed.stop();

    CHECK(recv.load() > 0);
    CHECK(badInst.load() == 0);
    CHECK(allow.load() + rej.load() == recv.load());
    std::printf("[feed-test] femas-sim recv=%llu allow=%llu reject=%llu\n",
                (unsigned long long)recv.load(), (unsigned long long)allow.load(),
                (unsigned long long)rej.load());
}

int main() {
    test_convert_utils();
    test_ctp_sim_feed();
    test_femas_sim_feed();
    std::printf("\n[feed-test] checks=%d fails=%d\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
