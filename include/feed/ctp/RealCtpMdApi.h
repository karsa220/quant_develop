#pragma once
// ============================================================================
// feed/ctp/RealCtpMdApi.h —— 真实 CTP 行情 API 封装（#ifdef HAS_CTP）
//
// 仅当编译期定义了 HAS_CTP（且 third_party/ctp 提供 ThostFtdcMdApi.h 与
// thostmduserapi 动态库）时才被编译。它把上期的 CThostFtdcMdApi / CThostFtdcMdSpi
// 「翻译」成项目内的 CtpMdApi / CtpMdSpi 契约，使 CtpMdFeed 上层的接入代码与
// 仿真后端完全一致、可无缝切换。
//
// 接入真实柜台：
//   1) 把期货公司提供的 include/ 与 lib/ 放到 third_party/ctp/
//   2) 编译时 -DHAS_CTP -Ithird_party/ctp/include，链接 thostmduserapi_se
//   3) FeedConfig{ .frontAddresses={...}, .brokerId, .userId, .password,
//                   .instruments={...}, .simulation=false }
// ============================================================================
#include "feed/ctp/CtpMdTypes.h"

#ifdef HAS_CTP
#include <ThostFtdcMdApi.h>

namespace md {

class RealCtpMdApi : public CtpMdApi {
public:
    static CtpMdApi* Create();
    RealCtpMdApi();
    ~RealCtpMdApi() override;

    void RegisterSpi(CtpMdSpi* spi) override;
    void RegisterFront(const char* frontAddr) override;
    void Init() override;
    void Join() override;
    void Release() override;
    int  SubscribeMarketData(const char* insts[], int nCount) override;
    int  ReqUserLogin(const char* brokerId, const char* userId,
                      const char* password, int nRequestID) override;

private:
    CThostFtdcMdApi* raw_;
    class Bridge;          // 继承 CThostFtdcMdSpi，转发到 CtpMdSpi
    Bridge* bridge_;
};

}  // namespace md
#endif  // HAS_CTP
