// ============================================================================
// feed/femas/RealFemasMdApi.cpp —— 真实 飞马 行情 API 封装（仅 #ifdef HAS_FEMAS 编译）
//
// 注意：飞马官方类名/字段名各版本可能不同（如 CFemasMdApi / CSecurityFtdcMdApi、
// OnRtnMarketData 等）。下面按「MdApi + SPI」通用形态翻译，请对照你拿到的头文件微调。
// ============================================================================
#include "feed/femas/RealFemasMdApi.h"

#ifdef HAS_FEMAS

#include <cstring>

namespace md {

class RealFemasMdApi::Bridge : public CFemasMdSpi {
public:
    void setTarget(FemasMdSpi* t) { target_ = t; }

    void OnFrontConnected() override { if (target_) target_->OnFrontConnected(); }
    void OnFrontDisconnected(int nReason) override { if (target_) target_->OnFrontDisconnected(nReason); }
    void OnRspUserLogin(CFemasRspUserLoginField* pRsp, CFemasRspInfoField* pInfo,
                        int nRequestID, bool bIsLast) override {
        if (!target_) return;
        FemasRspUserLogin r{}; FemasRspInfo i{};
        if (pRsp) {
            std::strncpy(r.BrokerID, pRsp->BrokerID, sizeof(r.BrokerID) - 1);
            std::strncpy(r.UserID,   pRsp->UserID,   sizeof(r.UserID) - 1);
            r.FrontID   = pRsp->FrontID;
            r.SessionID = pRsp->SessionID;
        }
        if (pInfo) { i.ErrorID = pInfo->ErrorID; std::strncpy(i.ErrorMsg, pInfo->ErrorMsg, sizeof(i.ErrorMsg) - 1); }
        target_->OnRspUserLogin(r, i, nRequestID, bIsLast);
    }
    void OnRspSubMarketData(CFemasSpecificInstrumentField* pInst, CFemasRspInfoField* pInfo,
                            int nRequestID, bool bIsLast) override {
        if (!target_) return;
        FemasRspInfo i{};
        const char* sym = (pInst && pInst->InstrumentID[0]) ? pInst->InstrumentID : "";
        if (pInfo) { i.ErrorID = pInfo->ErrorID; std::strncpy(i.ErrorMsg, pInfo->ErrorMsg, sizeof(i.ErrorMsg) - 1); }
        target_->OnRspSubMarketData(sym, i, nRequestID, bIsLast);
    }
    // 飞马行情回调常见名为 OnRtnMarketData，这里转发到统一的 OnRtnDepthMarketData
    void OnRtnMarketData(CFemasDepthMarketDataField* pDepth) override {
        if (!target_ || !pDepth) return;
        FemasDepthMarketData md{};
        std::strncpy(md.InstrumentID, pDepth->InstrumentID, sizeof(md.InstrumentID) - 1);
        md.LastPrice  = pDepth->LastPrice;
        md.Volume     = pDepth->Volume;
        md.BidPrice1  = pDepth->BidPrice1;
        md.AskPrice1  = pDepth->AskPrice1;
        md.BidVolume1 = pDepth->BidVolume1;
        md.AskVolume1 = pDepth->AskVolume1;
        std::strncpy(md.TradingDay, pDepth->TradingDay, sizeof(md.TradingDay) - 1);
        std::strncpy(md.UpdateTime, pDepth->UpdateTime, sizeof(md.UpdateTime) - 1);
        md.UpdateMillisec = pDepth->UpdateMillisec;
        target_->OnRtnDepthMarketData(md);
    }

private:
    FemasMdSpi* target_ = nullptr;
};

RealFemasMdApi::RealFemasMdApi()
    : raw_(CFemasMdApi::CreateFemasMdApi()), bridge_(new Bridge()) {
    raw_->RegisterSpi(bridge_);
}

RealFemasMdApi::~RealFemasMdApi() {
    delete bridge_;
    if (raw_) raw_->Release();
}

FemasMdApi* RealFemasMdApi::Create() { return new RealFemasMdApi(); }

void RealFemasMdApi::RegisterSpi(FemasMdSpi* spi) { bridge_->setTarget(spi); }
void RealFemasMdApi::RegisterFront(const char* frontAddr) { raw_->RegisterFront(const_cast<char*>(frontAddr)); }
void RealFemasMdApi::Init()  { raw_->Init(); }
void RealFemasMdApi::Join()  { raw_->Join(); }
void RealFemasMdApi::Release() { raw_->Release(); }

int RealFemasMdApi::SubscribeMarketData(const char* insts[], int nCount) {
    return raw_->SubscribeMarketData(const_cast<char**>(insts), nCount);
}

int RealFemasMdApi::ReqUserLogin(const char* brokerId, const char* userId,
                                 const char* password, int nRequestID) {
    CFemasReqUserLoginField f{};
    std::strncpy(f.BrokerID, brokerId, sizeof(f.BrokerID) - 1);
    std::strncpy(f.UserID,   userId,   sizeof(f.UserID) - 1);
    std::strncpy(f.Password, password, sizeof(f.Password) - 1);
    return raw_->ReqUserLogin(&f, nRequestID);
}

}  // namespace md
#endif  // HAS_FEMAS
