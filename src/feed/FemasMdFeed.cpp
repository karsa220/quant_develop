// ============================================================================
// feed/FemasMdFeed.cpp —— 飞马 行情源实现
// ============================================================================
#include "feed/FemasMdFeed.h"
#include "feed/femas/SimFemasMdApi.h"
#include "feed/FuturesFeed.h"

#include <cstdio>
#include <vector>

#ifdef HAS_FEMAS
#include "feed/femas/RealFemasMdApi.h"
#endif

namespace md {

bool FemasMdFeed::start(const FeedConfig& cfg) {
    cfg_ = cfg;
    sim_ = cfg.simulation;
#ifndef HAS_FEMAS
    if (!sim_) {
        std::fprintf(stderr, "[FemasMdFeed] HAS_FEMAS 未定义，回退到仿真后端\n");
        sim_ = true;
    }
#endif

    if (sim_) {
        api_ = new SimFemasMdApi();
    } else {
#ifdef HAS_FEMAS
        api_ = RealFemasMdApi::Create();
#else
        api_ = new SimFemasMdApi();
        sim_ = true;
#endif
    }

    api_->RegisterSpi(this);
    for (const auto& a : cfg.frontAddresses) api_->RegisterFront(a.c_str());
    api_->Init();
    running_ = true;
    return true;
}

void FemasMdFeed::stop() {
    if (api_) {
        api_->Release();
        api_->Join();
        delete api_;
        api_ = nullptr;
    }
    running_ = false;
}

void FemasMdFeed::OnFrontConnected() {
    std::printf("[FemasMdFeed] front connected (backend=%s)\n", backend());
}

void FemasMdFeed::OnFrontDisconnected(int nReason) {
    std::fprintf(stderr, "[FemasMdFeed] front disconnected, reason=%d\n", nReason);
}

void FemasMdFeed::OnRspUserLogin(const FemasRspUserLogin& /*rsp*/, const FemasRspInfo& info,
                                 int /*nRequestID*/, bool /*bIsLast*/) {
    if (info.ErrorID != 0) {
        std::fprintf(stderr, "[FemasMdFeed] login failed: [%d] %s\n", info.ErrorID, info.ErrorMsg);
        return;
    }
    std::printf("[FemasMdFeed] login ok, subscribing %d instrument(s)\n",
                (int)cfg_.instruments.size());
    subscribed_ = cfg_.instruments;
    std::vector<const char*> ptrs;
    ptrs.reserve(subscribed_.size());
    for (const auto& s : subscribed_) ptrs.push_back(s.c_str());
    if (!ptrs.empty()) api_->SubscribeMarketData(ptrs.data(), (int)ptrs.size());
}

void FemasMdFeed::OnRspSubMarketData(const char* instrument, const FemasRspInfo& info,
                                     int /*nRequestID*/, bool /*bIsLast*/) {
    if (info.ErrorID != 0)
        std::fprintf(stderr, "[FemasMdFeed] sub %s failed: [%d] %s\n",
                     instrument ? instrument : "", info.ErrorID, info.ErrorMsg);
}

void FemasMdFeed::OnRtnDepthMarketData(const FemasDepthMarketData& md) {
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
