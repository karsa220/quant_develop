#pragma once
// ============================================================================
// coro/EventLoop.h —— 基于 I/O 多路复用的单线程协程事件循环
//
// 把 net/IoMultiplexer（Linux epoll / Windows WSAPoll）改造成「协程就绪驱动器」：
//   协程执行到 co_await loop.read(fd) / loop.write(fd) 时挂起，并把 fd 注册到
//   多路复用器；事件循环在 fd 就绪后 resume 该协程。于是「每连接一协程」、
//   「同步写法表达异步」在单线程内完成，无锁、无上下文切换、缓存友好。
//
// 这是 JD 中「协程 + epoll + 网络 IO 降延迟 + 高并发」的落地点。
// ============================================================================
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <coroutine>
#include "coro/Coroutine.h"
#include "net/IoMultiplexer.h"
#include "net/Socket.h"

namespace md { namespace coro {

struct EventLoop {
    // co_await 的可等待对象：挂起当前协程，等待 fd 可读/可写
    struct IoAwaiter {
        EventLoop*         loop;
        net::sockfd_t      fd;
        bool               wantWrite;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            loop->registerWait(fd, h, wantWrite);
        }
        void await_resume() noexcept {}
    };

    EventLoop() = default;
    ~EventLoop() { stop(); }

    // 启动一个顶层协程（resume 使其从 initial_suspend 处开始执行）
    void spawn(Task<void>&& t) {
        auto h = t.handle();
        t.h_ = nullptr;                 // 转移所有权，避免 Task 析构重复 destroy
        spawned_.push_back(h);
        h.resume();
    }

    // 协程内使用：等待 fd 可读 / 可写（挂起，由事件循环 resume）
    IoAwaiter read (net::sockfd_t fd) { return IoAwaiter{this, fd, false}; }
    IoAwaiter write(net::sockfd_t fd) { return IoAwaiter{this, fd, true}; }

    // 运行事件循环，直到 stop()
    void run() {
        std::vector<net::mux_fd_t> rr, rw;
        while (!stop_) {
            int n = mux_.wait(100, rr, rw);
            if (n < 0) continue;
            // 先快照就绪集合，再逐个 resume（resume 可能改写 waiters_）
            std::vector<int> ready;
            for (auto& kv : waiters_) {
                int fd = kv.first;
                bool isRead  = !kv.second.wantWrite
                               && std::find(rr.begin(), rr.end(), (net::mux_fd_t)fd) != rr.end();
                bool isWrite =  kv.second.wantWrite
                               && std::find(rw.begin(), rw.end(), (net::mux_fd_t)fd) != rw.end();
                if (isRead || isWrite) ready.push_back(fd);
            }
            for (int fd : ready) {
                auto it = waiters_.find(fd);
                if (it == waiters_.end()) continue;
                auto h = it->second.h;
                waiters_.erase(it);      // 挂起前移除，防止重入重复 resume
                h.resume();
            }
        }
    }

    // 仅置停止标志（供协程内部调用，不析构任何协程，避免从自身帧内 destroy）
    void requestStop() { stop_ = true; }

    void stop() {
        stop_ = true;
        for (auto& h : spawned_) if (h) h.destroy();
        spawned_.clear();
        waiters_.clear();
        registered_.clear();
    }

    const char* backend() const { return mux_.backend(); }

    // 连接关闭时注销 fd（由上层协程在收尾时调用）
    void unregister(net::sockfd_t fd) {
        int k = (int)fd;
        mux_.remove(fd);
        registered_.erase(k);
        waiters_.erase(k);
    }

private:
    friend struct IoAwaiter;

    struct Waiter { std::coroutine_handle<> h; bool wantWrite; };

    void registerWait(net::sockfd_t fd, std::coroutine_handle<> h, bool wantWrite) {
        int k = (int)fd;
        if (!registered_.count(k)) {
            mux_.add(fd, true, true);    // 读+写都监听，按 wantWrite 区分
            registered_.insert(k);
        }
        waiters_[k] = Waiter{h, wantWrite};
    }

    net::IoMultiplexer mux_;
    std::unordered_map<int, Waiter>        waiters_;    // fd -> 等待中的协程
    std::unordered_set<int>                registered_; // 已加入 mux 的 fd
    std::vector<std::coroutine_handle<>>   spawned_;    // 顶层协程（stop 时统一销毁）
    bool stop_ = false;
};

}}  // namespace md::coro
