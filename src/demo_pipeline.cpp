// ============================================================================
// demo_pipeline.cpp —— 单进程完整链路演示
//
//   期货行情源(FuturesFeed) → CTP 编解码 → 行情网关(MdGateway) → 共享内存 SPSC 环
//        → 低延迟消费(TickConsumer) → 信号 + 端到端延迟统计
//
// 生产者/消费者跑在两个线程，共享同一段 mmap 内存，零拷贝传递行情。
// 真实部署中二者是独立进程（见 gateway_main / strategy_main），此处单进程便于演示与调试。
// ============================================================================
#include "gateway/MdGateway.h"
#include "strategy/TickConsumer.h"
#include "bench/LatencyRecorder.h"

#include <cstdio>
#include <thread>
#include <chrono>

using namespace md;

int main() {
    const char* shmName = "/md_demo";
    const uint64_t kCapacity = 1u << 18;   // 262144 槽 (~50MB)

    MdGateway gw;
    if (!gw.init(shmName, kCapacity)) {
        std::fprintf(stderr, "[demo] gateway init failed\n");
        return 1;
    }
    TickConsumer con;
    if (!con.init(shmName)) {
        std::fprintf(stderr, "[demo] consumer init failed\n");
        return 1;
    }

    LatencyRecorder lat;
    std::thread consumerThread([&]() { con.run(~0ull, &lat); });

    std::printf("================ Futures MD Pipeline Demo ================\n");
    std::printf("ring capacity : %llu slots (~%zu MB)\n",
                static_cast<unsigned long long>(gw.capacity()),
                SpscRingBuffer::requiredBytes(kCapacity) / (1024 * 1024));
    std::printf("running 4s: feed -> codec -> gateway -> mmap ring -> consumer ...\n\n");

    auto t0 = std::chrono::steady_clock::now();
    gw.run(~0ull, /*rate*/ 0.0, /*durationMs*/ 4000);
    auto t1 = std::chrono::steady_clock::now();

    // 给消费者一点时间排空，然后停止
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    con.requestStop();
    consumerThread.join();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("\n-------------------- Results --------------------\n");
    std::printf("published : %llu ticks\n", static_cast<unsigned long long>(gw.pubCount()));
    std::printf("dropped   : %llu (ring full / backpressure)\n", static_cast<unsigned long long>(gw.dropCount()));
    std::printf("consumed  : %llu ticks\n", static_cast<unsigned long long>(con.consumed()));
    std::printf("signals   : %llu (book-cross events)\n", static_cast<unsigned long long>(con.signals()));
    std::printf("throughput: %.2f M ticks/s (publish)\n", gw.pubCount() / secs / 1e6);
    if (lat.count()) {
        std::printf("\ntick->signal latency (ns):\n");
        std::printf("  p50    = %llu\n", static_cast<unsigned long long>(lat.p50()));
        std::printf("  p90    = %llu\n", static_cast<unsigned long long>(lat.p90()));
        std::printf("  p99    = %llu\n", static_cast<unsigned long long>(lat.p99()));
        std::printf("  p99.9  = %llu\n", static_cast<unsigned long long>(lat.p999()));
        std::printf("  max    = %llu\n", static_cast<unsigned long long>(lat.max()));
    }
    std::printf("===================================================\n");
    return 0;
}
