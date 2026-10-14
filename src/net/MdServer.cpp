// ============================================================================
// net/MdServer.cpp —— TCP / HTTP / UDP 三种对外数据接口
// ============================================================================
#include "net/MdServer.h"
#include "net/WireParse.h"
#include <chrono>
#include <cstring>
#include <thread>

namespace md { namespace net {

static TickPayload rowToTick(const BookRow& r, InstrumentId id) {
  TickPayload t{};
  t.inst = id; t.price = r.lastPx; t.qty = r.lastQty; t.side = r.lastSide;
  return t;
}

void MdServer::pushTick(const MarketTick& t, Nanos, Nanos) {
  std::lock_guard<std::mutex> lk(mtx_);
  BookRow& r = rows_[t.inst];
  if (r.symbol.empty() && t.inst < syms_.size()) r.symbol = syms_[t.inst];
  r.lastPx = t.price; r.lastQty = t.qty; r.lastSide = t.side; r.count++;
}

std::vector<BookRow> MdServer::snapshotRows() const {
  std::lock_guard<std::mutex> lk(mtx_);
  std::vector<BookRow> ordered;
  ordered.reserve(syms_.size());
  for (size_t i = 0; i < syms_.size(); ++i) {
    auto it = rows_.find((InstrumentId)i);
    if (it != rows_.end()) ordered.push_back(it->second);
  }
  return ordered;
}

bool MdServer::startTcp(uint16_t port) {
  tcpFd_ = tcp_listen(port);
  if (tcpFd_ == MD_INVALID_SOCKET) return false;
  muxTcp_.create();
  muxTcp_.add(tcpFd_);
  hasTcp_ = true;
  tt_ = std::thread(&MdServer::tcpLoop, this);
  return true;
}

bool MdServer::startHttp(uint16_t port) {
  httpFd_ = tcp_listen(port);
  if (httpFd_ == MD_INVALID_SOCKET) return false;
  muxHttp_.create();
  muxHttp_.add(httpFd_);
  hasHttp_ = true;
  th_ = std::thread(&MdServer::httpLoop, this);
  return true;
}

bool MdServer::startUdp(uint16_t port, const char* group) {
  udpFd_ = udp_open_send();
  if (udpFd_ == MD_INVALID_SOCKET) return false;
  udpPort_ = port; udpGroup_ = group ? group : "";
  hasUdp_ = true;
  tu_ = std::thread(&MdServer::udpLoop, this);
  return true;
}

void MdServer::requestStop() { stop_ = true; }

void MdServer::joinAll() {
  if (hasTcp_  && tt_.joinable()) tt_.join();
  if (hasHttp_ && th_.joinable()) th_.join();
  if (hasUdp_  && tu_.joinable()) tu_.join();
  hasTcp_ = hasHttp_ = hasUdp_ = false;
}

void MdServer::tcpLoop() {
  std::vector<mux_fd_t> rr, rw;
  while (!stop_) {
    // 用多路复用器等待监听口可读（Linux=epoll / Windows=WSAPoll），再非阻塞 accept
    if (muxTcp_.wait(200, rr, rw) <= 0) continue;
    bool ready = false;
    for (auto f : rr) if (f == tcpFd_) { ready = true; break; }
    if (!ready) continue;
    sockfd_t c = tcp_accept_nowait(tcpFd_);
    if (c == MD_INVALID_SOCKET) continue;
    tcpClients_++;
    auto rows = snapshotRows();
    for (auto& r : rows) {
      InstrumentId id = 0;
      for (size_t i = 0; i < syms_.size(); ++i) if (syms_[i] == r.symbol) { id = (InstrumentId)i; break; }
      TickPayload t = rowToTick(r, id);
      uint8_t msg[sizeof(MsgHeader) + sizeof(TickPayload)];
      uint32_t mlen = (uint32_t)writeMessage(msg, MSG_TICK, 1, 0, 0, &t, (uint32_t)sizeof(TickPayload));
      if (!tcp_send(c, msg, mlen)) break;
    }
    sock_close(c);
  }
}

void MdServer::httpLoop() {
  std::vector<mux_fd_t> rr, rw;
  while (!stop_) {
    if (muxHttp_.wait(200, rr, rw) <= 0) continue;
    bool ready = false;
    for (auto f : rr) if (f == httpFd_) { ready = true; break; }
    if (!ready) continue;
    sockfd_t c = tcp_accept_nowait(httpFd_);
    if (c == MD_INVALID_SOCKET) continue;
    httpReqs_++;
    char buf[4096];
    long n = tcp_recv(c, buf, sizeof buf - 1);
    std::string body;
    if (n > 0) {
      buf[n] = 0;
      std::string req(buf);
      size_t sp1 = req.find(' ');
      size_t sp2 = req.find(' ', sp1 + 1);
      std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                         ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
      if (path.rfind("/book", 0) == 0) {
        std::string sym;
        auto q = path.find('?');
        if (q != std::string::npos) {
          std::string qs = path.substr(q + 1);
          auto p = qs.find("sym=");
          if (p != std::string::npos) {
            sym = qs.substr(p + 4);
            auto a = sym.find('&');
            if (a != std::string::npos) sym = sym.substr(0, a);
          }
        }
        if (!sym.empty()) {
          auto rows = snapshotRows();
          for (auto& r : rows) if (r.symbol == sym) { body = bookRowToJson(r); break; }
          if (body.empty()) body = "{}";
        } else {
          body = snapshotJson();
        }
      } else {
        body = snapshotJson();
      }
    }
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
      + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    tcp_send(c, resp.data(), resp.size());
    sock_close(c);
  }
}

void MdServer::udpLoop() {
  while (!stop_) {
    auto rows = snapshotRows();
    for (auto& r : rows) {
      InstrumentId id = 0;
      for (size_t i = 0; i < syms_.size(); ++i) if (syms_[i] == r.symbol) { id = (InstrumentId)i; break; }
      TickPayload t = rowToTick(r, id);
      uint8_t msg[sizeof(MsgHeader) + sizeof(TickPayload)];
      uint32_t mlen = (uint32_t)writeMessage(msg, MSG_TICK, 1, 0, 0, &t, (uint32_t)sizeof(TickPayload));
      udp_sendto(udpFd_, udpGroup_.c_str(), udpPort_, msg, mlen);
      udpSent_++;
    }
    for (int i = 0; i < 20 && !stop_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}}  // namespace md::net
