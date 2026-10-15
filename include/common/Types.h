#pragma once
// ============================================================================
// common/Types.h —— 期货行情系统的二进制线格式（wire format）
//
// 设计原则：
//   - 全部定长、#pragma pack(1) 紧凑布局，避免运行时序列化开销；
//   - 价格用 int64「 tick 计数」而非浮点，杜绝浮点误差（真实交易系统通用做法）；
//   - 消息可在共享内存里按槽位原样 memcpy，消费者零解析即可读取。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace md {

// ---- 消息类型 ----
enum MsgType : uint8_t {
    MSG_TICK      = 1,  // 逐笔成交（主动方吃单）
    MSG_SNAPSHOT  = 2,  // 盘口快照 L2（5 档）
    MSG_HEARTBEAT = 3,  // 网关心跳 / 吞吐统计
    MSG_CONTROL   = 4,  // 会话控制（开盘 / 收盘）
};

using InstrumentId = uint32_t;   // 合约编码，如 0=IF 1=IC 2=TF ...
using PriceTicks   = int64_t;    // 价格以最小变动价位(tick)为单位的整数
using Qty          = uint32_t;   // 成交量（手）
using Nanos        = uint64_t;   // 纳秒时间戳
using Seq          = uint32_t;   // 全局递增序号

static constexpr uint16_t kMagic   = 0xEB90;   // 行情帧魔术字
static constexpr uint8_t  kSideBuy  = 0;
static constexpr uint8_t  kSideSell = 1;

// ---- 行情帧头（共享内存环里每条消息都带这个头）----
#pragma pack(push, 1)
struct MsgHeader {
    uint16_t magic;   // kMagic
    uint8_t  type;    // MsgType
    uint8_t  qos;     // 0=普通 1=重要(如快照/控制)
    Seq      seq;     // 网关分配的消息序号
    uint32_t len;     // payload 字节数
    Nanos    srcTs;   // 行情源时间戳（交易所 / 期货公司网关）
    Nanos    gwTs;    // 本网关接收时间戳
};
#pragma pack(pop)

// ---- 逐笔成交载荷 ----
#pragma pack(push, 1)
struct TickPayload {
    InstrumentId inst;
    PriceTicks   price;
    Qty          qty;
    uint8_t      side;      // 主动方方向：kSideBuy / kSideSell
    uint8_t      _pad[3];
};
#pragma pack(pop)

// 逻辑行情事件（逐笔成交）的别名，便于阅读
using MarketTick = TickPayload;

// ---- 盘口一档 ----
#pragma pack(push, 1)
struct BookLevel {
    PriceTicks price;
    Qty        qty;
};
#pragma pack(pop)

static constexpr uint8_t kBookLevels = 5;

// ---- 盘口快照载荷（5 档）----
#pragma pack(push, 1)
struct SnapPayload {
    InstrumentId inst;
    uint8_t      levels;   // = kBookLevels
    uint8_t      _pad[3];
    BookLevel    bids[kBookLevels];
    BookLevel    asks[kBookLevels];
};
#pragma pack(pop)

// ---- 心跳 / 统计载荷 ----
#pragma pack(push, 1)
struct HeartbeatPayload {
    uint64_t pubCount;      // 累计发布消息数
    uint64_t dropCount;     // 因 ring 满丢弃的消息数
    uint64_t consumerLagUs; // 消费者滞后（最新消息 age，微秒）
    uint32_t ringCapacity;  // 环形缓冲容量（槽）
    uint32_t pad;
};
#pragma pack(pop)

// ---- 会话控制载荷 ----
#pragma pack(push, 1)
struct ControlPayload {
    uint8_t  state;   // 0=收盘 1=开盘
    uint8_t  _pad[3];
};
#pragma pack(pop)

// 单个槽位能容纳的最大 payload（快照最大），加上头即为槽大小
static constexpr size_t kMaxPayload = sizeof(SnapPayload);
static constexpr size_t kSlotSize   = sizeof(MsgHeader) + kMaxPayload;  // 对齐由调用方保证

// 工具：把一条消息写入 buf（header + payload 连续），返回占用字节数
inline size_t writeMessage(uint8_t* buf, MsgType type, Seq seq,
                           Nanos srcTs, Nanos gwTs, const void* payload,
                           uint32_t payloadLen, uint8_t qos = 0) {
    MsgHeader* h = reinterpret_cast<MsgHeader*>(buf);
    h->magic = kMagic;
    h->type  = static_cast<uint8_t>(type);
    h->qos   = qos;
    h->seq   = seq;
    h->len   = payloadLen;
    h->srcTs = srcTs;
    h->gwTs  = gwTs;
    if (payload && payloadLen) {
        ::memcpy(buf + sizeof(MsgHeader), payload, payloadLen);
    }
    return sizeof(MsgHeader) + payloadLen;
}

}  // namespace md
