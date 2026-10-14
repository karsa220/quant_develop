#pragma once
// ============================================================================
// net/WireParse.h —— 线格式与抓包报文解析（header-only）
//
// 两个层面的解析：
//   1) decodeWireTick     : 解析「内部线格式消息」(MsgHeader + TickPayload)
//                           —— 这是网关/网络/共享内存里统一承载的字节流。
//   2) parseCapturedPacket: 解析「捕获到的网络报文」(以太网/IPv4/UDP 载荷)，
//                           抽出自 UDP 载荷后再交给 decodeWireTick。
//   3) buildSamplePacket  : 构造一段可解析的样本报文，供单测/演示使用。
//
// 抓包真实场景里，行情源把线格式消息塞进 UDP 载荷，经由网卡 → 内核协议栈
// （或内核旁路 DPDK）→ 用户态 recv。这里把「链路层 → 传输层 → 业务层」
// 的解析显式写出来，正是 HFT 岗常考的报文处理功底。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "common/Types.h"

namespace md { namespace net {

// 解析内部线格式消息（MsgHeader + TickPayload）。失败返回 false。
inline bool decodeWireTick(const uint8_t* buf, size_t len,
                           MarketTick& out, Nanos& srcTs, Nanos& gwTs) {
  if (!buf || len < sizeof(MsgHeader) + sizeof(TickPayload)) return false;
  const MsgHeader* h = reinterpret_cast<const MsgHeader*>(buf);
  if (h->magic != kMagic) return false;
  if (h->type != MSG_TICK) return false;
  if (h->len < sizeof(TickPayload)) return false;
  const TickPayload* p = reinterpret_cast<const TickPayload*>(buf + sizeof(MsgHeader));
  std::memcpy(&out, p, sizeof(TickPayload));
  srcTs = h->srcTs;
  gwTs = h->gwTs;
  return true;
}

// 构造一段样本报文：以太网(14) + IPv4(20) + UDP(8) + 线格式消息。
// 返回写入字节数（outCap 不足则返回 0）。host 字节序字段按网络序写入。
inline size_t buildSamplePacket(uint8_t* out, size_t outCap,
                                const MarketTick& t, Nanos srcTs, Nanos gwTs,
                                uint16_t udpSrcPort = 12345,
                                uint16_t udpDstPort = 9500) {
  uint8_t msg[sizeof(MsgHeader) + sizeof(TickPayload)];
  uint32_t mlen = (uint32_t)(sizeof(MsgHeader) + sizeof(TickPayload));
  writeMessage(msg, MSG_TICK, 1, srcTs, gwTs, &t, (uint32_t)sizeof(TickPayload));

  const size_t ethH = 14, ipH = 20, udpH = 8;
  const size_t total = ethH + ipH + udpH + mlen;
  if (outCap < total) return 0;
  std::memset(out, 0, total);

  // 以太网头：dst(6) src(6) ethertype(2)=0x0800(IPv4)
  out[12] = 0x08; out[13] = 0x00;

  // IPv4 头：version/ihl + total length + protocol(17=UDP)
  uint8_t* ip = out + ethH;
  ip[0] = 0x45;
  uint16_t ipTotal = (uint16_t)(ipH + udpH + mlen);
  ip[2] = (uint8_t)(ipTotal >> 8); ip[3] = (uint8_t)(ipTotal & 0xFF);
  ip[9] = 17;   // UDP

  // UDP 头：src/dst port + length
  uint8_t* udp = ip + ipH;
  udp[0] = (uint8_t)(udpSrcPort >> 8); udp[1] = (uint8_t)(udpSrcPort & 0xFF);
  udp[2] = (uint8_t)(udpDstPort >> 8);  udp[3] = (uint8_t)(udpDstPort & 0xFF);
  uint16_t udpTotal = (uint16_t)(udpH + mlen);
  udp[4] = (uint8_t)(udpTotal >> 8); udp[5] = (uint8_t)(udpTotal & 0xFF);

  std::memcpy(udp + udpH, msg, mlen);
  return total;
}

// 解析捕获报文。haveEthernet=true 表示从以太网头开始（Linux AF_PACKET），
// 否则从 IPv4 头开始（Windows 原始套接字）。仅支持 IPv4/UDP。
inline bool parseCapturedPacket(const uint8_t* pkt, size_t len, bool haveEthernet,
                                MarketTick& out, Nanos& srcTs, Nanos& gwTs) {
  if (!pkt) return false;
  size_t off = 0;
  if (haveEthernet) {
    if (len < 14) return false;
    if (pkt[12] != 0x08 || pkt[13] != 0x00) return false;  // 只支持 IPv4
    off = 14;
  }
  if (len < off + 20) return false;
  uint8_t ver = pkt[off] >> 4;
  if (ver != 4) return false;
  uint8_t ihl = (uint8_t)((pkt[off] & 0x0F) * 4);
  if (ihl < 20) return false;
  if (pkt[off + 9] != 17) return false;   // 只支持 UDP
  off += ihl;
  if (len < off + 8) return false;
  uint16_t udpLen = (uint16_t)((pkt[off + 4] << 8) | pkt[off + 5]);
  size_t payloadOff = off + 8;
  size_t payloadLen = udpLen >= 8 ? udpLen - 8 : 0;
  if (payloadOff + payloadLen > len) payloadLen = len - payloadOff;
  return decodeWireTick(pkt + payloadOff, payloadLen, out, srcTs, gwTs);
}

}}  // namespace md::net
