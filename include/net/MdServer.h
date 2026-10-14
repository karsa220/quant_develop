#pragma once
// ============================================================================
// net/MdServer.h —— 多协议数据接口（岗位要求的「开发各种数据接口」）
//
// 同一个行情源，按不同消费方提供三种对外接口：
//   1) TCP 二进制流  (端口 9001)：面向低延迟 C++ 客户端，直接发线格式消息；
//   2) HTTP REST/JSON(端口 8080)：面向研究/Python 客户端，GET /snapshot 拿快照；
//   3) UDP 组播      (端口 9002)：面向多个订阅者 fan-out（交易所行情经典模型）。
//
// 维护各合约「最新成交状态」，所有接口共享同一份状态。接口层与采集层、
// 共享内存层解耦：它们都从同一份 MarketTick 派生。
// ============================================================================
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/Types.h"
#include "net/Socket.h"
#include "net/JsonBook.h"

namespace md { namespace net {

class MdServer {
public:
  MdServer() = default;
  ~MdServer() { requestStop(); joinAll(); sock_close(tcpFd_); sock_close(httpFd_); sock_close(udpFd_); }

  void setSymbols(const std::vector<std::string>& syms) { syms_ = syms; }

  bool startTcp(uint16_t port);               // 二进制流接口
  bool startHttp(uint16_t port);              // REST/JSON 接口
  bool startUdp(uint16_t port, const char* group); // 组播接口

  void requestStop();
  void joinAll();

  // 由行情接收回调喂入最新成交，维护各合约最新状态
  void pushTick(const MarketTick& t, Nanos /*srcTs*/, Nanos /*gwTs*/);

  uint64_t tcpClients() const { return tcpClients_; }
  uint64_t httpReqs()   const { return httpReqs_; }
  uint64_t udpSent()    const { return udpSent_; }

  std::vector<BookRow> snapshotRows() const;
  std::string snapshotJson() const { return bookRowsToJson(snapshotRows()); }

private:
  void tcpLoop();
  void httpLoop();
  void udpLoop();

  std::vector<std::string> syms_;
  mutable std::mutex mtx_;
  std::unordered_map<InstrumentId, BookRow> rows_;

  sockfd_t tcpFd_ = MD_INVALID_SOCKET, httpFd_ = MD_INVALID_SOCKET, udpFd_ = MD_INVALID_SOCKET;
  std::string udpGroup_;
  uint16_t udpPort_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> tcpClients_{0}, httpReqs_{0}, udpSent_{0};
  std::thread tt_, th_, tu_;
  bool hasTcp_ = false, hasHttp_ = false, hasUdp_ = false;
};

}}  // namespace md::net
