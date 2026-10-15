// ============================================================================
// feed_demo.cpp —— 真实 CTP / 飞马 feed 接入演示（默认仿真：无需 SDK/账号）
//
// 演示「登录 → 订阅 → 行情 → 风控」整条链路：两个行情源（CTP、飞马）同时接入，
// 每笔行情转换为内部 MarketTick 后送 RiskControl 合规校验。仿真后端 2 秒即可看到
// 实际吞吐与零拒绝。接真机只需 FeedConfig.simulation=false + 编译期定义 HAS_CTP/
// HAS_FEMAS 并链接柜台动态库（见 RealCtpMdApi.h / RealFemasMdApi.h 头部说明）。
// ============================================================================
#include "feed/MarketFeed.h"
#include "feed/CtpMdFeed.h"
#include "feed/FemasMdFeed.h"
#include "risk/RiskControl.h"
#include "bench/LatencyRecorder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace md;

static Nanos nowNs() {
    return (Nanos)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    const std::vector<std::string> insts = {"IF", "IC", "TF", "RB", "AU", "CU"};

    RiskControl risk;
    risk.configureDefault(RiskConfig{ .minPrice = 0, .maxPrice = 0, .maxLong = 0,
                                       .maxShort = 0, .maxAgeNs = 0, .orderRatePerSec = 0 });

    std::atomic<uint64_t> ctpRecv{0}, ctpAllow{0}, ctpRej{0};
    std::atomic<uint64_t> fmRecv{0},  fmAllow{0},  fmRej{0};
    LatencyRecorder lat;

    FeedConfig cfg;
    cfg.instruments = insts;
    cfg.simulation  = true;     // 真机：改为 false 并定义 HAS_CTP / HAS_FEMAS

    CtpMdFeed ctp;
    ctp.setOnMarketData([&](const MarketTick& t, Nanos srcTs) {
        Nanos recv = nowNs();
        auto d = risk.checkTick(t, recv, srcTs);
        ctpRecv.fetch_add(1, std::memory_order_relaxed);
        if (d == RISK_ALLOW) ctpAllow.fetch_add(1, std::memory_order_relaxed);
        else                 ctpRej.fetch_add(1, std::memory_order_relaxed);
        Nanos age = (recv > srcTs) ? (recv - srcTs) : 0;
        if (lat.count() < 2'000'000) lat.record(age);
    });

    FemasMdFeed fm;
    fm.setOnMarketData([&](const MarketTick& t, Nanos srcTs) {
        Nanos recv = nowNs();
        auto d = risk.checkTick(t, recv, srcTs);
        fmRecv.fetch_add(1, std::memory_order_relaxed);
        if (d == RISK_ALLOW) fmAllow.fetch_add(1, std::memory_order_relaxed);
        else                 fmRej.fetch_add(1, std::memory_order_relaxed);
    });

    if (!ctp.start(cfg)) { std::fprintf(stderr, "[feed-demo] CTP start failed\n"); return 1; }
    if (!fm.start(cfg))  { std::fprintf(stderr, "[feed-demo] Femas start failed\n"); return 1; }

    std::printf("============ Real Feed Access Demo (CTP + Femas, simulation) ============\n");
    std::printf("backends: ctp=%s  femas=%s\n", ctp.backend(), fm.backend());
    std::printf("running 2s: login -> subscribe -> market data -> risk control ...\n\n");

    std::this_thread::sleep_for(std::chrono::seconds(2));
    ctp.stop();
    fm.stop();

    std::printf("\n-------------------- Results --------------------\n");
    std::printf("CTP(sim)   : recv=%llu  allow=%llu  reject=%llu\n",
                (unsigned long long)ctpRecv.load(), (unsigned long long)ctpAllow.load(),
                (unsigned long long)ctpRej.load());
    std::printf("Femas(sim) : recv=%llu  allow=%llu  reject=%llu\n",
                (unsigned long long)fmRecv.load(), (unsigned long long)fmAllow.load(),
                (unsigned long long)fmRej.load());
    if (lat.count())
        std::printf("tick age(ns): p50=%llu  p99=%llu  (exchange->riskcontrol)\n",
                    (unsigned long long)lat.p50(), (unsigned long long)lat.p99());
    std::printf("==================================================\n");
    return 0;
}
