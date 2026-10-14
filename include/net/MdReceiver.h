#pragma once
// ============================================================================
// net/MdReceiver.h —— 行情网络接收器（C++ 网络编程的「采集」环节）
//
// 从 UDP（组播/单播）或 TCP 接收「内部线格式消息」(MsgHeader + TickPayload)，
// 解析为 MarketTick 后：(a) 发布到 SpscRingBuffer（与已有低延迟链路衔接）；
// (b) 触发 onTick 回调（交给策略/数据接口使用）。
//
// 这就是真实期货公司行情网关「网络收包 → 解析 → 入环」的入口，对应岗位要求的
// 「行情采集 / 网络编程」。
// ============================================================================
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include "common/Types.h"
#include "ipc/SpscRingBuffer.h"
#include "net/Socket.h"

namespace md { namespace net {

class MdReceiver {
public:
  MdReceiver() = default;
  ~MdReceiver() { requestStop(); if (th_.joinable()) th_.join(); sock_close(fd_); }

  // UDP 模式：绑定 port；group 非空则加入组播（如 "239.1.1.1"）
  bool startUdp(uint16_t port, const std::string& group = "");
  // TCP 模式：连接行情源 host:port（逐笔流）
  bool startTcp(const std::string& host, uint16_t port);

  void setRing(SpscRingBuffer* r) { ring_ = r; }
  void setOnTick(std::function<void(const MarketTick&, Nanos, Nanos)> cb) { onTick_ = std::move(cb); }

  void requestStop() { stop_ = true; }
  void runAsync() { th_ = std::thread(&MdReceiver::loop, this); }
  void join() { if (th_.joinable()) th_.join(); }

  uint64_t received() const { return received_; }
  uint64_t bad() const { return bad_; }

private:
  void loop();
  sockfd_t fd_ = MD_INVALID_SOCKET;
  bool udp_ = true;
  std::atomic<bool> stop_{false};
  SpscRingBuffer* ring_ = nullptr;
  std::function<void(const MarketTick&, Nanos, Nanos)> onTick_;
  std::thread th_;
  uint64_t received_ = 0, bad_ = 0;
};

}}  // namespace md::net
