#include "ipc/SpscRingBuffer.h"

#include <cstdio>
#include <cstring>
#include <atomic>

namespace md {

bool SpscRingBuffer::attach(void* region, size_t regionBytes, uint64_t capacity, bool init) {
    if (!region) {
        std::fprintf(stderr, "[ring] null region\n");
        return false;
    }
    if (!std::atomic<uint64_t>::is_always_lock_free) {
        std::fprintf(stderr, "[ring][warn] std::atomic<uint64_t> not lock-free on this platform\n");
    }
    // 控制头在固定偏移，先就位（与容量无关）。ctrl_ 必须 64 字节对齐，
    // 否则 std::atomic 成员落在未对齐地址属未定义行为（x86 上亦可能出错）。
    uint8_t* base = static_cast<uint8_t*>(region);
    uintptr_t c = (reinterpret_cast<uintptr_t>(base) + 63) & ~static_cast<uintptr_t>(63);
    if (c < reinterpret_cast<uintptr_t>(base) + 16) {
        c += 64;   // 至少越过 RingHeader 区域
    }
    header_ = reinterpret_cast<RingHeader*>(base);
    ctrl_   = reinterpret_cast<RingCtrl*>(c);
    const size_t ctrlBytes = (sizeof(RingCtrl) + 63) & ~static_cast<size_t>(63);
    data_   = reinterpret_cast<uint8_t*>(c) + ctrlBytes;   // = base+256（当 base 64 对齐时）

    if (init) {
        const size_t need = requiredBytes(capacity);
        if (regionBytes < need) {
            std::fprintf(stderr, "[ring] region too small: %zu < %zu\n", regionBytes, need);
            return false;
        }
        header_->magic = kRingMagic;
        header_->total = regionBytes;
        ctrl_->head.store(0, std::memory_order_relaxed);
        ctrl_->tail.store(0, std::memory_order_relaxed);
        ctrl_->lost.store(0, std::memory_order_relaxed);
        ctrl_->capacity   = capacity;
        ctrl_->slotStride = slotStride();
    } else {
        if (header_->magic != kRingMagic) {
            std::fprintf(stderr, "[ring] magic mismatch (consumer attached before producer?)\n");
            return false;
        }
        const size_t need = kRingHdrBytes +
                            static_cast<size_t>(ctrl_->capacity) * ctrl_->slotStride;
        if (regionBytes < need) {
            std::fprintf(stderr, "[ring] region too small for capacity %llu: %zu < %zu\n",
                         static_cast<unsigned long long>(ctrl_->capacity), regionBytes, need);
            return false;
        }
        if (ctrl_->slotStride != slotStride()) {
            std::fprintf(stderr, "[ring] slotStride mismatch (producer/consumer build mismatch?)\n");
            return false;
        }
    }
    return true;
}

bool SpscRingBuffer::tryPublish(const uint8_t* msg, uint32_t msgLen) {
    const uint64_t h = ctrl_->head.load(std::memory_order_relaxed);
    const uint64_t t = ctrl_->tail.load(std::memory_order_acquire);
    if (h - t >= ctrl_->capacity) {              // 缓冲满
        ctrl_->lost.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const size_t off = static_cast<size_t>(h % ctrl_->capacity) * ctrl_->slotStride;
    std::memcpy(data_ + off, msg, msgLen);
    // release：保证消费者 load head 后能看到槽位全部写入
    ctrl_->head.store(h + 1, std::memory_order_release);
    return true;
}

bool SpscRingBuffer::tryConsume(uint8_t* out, uint32_t* outLen) {
    const uint64_t t = ctrl_->tail.load(std::memory_order_relaxed);
    const uint64_t h = ctrl_->head.load(std::memory_order_acquire);
    if (t == h) {                                // 空
        return false;
    }
    const size_t off = static_cast<size_t>(t % ctrl_->capacity) * ctrl_->slotStride;
    const MsgHeader* ph = reinterpret_cast<const MsgHeader*>(data_ + off);
    const uint32_t total = sizeof(MsgHeader) + ph->len;
    std::memcpy(out, data_ + off, total);
    *outLen = total;
    // release：通知生产者该槽位已可复用
    ctrl_->tail.store(t + 1, std::memory_order_release);
    return true;
}

}  // namespace md
