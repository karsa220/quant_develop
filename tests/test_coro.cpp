// ============================================================================
// tests/test_coro.cpp —— C++20 协程单元测试 + 协程异步服务集成测试
// （JD 加分项：协程；JD 职责3：集成测试保障高并发稳定）
//
// 覆盖：
//   1) Generator<T>：co_yield 惰性序列正确性；
//   2) Task<T>：co_await 嵌套组合 + 返回值；
//   3) 协程异步行情服务：单线程事件循环驱动「服务器协程 + 多客户端协程」，
//      收 CTP 风格 tick → 风控 → 回 ack，断言零丢失、风控统计、延迟可测。
// 编译：见 Makefile 的 `cotest` 目标（需 -std=c++20）。
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "coro/Coroutine.h"
#include "coro/AsyncMdServer.h"
#include "risk/RiskControl.h"
#include "feed/CtpLikeCodec.h"
#include "net/Socket.h"
#include "bench/LatencyRecorder.h"

using namespace md;
using namespace md::coro;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---------------------------------------------------------------------------
// 1) Generator<T>：co_yield 惰性序列
// ---------------------------------------------------------------------------
static Generator<int> iota1(int n) {
    for (int i = 1; i <= n; ++i) co_yield i;
}

static void test_generator() {
    auto g = iota1(100);
    long sum = 0, cnt = 0;
    while (g.next()) { sum += g.value(); ++cnt; }
    CHECK(cnt == 100);
    CHECK(sum == 5050);          // 1+2+...+100
    CHECK(!g.valid());           // 序列已结束
}

// ---------------------------------------------------------------------------
// 2) Task<T>：co_await 嵌套组合
// ---------------------------------------------------------------------------
static Task<int> addOne(int x) { co_return x + 1; }

static Task<int> compose() {
    int a = co_await addOne(41);
    int b = co_await addOne(a);
    co_return b;                  // 期望 43
}

static void test_task() {
    auto t = compose();
    t.handle().resume();         // 同步驱动整条链（await_suspend 续跑被等待协程）
    CHECK(t.done());
    CHECK(t.result() == 43);
}

// ---------------------------------------------------------------------------
// 3) 协程异步服务集成测试
// ---------------------------------------------------------------------------
static Task<void> testClient(AsyncMdServer& srv, EventLoop& loop, net::sockfd_t fd,
                             int n, std::atomic<uint64_t>* acked,
                             std::atomic<int>* remaining, LatencyRecorder* lat) {
    uint8_t frame[md::kTickFrameSize];
    uint8_t ackbuf[kAckSize];
    for (int i = 0; i < n; ++i) {
        MarketTick t{};
        t.inst  = (InstrumentId)(i % FuturesFeed::kNumInstruments);
        t.price = 1000 + (i % 50);
        t.qty   = 1;
        t.side  = (i & 1) ? md::kSideSell : md::kSideBuy;
        CtpLikeCodec_encodeTick(t, frame);

        auto t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
        co_await loop.write(fd);
        net::tcp_send(fd, frame, md::kTickFrameSize);
        co_await loop.read(fd);
        long r = net::tcp_recv(fd, ackbuf, kAckSize);
        if (r == (long)kAckSize) {
            acked->fetch_add(1, std::memory_order_relaxed);
            auto t1 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
            if (lat) lat->record((uint64_t)(t1 - t0));
        }
    }
    net::sock_close(fd);
    loop.unregister(fd);
    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) srv.requestStop();
    co_return;
}

static void test_async_server() {
    const uint16_t port = 19091;
    const int      clients = 4;
    const int      ticksPerClient = 1000;
    const uint64_t total = (uint64_t)clients * ticksPerClient;

    RiskControl risk;
    risk.configureDefault(RiskConfig{
        .minPrice = 900, .maxPrice = 1100, .maxLong = 1'000'000,
        .maxShort = -1'000'000, .maxAgeNs = 0, .orderRatePerSec = 0 });

    LatencyRecorder lat;
    std::atomic<uint64_t> acked{0};
    std::atomic<int>      remaining{clients};

    AsyncMdServer srv(&risk, [](RiskDecision, const MarketTick&, Nanos) {});
    CHECK(srv.start(port));
    EventLoop& loop = srv.loop();
    std::printf("[coro-test] backend=%s\n", srv.backend());

    std::vector<net::sockfd_t> fds;
    for (int i = 0; i < clients; ++i) {
        net::sockfd_t c = net::tcp_connect("127.0.0.1", port);
        CHECK(c != MD_INVALID_SOCKET);
        fds.push_back(c);
    }
    for (auto fd : fds)
        loop.spawn(testClient(srv, loop, fd, ticksPerClient, &acked, &remaining, &lat));

    srv.run();   // 单线程事件循环，直到最后一个客户端完成

    CHECK(srv.accepted() == (uint64_t)fds.size());
    CHECK(acked.load() == total);                          // 零丢失：每笔 tick 都收到 ack
    CHECK(srv.ticks() + srv.rejected() == total);         // 每笔 tick 都过风控
    CHECK(lat.count() == total);                           // 延迟可观测
    std::printf("[coro-test] accepted=%llu ticks=%llu rejected=%llu acked=%llu "
                "RTT(ns) p50=%llu p99=%llu\n",
                (unsigned long long)srv.accepted(), (unsigned long long)srv.ticks(),
                (unsigned long long)srv.rejected(), (unsigned long long)acked.load(),
                (unsigned long long)lat.p50(), (unsigned long long)lat.p99());
}

int main() {
    test_generator();
    test_task();
    test_async_server();
    std::printf("\n[coro-test] checks=%d fails=%d\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
