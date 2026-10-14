// ============================================================================
// net/MdReceiver.cpp
// ============================================================================
#include "net/MdReceiver.h"
#include "net/WireParse.h"

namespace md { namespace net {

bool MdReceiver::startUdp(uint16_t port, const std::string& group) {
  udp_ = true;
  fd_ = udp_bind_recv(port, group.empty() ? nullptr : group.c_str());
  return fd_ != MD_INVALID_SOCKET;
}

bool MdReceiver::startTcp(const std::string& host, uint16_t port) {
  udp_ = false;
  fd_ = tcp_connect(host.c_str(), port);
  return fd_ != MD_INVALID_SOCKET;
}

void MdReceiver::loop() {
  uint8_t buf[2048];
  Seq seq = 0;
  while (!stop_) {
    long n = udp_ ? udp_recv(fd_, buf, sizeof buf) : tcp_recv(fd_, buf, sizeof buf);
    if (n <= 0) {
      if (!udp_ && n == 0) break;   // TCP 对端关闭
      continue;                      // 超时：继续检查 stop_
    }
    MarketTick t; Nanos srcTs = 0, gwTs = 0;
    if (decodeWireTick(buf, (size_t)n, t, srcTs, gwTs)) {
      ++received_;
      if (ring_) {
        uint8_t msg[kSlotSize];
        uint32_t mlen = (uint32_t)writeMessage(msg, MSG_TICK, ++seq, srcTs, gwTs,
                                               &t, (uint32_t)sizeof(TickPayload));
        ring_->tryPublish(msg, mlen);
      }
      if (onTick_) onTick_(t, srcTs, gwTs);
    } else {
      ++bad_;
    }
  }
}

}}  // namespace md::net
