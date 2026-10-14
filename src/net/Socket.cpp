// ============================================================================
// net/Socket.cpp —— 跨平台 socket 实现
// ============================================================================
#include "net/Socket.h"
#include <cstring>

#ifdef _WIN32
#  include <mstcpip.h>
#else
#  include <sys/types.h>
#  include <sys/select.h>
#  include <fcntl.h>
#endif

namespace md { namespace net {

static bool g_init = false;

bool sock_init() {
#ifdef _WIN32
  if (g_init) return true;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  g_init = true;
  return true;
#else
  g_init = true;
  return true;
#endif
}

void sock_cleanup() {
#ifdef _WIN32
  if (g_init) { WSACleanup(); g_init = false; }
#endif
}

void sock_close(sockfd_t fd) {
  if (fd == MD_INVALID_SOCKET) return;
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

void set_recv_timeout(sockfd_t fd, int ms) {
#ifdef _WIN32
  DWORD t = (DWORD)ms;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

sockfd_t udp_bind_recv(uint16_t port, const char* group) {
  sockfd_t fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == MD_INVALID_SOCKET) return MD_INVALID_SOCKET;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { sock_close(fd); return MD_INVALID_SOCKET; }
  if (group && group[0]) {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    // 组播加入失败不致命（仍可当单播收），仅打印
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) != 0) {
      std::fprintf(stderr, "[net] 加入组播 %s 失败（将以单播接收）\n", group);
    }
  }
  set_recv_timeout(fd, 200);
  return fd;
}

sockfd_t udp_open_send() {
  sockfd_t fd = socket(AF_INET, SOCK_DGRAM, 0);
  return fd;
}

bool udp_sendto(sockfd_t fd, const char* host, uint16_t port, const void* data, size_t len) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr(host);
  if (addr.sin_addr.s_addr == INADDR_NONE) return false;
  long n = sendto(fd, (const char*)data, (int)len, 0, (sockaddr*)&addr, sizeof(addr));
  return n == (long)len;
}

long udp_recv(sockfd_t fd, void* buf, size_t cap) {
  sockaddr_in from{};
  socklen_t fl = sizeof(from);
  return recvfrom(fd, (char*)buf, (int)cap, 0, (sockaddr*)&from, &fl);
}

sockfd_t tcp_listen(uint16_t port, int backlog) {
  sockfd_t fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == MD_INVALID_SOCKET) return MD_INVALID_SOCKET;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { sock_close(fd); return MD_INVALID_SOCKET; }
  if (listen(fd, backlog) != 0) { sock_close(fd); return MD_INVALID_SOCKET; }
  set_recv_timeout(fd, 200);
  return fd;
}

sockfd_t tcp_accept(sockfd_t listenFd) {
  // Windows 的 accept() 不响应 SO_RCVTIMEO，会一直阻塞；用 select() 实现可中断的超时等待
  // （POSIX 同样适用），保证 stop 信号能及时让服务线程退出。
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(listenFd, &rfds);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 200000;  // 200ms
#ifdef _WIN32
  int r = select(0, &rfds, nullptr, nullptr, &tv);
#else
  int r = select(listenFd + 1, &rfds, nullptr, nullptr, &tv);
#endif
  if (r <= 0) return MD_INVALID_SOCKET;
  sockaddr_in from{};
  socklen_t fl = sizeof(from);
  sockfd_t c = accept(listenFd, (sockaddr*)&from, &fl);
  if (c != MD_INVALID_SOCKET) set_recv_timeout(c, 500);
  return c;
}

sockfd_t tcp_accept_nowait(sockfd_t listenFd) {
  // 仅在已由 IoMultiplexer 确认可读后调用，不会阻塞
  sockaddr_in from{};
  socklen_t fl = sizeof(from);
  sockfd_t c = accept(listenFd, (sockaddr*)&from, &fl);
  if (c != MD_INVALID_SOCKET) set_recv_timeout(c, 500);
  return c;
}

sockfd_t tcp_connect(const char* host, uint16_t port) {
  sockfd_t fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == MD_INVALID_SOCKET) return MD_INVALID_SOCKET;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr(host);
  if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { sock_close(fd); return MD_INVALID_SOCKET; }
  set_recv_timeout(fd, 500);
  return fd;
}

bool tcp_send(sockfd_t fd, const void* data, size_t len) {
  const char* p = (const char*)data;
  size_t sent = 0;
  while (sent < len) {
    long n = send(fd, p + sent, (int)(len - sent), 0);
    if (n <= 0) return false;
    sent += (size_t)n;
  }
  return true;
}

long tcp_recv(sockfd_t fd, void* buf, size_t cap) {
  return recv(fd, (char*)buf, (int)cap, 0);
}

}}  // namespace md::net
