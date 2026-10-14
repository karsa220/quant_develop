#pragma once
// ============================================================================
// monitor/Monitor.h —— 交易日志 / 监控告警 / 看门狗（JD 职责：交易日志、监控告警
// 系统的开发，支持线上问题快速定位与恢复）
//
// 能力：
//   - 分级日志 INFO/WARN/ERROR/ALERT（spinlock 保护，低开销，可定向到 stdout/文件）；
//   - 指标计数器：发布/消费/丢弃/信号/风控拒绝/告警；
//   - 端到端延迟直方图（p50/p90/p99/p99.9），基于 LatencyRecorder；
//   - 阈值告警：丢弃率越界 / p99 延迟越界 / 行情卡顿 → 记 ALERT 并可联动熔断；
//   - 看门狗线程：周期性自检，发现异常即告警，必要时触发 RiskControl 熔断拉闸；
//   - 遥测快照 telemetryJson()：供 HTTP /metrics 接口对外暴露（对接 Grafana 思路）。
// ============================================================================
#include <cstdint>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "common/Types.h"
#include "bench/LatencyRecorder.h"

namespace md {

enum LogLevel : uint8_t { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2, LOG_ALERT = 3 };
inline const char* logLevelName(LogLevel l) {
    switch (l) {
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_ALERT: return "ALERT";
        default:        return "INFO";
    }
}

class RiskControl;   // 前向，看门狗可联动熔断

class Monitor {
public:
    Monitor();
    ~Monitor();

    // 分级日志（变参，线程安全）
    void logf(LogLevel lvl, const char* fmt, ...);

    // 指标计数（热路径，原子）
    void recordPublished(uint64_t n = 1) { published_.fetch_add(n, std::memory_order_relaxed); }
    void recordConsumed(uint64_t n = 1) { consumed_.fetch_add(n, std::memory_order_relaxed); }
    void recordDropped(uint64_t n = 1)  { dropped_.fetch_add(n, std::memory_order_relaxed); }
    void recordSignal(uint64_t n = 1)   { signals_.fetch_add(n, std::memory_order_relaxed); }
    void recordRejected(uint64_t n = 1) { rejected_.fetch_add(n, std::memory_order_relaxed); }
    void recordAlert(uint64_t n = 1)    { alerts_.fetch_add(n, std::memory_order_relaxed); }

    // 端到端延迟采样（纳秒），由消费线程按抽样调用
    void recordLatency(uint64_t ns) { lat_.record(ns); }

    // 行情心跳（由消费线程每次 tick 喂一次，看门狗据此判断卡顿）
    void heartbeat(Nanos ts) { lastTickNs_.store(ts, std::memory_order_release); }

    // 阈值配置
    void setAlertThresholds(double maxDropRate = 0.01,     // 丢弃率 > 1% 告警
                            uint64_t maxP99Ns = 2'000'000,  // p99 > 2ms 告警
                            Nanos stallTimeoutNs = 3'000'000'000ULL); // 3s 无行情=卡顿

    // 看门狗：周期自检；risk 非空时可在严重越界时联动熔断
    void startWatchdog(RiskControl* risk, uint64_t periodMs = 1000);
    void stopWatchdog();

    // 遥测快照（JSON），供 /metrics 暴露
    std::string telemetryJson() const;

    // 只读统计
    uint64_t published() const { return published_.load(std::memory_order_relaxed); }
    uint64_t consumed()  const { return consumed_.load(std::memory_order_relaxed); }
    uint64_t dropped()   const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t signals()   const { return signals_.load(std::memory_order_relaxed); }
    uint64_t rejected()  const { return rejected_.load(std::memory_order_relaxed); }
    uint64_t alerts()    const { return alerts_.load(std::memory_order_relaxed); }

private:
    void watchdogLoop();

    alignas(64) std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> consumed_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> signals_{0};
    std::atomic<uint64_t> rejected_{0};
    alignas(64) std::atomic<uint64_t> alerts_{0};
    LatencyRecorder lat_;

    alignas(64) std::atomic<Nanos> lastTickNs_{0};
    std::atomic<double>    maxDropRate_{0.01};
    std::atomic<uint64_t>  maxP99Ns_{2'000'000};
    std::atomic<Nanos>     stallTimeoutNs_{3'000'000'000ULL};

    RiskControl* risk_ = nullptr;
    std::atomic<bool>   wdRunning_{false};
    std::thread         wdThread_;

    // 日志 spinlock（比 mutex 轻，且不进入内核）
    std::atomic_flag    logLock_ = ATOMIC_FLAG_INIT;
};

}  // namespace md
