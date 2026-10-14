#pragma once
// ============================================================================
// feed/CtpLikeCodec.h —— CTP 风格二进制行情帧编解码
//
// 模拟期货公司网关推送的「线路格式」：定长头 + 长度 + 载荷 + 校验。
//   frame = [magic:2 LE][type:1][len:2 LE][payload:len][checksum:1]
//
// 网关收到的就是这种字节流，需要解析成结构化 MarketTick 再发布到内部环形缓冲。
// 这是「采集 → 解析」环节的工程量体现：真实系统里还要处理半包/粘包/大小端。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include "common/Types.h"

namespace md {

// 把一条逐笔成交编码为 CTP 风格帧，返回帧字节数（写入 buf）。
// 调用方需保证 buf 至少 kMaxFrame 字节。
uint32_t CtpLikeCodec_encodeTick(const MarketTick& t, uint8_t* buf);

// 解析帧为 MarketTick。校验 magic / 长度 / 校验和；失败返回 false。
bool CtpLikeCodec_decodeTick(const uint8_t* frame, uint32_t frameLen, MarketTick& out);

// 帧字节数（tick 载荷固定）
static constexpr uint32_t kTickFrameSize =
    sizeof(uint16_t) + 1 + sizeof(uint16_t) + sizeof(TickPayload) + 1;  // = 22
static constexpr uint32_t kMaxFrame = 64;

}  // namespace md
