#pragma once
// ============================================================================
// sched/PeriodicTimer.h —— 高精度周期 / 绝对定时任务
//
//   Linux  : timerfd_create + timerfd_settime（CLOCK_MONOTONIC 周期 / CLOCK_REALTIME 绝对）
//   Windows: CreateWaitableTimerExW + SetWaitableTimer（高分辨率等待定时器）
//
// 这是「Linux 定时任务」的工程实现：行情网关用周期定时器发心跳 / 刷统计；
// 交易系统用绝对定时器在 09:30 开盘、15:00 收盘做会话切换（等价于 cron 的微秒级替代）。
// 相比 busy-wait 或 sleep 轮询，timerfd / 等待定时器由内核唤醒，不占 CPU。
// ============================================================================
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

namespace md {

using Nanos = uint64_t;

class PeriodicTimer {
public:
    using Callback = std::function<void()>;

    PeriodicTimer() = default;
    ~PeriodicTimer() { stop(); }

    PeriodicTimer(const PeriodicTimer&) = delete;
    PeriodicTimer& operator=(const PeriodicTimer&) = delete;

    // 周期触发：每 periodUs 微秒调用一次 cb（创建独立工作线程）
    bool startPeriodic(uint64_t periodUs, Callback cb);

    // 绝对触发：在 epochNs（Unix 纳秒，自 1970）这一时刻调用一次 cb（如开盘/收盘）
    bool startAtWallClock(Nanos epochNs, Callback cb);

    void stop();
    bool running() const { return running_.load(); }

private:
    void worker();

    std::thread    thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> periodic_{false};
    Callback        cb_;
#if defined(__linux__)
    int             fd_ = -1;
#else
    void*           timer_ = nullptr;    // HANDLE (waitable timer)
    void*           stopEvent_ = nullptr; // HANDLE (manual-reset stop event)
    uint64_t        periodUs_ = 0;
    Nanos           absNs_    = 0;
#endif
};

}  // namespace md
