#include "feed/FuturesFeed.h"
#include "feed/CtpLikeCodec.h"

#include <cmath>
#include <chrono>

namespace md {

static constexpr Instrument kInstruments[FuturesFeed::kNumInstruments] = {
    {0, "IF",  350000, 2,    2.0},   // 沪深300股指期货, 0.2 点/tick (价格以 0.1 计)
    {1, "IC",  520000, 2,    3.0},   // 中证500
    {2, "TF", 1020000, 5,    1.0},   // 国债期货, 0.005 点/tick
    {3, "RB",  360000, 10,   4.0},   // 螺纹钢, 1 元/tick
    {4, "AU", 4600000, 20,   5.0},   // 黄金, 0.02 元/tick
    {5, "CU", 6800000, 100,  8.0},   // 铜,   0.1 元/tick
};

FuturesFeed::FuturesFeed(uint64_t seed) : rng_(seed) {
    for (size_t i = 0; i < kNumInstruments; ++i) {
        inst_[i] = kInstruments[i];
        mid_[i]  = kInstruments[i].basePrice;
    }
}

bool FuturesFeed::nextTick(MarketTick& out, Nanos& srcTs) {
    if (rate_ > 0.0) {
        // 泊松到达：用指数分布推进下一个事件时刻
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        Nanos nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        if (nowNs < nextEmitNs_) return false;  // 节流未到
        double dt = rng_.nextExponential(rate_);      // 秒
        nextEmitNs_ = nowNs + static_cast<Nanos>(dt * 1e9);
    }

    const uint32_t idx = rng_.nextU32() % kNumInstruments;
    const Instrument& ins = inst_[idx];

    // 随机游走中间价
    double step = (rng_.nextDouble() - 0.5) * 2.0 * ins.sigma;
    PriceTicks delta = static_cast<PriceTicks>(step / ins.tickSize);
    if (delta == 0) delta = (rng_.nextU32() & 1) ? 1 : -1;
    mid_[idx] += delta * ins.tickSize;
    if (mid_[idx] < ins.tickSize) mid_[idx] = ins.tickSize;

    // 成交价围绕中间价 ±1 tick，方向随机
    const bool buy = (rng_.nextU32() & 1) == 0;
    out.inst  = ins.id;
    out.price = mid_[idx] + (buy ? ins.tickSize : -ins.tickSize);
    out.qty   = static_cast<Qty>(1 + (rng_.nextU32() % 50));
    out.side  = buy ? kSideBuy : kSideSell;

    srcTs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    return true;
}

bool FuturesFeed::nextFrame(uint8_t* buf, uint32_t* outLen, MarketTick& out, Nanos& srcTs) {
    if (!buf) return false;                 // 无输出缓冲区直接失败（不写空指针）
    if (!nextTick(out, srcTs)) return false;
    *outLen = CtpLikeCodec_encodeTick(out, buf);
    return true;
}

}  // namespace md
