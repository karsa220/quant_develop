#pragma once
// ============================================================================
// risk/RiskControl.h —— 交易风控模块（JD 职责：负责风控模块的 C++ 实现与优化）
//
// 负责在「行情进入策略」和「策略生成信号拟下单」两处做合规校验：
//   1) 价格合法性：成交价必须在 [minPrice, maxPrice] 区间内（过滤坏点/越界）；
//   2) 行情时效性：srcTs 不能来自「未来」（时钟漂移/重放攻击）；
//   3) 持仓限额：净持仓不得超过 maxLong / maxShort（单边限额）；
//   4) 下单频率：每个合约每秒下单数受令牌桶限流（防雪崩/乌龙指）；
//   5) 熔断开关(kill-switch)：触发后所有决策返回 HALT，支持监控联动拉闸。
//
// 设计：热路径零分配、零锁（per-instrument 状态由单一消费线程访问，仅 halted_
// 标志跨线程，用 acquire/release）；cache-line 隔离防伪共享。
// ============================================================================
#include <cstdint>
#include <atomic>
#include "common/Types.h"
#include "feed/FuturesFeed.h"

namespace md {

enum RiskDecision : uint8_t {
    RISK_ALLOW  = 0,   // 放行
    RISK_REJECT = 1,   // 拒绝（本笔不成交/不下单，但系统继续运行）
    RISK_HALT   = 2,   // 熔断：全局停摆，等待人工/自动恢复
};

inline const char* riskDecisionName(RiskDecision d) {
    return d == RISK_ALLOW ? "ALLOW" : (d == RISK_REJECT ? "REJECT" : "HALT");
}

struct RiskConfig {
    PriceTicks minPrice  = 0;      // 价格下限（tick 计数）；0 表示不检查
    PriceTicks maxPrice  = 0;      // 价格上限（tick 计数）；0 表示不检查
    int64_t    maxLong   = 0;      // 最大净多仓（手）；0 表示不检查
    int64_t    maxShort  = 0;      // 最大净空仓（手）；<=0 表示不检查
    Nanos      maxAgeNs  = 0;      // 行情最大允许滞后再判定时效（ns）；0 不检查
    double     orderRatePerSec = 0;// 每合约下单频率上限（笔/秒）；0 不限
};

class RiskControl {
public:
    RiskControl();

    // 单个合约配置
    void configure(InstrumentId id, const RiskConfig& cfg);
    // 默认配置应用到所有合约
    void configureDefault(const RiskConfig& cfg);

    // 行情进入策略前的合规校验（热路径，单消费线程调用）
    // 通过则按 side 更新模拟净持仓（买 +qty / 卖 -qty），返回 ALLOW。
    RiskDecision checkTick(const MarketTick& t, Nanos now, Nanos srcTs);

    // 策略产生信号(拟下单)前的限额 + 频率校验
    // dir: +1 买 / -1 卖；qty 手数。返回 ALLOW 时不清持仓（持仓由成交回补更新）。
    RiskDecision checkOrder(InstrumentId id, int8_t dir, Qty qty, Nanos now);

    // 熔断：触发后所有决策返回 HALT
    void triggerHalt(const char* reason);
    void resume();
    bool halted() const { return halted_.load(std::memory_order_acquire); }
    const char* haltReason() const { return haltReason_.load(std::memory_order_acquire); }

    // 统计（原子，便于监控线程读取）
    uint64_t allowed()    const { return allowed_.load(std::memory_order_relaxed); }
    uint64_t rejected()   const { return rejected_.load(std::memory_order_relaxed); }
    uint64_t haltedCount()const { return haltedCount_.load(std::memory_order_relaxed); }
    // 查询某合约当前模拟净持仓（演示用）
    int64_t  position(InstrumentId id) const {
        return id < FuturesFeed::kNumInstruments
                   ? inst_[id].position.load(std::memory_order_relaxed) : 0;
    }

private:
    struct InstState {
        alignas(64) std::atomic<PriceTicks> minP{0};
        std::atomic<PriceTicks> maxP{0};
        std::atomic<int64_t>    maxLong{0};
        std::atomic<int64_t>    maxShort{0};
        std::atomic<Nanos>      maxAge{0};
        std::atomic<double>     orderRate{0};
        alignas(64) std::atomic<int64_t> position{0};   // 模拟净持仓
        std::atomic<uint64_t>   tokens{0};             // 令牌桶
        std::atomic<uint64_t>   windowStartNs{0};
    };

    InstState inst_[FuturesFeed::kNumInstruments];
    std::atomic<bool>        halted_{false};
    std::atomic<const char*> haltReason_{nullptr};
    alignas(64) std::atomic<uint64_t> allowed_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> haltedCount_{0};
};

}  // namespace md
