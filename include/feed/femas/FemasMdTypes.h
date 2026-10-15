#pragma once
// ============================================================================
// feed/femas/FemasMdTypes.h —— 镜像 飞马(Femas) 行情 API
//
// 飞马是另一家期货公司柜台的行情接口，与 CTP 形态相近（MdApi + SPI 回调）。
// 这里同样只镜像行情所需字段/方法，使 SimFemasMdApi 与 RealFemasMdApi 实现同一
// 套契约，FemasMdFeed 上层代码与 CTP 完全一致。
//
// 注：飞马官方头文件字段名/类型可能与此处略有差异（各柜台版本不同），
// RealFemasMdApi.cpp (#ifdef HAS_FEMAS) 中的「翻译」请以你拿到的官方头文件为准，
// 下面注释标注了常规字段名。
// ============================================================================
#include <cstdint>
#include <cstddef>

namespace md {

// ---- 行情深度数据（官方常见：CFemasDepthMarketDataField / CSecurityFtdcDepthMarketDataField）----
#pragma pack(push, 1)
struct FemasDepthMarketData {
    char    InstrumentID[31];
    double  LastPrice;
    int32_t Volume;
    double  BidPrice1;
    double  AskPrice1;
    int32_t BidVolume1;
    int32_t AskVolume1;
    char    TradingDay[9];
    char    UpdateTime[9];
    int32_t UpdateMillisec;
};
#pragma pack(pop)

// ---- 登录应答 ----
#pragma pack(push, 1)
struct FemasRspUserLogin {
    char    TradingDay[9];
    char    LoginTime[9];
    char    BrokerID[11];
    char    UserID[16];
    int32_t FrontID;
    int32_t SessionID;
    char    MaxOrderRef[13];
};
#pragma pack(pop)

// ---- 错误应答 ----
#pragma pack(push, 1)
struct FemasRspInfo {
    int32_t ErrorID;
    char    ErrorMsg[81];
};
#pragma pack(pop)

// ---- 行情回调（官方常见：CFemasMdSpi）----
class FemasMdSpi {
public:
    virtual ~FemasMdSpi() = default;
    virtual void OnFrontConnected() {}
    virtual void OnFrontDisconnected(int /*nReason*/) {}
    virtual void OnRspUserLogin(const FemasRspUserLogin& /*rsp*/, const FemasRspInfo& /*info*/,
                                int /*nRequestID*/, bool /*bIsLast*/) {}
    virtual void OnRspSubMarketData(const char* /*instrument*/, const FemasRspInfo& /*info*/,
                                    int /*nRequestID*/, bool /*bIsLast*/) {}
    virtual void OnRtnDepthMarketData(const FemasDepthMarketData& /*md*/) {}
};

// ---- 行情 API（官方常见：CFemasMdApi）----
class FemasMdApi {
public:
    virtual ~FemasMdApi() = default;
    virtual void RegisterSpi(FemasMdSpi* spi) = 0;
    virtual void RegisterFront(const char* frontAddr) = 0;
    virtual void Init() = 0;
    virtual void Join() = 0;
    virtual void Release() = 0;
    virtual int  SubscribeMarketData(const char* insts[], int nCount) = 0;
    virtual int  ReqUserLogin(const char* brokerId, const char* userId,
                              const char* password, int nRequestID) = 0;
};

}  // namespace md
