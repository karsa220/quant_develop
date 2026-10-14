#pragma once
// ============================================================================
// shm/ShmSegment.h —— 跨平台共享内存封装
//
//   Linux  : shm_open() + ftruncate() + mmap()  (/dev/shm 下，真正的 mmap 共享内存)
//   Windows: CreateFileMappingW() + MapViewOfFile()（页文件支持的命名映射，跨进程可见）
//
// 生产者用 create() 开辟，消费者用 open() 挂接同一名字的映射，
// 二者拿到同一段物理内存，构成「行情网关 → 策略进程」的低延迟 IPC 通道。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <string>

namespace md {

class ShmSegment {
public:
    ShmSegment() = default;
    ~ShmSegment() { close(); }

    // 生产者：开辟 size 字节共享内存并映射
    bool create(const char* name, size_t size);
    // 消费者：挂接已存在的共享内存；outSize 返回实际大小
    bool open(const char* name, size_t* outSize = nullptr);

    void* data() { return data_; }
    size_t size() const { return size_; }
    bool isValid() const { return data_ != nullptr; }
    const std::string& name() const { return name_; }

    void close();

    ShmSegment(const ShmSegment&) = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;

private:
    std::string name_;
    void*       data_ = nullptr;
    size_t      size_ = 0;
#if defined(_WIN32)
    void*       handle_ = nullptr;  // HANDLE
#else
    int         fd_ = -1;
#endif
};

}  // namespace md
