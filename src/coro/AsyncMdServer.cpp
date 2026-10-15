// ============================================================================
// coro/AsyncMdServer.cpp —— 协程驱动的异步行情接入服务实现
// ============================================================================
#include "coro/AsyncMdServer.h"

#include <chrono>
#include <cstring>

namespace md { namespace coro {

static Nanos nowNs() {
    using namespace std::chrono;
    return (Nanos)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

bool AsyncMdServer::start(uint16_t port) {
    net::sock_init();
    listenFd_ = net::tcp_listen(port);
    if (listenFd_ == MD_INVALID_SOCKET) return false;
    loop_.spawn(acceptLoop());     // 启动 accept 协程（不阻塞）
    return true;
}

Task<void> AsyncMdServer::acceptLoop() {
    while (!stop_) {
        co_await loop_.read(listenFd_);              // 挂起，直到监听口可读
        net::sockfd_t c = net::tcp_accept_nowait(listenFd_);
        if (c != MD_INVALID_SOCKET) {
            accepted_.fetch_add(1, std::memory_order_relaxed);
            loop_.spawn(handleClient(c));            // 每连接一协程
        }
    }
    co_return;
}

Task<void> AsyncMdServer::handleClient(net::sockfd_t fd) {
    uint8_t buf[md::kMaxFrame * 8];
    size_t  off = 0;
    uint32_t ackSeq = 0;

    while (!stop_) {
        co_await loop_.read(fd);                      // 等待该连接可读
        long n = net::tcp_recv(fd, buf + off, (long)(sizeof(buf) - off));
        if (n <= 0) break;                            // 对端关闭 / 出错
        off += (size_t)n;

        size_t consumed = 0;
        while (off - consumed >= md::kTickFrameSize) {
            MarketTick t{};
            if (CtpLikeCodec_decodeTick(buf + consumed, md::kTickFrameSize, t)) {
                Nanos recvTs = nowNs();
                RiskDecision d = risk_
                    ? risk_->checkTick(t, recvTs, recvTs)   // 演示：srcTs 用接收时刻
                    : RISK_ALLOW;
                if (cb_) cb_(d, t, recvTs);

                // 回 ack（承载风控结论 + 序号）
                AckFrame ack;
                ack.magic  = md::kMagic;
                ack.type   = kAckType;
                ack.status = (uint8_t)d;
                ack.seq    = ackSeq++;
                co_await loop_.write(fd);             // 等待可写
                net::tcp_send(fd, &ack, kAckSize);

                if (d == RISK_ALLOW) ticks_.fetch_add(1, std::memory_order_relaxed);
                else                 rejected_.fetch_add(1, std::memory_order_relaxed);
                consumed += md::kTickFrameSize;
            } else {
                // 坏帧：扫描到下一个 magic 重新对齐（防卡死）
                size_t adv = 1;
                while (consumed + adv + md::kTickFrameSize <= off
                       && !(buf[consumed + adv] == (md::kMagic & 0xFF)
                            && buf[consumed + adv + 1] == (md::kMagic >> 8)))
                    ++adv;
                consumed += adv;
            }
        }
        if (consumed > 0 && consumed < off) {          // 保留尾部半包
            std::memmove(buf, buf + consumed, off - consumed);
            off -= consumed;
        } else if (consumed >= off) {
            off = 0;
        }
    }

    net::sock_close(fd);
    loop_.unregister(fd);
    co_return;
}

}}  // namespace md::coro
