#pragma once
// ============================================================================
// feed/femas/RealFemasMdApi.h —— 真实 飞马 行情 API 封装（#ifdef HAS_FEMAS）
//
// 接入真实柜台：把 third_party/femas 的头文件/库就位后，编译期 -DHAS_FEMAS
// -Ithird_party/femas/include 并链接对应库即可。字段名以你拿到的官方头文件为准。
// ============================================================================
#include "feed/femas/FemasMdTypes.h"

#ifdef HAS_FEMAS
// 飞马官方头文件名常见为 FemasMdApi.h / securitiesthreadapi.h 等，按实际放位置调整：
#include <FemasMdApi.h>

namespace md {

class RealFemasMdApi : public FemasMdApi {
public:
    static FemasMdApi* Create();
    RealFemasMdApi();
    ~RealFemasMdApi() override;

    void RegisterSpi(FemasMdSpi* spi) override;
    void RegisterFront(const char* frontAddr) override;
    void Init() override;
    void Join() override;
    void Release() override;
    int  SubscribeMarketData(const char* insts[], int nCount) override;
    int  ReqUserLogin(const char* brokerId, const char* userId,
                      const char* password, int nRequestID) override;

private:
    CFemasMdApi* raw_;
    class Bridge;
    Bridge* bridge_;
};

}  // namespace md
#endif  // HAS_FEMAS
