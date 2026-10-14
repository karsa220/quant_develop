#pragma once
// ============================================================================
// ipc/SpscRingBuffer.h —— 无锁单生产者/单消费者(SPSC)环形缓冲
//
// 这是「行情网关 → 策略进程」之间低延迟 IPC 的核心。特点：
//   - 全程无锁：生产者只写 head，消费者只写 tail，天然无竞争；
//   - head / tail / lost 各自独占一个缓存行(64B)，消除伪共享(false sharing)；
//   - 用 acquire/release 内存序（非 seq_cst），把 cache 同步开销压到最低；
//   - 跑在共享内存里，跨进程零拷贝（生产者写槽位，消费者读槽位）。
//
// 复杂度：发布/消费均 O(1)，与消息速率无关。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <atomic>

#include "common/Types.h"   // 复用 kSlotSize / kMaxPayload 等常量

namespace md {

// 环形缓冲控制头（位于共享内存 region 起始处）
struct RingHeader {
    uint64_t magic;    // kRingMagic
    uint64_t total;    // 整段共享内存字节数
};

struct RingCtrl {
    alignas(64) std::atomic<uint64_t> head;  // 生产者下一个写入序号（单调递增）
    alignas(64) std::atomic<uint64_t> tail;  // 消费者下一个读取序号（单调递增）
    alignas(64) std::atomic<uint64_t> lost;  // 因缓冲满丢弃的消息数
    uint64_t capacity;                       // 槽位数
    uint64_t slotStride;                     // 每槽字节数（64B 对齐后）
};

static constexpr uint64_t kRingMagic    = 0x52494E47;  // "RING"
static constexpr size_t   kRingHdrBytes  = 256;         // RingHeader + RingCtrl 预留

class SpscRingBuffer {
public:
    // 给定容量，算出所需共享内存大小（槽位按 64B 对齐）
    static size_t requiredBytes(uint64_t capacity) {
        const size_t stride = slotStride();
        return kRingHdrBytes + static_cast<size_t>(capacity) * stride;
    }

    // 挂接到一段共享内存。init=true 表示生产者首次创建（清零并写控制头）。
    bool attach(void* region, size_t regionBytes, uint64_t capacity, bool init);

    // 生产者：发布一条消息（header+payload 连续）。满则返回 false 并计数丢弃。
    bool tryPublish(const uint8_t* msg, uint32_t msgLen);

    // 消费者：取出一条消息到 out，*outLen 返回字节数。空返回 false。
    bool tryConsume(uint8_t* out, uint32_t* outLen);

    size_t  sizeApprox() const {
        return static_cast<size_t>(ctrl_->head.load(std::memory_order_acquire) -
                                   ctrl_->tail.load(std::memory_order_acquire));
    }
    bool     empty() const { return sizeApprox() == 0; }
    uint64_t dropped() const { return ctrl_->lost.load(std::memory_order_relaxed); }
    uint64_t capacity() const { return ctrl_->capacity; }

private:
    static size_t slotStride() {
        // 槽位 = 头 + 最大 payload，按 64B 对齐（缓存行友好）
        return ((kSlotSize + 63) / 64) * 64;
    }

    RingHeader* header_ = nullptr;
    RingCtrl*   ctrl_   = nullptr;
    uint8_t*    data_   = nullptr;
};

}  // namespace md
