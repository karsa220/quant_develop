// ============================================================================
// test_system.cpp —— 单元测试（零第三方依赖）
//
// 覆盖：环形缓冲正确性（无丢失 / 顺序 / 满则丢弃）、编解码往返、行情合法性、
//      定时任务触发。构建见 Makefile / CMakeLists.txt。
// ============================================================================
#include "ipc/SpscRingBuffer.h"
#include "shm/ShmSegment.h"
#include "feed/FuturesFeed.h"
#include "feed/CtpLikeCodec.h"
#include "sched/PeriodicTimer.h"
#include "common/Types.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

using namespace md;

static int g_checks = 0, g_fail = 0;
static void check(bool c, const char* e, int line) {
    ++g_checks;
    if (!c) { ++g_fail; std::printf("  [FAIL] line %d: %s\n", line, e); }
}
#define CHECK(c) check((c), #c, __LINE__)

static uint32_t buildTickMsg(uint8_t* buf, Seq seq, Nanos srcTs, InstrumentId inst,
                             PriceTicks price, Qty qty, uint8_t side) {
    MarketTick t{};
    t.inst = inst; t.price = price; t.qty = qty; t.side = side;
    return static_cast<uint32_t>(writeMessage(buf, MSG_TICK, seq, srcTs, srcTs, &t, sizeof(TickPayload)));
}

// ---------------- 1. 环形缓冲：无丢失 + 顺序 ----------------
void testRingOrder() {
    // 注意：本用例先连发 N 条再消费，故容量须 > N，否则会触发「满则丢弃」。
    const uint64_t cap = 8192;
    std::vector<uint8_t> backing(SpscRingBuffer::requiredBytes(cap));
    SpscRingBuffer ring;
    CHECK(ring.attach(backing.data(), backing.size(), cap, true));

    const uint64_t N = 5000;
    uint8_t msg[kSlotSize];
    for (uint64_t i = 0; i < N; ++i) {
        uint32_t len = buildTickMsg(msg, static_cast<Seq>(i), i * 1000, 0, 1000, 5, kSideBuy);
        CHECK(ring.tryPublish(msg, len));
    }
    CHECK(ring.sizeApprox() == N);

    uint8_t out[kSlotSize]; uint32_t len = 0;
    for (uint64_t i = 0; i < N; ++i) {
        CHECK(ring.tryConsume(out, &len));
        const MsgHeader* h = reinterpret_cast<const MsgHeader*>(out);
        CHECK(h->seq == static_cast<Seq>(i));        // 顺序保持（FIFO）
        CHECK(h->type == MSG_TICK);
        const TickPayload* t = reinterpret_cast<const TickPayload*>(out + sizeof(MsgHeader));
        CHECK(t->price == 1000 && t->qty == 5);
    }
    CHECK(ring.empty());
    CHECK(ring.dropped() == 0);
}

// ---------------- 2. 环形缓冲：满则丢弃并计数 ----------------
void testRingFull() {
    const uint64_t cap = 16;
    std::vector<uint8_t> backing(SpscRingBuffer::requiredBytes(cap));
    SpscRingBuffer ring;
    CHECK(ring.attach(backing.data(), backing.size(), cap, true));

    uint8_t msg[kSlotSize];
    const uint64_t N = cap + 50;
    for (uint64_t i = 0; i < N; ++i) {
        uint32_t len = buildTickMsg(msg, static_cast<Seq>(i), i, 0, 100, 1, kSideSell);
        ring.tryPublish(msg, len);   // 前 cap 个成功，后 50 个因满丢弃
    }
    CHECK(ring.dropped() == 50);
    CHECK(ring.sizeApprox() == cap);

    uint8_t out[kSlotSize]; uint32_t len = 0;
    for (uint64_t i = 0; i < cap; ++i) {
        CHECK(ring.tryConsume(out, &len));
        CHECK(reinterpret_cast<const MsgHeader*>(out)->seq == static_cast<Seq>(i));  // 0..cap-1
    }
    CHECK(ring.empty());
}

// ---------------- 3. 环形缓冲：生产者/消费者并发 ----------------
void testRingConcurrent() {
    const uint64_t cap = 4096;
    std::vector<uint8_t> backing(SpscRingBuffer::requiredBytes(cap));
    SpscRingBuffer ring;
    CHECK(ring.attach(backing.data(), backing.size(), cap, true));

    const uint64_t N = 200'000;
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> done{false};

    std::thread prod([&]() {
        uint8_t msg[kSlotSize];
        for (uint64_t i = 0; i < N; ++i) {
            uint32_t len = buildTickMsg(msg, static_cast<Seq>(i), i, (uint32_t)(i % 6), 100, 1, kSideBuy);
            while (!ring.tryPublish(msg, len)) {}   // 自旋直到成功
            ++produced;
        }
        done = true;
    });
    std::thread cons([&]() {
        uint8_t out[kSlotSize]; uint32_t len = 0;
        Seq expect = 0;
        while (consumed.load() < N) {
            if (!ring.tryConsume(out, &len)) { std::this_thread::yield(); continue; }
            CHECK(reinterpret_cast<const MsgHeader*>(out)->seq == expect);  // 并发下仍保序
            ++expect;
            ++consumed;
        }
    });
    prod.join(); cons.join();
    CHECK(produced == N && consumed == N);   // 每条都送达且保序 == 无真实丢包
    // 注：自旋生产者遇满会重试，retry 也会计入 dropped() 计数（背压事件），
    // 故此处不要求 dropped()==0，只要求「送达数 == 消费数」即数据零丢失。
}

// ---------------- 4. 编解码往返 ----------------
void testCodecRoundTrip() {
    FuturesFeed feed(99);
    uint8_t frame[kMaxFrame];
    for (int i = 0; i < 20000; ++i) {
        MarketTick t{}; Nanos srcTs = 0; uint32_t flen = 0;
        CHECK(feed.nextFrame(frame, &flen, t, srcTs));
        MarketTick out{};
        CHECK(CtpLikeCodec_decodeTick(frame, flen, out));
        CHECK(out.inst == t.inst && out.price == t.price &&
              out.qty == t.qty && out.side == t.side);
    }
}

// ---------------- 5. 行情合法性 ----------------
void testFeedValidity() {
    FuturesFeed feed(7);
    for (int i = 0; i < 100000; ++i) {
        MarketTick t{}; Nanos srcTs = 0; uint32_t flen = 0;
        CHECK(feed.nextFrame(nullptr, &flen, t, srcTs) == false);  // 无 buf 应失败
        // 用带 buf 的版本
        uint8_t frame[kMaxFrame];
        CHECK(feed.nextFrame(frame, &flen, t, srcTs));
        CHECK(t.price > 0);
        CHECK(t.qty >= 1 && t.qty <= 50);
        CHECK(t.side == kSideBuy || t.side == kSideSell);
        CHECK(t.inst < FuturesFeed::kNumInstruments);
        CHECK(flen == kTickFrameSize);
    }
}

// ---------------- 6. 定时任务触发 ----------------
void testTimer() {
    int fires = 0;
    PeriodicTimer t;
    CHECK(t.startPeriodic(1000, [&]() { ++fires; }));   // 1ms 周期
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    t.stop();
    CHECK(fires > 10);   // 60ms 内应触发多次
}

int main() {
    testRingOrder();
    testRingFull();
    testRingConcurrent();
    testCodecRoundTrip();
    testFeedValidity();
    testTimer();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail) { std::printf("TESTS FAILED\n"); return 1; }
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
