// ============================================================================
// test_net.cpp —— 网络层 / 抓包 / 数据接口 单元测试
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include "common/Types.h"
#include "net/Socket.h"
#include "net/WireParse.h"
#include "net/MdReceiver.h"
#include "net/JsonBook.h"
#include "net/MdServer.h"

using namespace md;
using namespace md::net;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// 抓包解析：以太网+IP+UDP 载荷 → MarketTick（含无以太网头的 Windows 路径）
static void testParseCaptured() {
  MarketTick t; t.inst = 2; t.price = 12345; t.qty = 7; t.side = kSideBuy;
  uint8_t pkt[256];
  size_t n = buildSamplePacket(pkt, sizeof pkt, t, 1000, 2000);
  CHECK(n > 0);

  MarketTick out; Nanos s = 0, g = 0;
  CHECK(parseCapturedPacket(pkt, n, /*haveEthernet=*/true, out, s, g));
  CHECK(out.inst == 2 && out.price == 12345 && out.qty == 7 && out.side == kSideBuy);
  CHECK(s == 1000 && g == 2000);

  MarketTick out2; Nanos s2 = 0, g2 = 0;
  CHECK(parseCapturedPacket(pkt + 14, n - 14, /*haveEthernet=*/false, out2, s2, g2));
  CHECK(out2.inst == 2 && out2.price == 12345);
}

// 线格式消息往返
static void testDecodeWire() {
  MarketTick t; t.inst = 1; t.price = 999; t.qty = 3; t.side = kSideSell;
  uint8_t msg[sizeof(MsgHeader) + sizeof(TickPayload)];
  uint32_t mlen = (uint32_t)writeMessage(msg, MSG_TICK, 5, 11, 22, &t, (uint32_t)sizeof(TickPayload));
  MarketTick out; Nanos s = 0, g = 0;
  CHECK(decodeWireTick(msg, mlen, out, s, g));
  CHECK(out.inst == 1 && out.price == 999 && out.qty == 3 && out.side == kSideSell);
  CHECK(s == 11 && g == 22);
}

// 真实 UDP 回环：发送 → 接收 → 解析
static void testLoopbackUdp() {
  MdReceiver r;
  CHECK(r.startUdp(9511));
  std::atomic<int> got{0};
  std::atomic<MarketTick> gotTick{};
  r.setOnTick([&](const MarketTick& t, Nanos, Nanos) { got++; gotTick.store(t); });
  r.runAsync();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MarketTick t; t.inst = 4; t.price = 555; t.qty = 9; t.side = kSideBuy;
  uint8_t msg[sizeof(MsgHeader) + sizeof(TickPayload)];
  uint32_t mlen = (uint32_t)writeMessage(msg, MSG_TICK, 1, 0, 0, &t, (uint32_t)sizeof(TickPayload));
  sockfd_t pub = udp_open_send();
  for (int i = 0; i < 50 && got == 0; ++i) {
    udp_sendto(pub, "127.0.0.1", 9511, msg, mlen);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  sock_close(pub);
  r.requestStop(); r.join();
  CHECK(got > 0);
  CHECK(gotTick.load().inst == 4 && gotTick.load().price == 555);
}

// HTTP REST 接口：server 维护状态，/snapshot 返回 JSON
static void testHttpSnapshot() {
  MdServer srv;
  srv.setSymbols({"IF", "RB"});
  CHECK(srv.startHttp(8099));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  MarketTick t; t.inst = 0; t.price = 4000; t.qty = 10; t.side = kSideBuy;
  srv.pushTick(t, 0, 0);
  t.inst = 1; t.price = 3500; t.qty = 5; t.side = kSideSell;
  srv.pushTick(t, 0, 0);

  // 以 TCP 客户端连上，发 HTTP GET，收 JSON
  sockfd_t c = tcp_connect("127.0.0.1", 8099);
  CHECK(c != MD_INVALID_SOCKET);
  std::string req = "GET /snapshot HTTP/1.1\r\nHost: localhost\r\n\r\n";
  CHECK(tcp_send(c, req.data(), req.size()));
  char buf[4096];
  long n = tcp_recv(c, buf, sizeof buf - 1);
  CHECK(n > 0);
  std::string resp(buf, (size_t)n);
  CHECK(resp.find("\"IF\"") != std::string::npos);
  CHECK(resp.find("\"RB\"") != std::string::npos);
  CHECK(resp.find("\"BUY\"") != std::string::npos);
  sock_close(c);

  srv.requestStop(); srv.joinAll();
}

// JSON 序列化
static void testJson() {
  std::vector<BookRow> rows;
  rows.push_back({"IF", 4000, 10, 0, 100});
  rows.push_back({"RB", 3500, 5, 1, 50});
  std::string j = bookRowsToJson(rows);
  CHECK(j.find("\"IF\"") != std::string::npos);
  CHECK(j.find("\"RB\"") != std::string::npos);
  CHECK(j.find("\"BUY\"") != std::string::npos);
  CHECK(j.find("\"SELL\"") != std::string::npos);
}

int main() {
  sock_init();
  testParseCaptured();
  testDecodeWire();
  testLoopbackUdp();
  testHttpSnapshot();
  testJson();
  sock_cleanup();
  std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
  if (g_fail) { std::printf("NET TESTS FAILED\n"); return 1; }
  std::printf("ALL NET TESTS PASSED\n");
  return 0;
}
