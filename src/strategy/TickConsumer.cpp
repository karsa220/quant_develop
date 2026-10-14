#include "strategy/TickConsumer.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace md {

static uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool TickConsumer::init(const char* shmName) {
    size_t bytes = 0;
    if (!shm_.open(shmName, &bytes)) return false;
    // capacity 由 ring 控制头读出，这里先尝试一个上限值再让 attach 校验
    const uint64_t cap = 1ULL << 30;   // attach 内部会按 ring_->capacity 校验
    if (!ring_.attach(shm_.data(), bytes, cap, /*init*/ false)) return false;
    return true;
}

void TickConsumer::run(uint64_t maxTicks, LatencyRecorder* latencies) {
    uint32_t len = 0;
    uint64_t tickProcessed = 0;
    while (running_.load() && tickProcessed < maxTicks) {
        if (!ring_.tryConsume(msgBuf_, &len)) {
            // 空：极短自旋（低延迟路径常驻），避免线程被 OS 切换
            // 生产者已结束且环已排空 → 优雅退出（不依赖 tick 数达标）
            if (done_ && done_->load(std::memory_order_acquire) && ring_.sizeApprox() == 0) {
                running_ = false;
                break;
            }
            std::this_thread::yield();
            continue;
        }
        const MsgHeader* h = reinterpret_cast<const MsgHeader*>(msgBuf_);
        if (h->magic != kMagic) { ++bad_; continue; }

        if (h->type == MSG_TICK) {
            const TickPayload* t = reinterpret_cast<const TickPayload*>(msgBuf_ + sizeof(MsgHeader));

            // ---- 风控：行情进入策略前的合规校验（价格/时效/持仓）----
            if (risk_) {
                RiskDecision d = risk_->checkTick(*t, nowNs(), h->srcTs);
                if (d == RISK_HALT) { running_ = false; break; }
                if (d == RISK_REJECT) { ++bad_; continue; }
            }
            if (mon_) mon_->heartbeat(h->srcTs);

            // 重建简化盘口：买成交更新 bid，卖成交更新 ask
            if (t->inst < FuturesFeed::kNumInstruments) {
                if (t->side == kSideBuy)  lastBid_[t->inst] = t->price;
                else                      lastAsk_[t->inst] = t->price;
                // 反转信号：价格相对上一笔反向运动（微结构常用的逐笔反转因子）
                const PriceTicks p = t->price;
                const int8_t dir = (p > lastPx_[t->inst]) ? 1 : (p < lastPx_[t->inst] ? -1 : 0);
                if (dir != 0 && prevDir_[t->inst] != 0 && dir != prevDir_[t->inst]) {
                    // 方向反转 → 产生交易信号（拟下单）
                    if (risk_) {
                        RiskDecision od = risk_->checkOrder(t->inst, dir, /*qty*/t->qty, nowNs());
                        if (od == RISK_HALT) { running_ = false; break; }
                        if (od == RISK_REJECT) { if (mon_) mon_->recordRejected(); }
                        else { ++signals_; if (mon_) mon_->recordSignal(); }
                    } else {
                        ++signals_; if (mon_) mon_->recordSignal();
                    }
                }
                if (dir != 0) prevDir_[t->inst] = dir;
                lastPx_[t->inst] = p;
            }
            // 端到端延迟：行情源时间戳 → 此刻（抽样记录以控制内存）
            const uint64_t lat = nowNs() - h->srcTs;
            if (mon_ && ((tickProcessed & 63) == 0)) mon_->recordLatency(lat);
            if (latencies && ((tickProcessed & 63) == 0)) latencies->record(lat);
            if (mon_) mon_->recordConsumed();
            ++consumed_;
            ++ticks_;
            ++tickProcessed;
        } else if (h->type == MSG_HEARTBEAT) {
            const HeartbeatPayload* hb =
                reinterpret_cast<const HeartbeatPayload*>(msgBuf_ + sizeof(MsgHeader));
            // 心跳：只读统计，可在此触发快照落盘等定时任务
            (void)hb;
        } else if (h->type == MSG_CONTROL) {
            const ControlPayload* c =
                reinterpret_cast<const ControlPayload*>(msgBuf_ + sizeof(MsgHeader));
            std::printf("[strategy] <<< SESSION %s\n", c->state ? "OPEN" : "CLOSE");
        }
        // 注意：consumed_ 统计含所有消息类型；tick 计数用 tickProcessed
    }
    running_ = false;
}

}  // namespace md
