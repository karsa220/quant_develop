#pragma once
// ============================================================================
// bench/LatencyRecorder.h —— 分位延迟统计（头文件库）
//
// 收集一批延迟样本（纳秒），结束时排序求 p50/p90/p99/p99.9 与最大/均值。
// 用预分配 vector 减少分配开销；仅在需要分位数时排序一次。
// ============================================================================
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

namespace md {

class LatencyRecorder {
public:
    explicit LatencyRecorder(size_t reserve = 1 << 20) { samples_.reserve(reserve); }

    void record(uint64_t ns) { samples_.push_back(ns); }

    size_t count() const { return samples_.size(); }

    uint64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        std::vector<uint64_t> s = samples_;   // 副本排序，不改原序
        std::sort(s.begin(), s.end());
        double rank = (p / 100.0) * (s.size() - 1);
        size_t lo = static_cast<size_t>(std::floor(rank));
        size_t hi = static_cast<size_t>(std::ceil(rank));
        if (lo == hi) return s[lo];
        double frac = rank - lo;
        return static_cast<uint64_t>(s[lo] * (1 - frac) + s[hi] * frac);
    }

    uint64_t p50() const { return percentile(50); }
    uint64_t p90() const { return percentile(90); }
    uint64_t p99() const { return percentile(99); }
    uint64_t p999() const { return percentile(99.9); }

    uint64_t max() const {
        uint64_t m = 0;
        for (uint64_t v : samples_) if (v > m) m = v;
        return m;
    }
    double mean() const {
        if (samples_.empty()) return 0.0;
        double sum = 0;
        for (uint64_t v : samples_) sum += static_cast<double>(v);
        return sum / samples_.size();
    }

private:
    std::vector<uint64_t> samples_;
};

}  // namespace md
