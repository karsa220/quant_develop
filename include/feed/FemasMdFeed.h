#pragma once
// ============================================================================
// feed/FemasMdFeed.h —— 飞马(Femas) 行情源（实现统一 MarketFeed 接口）
// ============================================================================
#include "feed/MarketFeed.h"
#include "feed/femas/FemasMdTypes.h"

#include <string>
#include <vector>

namespace md {

class FemasMdFeed : public MarketFeed, public FemasMdSpi {
public:
    const char* backend() const override { return sim_ ? "femas-sim" : "femas"; }

    bool start(const FeedConfig& cfg) override;
    void stop() override;

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(const FemasRspUserLogin& rsp, const FemasRspInfo& info,
                        int nRequestID, bool bIsLast) override;
    void OnRspSubMarketData(const char* instrument, const FemasRspInfo& info,
                            int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(const FemasDepthMarketData& md) override;

private:
    FemasMdApi*   api_ = nullptr;
    bool          sim_ = true;
    FeedConfig     cfg_;
    std::vector<std::string> subscribed_;
};

}  // namespace md
