#pragma once
// ============================================================================
// gateway/MdGateway.h —— 行情网关（采集层 → 解析层 → 发布层）
//
// 职责链：FuturesFeed(合成行情) → CtpLikeCodec(编码为线路帧，模拟网络)
//        → CtpLikeCodec(解析回结构化 tick) → SpscRingBuffer(写入共享内存)。
//
// 单生产者：所有「发布到 ring」的动作都发生在生产者线程（保证 SPSC 不变式）；
// timerfd 周期任务只做统计刷盘等只读副作用，不碰 ring。
// 另用 PeriodicTimer（绝对定时）演示「开盘/收盘会话切换」——即 Linux 定时任务。
// ============================================================================
#include <cstdint>
#include <atomic>
#include "shm/ShmSegment.h"
#include "ipc/SpscRingBuffer.h"
#include "feed/FuturesFeed.h"
#include "feed/CtpLikeCodec.h"
#include "sched/PeriodicTimer.h"

namespace md {

class MdGateway {
public:
    MdGateway() : feed_(42) {}

    // 创建共享内存并初始化环形缓冲（capacity = 槽位数）
    bool init(const char* shmName, uint64_t capacity);

    // 运行生产者：持续发布，直到 totalTicks 发布完或 requestStop()
    //   rate = 泊松到达速率(ticks/s)，0 = 不限速(压测)
    //   durationMs = 运行最长时间(毫秒)，0 = 不限时
    void run(uint64_t totalTicks, double rate = 0.0, uint64_t durationMs = 0);

    void requestStop() { running_ = false; }

    uint64_t pubCount()  const { return pubCount_.load(); }
    uint64_t dropCount() const { return dropCount_.load(); }
    uint64_t capacity()  const { return ring_.capacity(); }

    // 供定时任务回调读取的只读统计
    uint64_t statsPub()  const { return pubCount_.load(); }
    uint64_t statsDrop() const { return dropCount_.load(); }

private:
    void maybeHeartbeat(uint64_t nowNs);
    void maybeSession(uint64_t nowNs);

    FuturesFeed    feed_;
    ShmSegment     shm_;
    SpscRingBuffer ring_;
    PeriodicTimer  statsTimer_;   // timerfd 周期统计刷盘（Linux 定时任务示例）

    std::atomic<uint64_t> pubCount_{0};
    std::atomic<uint64_t> dropCount_{0};
    std::atomic<bool>     running_{true};

    uint64_t startNs_   = 0;
    uint64_t lastHbNs_  = 0;
    bool     openSent_  = false;
    bool     closeSent_ = false;

    uint8_t frameBuf_[kMaxFrame];        // 线路帧缓冲（编解码用）
    uint8_t msgBuf_[kSlotSize];
};

}  // namespace md
