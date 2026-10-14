// ============================================================================
// gateway_main.cpp —— 行情网关进程（独立进程，跨进程 mmap IPC）
//
// 用法: gateway_main [shmName] [capacity] [durationMs]
//   与 strategy_main 共享同一段命名共享内存，构成真实「进程间」低延迟行情通道。
//   Linux: shmName 形如 "/md_xprocess"，落在 /dev/shm；Windows: 命名页文件映射。
//   生产部署由 systemd 拉起（见 scripts/md-gateway.service）。
// ============================================================================
#include "gateway/MdGateway.h"

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
    const uint64_t cap  = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : (1u << 18);
    const uint64_t durMs = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 0;

    std::signal(SIGINT, onSig);
    std::signal(SIGTERM, onSig);

    MdGateway gw;
    if (!gw.init(name, cap)) {
        std::fprintf(stderr, "[gateway] init failed (shm '%s')\n", name);
        return 1;
    }
    std::printf("[gateway] up: shm=%s cap=%llu durMs=%llu\n", name,
                static_cast<unsigned long long>(cap),
                static_cast<unsigned long long>(durMs));

    if (durMs > 0) {
        gw.run(~0ull, 0.0, durMs);
    } else {
        while (!g_stop) gw.run(~0ull, 0.0, 500);   // 每 500ms 续跑，直到信号
    }
    gw.requestStop();
    std::printf("[gateway] stopped: pub=%llu drop=%llu\n",
                static_cast<unsigned long long>(gw.pubCount()),
                static_cast<unsigned long long>(gw.dropCount()));
    return 0;
}
