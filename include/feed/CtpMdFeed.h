#pragma once
// ============================================================================
// feed/CtpMdFeed.h —— CTP 行情源（实现统一 MarketFeed 接口）
//
// 把 CTP MdApi 的 push 回调（OnRtnDepthMarketData）转换为项目内部 MarketTick，
// 经 OnMarketData 上送。背后是 SimCtpMdApi（仿真）或 RealCtpMdApi（真机），
// 由 FeedConfig.simulation / HAS_CTP 决定，上层无感。
// ============================================================================
#include "feed/MarketFeed.h"
#include "feed/ctp/CtpMdTypes.h"

#include <string>
#include <vector>

namespace md {

class CtpMdFeed : public MarketFeed, public CtpMdSpi {
public:
    const char* backend() const override { return sim_ ? "ctp-sim" : "ctp"; }

    bool start(const FeedConfig& cfg) override;
    void stop() override;

    // ---- CtpMdSpi（柜台回调）----
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(const CtpRspUserLogin& rsp, const CtpRspInfo& info,
                        int nRequestID, bool bIsLast) override;
    void OnRspSubMarketData(const char* instrument, const CtpRspInfo& info,
                            int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(const CtpDepthMarketData& md) override;

private:
    CtpMdApi*      api_ = nullptr;
    bool           sim_ = true;
    FeedConfig      cfg_;
    std::vector<std::string> subscribed_;
};

}  // namespace md
