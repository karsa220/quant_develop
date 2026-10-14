#pragma once
// ============================================================================
// net/IoMultiplexer.h —— I/O 多路复用（JD 加分项：epoll、零拷贝、高并发）
//
// Linux 生产路径使用 epoll（边缘/水平触发，O(1) 就绪通知，支撑上万并发连接）；
// Windows 无 epoll，用 WSAPoll 提供等价能力，便于跨平台编译与本机验证。
//
// 接入点：MdServer 的监听套接字注册到多路复用器，accept 前先 wait() 确认可读，
// 避免阻塞、把 accept 放进事件循环（典型 reactor 模式）。
// ============================================================================
#include <vector>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
   typedef SOCKET mux_fd_t;
#else
   typedef int mux_fd_t;
#endif

namespace md { namespace net {

class IoMultiplexer {
public:
    IoMultiplexer();
    ~IoMultiplexer();

    // 创建底层实例（epoll_create1 / Windows 无需）
    bool create();
    // 注册/注销监听 fd（read=可读事件，write=可写事件）
    void add(mux_fd_t fd, bool read = true, bool write = false);
    void remove(mux_fd_t fd);
    // 等待就绪；返回就绪 fd 数，分别填充 readyRead / readyWrite
    int wait(int timeoutMs, std::vector<mux_fd_t>& readyRead,
             std::vector<mux_fd_t>& readyWrite);
    // 后端名称：Linux="epoll"，Windows="poll"
    const char* backend() const;

private:
#ifdef __linux__
    int epfd_ = -1;
    std::vector<struct epoll_event> evs_;
#else
    std::vector<pollfd> fds_;
#endif
};

}}  // namespace md::net
