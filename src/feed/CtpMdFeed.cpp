// ============================================================================
// feed/CtpMdFeed.cpp —— CTP 行情源实现
// ============================================================================
#include "feed/CtpMdFeed.h"
#include "feed/ctp/SimCtpMdApi.h"
#include "feed/FuturesFeed.h"

#include <cstdio>
#include <vector>

#ifdef HAS_CTP
#include "feed/ctp/RealCtpMdApi.h"
#endif

namespace md {

bool CtpMdFeed::start(const FeedConfig& cfg) {
    cfg_ = cfg;
    sim_ = cfg.simulation;
#ifndef HAS_CTP
    if (!sim_) {
        std::fprintf(stderr, "[CtpMdFeed] HAS_CTP 未定义，回退到仿真后端\n");
        sim_ = true;
    }
#endif

    if (sim_) {
        api_ = new SimCtpMdApi();
    } else {
#ifdef HAS_CTP
        api_ = RealCtpMdApi::Create();
#else
        api_ = new SimCtpMdApi();
        sim_ = true;
#endif
    }

    api_->RegisterSpi(this);
    for (const auto& a : cfg.frontAddresses) api_->RegisterFront(a.c_str());
    api_->Init();
    running_ = true;
    return true;
}

void CtpMdFeed::stop() {
    if (api_) {
        api_->Release();
        api_->Join();
        delete api_;
        api_ = nullptr;
    }
    running_ = false;
}

void CtpMdFeed::OnFrontConnected() {
    std::printf("[CtpMdFeed] front connected (backend=%s)\n", backend());
}

void CtpMdFeed::OnFrontDisconnected(int nReason) {
    std::fprintf(stderr, "[CtpMdFeed] front disconnected, reason=%d\n", nReason);
}

void CtpMdFeed::OnRspUserLogin(const CtpRspUserLogin& /*rsp*/, const CtpRspInfo& info,
                               int /*nRequestID*/, bool /*bIsLast*/) {
    if (info.ErrorID != 0) {
        std::fprintf(stderr, "[CtpMdFeed] login failed: [%d] %s\n", info.ErrorID, info.ErrorMsg);
        return;
    }
    std::printf("[CtpMdFeed] login ok, subscribing %d instrument(s)\n",
                (int)cfg_.instruments.size());
    subscribed_ = cfg_.instruments;
    std::vector<const char*> ptrs;
    ptrs.reserve(subscribed_.size());
    for (const auto& s : subscribed_) ptrs.push_back(s.c_str());
    if (!ptrs.empty()) api_->SubscribeMarketData(ptrs.data(), (int)ptrs.size());
}

void CtpMdFeed::OnRspSubMarketData(const char* instrument, const CtpRspInfo& info,
                                  int /*nRequestID*/, bool /*bIsLast*/) {
    if (info.ErrorID != 0)
        std::fprintf(stderr, "[CtpMdFeed] sub %s failed: [%d] %s\n",
                     instrument ? instrument : "", info.ErrorID, info.ErrorMsg);
}

void CtpMdFeed::OnRtnDepthMarketData(const CtpDepthMarketData& md) {
    MarketTick t{};
    Nanos srcTs = parseExchangeTimeNs(md.TradingDay, md.UpdateTime, md.UpdateMillisec);
    t.inst  = symbolToInst(md.InstrumentID);
    if (t.inst >= FuturesFeed::kNumInstruments) return;   // 未知合约，丢弃
    t.price = priceToTicks(md.LastPrice, md.InstrumentID);
    t.qty   = static_cast<Qty>(md.Volume);
    t.side  = aggressorSide(md.LastPrice, md.BidPrice1, md.AskPrice1);
    emit(t, srcTs);
}

}  // namespace md
