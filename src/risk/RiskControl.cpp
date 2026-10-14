#include "risk/RiskControl.h"

#include <cstdio>

namespace md {

RiskControl::RiskControl() {
    // 出厂默认：宽松（不检查），由 configure* 按需收紧
}

void RiskControl::configure(InstrumentId id, const RiskConfig& cfg) {
    if (id >= FuturesFeed::kNumInstruments) return;
    InstState& s = inst_[id];
    s.minP.store(cfg.minPrice,  std::memory_order_relaxed);
    s.maxP.store(cfg.maxPrice,  std::memory_order_relaxed);
    s.maxLong.store(cfg.maxLong, std::memory_order_relaxed);
    s.maxShort.store(cfg.maxShort, std::memory_order_relaxed);
    s.maxAge.store(cfg.maxAgeNs, std::memory_order_relaxed);
    s.orderRate.store(cfg.orderRatePerSec, std::memory_order_relaxed);
}

void RiskControl::configureDefault(const RiskConfig& cfg) {
    for (uint32_t i = 0; i < FuturesFeed::kNumInstruments; ++i) configure(i, cfg);
}

RiskDecision RiskControl::checkTick(const MarketTick& t, Nanos now, Nanos srcTs) {
    if (halted_.load(std::memory_order_acquire)) return RISK_HALT;

    const InstrumentId id = t.inst;
    if (id >= FuturesFeed::kNumInstruments) { ++rejected_; return RISK_REJECT; }

    InstState& s = inst_[id];

    // 1) 价格区间
    const RiskConfig cfg{
        s.minP.load(std::memory_order_relaxed),
        s.maxP.load(std::memory_order_relaxed),
        s.maxLong.load(std::memory_order_relaxed),
        s.maxShort.load(std::memory_order_relaxed),
        s.maxAge.load(std::memory_order_relaxed),
        s.orderRate.load(std::memory_order_relaxed),
    };
    if (cfg.minPrice != 0 && t.price < cfg.minPrice) { ++rejected_; return RISK_REJECT; }
    if (cfg.maxPrice != 0 && t.price > cfg.maxPrice) { ++rejected_; return RISK_REJECT; }

    // 2) 行情时效性（不允许来自未来超过 1s 的时钟漂移）
    if (cfg.maxAgeNs != 0 && now > srcTs && (now - srcTs) > cfg.maxAgeNs) {
        ++rejected_; return RISK_REJECT;
    }

    // 通过：按 side 更新模拟净持仓（买 +qty / 卖 -qty）
    const int64_t delta = (t.side == kSideBuy) ? static_cast<int64_t>(t.qty)
                                               : -static_cast<int64_t>(t.qty);
    s.position.fetch_add(delta, std::memory_order_relaxed);
    ++allowed_;
    return RISK_ALLOW;
}

RiskDecision RiskControl::checkOrder(InstrumentId id, int8_t dir, Qty qty, Nanos now) {
    if (halted_.load(std::memory_order_acquire)) return RISK_HALT;
    if (id >= FuturesFeed::kNumInstruments) { ++rejected_; return RISK_REJECT; }

    InstState& s = inst_[id];
    const int64_t maxLong  = s.maxLong.load(std::memory_order_relaxed);
    const int64_t maxShort = s.maxShort.load(std::memory_order_relaxed);

    // 3) 持仓限额：投影新净持仓
    const int64_t cur = s.position.load(std::memory_order_relaxed);
    const int64_t proj = cur + (dir > 0 ? static_cast<int64_t>(qty) : -static_cast<int64_t>(qty));
    if (maxLong > 0 && proj > maxLong)  { ++rejected_; return RISK_REJECT; }
    if (maxShort > 0 && proj < -maxShort) { ++rejected_; return RISK_REJECT; }

    // 4) 下单频率：令牌桶（每秒 refill orderRate 个令牌）
    const double rate = s.orderRate.load(std::memory_order_relaxed);
    if (rate > 0.0) {
        uint64_t ws = s.windowStartNs.load(std::memory_order_relaxed);
        uint64_t tok = s.tokens.load(std::memory_order_relaxed);
        if (now - ws >= 1'000'000'000ULL) {
            ws = now;
            tok = static_cast<uint64_t>(rate);
            s.windowStartNs.store(ws, std::memory_order_relaxed);
        }
        if (tok == 0) { ++rejected_; return RISK_REJECT; }
        --tok;
        s.tokens.store(tok, std::memory_order_relaxed);
    }

    ++allowed_;
    return RISK_ALLOW;
}

void RiskControl::triggerHalt(const char* reason) {
    halted_.store(true, std::memory_order_release);
    haltReason_.store(reason, std::memory_order_release);
    haltedCount_.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[risk] *** HALT triggered: %s ***\n", reason ? reason : "?");
}

void RiskControl::resume() {
    halted_.store(false, std::memory_order_release);
    haltReason_.store(nullptr, std::memory_order_release);
}

}  // namespace md
