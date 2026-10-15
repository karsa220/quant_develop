#pragma once
// ============================================================================
// feed/ctp/CtpMdTypes.h —— 镜像 CTP 行情 API（CThostFtdcMdApi / CThostFtdcMdSpi）
//
// 这是对上期技术 CTP 行情接口「契约」的镜像（仅摘取行情所需字段与方法），
// 目的是让仿真后端与真实封装实现同一套接口，上层 CtpMdFeed 不必关心背后是哪边。
//
// 真实封装（RealCtpMdApi.cpp，#ifdef HAS_CTP）会把这份结构「翻译」成真正的
// CThostFtdc* 类型——字段名/类型与官方头文件一致（请以后以官方 ThostFtdcMdApi.h
// / ThostFtdcUserApiStruct.h 为准，下面注释标了官方原名）。
// ============================================================================
#include <cstdint>
#include <cstddef>

namespace md {

// ---- 行情深度数据（官方：CThostFtdcDepthMarketDataField，部分字段）----
#pragma pack(push, 1)
struct CtpDepthMarketData {
    char    InstrumentID[31];   // 合约代码
    double  LastPrice;          // 最新价（元，double）       官方: LastPrice
    int32_t Volume;             // 成交量（累计，手）         官方: Volume
    double  BidPrice1;          // 买一价                    官方: BidPrice1
    double  AskPrice1;          // 卖一价                    官方: AskPrice1
    int32_t BidVolume1;         // 买一量                    官方: BidVolume1
    int32_t AskVolume1;         // 卖一量                    官方: AskVolume1
    char    TradingDay[9];      // 交易日 "YYYYMMDD"         官方: TradingDay
    char    UpdateTime[9];      // 时间 "HH:MM:SS"           官方: UpdateTime
    int32_t UpdateMillisec;     // 毫秒                      官方: UpdateMillisec
};
#pragma pack(pop)

// ---- 登录应答（官方：CThostFtdcRspUserLoginField，部分字段）----
#pragma pack(push, 1)
struct CtpRspUserLogin {
    char    TradingDay[9];
    char    LoginTime[9];
    char    BrokerID[11];
    char    UserID[16];
    int32_t FrontID;
    int32_t SessionID;
    char    MaxOrderRef[13];
};
#pragma pack(pop)

// ---- 错误应答（官方：CThostFtdcRspInfoField）----
#pragma pack(push, 1)
struct CtpRspInfo {
    int32_t ErrorID;
    char    ErrorMsg[81];
};
#pragma pack(pop)

// ---- 行情回调（官方：CThostFtdcMdSpi）----
class CtpMdSpi {
public:
    virtual ~CtpMdSpi() = default;
    virtual void OnFrontConnected() {}
    virtual void OnFrontDisconnected(int /*nReason*/) {}
    virtual void OnRspUserLogin(const CtpRspUserLogin& /*rsp*/, const CtpRspInfo& /*info*/,
                               int /*nRequestID*/, bool /*bIsLast*/) {}
    virtual void OnRspSubMarketData(const char* /*instrument*/, const CtpRspInfo& /*info*/,
                                    int /*nRequestID*/, bool /*bIsLast*/) {}
    virtual void OnRtnDepthMarketData(const CtpDepthMarketData& /*md*/) {}
};

// ---- 行情 API（官方：CThostFtdcMdApi）----
class CtpMdApi {
public:
    virtual ~CtpMdApi() = default;
    virtual void RegisterSpi(CtpMdSpi* spi) = 0;
    virtual void RegisterFront(const char* frontAddr) = 0;
    virtual void Init() = 0;
    virtual void Join() = 0;
    virtual void Release() = 0;
    virtual int  SubscribeMarketData(const char* insts[], int nCount) = 0;
    virtual int  ReqUserLogin(const char* brokerId, const char* userId,
                              const char* password, int nRequestID) = 0;
};

}  // namespace md
