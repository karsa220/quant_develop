#include "monitor/Monitor.h"
#include "risk/RiskControl.h"

#include <cstdio>
#include <cstdarg>
#include <chrono>

namespace md {

static uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

Monitor::Monitor() = default;

Monitor::~Monitor() {
    stopWatchdog();
}

void Monitor::logf(LogLevel lvl, const char* fmt, ...) {
    // spinlock 保护，避免多线程交错打印
    while (logLock_.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char* tag = logLevelName(lvl);
    if (lvl >= LOG_ALERT) {
        std::fprintf(stderr, "[monitor][%s] %s\n", tag, buf);
    } else {
        std::fprintf(stdout, "[monitor][%s] %s\n", tag, buf);
    }
    logLock_.clear(std::memory_order_release);
}

void Monitor::setAlertThresholds(double maxDropRate, uint64_t maxP99Ns, Nanos stallTimeoutNs) {
    maxDropRate_.store(maxDropRate, std::memory_order_relaxed);
    maxP99Ns_.store(maxP99Ns, std::memory_order_relaxed);
    stallTimeoutNs_.store(stallTimeoutNs, std::memory_order_relaxed);
}

void Monitor::startWatchdog(RiskControl* risk, uint64_t periodMs) {
    if (wdRunning_.load()) return;
    risk_ = risk;
    wdRunning_.store(true);
    wdThread_ = std::thread([this, periodMs]() {
        uint64_t prevPub = 0, prevDrop = 0;
        while (wdRunning_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
            if (!wdRunning_.load()) break;

            const uint64_t pub  = published_.load();
            const uint64_t drop = dropped_.load();
            const uint64_t dPub  = pub - prevPub;
            const uint64_t dDrop = drop - prevDrop;
            prevPub = pub; prevDrop = drop;

            // 1) 丢弃率越界
            if (dPub > 0) {
                const double dropRate = static_cast<double>(dDrop) / static_cast<double>(dPub);
                if (dropRate > maxDropRate_.load()) {
                    logf(LOG_ALERT, "drop-rate %.4f > threshold %.4f (pub=%llu drop=%llu)",
                         dropRate, maxDropRate_.load(),
                         static_cast<unsigned long long>(dPub),
                         static_cast<unsigned long long>(dDrop));
                    recordAlert();
                    if (risk_ && !risk_->halted())
                        risk_->triggerHalt("drop-rate breach");
                }
            }

            // 2) p99 延迟越界
            if (lat_.count() > 0) {
                const uint64_t p99 = lat_.p99();
                if (p99 > maxP99Ns_.load()) {
                    logf(LOG_ALERT, "p99 latency %.3f ms > threshold %.3f ms",
                         p99 / 1e6, maxP99Ns_.load() / 1e6);
                    recordAlert();
                }
            }

            // 3) 行情卡顿（无心跳）
            const Nanos last = lastTickNs_.load(std::memory_order_acquire);
            if (last != 0) {
                const Nanos age = (nowNs() > last) ? (nowNs() - last) : 0;
                if (age > stallTimeoutNs_.load()) {
                    logf(LOG_ALERT, "market-data stall %.2f s (no tick)", age / 1e9);
                    recordAlert();
                }
            }
        }
    });
}

void Monitor::stopWatchdog() {
    if (wdRunning_.load()) {
        wdRunning_.store(false);
        if (wdThread_.joinable()) wdThread_.join();
    }
}

std::string Monitor::telemetryJson() const {
    char buf[1024];
    const uint64_t pub = published_.load(), cons = consumed_.load(),
                   drop = dropped_.load(), sig = signals_.load(),
                   rej = rejected_.load(), al = alerts_.load();
    const uint64_t p50 = lat_.count() ? lat_.p50() : 0;
    const uint64_t p99 = lat_.count() ? lat_.p99() : 0;
    const uint64_t p999 = lat_.count() ? lat_.p999() : 0;
    snprintf(buf, sizeof(buf),
        "{\"published\":%llu,\"consumed\":%llu,\"dropped\":%llu,"
        "\"signals\":%llu,\"rejected\":%llu,\"alerts\":%llu,"
        "\"latency_us\":{\"p50\":%.3f,\"p99\":%.3f,\"p99_9\":%.3f}}",
        static_cast<unsigned long long>(pub), static_cast<unsigned long long>(cons),
        static_cast<unsigned long long>(drop), static_cast<unsigned long long>(sig),
        static_cast<unsigned long long>(rej), static_cast<unsigned long long>(al),
        p50 / 1e3, p99 / 1e3, p999 / 1e3);
    return std::string(buf);
}

}  // namespace md
