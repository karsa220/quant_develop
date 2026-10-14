// ============================================================================
// net_demo.cpp —— 端到端网络链路演示
//
//  模拟行情源(UDP 发送) → MdReceiver(网络采集/解析) → SpscRingBuffer(低延迟入环)
//                     → MdServer(三种对外数据接口) + PcapCapture(链路层抓包)
//
// 运行：
//   ./build/net_demo            （默认跑 5 秒）
// 之后可用：  curl http://127.0.0.1:8080/snapshot   看 REST 接口
//            curl http://127.0.0.1:8080/book?sym=IF 看单合约
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include "common/Types.h"
#include "feed/FuturesFeed.h"
#include "ipc/SpscRingBuffer.h"
#include "net/Socket.h"
#include "net/MdReceiver.h"
#include "net/PacketCapture.h"
#include "net/MdServer.h"

using namespace md;
using namespace md::net;

int main() {
  sock_init();

  const std::vector<std::string> syms = {"IF", "IC", "TF", "RB", "AU", "CU"};
  const uint64_t cap = 262144;
  std::vector<uint8_t> region(SpscRingBuffer::requiredBytes(cap));
  SpscRingBuffer ring;
  if (!ring.attach(region.data(), region.size(), cap, /*init=*/true)) {
    std::fprintf(stderr, "[net_demo] ring attach failed\n");
    return 1;
  }

  // 多协议数据接口
  MdServer server;
  server.setSymbols(syms);
  server.startTcp(9001);
  server.startHttp(8080);
  server.startUdp(9002, "239.1.1.1");

  // 网络接收器：绑定 UDP 9500，解析后入环 + 喂给 server
  MdReceiver recv;
  recv.setRing(&ring);
  recv.setOnTick([&](const MarketTick& t, Nanos s, Nanos g) { server.pushTick(t, s, g); });
  if (!recv.startUdp(9500)) {
    std::fprintf(stderr, "[net_demo] 无法绑定 UDP 9500\n");
    return 1;
  }
  recv.runAsync();

  // 链路层抓包（无权限则优雅跳过，仅打印提示）
  PcapCapture sniffer;
  bool capOk = sniffer.start(9500, [&](const MarketTick& t, Nanos, Nanos) {
    (void)t; /* 抓到的报文可在这里做延迟核对/留痕 */
  });
  if (!capOk) {
    std::printf("[note] 链路层抓包需要权限（Linux CAP_NET_RAW / Windows 管理员），本机以网络接收路径演示\n");
  }

  // 模拟行情源：UDP 发送线格式消息到 127.0.0.1:9500
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> sent{0};
  sockfd_t pub = udp_open_send();
  std::thread pubTh([&] {
    FuturesFeed feed(12345);
    Nanos srcTs = 0;
    MarketTick t;
    while (!stop) {
      if (feed.nextTick(t, srcTs)) {
        uint8_t msg[sizeof(MsgHeader) + sizeof(TickPayload)];
        uint32_t mlen = (uint32_t)writeMessage(msg, MSG_TICK, (Seq)(sent.load() + 1),
                                               srcTs, 0, &t, (uint32_t)sizeof(TickPayload));
        if (udp_sendto(pub, "127.0.0.1", 9500, msg, mlen)) sent++;
      }
    }
  });

  std::printf("=== 网络链路演示（5 秒）===\n");
  std::printf("TCP 二进制接口 :9001 | HTTP REST :8080 | UDP 组播 239.1.1.1:9002 | 行情源→接收 :9500\n");
  std::this_thread::sleep_for(std::chrono::seconds(5));

  stop = true;
  pubTh.join();
  sock_close(pub);
  recv.requestStop(); recv.join();
  sniffer.requestStop(); sniffer.join();
  server.requestStop(); server.joinAll();
  sock_cleanup();

  std::printf("\n=== 网络层压测结果 ===\n");
  std::printf("UDP 发送报文      : %llu\n", static_cast<unsigned long long>(sent.load()));
  std::printf("接收解析成功/坏帧 : %llu / %llu\n",
              static_cast<unsigned long long>(recv.received()),
              static_cast<unsigned long long>(recv.bad()));
  std::printf("ring 入环(dropped) : %llu\n", static_cast<unsigned long long>(ring.dropped()));
  std::printf("TCP 接口服务客户端 : %llu\n", static_cast<unsigned long long>(server.tcpClients()));
  std::printf("HTTP 接口请求数   : %llu\n", static_cast<unsigned long long>(server.httpReqs()));
  std::printf("UDP 组播发送快照  : %llu\n", static_cast<unsigned long long>(server.udpSent()));
  if (capOk) std::printf("抓包解析报文      : %llu\n", static_cast<unsigned long long>(sniffer.captured()));
  std::printf("\nHTTP 快照示例 (curl http://127.0.0.1:8080/snapshot):\n%s\n",
              server.snapshotJson().c_str());
  return 0;
}
