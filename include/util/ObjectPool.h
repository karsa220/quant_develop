#pragma once
// ============================================================================
// util/ObjectPool.h —— 无锁对象池 / 内存池（JD 加分项：内存池、对象池）
//
// 热路径上避免反复 new/delete（系统调用 + 锁 + 碎片），改为从预分配池中
// acquire/release。实现为 Treiber 栈（无锁 LIFO 自由表），多生产者多消费者安全。
//
// 用法：高频行情/订单对象在策略线程内复用，降低尾延迟与缓存抖动。
// ============================================================================
#include <atomic>
#include <array>
#include <cstddef>

namespace md {

template <typename T, size_t N>
class ObjectPool {
public:
    ObjectPool() {
        // 初始把所有节点压入自由表
        for (size_t i = 0; i < N; ++i) {
            storage_[i].next = freeHead_.load(std::memory_order_relaxed);
            freeHead_.store(&storage_[i], std::memory_order_relaxed);
        }
    }

    // 取一个对象（构造未做，调用方负责 placement-new 或直接赋值 POD）
    // 池空返回 nullptr（背压信号，调用方可自旋或丢弃）
    T* acquire() {
        Node* old = freeHead_.load(std::memory_order_acquire);
        while (old) {
            Node* next = old->next;
            if (freeHead_.compare_exchange_weak(old, next,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                return reinterpret_cast<T*>(old);
            }
        }
        return nullptr;
    }

    // 归还对象到池
    void release(T* p) {
        Node* n = reinterpret_cast<Node*>(p);
        Node* old = freeHead_.load(std::memory_order_relaxed);
        do {
            n->next = old;
        } while (!freeHead_.compare_exchange_weak(old, n,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed));
    }

    static constexpr size_t capacity() { return N; }

    // 当前可用数（近似，仅供监控/测试）
    size_t availableApprox() const {
        size_t c = 0;
        for (Node* n = freeHead_.load(std::memory_order_acquire); n; n = n->next) ++c;
        return c;
    }

private:
    struct Node {
        alignas(alignof(T)) unsigned char obj[sizeof(T)];   // 对象存储（不调用构造）
        Node* next;
    };
    std::array<Node, N> storage_;
    std::atomic<Node*>  freeHead_{nullptr};
};

}  // namespace md
