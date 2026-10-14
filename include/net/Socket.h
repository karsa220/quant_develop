#pragma once
// ============================================================================
// net/Socket.h —— 跨平台 C++ 网络编程封装（Windows Winsock2 / POSIX Berkeley）
//
// 这一层是「行情网络采集 / 抓包 / 多协议数据接口」的基础设施：
//   - 统一 socket 句柄类型，屏蔽 winsock 与 POSIX 的差异；
//   - UDP 组播、TCP 监听/连接、带超时的阻塞收发（便于循环检查停止标志）；
//   - 所有句柄 RAII 关闭（sock_close）。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sockfd_t;
#  define MD_INVALID_SOCKET INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sockfd_t;
#  define MD_INVALID_SOCKET (-1)
#endif

namespace md { namespace net {

// 进程内只需调用一次（Windows 需要 WSAStartup）
bool sock_init();
void sock_cleanup();
void sock_close(sockfd_t fd);

// 设置接收超时（毫秒），让 recv 循环能周期性检查停止标志
void set_recv_timeout(sockfd_t fd, int ms);

// ---- UDP ----
sockfd_t udp_bind_recv(uint16_t port, const char* group = nullptr); // group 非空则加入组播
sockfd_t udp_open_send();
bool     udp_sendto(sockfd_t fd, const char* host, uint16_t port, const void* data, size_t len);
long     udp_recv(sockfd_t fd, void* buf, size_t cap);

// ---- TCP ----
sockfd_t tcp_listen(uint16_t port, int backlog = 8);
sockfd_t tcp_accept(sockfd_t listenFd);
sockfd_t tcp_connect(const char* host, uint16_t port);
bool     tcp_send(sockfd_t fd, const void* data, size_t len);
long     tcp_recv(sockfd_t fd, void* buf, size_t cap);

}}  // namespace md::net
