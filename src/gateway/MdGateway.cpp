#include "gateway/MdGateway.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace md {

static uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool MdGateway::init(const char* shmName, uint64_t capacity) {
    const size_t bytes = SpscRingBuffer::requiredBytes(capacity);
    if (!shm_.create(shmName, bytes)) return false;
    if (!ring_.attach(shm_.data(), shm_.size(), capacity, /*init*/ true)) return false;
    return true;
}

void MdGateway::maybeHeartbeat(uint64_t nowNs) {
    if (nowNs - lastHbNs_ < 1'000'000'000ULL) return;   // 每 1 秒
    lastHbNs_ = nowNs;
    HeartbeatPayload hb{};
    hb.pubCount      = pubCount_.load();
    hb.dropCount     = dropCount_.load();
    hb.consumerLagUs = 0;  // 精确滞后由消费者侧量；此处占位
    hb.ringCapacity  = static_cast<uint32_t>(ring_.capacity());
    hb.pad           = 0;
    const uint32_t len = sizeof(MsgHeader) + sizeof(HeartbeatPayload);
    writeMessage(msgBuf_, MSG_HEARTBEAT, /*seq*/ static_cast<Seq>(pubCount_.load()),
                 nowNs, nowNs, &hb, sizeof(HeartbeatPayload), /*qos*/1);
    ring_.tryPublish(msgBuf_, len);   // 心跳丢弃不计入 drop
}

void MdGateway::maybeSession(uint64_t nowNs) {
    const uint64_t elapsed = nowNs - startNs_;
    if (!openSent_ && elapsed >= 1'500'000'000ULL) {     // 演示：开盘 1.5s
        openSent_ = true;
        ControlPayload ctl{};
        ctl.state = 1;
        writeMessage(msgBuf_, MSG_CONTROL, static_cast<Seq>(pubCount_.load()),
                     nowNs, nowNs, &ctl, sizeof(ControlPayload), 1);
        ring_.tryPublish(msgBuf_, sizeof(MsgHeader) + sizeof(ControlPayload));
        std::printf("[gateway] >>> SESSION OPEN (09:30)\n");
    }
    if (!closeSent_ && elapsed >= 3'000'000'000ULL) {    // 演示：收盘 3s
        closeSent_ = true;
        ControlPayload ctl{};
        ctl.state = 0;
        writeMessage(msgBuf_, MSG_CONTROL, static_cast<Seq>(pubCount_.load()),
                     nowNs, nowNs, &ctl, sizeof(ControlPayload), 1);
        ring_.tryPublish(msgBuf_, sizeof(MsgHeader) + sizeof(ControlPayload));
        std::printf("[gateway] >>> SESSION CLOSE (15:00)\n");
    }
}

void MdGateway::run(uint64_t totalTicks, double rate, uint64_t durationMs) {
    feed_.setRate(rate);
    startNs_  = nowNs();
    lastHbNs_ = startNs_;

    // Linux 定时任务：每 1 秒刷一次统计到 stdout（timerfd 驱动，只读副作用，不碰 ring）
    statsTimer_.startPeriodic(1'000'000ULL, [this]() {
        const uint64_t pub = pubCount_.load();
        const uint64_t drop = dropCount_.load();
        const double  secs = static_cast<double>(nowNs() - startNs_) / 1e9;
        const double  eps  = secs > 0 ? pub / secs : 0.0;
        std::printf("[gateway][timerfd] pub=%llu drop=%llu  %.0f ticks/s\n",
                    static_cast<unsigned long long>(pub),
                    static_cast<unsigned long long>(drop), eps);
    });

    Seq seq = 0;
    while (running_.load() && pubCount_.load() < totalTicks) {
        if (durationMs && (nowNs() - startNs_) >= durationMs * 1'000'000ULL) break;

        MarketTick tick{};
        Nanos     srcTs = 0;
        uint32_t  flen  = 0;
        if (!feed_.nextFrame(frameBuf_, &flen, tick, srcTs)) {
            std::this_thread::yield();   // rate-limited 模式下节流
            continue;
        }
        // 网关解析线路帧（模拟真实网关解码）
        MarketTick parsed{};
        if (!CtpLikeCodec_decodeTick(frameBuf_, flen, parsed)) {
            ++dropCount_;                // 校验失败，丢弃
            continue;
        }
        const uint64_t gwTs = nowNs();
        writeMessage(msgBuf_, MSG_TICK, seq, srcTs, gwTs,
                     &parsed, sizeof(TickPayload));
        if (ring_.tryPublish(msgBuf_, sizeof(MsgHeader) + sizeof(TickPayload))) {
            ++pubCount_;
            ++seq;
        } else {
            ++dropCount_;               // ring 满，丢弃（背压信号）
        }
        maybeHeartbeat(gwTs);
        maybeSession(gwTs);
    }
    running_ = false;
    statsTimer_.stop();
}

}  // namespace md
