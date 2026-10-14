// ============================================================================
// bench_main.cpp —— 性能基准
//
//   1) 编解码吞吐：encode+decode 逐笔成交，测 ns/tick
//   2) 无锁 SPSC 环形缓冲端到端：吞吐(M/s) + 分位延迟 + 丢弃
//   3) 朴素互斥队列对照：同样负载下的吞吐/延迟（展示「性能优化」的收益）
//
// 所有数值为本机 -O2 实测；写进简历时请替换为你的机器结果。
// ============================================================================
#include "feed/FuturesFeed.h"
#include "feed/CtpLikeCodec.h"
#include "ipc/SpscRingBuffer.h"
#include "bench/LatencyRecorder.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

using namespace md;

static uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------- 1. Codec 基准 ----------------
static void benchCodec(uint64_t n) {
    FuturesFeed feed(123);
    uint8_t frame[kMaxFrame];
    MarketTick tick{};
    Nanos srcTs = 0;
    uint32_t flen = 0;
    // warmup
    for (uint64_t i = 0; i < 10000; ++i) feed.nextFrame(frame, &flen, tick, srcTs);
    auto t0 = nowNs();
    uint64_t ok = 0;
    for (uint64_t i = 0; i < n; ++i) {
        feed.nextFrame(frame, &flen, tick, srcTs);
        MarketTick out{};
        if (CtpLikeCodec_decodeTick(frame, flen, out)) ++ok;
    }
    auto t1 = nowNs();
    double secs = (t1 - t0) / 1e9;
    std::printf("  [codec]  %llu ticks in %.3fs  => %.2f M ticks/s  (%.1f ns/tick, decode-ok=%llu)\n",
                static_cast<unsigned long long>(n), secs,
                n / secs / 1e6, 1e9 * secs / n, static_cast<unsigned long long>(ok));
}

// ---------------- 朴素互斥队列（对照）----------------
class MutexQueue {
public:
    explicit MutexQueue(size_t cap) : cap_(cap) {}
    bool push(const uint8_t* p, uint32_t n) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= cap_) return false;
        q_.emplace_back(p, p + n);
        return true;
    }
    bool pop(uint8_t* out, uint32_t* n) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        const auto& v = q_.front();
        std::memcpy(out, v.data(), v.size());
        *n = static_cast<uint32_t>(v.size());
        q_.pop_front();
        return true;
    }
private:
    std::mutex m_;
    std::deque<std::vector<uint8_t>> q_;
    size_t cap_;
};

// ---------------- 2/3. 端到端吞吐 + 延迟 ----------------
struct EndToEndResult {
    uint64_t pub = 0, drop = 0, cons = 0;
    double   secs = 0;
    uint64_t p50 = 0, p99 = 0, max = 0;
};

static EndToEndResult benchRing(uint64_t cap, uint64_t nTicks, bool useRing) {
    EndToEndResult r;
    LatencyRecorder lat;
    const uint32_t msgLen = sizeof(MsgHeader) + sizeof(TickPayload);

    // 背衬内存
    std::vector<uint8_t> backing(SpscRingBuffer::requiredBytes(cap));
    SpscRingBuffer ring;
    if (!ring.attach(backing.data(), backing.size(), cap, /*init*/ true)) {
        std::fprintf(stderr, "[bench] ring attach failed\n");
        return EndToEndResult{};
    }
    MutexQueue mq(cap);

    std::atomic<uint64_t> produced{0};
    std::atomic<bool> done{false};

    // 生产者
    std::thread prod([&]() {
        Seq seq = 0;
        FuturesFeed feed(7);
        uint8_t frameBuf[kMaxFrame];
        while (produced.load() < nTicks) {
            MarketTick t{}; Nanos srcTs = 0; uint32_t flen = 0;
            feed.nextFrame(frameBuf, &flen, t, srcTs);   // 取一条 tick（仅用其逻辑值）
            // 构造一条规范消息（专注 IPC 路径，跳过外部线路帧）
            uint8_t msg[msgLen];
            writeMessage(msg, MSG_TICK, seq, srcTs, srcTs, &t, sizeof(TickPayload));
            if (useRing) {
                while (!ring.tryPublish(msg, msgLen)) { /* 满则自旋等待消费者 */ }
            } else {
                while (!mq.push(msg, msgLen)) { /* 满则自旋 */ }
            }
            ++produced;
            ++seq;
        }
        done = true;
    });

    // 消费者
    std::thread cons([&]() {
        uint8_t buf[msgLen]; uint32_t len = 0;
        uint64_t got = 0;
        while (got < nTicks) {
            bool rc = useRing ? ring.tryConsume(buf, &len) : mq.pop(buf, &len);
            if (!rc) { std::this_thread::yield(); continue; }
            const MsgHeader* h = reinterpret_cast<const MsgHeader*>(buf);
            if ((got & 63) == 0) lat.record(nowNs() - h->srcTs);
            ++got;
        }
        r.cons = got;
    });

    auto t0 = nowNs();
    prod.join();
    cons.join();
    auto t1 = nowNs();
    r.secs = (t1 - t0) / 1e9;
    r.pub = produced.load();
    r.drop = 0;  // 自旋版不丢弃（满则让生产者自旋等待），此处吞吐即真实处理速率
    r.p50 = lat.p50();
    r.p99 = lat.p99();
    r.max = lat.max();
    return r;
}

int main(int argc, char** argv) {
    const uint64_t cap    = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : (1u << 18);
    const uint64_t nTicks = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 20'000'000;

    std::printf("================ Futures MD System Benchmark ================\n");
    std::printf("ring capacity = %llu slots, total ticks = %llu\n\n",
                static_cast<unsigned long long>(cap), static_cast<unsigned long long>(nTicks));

    benchCodec(5'000'000);

    std::printf("\n  --- end-to-end (producer+consumer, %s) ---\n",
                "in-process shared memory");
    EndToEndResult ring = benchRing(cap, nTicks, true);
    std::printf("  [ring/SPSC] pub=%llu cons=%llu  %.2f M msg/s  p50=%llu ns p99=%llu ns max=%llu ns\n",
                static_cast<unsigned long long>(ring.pub),
                static_cast<unsigned long long>(ring.cons),
                ring.pub / ring.secs / 1e6,
                static_cast<unsigned long long>(ring.p50),
                static_cast<unsigned long long>(ring.p99),
                static_cast<unsigned long long>(ring.max));

    EndToEndResult mq = benchRing(cap, nTicks, false);
    std::printf("  [mutex queue] pub=%llu cons=%llu  %.2f M msg/s  p50=%llu ns p99=%llu ns max=%llu ns\n",
                static_cast<unsigned long long>(mq.pub),
                static_cast<unsigned long long>(mq.cons),
                mq.pub / mq.secs / 1e6,
                static_cast<unsigned long long>(mq.p50),
                static_cast<unsigned long long>(mq.p99),
                static_cast<unsigned long long>(mq.max));

    double speedup = (ring.pub / ring.secs) / (mq.pub / mq.secs);
    std::printf("\n  => 无锁 SPSC 相对互斥队列吞吐提升约 %.1fx\n", speedup);
    std::printf("=============================================================\n");
    return 0;
}
