#include "sched/PeriodicTimer.h"

#include <cstdio>

#if defined(__linux__)
#  include <sys/timerfd.h>
#  include <unistd.h>
#  include <time.h>
#elif defined(_WIN32)
#  include <windows.h>
#else
#  error "unsupported platform"
#endif

namespace md {

#if defined(__linux__)
bool PeriodicTimer::startPeriodic(uint64_t periodUs, Callback cb) {
    stop();
    cb_ = std::move(cb);
    fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd_ < 0) { std::perror("[timer] timerfd_create"); return false; }
    const uint64_t ns = periodUs * 1000ULL;
    struct itimerspec spec {};
    spec.it_interval.tv_sec  = static_cast<time_t>(ns / 1000000000ULL);
    spec.it_interval.tv_nsec = static_cast<long>(ns % 1000000000ULL);
    spec.it_value = spec.it_interval;  // 立即开始首个周期
    if (::timerfd_settime(fd_, 0, &spec, nullptr) < 0) {
        std::perror("[timer] timerfd_settime"); ::close(fd_); fd_ = -1; return false;
    }
    periodic_ = true;
    running_ = true;
    thread_ = std::thread(&PeriodicTimer::worker, this);
    return true;
}

bool PeriodicTimer::startAtWallClock(Nanos epochNs, Callback cb) {
    stop();
    cb_ = std::move(cb);
    fd_ = ::timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (fd_ < 0) { std::perror("[timer] timerfd_create(REALTIME)"); return false; }
    struct itimerspec spec {};
    spec.it_value.tv_sec  = static_cast<time_t>(epochNs / 1000000000ULL);
    spec.it_value.tv_nsec = static_cast<long>(epochNs % 1000000000ULL);
    // it_interval = 0 => 单次触发（开盘/收盘）
    if (::timerfd_settime(fd_, TFD_TIMER_ABSTIME, &spec, nullptr) < 0) {
        std::perror("[timer] timerfd_settime(abstime)"); ::close(fd_); fd_ = -1; return false;
    }
    periodic_ = false;
    running_ = true;
    thread_ = std::thread(&PeriodicTimer::worker, this);
    return true;
}

void PeriodicTimer::worker() {
    uint64_t expirations = 0;
    while (running_.load()) {
        uint64_t buf = 0;
        ssize_t n = ::read(fd_, &buf, sizeof(buf));
        if (!running_.load()) break;
        if (n == sizeof(buf)) {
            expirations += buf;
            if (cb_) cb_();
        }
        if (!periodic_) { running_ = false; break; }  // 绝对单次：触发一次即退出
    }
}

void PeriodicTimer::stop() {
    if (!running_.load()) return;
    running_ = false;
    if (fd_ >= 0) {
        // 关闭 fd 让阻塞的 read() 返回，线程自然退出
        ::close(fd_);
        fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}
#else
bool PeriodicTimer::startPeriodic(uint64_t periodUs, Callback cb) {
    stop();
    cb_ = std::move(cb);
    timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!timer_ || !stopEvent_) {
        std::fprintf(stderr, "[timer] CreateWaitableTimer/Event failed\n");
        return false;
    }
    LARGE_INTEGER li;
    li.QuadPart = -static_cast<LONGLONG>(periodUs * 10ULL);  // 负 = 相对(100ns)
    LONG periodMs = static_cast<LONG>(periodUs / 1000ULL);
    if (periodMs == 0) periodMs = 1;
    ::SetWaitableTimer(timer_, &li, periodMs, nullptr, nullptr, FALSE);
    periodUs_ = periodUs;
    periodic_ = true;
    running_  = true;
    thread_ = std::thread(&PeriodicTimer::worker, this);
    return true;
}

bool PeriodicTimer::startAtWallClock(Nanos epochNs, Callback cb) {
    stop();
    cb_ = std::move(cb);
    timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!timer_ || !stopEvent_) {
        std::fprintf(stderr, "[timer] CreateWaitableTimer/Event failed\n");
        return false;
    }
    // Unix 纳秒 -> FILETIME（1601 纪元，100ns 单位），单次绝对触发
    const uint64_t unixToWin = 116444736000000000ULL;
    ULARGE_INTEGER ft {};
    ft.QuadPart = epochNs / 100ULL + unixToWin;
    LARGE_INTEGER li;
    li.LowPart  = ft.LowPart;
    li.HighPart = ft.HighPart;
    ::SetWaitableTimer(timer_, &li, 0, nullptr, nullptr, FALSE);
    absNs_    = epochNs;
    periodic_ = false;
    running_  = true;
    thread_ = std::thread(&PeriodicTimer::worker, this);
    return true;
}

void PeriodicTimer::worker() {
    HANDLE h[2] = { static_cast<HANDLE>(timer_), static_cast<HANDLE>(stopEvent_) };
    for (;;) {
        DWORD r = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 1) break;          // 收到 stop 事件
        if (r == WAIT_OBJECT_0) {
            if (cb_) cb_();
            if (!periodic_) break;                  // 绝对单次：触发一次即退出
        } else {
            break;                                   // 异常
        }
    }
}

void PeriodicTimer::stop() {
    if (!running_.load()) return;
    running_ = false;
    if (stopEvent_) { ::SetEvent(stopEvent_); }
    if (thread_.joinable()) thread_.join();
    if (timer_)    { ::CloseHandle(timer_);    timer_    = nullptr; }
    if (stopEvent_) { ::CloseHandle(stopEvent_); stopEvent_ = nullptr; }
}
#endif

}  // namespace md
