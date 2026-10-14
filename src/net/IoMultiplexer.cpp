// ============================================================================
// net/IoMultiplexer.cpp —— epoll( Linux ) / WSAPoll( Windows ) 实现
// ============================================================================
#include "net/IoMultiplexer.h"

#ifdef __linux__
#  include <sys/epoll.h>
#  include <algorithm>
#endif

namespace md { namespace net {

IoMultiplexer::IoMultiplexer() = default;
IoMultiplexer::~IoMultiplexer() {
#ifdef __linux__
    if (epfd_ != -1) { close(epfd_); epfd_ = -1; }
#endif
}

bool IoMultiplexer::create() {
#ifdef __linux__
    epfd_ = epoll_create1(0);
    return epfd_ != -1;
#else
    fds_.clear();
    return true;   // WSAPoll 无需预先创建实例
#endif
}

void IoMultiplexer::add(mux_fd_t fd, bool read, bool write) {
#ifdef __linux__
    if (epfd_ == -1) return;
    struct epoll_event ev{};
    ev.events = (read ? EPOLLIN : 0) | (write ? EPOLLOUT : 0);
    ev.data.fd = fd;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, (int)fd, &ev);
#else
    pollfd p{};
    p.fd = fd;
    p.events = (read ? POLLIN : 0) | (write ? POLLOUT : 0);
    fds_.push_back(p);
#endif
}

void IoMultiplexer::remove(mux_fd_t fd) {
#ifdef __linux__
    if (epfd_ != -1) epoll_ctl(epfd_, EPOLL_CTL_DEL, (int)fd, nullptr);
#else
    for (size_t i = 0; i < fds_.size(); ++i) {
        if (fds_[i].fd == fd) { fds_.erase(fds_.begin() + i); break; }
    }
#endif
}

int IoMultiplexer::wait(int timeoutMs, std::vector<mux_fd_t>& readyRead,
                        std::vector<mux_fd_t>& readyWrite) {
    readyRead.clear(); readyWrite.clear();
#ifdef __linux__
    if (epfd_ == -1) return -1;
    evs_.resize(64);
    int n = epoll_wait(epfd_, evs_.data(), (int)evs_.size(), timeoutMs);
    if (n <= 0) return n;
    for (int i = 0; i < n; ++i) {
        if (evs_[i].events & EPOLLIN)  readyRead.push_back(evs_[i].data.fd);
        if (evs_[i].events & EPOLLOUT) readyWrite.push_back(evs_[i].data.fd);
    }
    return n;
#else
    if (fds_.empty()) return 0;
    int n = WSAPoll(fds_.data(), (ULONG)fds_.size(), timeoutMs);
    if (n <= 0) return n;
    for (auto& p : fds_) {
        if (p.revents & POLLIN)  readyRead.push_back(p.fd);
        if (p.revents & POLLOUT) readyWrite.push_back(p.fd);
    }
    return n;
#endif
}

const char* IoMultiplexer::backend() const {
#ifdef __linux__
    return "epoll";
#else
    return "poll";
#endif
}

}}  // namespace md::net
