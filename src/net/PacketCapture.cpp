// ============================================================================
// net/PacketCapture.cpp —— 链路层 / 传输层抓包实现
// ============================================================================
#include "net/PacketCapture.h"
#include "net/WireParse.h"

namespace md { namespace net {

bool PcapCapture::start(uint16_t port, TickFn cb) {
  port_ = port;
  cb_ = std::move(cb);

#if defined(__linux__)
  // 抓整个以太网帧（含链路层），需 CAP_NET_RAW
  fd_ = socket(AF_PACKET, SOCK_RAW, htons(0x0003) /*ETH_P_ALL*/);
  if (fd_ == MD_INVALID_SOCKET) return false;
  available_ = true;
  th_ = std::thread(&PcapCapture::loop, this);
  return true;

#elif defined(_WIN32)
  // Windows 原始套接字需要管理员权限；失败则优雅降级
  fd_ = WSASocket(AF_INET, SOCK_RAW, IPPROTO_IP, NULL, 0, 0);
  if (fd_ == MD_INVALID_SOCKET) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) != 0) { sock_close(fd_); fd_ = MD_INVALID_SOCKET; return false; }
  DWORD mode = 1 /*RCVALL_ON*/;
  DWORD out = 0;
  if (WSAIoctl(fd_, SIO_RCVALL, &mode, sizeof(mode), NULL, 0, &out, NULL, NULL) != 0) {
    sock_close(fd_); fd_ = MD_INVALID_SOCKET; return false;
  }
  available_ = true;
  th_ = std::thread(&PcapCapture::loop, this);
  return true;

#else
  (void)port; (void)cb;
  return false;
#endif
}

void PcapCapture::loop() {
  uint8_t buf[4096];
  while (!stop_) {
    long n = recv(fd_, (char*)buf, sizeof buf, 0);
    if (n < 0) break;
    if (n == 0) continue;
    MarketTick t; Nanos s = 0, g = 0;
#if defined(__linux__)
    if (parseCapturedPacket(buf, (size_t)n, /*haveEthernet=*/true, t, s, g)) {
#elif defined(_WIN32)
    if (parseCapturedPacket(buf, (size_t)n, /*haveEthernet=*/false, t, s, g)) {
#else
    (void)buf;
    if (false) {
#endif
      ++captured_;
      if (cb_) cb_(t, s, g);
    }
  }
}

}}  // namespace md::net
