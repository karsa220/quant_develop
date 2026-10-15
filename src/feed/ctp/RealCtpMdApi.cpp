// ============================================================================
// feed/ctp/RealCtpMdApi.cpp —— 真实 CTP 行情 API 封装（仅 #ifdef HAS_CTP 编译）
// ============================================================================
#include "feed/ctp/RealCtpMdApi.h"

#ifdef HAS_CTP

#include <cstring>

namespace md {

// 把上期 SPI 回调「翻译」成项目内 CtpMdSpi 契约
class RealCtpMdApi::Bridge : public CThostFtdcMdSpi {
public:
    void setTarget(CtpMdSpi* t) { target_ = t; }

    void OnFrontConnected() override {
        if (target_) target_->OnFrontConnected();
    }
    void OnFrontDisconnected(int nReason) override {
        if (target_) target_->OnFrontDisconnected(nReason);
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRsp,
                        CThostFtdcRspInfoField* pInfo, int nRequestID, bool bIsLast) override {
        if (!target_) return;
        CtpRspUserLogin r{};
        CtpRspInfo      i{};
        if (pRsp) {
            std::strncpy(r.BrokerID, pRsp->BrokerID, sizeof(r.BrokerID) - 1);
            std::strncpy(r.UserID,   pRsp->UserID,   sizeof(r.UserID) - 1);
            r.FrontID    = pRsp->FrontID;
            r.SessionID  = pRsp->SessionID;
        }
        if (pInfo) {
            i.ErrorID = pInfo->ErrorID;
            std::strncpy(i.ErrorMsg, pInfo->ErrorMsg, sizeof(i.ErrorMsg) - 1);
        }
        target_->OnRspUserLogin(r, i, nRequestID, bIsLast);
    }
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pInst,
                            CThostFtdcRspInfoField* pInfo, int nRequestID, bool bIsLast) override {
        if (!target_) return;
        CtpRspInfo i{};
        const char* sym = (pInst && pInst->InstrumentID[0]) ? pInst->InstrumentID : "";
        if (pInfo) {
            i.ErrorID = pInfo->ErrorID;
            std::strncpy(i.ErrorMsg, pInfo->ErrorMsg, sizeof(i.ErrorMsg) - 1);
        }
        target_->OnRspSubMarketData(sym, i, nRequestID, bIsLast);
    }
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepth) override {
        if (!target_ || !pDepth) return;
        CtpDepthMarketData md{};
        std::strncpy(md.InstrumentID, pDepth->InstrumentID, sizeof(md.InstrumentID) - 1);
        md.LastPrice   = pDepth->LastPrice;
        md.Volume      = pDepth->Volume;
        md.BidPrice1   = pDepth->BidPrice1;
        md.AskPrice1   = pDepth->AskPrice1;
        md.BidVolume1  = pDepth->BidVolume1;
        md.AskVolume1  = pDepth->AskVolume1;
        std::strncpy(md.TradingDay,  pDepth->TradingDay,  sizeof(md.TradingDay) - 1);
        std::strncpy(md.UpdateTime,  pDepth->UpdateTime,  sizeof(md.UpdateTime) - 1);
        md.UpdateMillisec = pDepth->UpdateMillisec;
        target_->OnRtnDepthMarketData(md);
    }

private:
    CtpMdSpi* target_ = nullptr;
};

RealCtpMdApi::RealCtpMdApi()
    : raw_(CThostFtdcMdApi::CreateFtdcMdApi()), bridge_(new Bridge()) {
    raw_->RegisterSpi(bridge_);
}

RealCtpMdApi::~RealCtpMdApi() {
    // 注意：CTP 要求先 Release 再 delete；本类析构前调用方应已 Release。
    delete bridge_;
    if (raw_) raw_->Release();
}

CtpMdApi* RealCtpMdApi::Create() { return new RealCtpMdApi(); }

void RealCtpMdApi::RegisterSpi(CtpMdSpi* spi) { bridge_->setTarget(spi); }

void RealCtpMdApi::RegisterFront(const char* frontAddr) {
    raw_->RegisterFront(const_cast<char*>(frontAddr));
}

void RealCtpMdApi::Init()  { raw_->Init(); }
void RealCtpMdApi::Join()  { raw_->Join(); }
void RealCtpMdApi::Release() { raw_->Release(); }

int RealCtpMdApi::SubscribeMarketData(const char* insts[], int nCount) {
    return raw_->SubscribeMarketData(const_cast<char**>(insts), nCount);
}

int RealCtpMdApi::ReqUserLogin(const char* brokerId, const char* userId,
                               const char* password, int nRequestID) {
    CThostFtdcReqUserLoginField f{};
    std::strncpy(f.BrokerID, brokerId, sizeof(f.BrokerID) - 1);
    std::strncpy(f.UserID,   userId,   sizeof(f.UserID) - 1);
    std::strncpy(f.Password, password, sizeof(f.Password) - 1);
    return raw_->ReqUserLogin(&f, nRequestID);
}

}  // namespace md
#endif  // HAS_CTP
