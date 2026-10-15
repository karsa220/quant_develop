#pragma once
// ============================================================================
// coro/Coroutine.h —— C++20 无栈协程基础设施（JD 加分项：协程）
//
// 提供两类生产级可等待对象：
//   1) Task<T>  —— 可组合异步任务（支持 co_await 嵌套、返回值），
//                  用于把「一次连接 / 一次请求」建模为一个协程。
//   2) Generator<T> —— 惰性序列（co_yield 产出元素），用于演示 co_yield。
//
// 与本项目的关系：
//   - 协程让单线程事件循环能以「同步写法」表达异步逻辑，避免回调地狱；
//   - 配合 net/IoMultiplexer（epoll/WSAPoll）即可实现单线程万级并发连接，
//     相比「每连接一线程」显著降低上下文切换与缓存抖动，契合低延迟目标。
//
// 注意：本文件使用 C++20 协程关键字（co_await/co_yield/co_return），
// 编译单元须以 -std=c++20（GCC/Clang）或 /std:c++20（MSVC）编译。
// ============================================================================
#include <coroutine>
#include <cstddef>
#include <utility>

namespace md { namespace coro {

// ---------------------------------------------------------------------------
// Task<T>：最基本的异步任务。co_await 一个 Task 会挂起当前协程，
// 待其内部协程运行完毕后恢复，并取回返回值。
// ---------------------------------------------------------------------------
template <typename T>
struct Task {
    struct promise_type {
        T value_{};
        std::coroutine_handle<> continuation_{};   // 被 co_await 时的调用方续体

        Task get_return_object() noexcept {
            return Task{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept {
            struct F {
                promise_type* p;
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type>) noexcept {
                    if (p->continuation_) p->continuation_.resume();
                }
                void await_resume() noexcept {}
            };
            return F{ this };
        }
        void return_value(T v) { value_ = std::move(v); }
        void unhandled_exception() { throw; }
    };

    // 让 Task 自身成为可等待对象（支持 co_await 嵌套）
    struct Awaiter {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> cont) noexcept {
            h.promise().continuation_ = cont;
            h.resume();   // 同步驱动被等待的协程（本演示中不含 I/O 挂起）
        }
        T await_resume() noexcept { return std::move(h.promise().value_); }
    };

    using handle_t = std::coroutine_handle<promise_type>;
    handle_t h_{nullptr};

    explicit Task(handle_t h) noexcept : h_(h) {}
    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { if (h_) h_.destroy(); }

    Awaiter operator co_await() noexcept { return Awaiter{ h_ }; }

    handle_t handle() const noexcept { return h_; }
    bool     done() const noexcept { return h_ && h_.done(); }
    T        result() { return std::move(h_.promise().value_); }
};

// Task<void> 特化：协程体用 co_return;（无返回值）
template <>
struct Task<void> {
    struct promise_type {
        std::coroutine_handle<> continuation_{};
        Task get_return_object() noexcept {
            return Task{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept {
            struct F {
                promise_type* p;
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type>) noexcept {
                    if (p->continuation_) p->continuation_.resume();
                }
                void await_resume() noexcept {}
            };
            return F{ this };
        }
        void return_void() {}
        void unhandled_exception() { throw; }
    };

    struct Awaiter {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> cont) noexcept {
            h.promise().continuation_ = cont;
            h.resume();
        }
        void await_resume() noexcept {}
    };

    using handle_t = std::coroutine_handle<promise_type>;
    handle_t h_{nullptr};

    explicit Task(handle_t h) noexcept : h_(h) {}
    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { if (h_) h_.destroy(); }

    Awaiter operator co_await() noexcept { return Awaiter{ h_ }; }

    handle_t handle() const noexcept { return h_; }
    bool     done() const noexcept { return h_ && h_.done(); }
};

// ---------------------------------------------------------------------------
// Generator<T>：惰性序列。用 co_yield 产出元素，next() 推进、value() 取值。
// ---------------------------------------------------------------------------
template <typename T>
struct Generator {
    struct promise_type {
        T current_{};
        Generator get_return_object() noexcept {
            return Generator{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_always initial_suspend() noexcept { return {}; }   // 首次 next() 才产出第一个
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T v) noexcept { current_ = std::move(v); return {}; }
        void return_void() {}
        void unhandled_exception() { throw; }
        T& await_resume() noexcept { return current_; }
    };

    using handle_t = std::coroutine_handle<promise_type>;
    handle_t h_{nullptr};

    explicit Generator(handle_t h) noexcept : h_(h) {}
    Generator(Generator&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    ~Generator() { if (h_) h_.destroy(); }

    bool next() {
        if (!h_ || h_.done()) return false;
        h_.resume();
        return !h_.done();
    }
    T value() { return std::move(h_.promise().current_); }
    bool valid() const noexcept { return h_ && !h_.done(); }
};

}}  // namespace md::coro
