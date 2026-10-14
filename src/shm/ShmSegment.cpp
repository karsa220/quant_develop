#include "shm/ShmSegment.h"

#include <cstdio>
#include <cstring>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#elif defined(_WIN32)
#  include <windows.h>
#else
#  error "unsupported platform"
#endif

namespace md {

#if defined(__linux__)
bool ShmSegment::create(const char* name, size_t size) {
    close();
    name_ = name;
    fd_ = ::shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        std::fprintf(stderr, "[shm] shm_open(create) %s failed\n", name);
        return false;
    }
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        std::fprintf(stderr, "[shm] ftruncate %zu failed\n", size);
        return false;
    }
    data_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        std::fprintf(stderr, "[shm] mmap failed\n");
        return false;
    }
    size_ = size;
    // 建议大页/锁定在内存中（生产可加 mlock / MAP_HUGETLB）
    ::madvise(data_, size, MADV_SEQUENTIAL);
    return true;
}

bool ShmSegment::open(const char* name, size_t* outSize) {
    close();
    name_ = name;
    fd_ = ::shm_open(name, O_RDWR, 0600);
    if (fd_ < 0) {
        std::fprintf(stderr, "[shm] shm_open(open) %s failed (producer not up?)\n", name);
        return false;
    }
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        std::fprintf(stderr, "[shm] fstat failed\n");
        return false;
    }
    size_ = static_cast<size_t>(st.st_size);
    data_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        return false;
    }
    if (outSize) *outSize = size_;
    return true;
}
#else
bool ShmSegment::create(const char* name, size_t size) {
    close();
    name_ = name;
    // Windows 命名映射需加 "Local\\" 前缀；名字里去掉前导 '/'
    std::string wname = "Local\\md_";
    for (const char* p = name; *p; ++p) {
        if (*p == '/') continue;
        wname.push_back(*p);
    }
    // UTF-8 -> UTF-16（ASCII 场景足够）
    wchar_t wbuf[256];
    size_t i = 0;
    for (; i + 1 < sizeof(wbuf) / sizeof(wchar_t) && i < wname.size(); ++i)
        wbuf[i] = static_cast<wchar_t>(wname[i]);
    wbuf[i] = 0;
    handle_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    static_cast<DWORD>(size >> 32),
                                    static_cast<DWORD>(size & 0xFFFFFFFFu),
                                    wbuf);
    if (!handle_) {
        std::fprintf(stderr, "[shm] CreateFileMapping failed\n");
        return false;
    }
    data_ = ::MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, size);
    if (!data_) {
        std::fprintf(stderr, "[shm] MapViewOfFile failed\n");
        return false;
    }
    size_ = size;
    return true;
}

bool ShmSegment::open(const char* name, size_t* outSize) {
    close();
    name_ = name;
    std::string wname = "Local\\md_";
    for (const char* p = name; *p; ++p) {
        if (*p == '/') continue;
        wname.push_back(*p);
    }
    wchar_t wbuf[256];
    size_t i = 0;
    for (; i + 1 < sizeof(wbuf) / sizeof(wchar_t) && i < wname.size(); ++i)
        wbuf[i] = static_cast<wchar_t>(wname[i]);
    wbuf[i] = 0;
    handle_ = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wbuf);
    if (!handle_) {
        std::fprintf(stderr, "[shm] OpenFileMapping(open) %s failed (producer not up?)\n", name);
        return false;
    }
    MEMORY_BASIC_INFORMATION info {};
    if (::VirtualQuery(data_ ? data_ : handle_, &info, sizeof(info)) == 0) {
        // 先尝试 map 再 query
    }
    data_ = ::MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, 0);
    if (!data_) {
        std::fprintf(stderr, "[shm] MapViewOfFile(open) failed\n");
        return false;
    }
    // 用 VirtualQuery 拿到映射大小
    MEMORY_BASIC_INFORMATION mi {};
    if (::VirtualQuery(data_, &mi, sizeof(mi))) {
        size_ = mi.RegionSize;
    } else {
        size_ = 0;
    }
    if (outSize) *outSize = size_;
    return true;
}
#endif

void ShmSegment::close() {
#if defined(__linux__)
    if (data_) { ::munmap(data_, size_); data_ = nullptr; }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        // 仅创建者负责 unlink，这里保守不 unlink，交由 demo 显式清理
    }
#else
    if (data_) { ::UnmapViewOfFile(data_); data_ = nullptr; }
    if (handle_) { ::CloseHandle(handle_); handle_ = nullptr; }
#endif
    size_ = 0;
}

}  // namespace md
