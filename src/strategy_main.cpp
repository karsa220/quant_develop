// ============================================================================
// strategy_main.cpp —— 策略/执行进程（独立进程，跨进程 mmap IPC）
//
// 用法: strategy_main [shmName] [durationMs]
//   挂接 gateway_main 创建的共享内存，消费行情、生成信号。两者跨进程零拷贝通信。
// ============================================================================
#include "strategy/TickConsumer.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <chrono>

using namespace md;

static std::atomic<bool> g_stop{false};
static void onSig(int) { g_stop = true; }

int main(int argc, char** argv) {
    const char* name = (argc > 1) ? argv[1] : "/md_xprocess";
    const uint64_t durMs = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 0;

    std::signal(SIGINT, onSig);
    std::signal(SIGTERM, onSig);

    TickConsumer con;
    // 等待网关先建立共享内存（最多 ~5s 轮询）
    size_t tries = 0;
    while (!con.init(name) && tries < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++tries;
    }
    if (tries == 50) {
        std::fprintf(stderr, "[strategy] cannot open shm '%s' (gateway up?)\n", name);
        return 1;
    }
    std::printf("[strategy] up: shm=%s durMs=%llu\n", name,
                static_cast<unsigned long long>(durMs));

    std::thread ct([&]() { con.run(~0ull, nullptr); });
    if (durMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(durMs));
    } else {
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    con.requestStop();
    ct.join();

    std::printf("[strategy] stopped: consumed=%llu signals=%llu\n",
                static_cast<unsigned long long>(con.consumed()),
                static_cast<unsigned long long>(con.signals()));
    return 0;
}
