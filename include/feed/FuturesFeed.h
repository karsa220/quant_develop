#pragma once
// ============================================================================
// feed/FuturesFeed.h —— 期货行情源（模拟 CTP / 交易所逐笔推送）
//
// 真实环境里，期货公司网关（如 CTP 的 MdApi）通过 TCP 推送逐笔成交与行情快照。
// 这里用「随机游走中间价 + 泊松式到达」合成多合约行情，既可控又可复现，
// 作为整条链路的入口（采集层）。每次 nextTick 给行情打上「交易所时间戳」srcTs。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <cmath>
#include "common/Types.h"

namespace md {

// 轻量高性能 RNG（xorshift64*），热路径避免 mt19937 的开销
class FastRng {
public:
    explicit FastRng(uint64_t seed = 0x9E3779B97F4A7C15ULL) : s_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t nextU64() {
        uint64_t x = s_;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        s_ = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    uint32_t nextU32() { return static_cast<uint32_t>(nextU64()); }
    // [0,1) 浮点
    double nextDouble() {
        return static_cast<double>(nextU64() >> 11) / static_cast<double>(1ULL << 53);
    }
    // 指数分布（泊松过程到达间隔，单位：秒）
    double nextExponential(double lambda) {
        double u = nextDouble();
        if (u <= 1e-12) u = 1e-12;
        return -std::log(u) / lambda;
    }
private:
    uint64_t s_;
};

struct Instrument {
    InstrumentId id;
    const char*  symbol;
    PriceTicks   basePrice;   // 初始中间价（tick）
    PriceTicks   tickSize;    // 最小变动价位（tick）
    double       sigma;       // 每步价格波动（tick）
};

class FuturesFeed {
public:
    static constexpr size_t kNumInstruments = 6;

    explicit FuturesFeed(uint64_t seed = 42);

    // 生成下一个逐笔成交（逻辑事件）。srcTs = 行情源时间戳（交易所）。
    // 返回 false 表示按泊松节流「本时刻无事件」（仅 rate-limited 模式）。
    bool nextTick(MarketTick& out, Nanos& srcTs);

    // 生成并编码为一帧 CTP 风格二进制帧（模拟网络字节流）。
    bool nextFrame(uint8_t* buf, uint32_t* outLen, MarketTick& out, Nanos& srcTs);

    // 设置泊松到达速率（ticks/秒）。0 = 不限速（尽可能快，用于压测）。
    void setRate(double ticksPerSec) { rate_ = ticksPerSec; }

    const Instrument& instrument(size_t i) const { return inst_[i]; }

private:
    FastRng      rng_;
    Instrument   inst_[kNumInstruments];
    PriceTicks   mid_[kNumInstruments];   // 当前中间价（tick）
    double       rate_ = 0.0;
    Nanos        nextEmitNs_ = 0;         // 下一允许事件时刻（rate-limited 用）
};

}  // namespace md
