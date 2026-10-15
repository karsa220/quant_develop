// ============================================================================
// coro_main.cpp —— 协程异步行情服务演示（单线程并发 + 低延迟回环）
//
// 启动一个 AsyncMdServer，再用同一事件循环 spawn 多个「客户端协程」，
// 每个客户端连上服务器、连续发送 N 笔 CTP 风格 tick 并等待 ack，统计 RTT。
// 全程单线程：accept、每个连接、每个客户端都是协程，由 EventLoop 调度。
//
// 运行：./coro_main            （或 coro_main.exe on Windows）
// ============================================================================
#include "coro/AsyncMdServer.h"
#include "coro/Coroutine.h"
#include "risk/RiskControl.h"
#include "feed/CtpLikeCodec.h"
#include "net/Socket.h"
#include "bench/LatencyRecorder.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace md;
using namespace md::coro;

static Nanos nowNs() {
    using namespace std::chrono;
    return (Nanos)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// 客户端协程：连上后发 n 笔 tick，每笔等一个 ack，记录 RTT
Task<void> clientSession(AsyncMdServer& srv, EventLoop& loop, net::sockfd_t fd, int n,
                         std::atomic<uint64_t>* acked, std::atomic<int>* remaining,
                         LatencyRecorder* lat) {
    uint8_t frame[md::kTickFrameSize];
    uint8_t ackbuf[kAckSize];
    for (int i = 0; i < n; ++i) {
        MarketTick t{};
        t.inst  = (InstrumentId)(i % FuturesFeed::kNumInstruments);
        t.price = 1000 + (i % 50);
        t.qty   = 1;
        t.side  = (i & 1) ? md::kSideSell : md::kSideBuy;
        CtpLikeCodec_encodeTick(t, frame);

        Nanos t0 = nowNs();
        co_await loop.write(fd);                       // 等可写
        net::tcp_send(fd, frame, md::kTickFrameSize);
        co_await loop.read(fd);                        // 等 ack
        long r = net::tcp_recv(fd, ackbuf, kAckSize);
        if (r == (long)kAckSize) {
            acked->fetch_add(1, std::memory_order_relaxed);
            if (lat) lat->record(nowNs() - t0);
        }
    }
    net::sock_close(fd);
    loop.unregister(fd);
    // 最后一个客户端完成 → 请求事件循环退出（run() 自然返回，再由析构回收协程）
    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) srv.requestStop();
    co_return;
}

int main() {
    const uint16_t port = 19090;
    const int      clients = 8;
    const int      ticksPerClient = 5000;

    RiskControl risk;
    risk.configureDefault(RiskConfig{
        .minPrice = 900, .maxPrice = 1100, .maxLong = 1'000'000,
        .maxShort = -1'000'000, .maxAgeNs = 0, .orderRatePerSec = 0 });

    LatencyRecorder lat;
    std::atomic<uint64_t> acked{0};
    std::atomic<int> remaining{clients};

    AsyncMdServer srv(&risk, [&](RiskDecision, const MarketTick&, Nanos) {});
    if (!srv.start(port)) {
        std::cerr << "server start failed (port " << port << ")\n";
        return 1;
    }
    EventLoop& loop = srv.loop();

    // 先同步建连（localhost 快速），再把客户端作为协程放进同一事件循环
    std::vector<net::sockfd_t> fds;
    for (int i = 0; i < clients; ++i) {
        net::sockfd_t c = net::tcp_connect("127.0.0.1", port);
        if (c == MD_INVALID_SOCKET) { std::cerr << "connect failed\n"; continue; }
        fds.push_back(c);
    }
    for (auto fd : fds)
        loop.spawn(clientSession(srv, loop, fd, ticksPerClient, &acked, &remaining, &lat));

    std::cout << "[coro-demo] backend=" << srv.backend()
              << " clients=" << fds.size()
              << " ticks/client=" << ticksPerClient << "\n";
    std::cout << "[coro-demo] running single-threaded event loop...\n";

    srv.run();   // 阻塞，直到所有客户端完成并 stop()

    std::cout << "[coro-demo] accepted="   << srv.accepted()
              << " ticks="      << srv.ticks()
              << " rejected="   << srv.rejected()
              << " acked="      << acked.load() << "\n";
    if (lat.count()) {
        std::cout << "[coro-demo] client RTT(ns): p50=" << lat.p50()
                  << " p99=" << lat.p99()
                  << " p999=" << lat.p999()
                  << " max=" << lat.max() << "\n";
    }
    net::sock_cleanup();
    return 0;
}
