#pragma once
// ============================================================================
// net/PacketCapture.h —— 链路层 / 传输层抓包（岗位要求的「抓包」）
//
// 在真实行情系统里，定位延迟、核对柜台/交易所原始报文、做合规留痕，
// 往往需要在「比应用层 recv 更早」的位置抓包。这里两种实现：
//   - Linux : AF_PACKET(SOCK_RAW) 抓整个以太网帧，解析 Eth/IPv4/UDP；
//   - Win   : 原始套接字 + SIO_RCVALL 抓 IP 层报文（无以太网头）。
// 都需要权限（Linux CAP_NET_RAW / Windows 管理员），无权限时优雅降级、
// 仅由单测覆盖解析逻辑 —— 解析路径本身与权限无关，可独立验证。
// ============================================================================
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include "common/Types.h"
#include "net/Socket.h"

namespace md { namespace net {

class PcapCapture {
public:
  using TickFn = std::function<void(const MarketTick&, Nanos, Nanos)>;
  ~PcapCapture() { requestStop(); if (th_.joinable()) th_.join(); sock_close(fd_); }

  // 启动抓包（best-effort：无权限返回 false，不抛异常）。
  bool start(uint16_t port, TickFn cb);

  void requestStop() { stop_ = true; }
  void join() { if (th_.joinable()) th_.join(); }
  uint64_t captured() const { return captured_; }
  bool     available() const { return available_; }

private:
  void loop();
  sockfd_t fd_ = MD_INVALID_SOCKET;
  uint16_t port_ = 0;
  TickFn cb_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> captured_{0};
  bool available_ = false;
  std::thread th_;
};

}}  // namespace md::net
