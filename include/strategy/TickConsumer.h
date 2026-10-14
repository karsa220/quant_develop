#pragma once
// ============================================================================
// strategy/TickConsumer.h —— 低延迟消费 / 策略处理
//
// 从共享内存环形缓冲读取行情 → 解码 → 重建逐合约盘口(简化) → 生成信号 →
// 测量「行情源时间戳 → 信号生成」的端到端延迟。这是 HFT 系统里策略/执行进程的角色。
// ============================================================================
#include <cstdint>
#include <atomic>
#include <vector>
#include "shm/ShmSegment.h"
#include "ipc/SpscRingBuffer.h"
#include "feed/FuturesFeed.h"
#include "bench/LatencyRecorder.h"
#include "risk/RiskControl.h"
#include "monitor/Monitor.h"

namespace md {

class TickConsumer {
public:
    TickConsumer() = default;

    // 挂接已存在的共享内存环形缓冲（capacity 由 ring 控制头读出）
    bool init(const char* shmName);

    // 绑定风控与监控（策略执行路径的合规校验 + 指标采集）；可为 nullptr
    void bind(RiskControl* risk, Monitor* mon) { risk_ = risk; mon_ = mon; }

    // 设置「生产者结束」标志：生产者线程退出后置 true，消费者排空环后随之退出
    // （避免生产者因背压少发 tick 时消费者永久自旋）
    void setDrainSignal(std::atomic<bool>* done) { done_ = done; }

    // 运行消费者：持续读取，直到 requestStop() 或 maxTicks 条 tick 处理完。
    // latencies 收集 tick→信号 延迟（纳秒），可为 nullptr。
    void run(uint64_t maxTicks, LatencyRecorder* latencies);

    void requestStop() { running_ = false; }

    uint64_t consumed() const { return consumed_.load(); }
    uint64_t bad()     const { return bad_.load(); }
    uint64_t signals() const { return signals_.load(); }
    uint64_t ticksConsumed() const { return ticks_.load(); }   // 已处理的逐笔成交数

    // 各合约最新成交价（演示用）
    PriceTicks lastPrice(InstrumentId id) const {
        return id < FuturesFeed::kNumInstruments ? lastPx_[id] : 0;
    }

private:
    ShmSegment     shm_;
    SpscRingBuffer ring_;
    RiskControl*   risk_ = nullptr;
    Monitor*       mon_  = nullptr;
    std::atomic<bool>* done_ = nullptr;
    std::atomic<uint64_t> consumed_{0};
    std::atomic<uint64_t> bad_{0};
    std::atomic<uint64_t> signals_{0};
    std::atomic<uint64_t> ticks_{0};
    std::atomic<bool>     running_{true};

    // 逐合约：上一笔买价 / 卖价（用于构造简化盘口与微价信号）
    PriceTicks lastBid_[FuturesFeed::kNumInstruments] = {0};
    PriceTicks lastAsk_[FuturesFeed::kNumInstruments] = {0};
    PriceTicks lastPx_[FuturesFeed::kNumInstruments]  = {0};   // 最新成交价
    int8_t     prevDir_[FuturesFeed::kNumInstruments] = {0};   // 上一笔方向(+1/-1)，用于反转信号

    uint8_t msgBuf_[kSlotSize];
};

}  // namespace md
