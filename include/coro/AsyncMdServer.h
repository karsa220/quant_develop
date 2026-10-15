#pragma once
// ============================================================================
// coro/AsyncMdServer.h —— 协程驱动的异步行情接入服务（JD 加分项：协程 + epoll）
//
// 与既有 MdServer（每连接一线程 / reactor + 阻塞 accept）不同，这里用「每连接
// 一协程」的单线程模型：accept 与每个连接的处理都是协程，全部由 EventLoop 在
// 单线程内调度。协程在 co_await socket 就绪时挂起、就绪时恢复，逻辑是同步写法，
// 却没有线程切换与锁开销——这正是低延迟网络服务的常见范式。
//
// 数据流：TCP 收到 CTP 风格 tick 帧 → 解析 → 风控校验(checkTick) → 回 ack；
// 通过的 tick 通过 OnTick 回调交给上层（演示里接延迟统计 + 对象池 + 无锁环）。
// ============================================================================
#include <atomic>
#include <cstdint>
#include <functional>
#include "common/Types.h"
#include "coro/Coroutine.h"
#include "coro/EventLoop.h"
#include "net/Socket.h"
#include "risk/RiskControl.h"
#include "feed/CtpLikeCodec.h"

namespace md { namespace coro {

// 单字节 ack 帧（演示用，承载风控结论与序号）
#pragma pack(push, 1)
struct AckFrame {
    uint16_t magic;     // kMagic
    uint8_t  type;      // 0xAB = ack
    uint8_t  status;    // RiskDecision
    uint32_t seq;
};
#pragma pack(pop)
static constexpr uint8_t kAckType = 0xAB;
static constexpr size_t  kAckSize = sizeof(AckFrame);

class AsyncMdServer {
public:
    // 每处理一笔 tick 回调给上层（d=风控结论，t=行情，recvTs=收到时刻）
    using OnTick = std::function<void(RiskDecision d, const MarketTick& t, Nanos recvTs)>;

    AsyncMdServer(RiskControl* risk, OnTick cb = nullptr)
        : risk_(risk), cb_(std::move(cb)) {}

    // 监听端口并启动 accept 协程（不阻塞）
    bool start(uint16_t port);

    // 运行事件循环（阻塞，直到 stop）
    void run() { loop_.run(); }
    // 完整停止并回收协程（通常在 run() 返回后由析构触发）
    void stop() { stop_ = true; loop_.stop(); }
    // 仅发停止信号（可安全地从协程内部调用，让 run() 自然退出）
    void requestStop() { stop_ = true; loop_.requestStop(); }

    // 暴露底层事件循环，便于上层把客户端协程也放进同一循环（单线程并发）
    EventLoop& loop() { return loop_; }

    uint64_t accepted()  const { return accepted_.load(); }
    uint64_t ticks()     const { return ticks_.load(); }
    uint64_t rejected()  const { return rejected_.load(); }
    const char* backend() const { return loop_.backend(); }

private:
    Task<void> acceptLoop();
    Task<void> handleClient(net::sockfd_t fd);

    EventLoop     loop_;
    RiskControl*  risk_;
    OnTick        cb_;
    net::sockfd_t listenFd_ = MD_INVALID_SOCKET;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> accepted_{0}, ticks_{0}, rejected_{0};
};

}}  // namespace md::coro
