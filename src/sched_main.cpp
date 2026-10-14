// ============================================================================
// sched_main.cpp —— Linux 定时任务演示（期货行情采集的会话编排）
//
// 用 PeriodicTimer 演示真实交易系统里「定时任务」的两类用法：
//   1) 绝对定时：09:30 开盘 / 15:00 收盘 会话切换（等价于 cron 的微秒级替代，
//      由内核 timerfd / 等待定时器唤醒，不占 CPU）；
//   2) 周期定时：每秒刷一次统计 / 心跳（心跳丢失 = 链路异常，监控可告警）。
//
// 生产用法: sched_main --prod   （按当天 09:30 / 15:00 绝对时刻触发）
// 本地演示 : sched_main          （用「现在 +3s / +6s」让事件立刻可观测）
//
// 这套定时能力与 systemd(保活) + cron(收盘快照) 共同构成 Linux 侧调度。
// ============================================================================
#include "sched/PeriodicTimer.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>

using namespace md;

// 计算今天 h:m:s 的 Unix 纳秒（对应 CLOCK_REALTIME / system_clock）
static Nanos wallClockNs(int h, int m, int s) {
    using namespace std::chrono;
    std::time_t tt = std::time(nullptr);
    std::tm* loc = std::localtime(&tt);
    loc->tm_hour = h; loc->tm_min = m; loc->tm_sec = s;
    std::time_t target = std::mktime(loc);
    if (target == static_cast<std::time_t>(-1)) return 0;
    return static_cast<Nanos>(target) * 1000000000ULL;
}

int main(int argc, char** argv) {
    const bool prod = (argc > 1) && std::strcmp(argv[1], "--prod") == 0;

    Nanos openNs, closeNs;
    if (prod) {
        openNs  = wallClockNs(9, 30, 0);
        closeNs = wallClockNs(15, 0, 0);
        std::printf("[sched] 生产模式：等待今天 09:30 / 15:00 绝对时刻...\n");
    } else {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        Nanos nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        openNs  = nowNs + 3'000'000'000ULL;   // 演示：+3s 开盘
        closeNs = nowNs + 6'000'000'000ULL;   // 演示：+6s 收盘
        std::printf("[sched] 演示模式：开盘 +3s、收盘 +6s、统计每秒一次，运行 8s\n");
    }

    PeriodicTimer openT, closeT, statsT;

    openT.startAtWallClock(openNs, []() {
        std::printf("[sched] >>> SESSION OPEN  (09:30 开盘) —— 开始接收行情\n");
    });
    closeT.startAtWallClock(closeNs, []() {
        std::printf("[sched] >>> SESSION CLOSE (15:00 收盘) —— 停止接收，准备收盘快照\n");
    });
    statsT.startPeriodic(1000 * 1000, []() {   // 每 1s
        std::printf("[sched] ... 心跳 / 统计刷盘（1s 周期）\n");
    });

    // 演示模式跑 8s 即可看到全部事件；生产模式会一直跑到收盘后
    const int runSec = prod ? 86400 : 8;
    std::this_thread::sleep_for(std::chrono::seconds(runSec));

    openT.stop(); closeT.stop(); statsT.stop();
    std::printf("[sched] done\n");
    return 0;
}
