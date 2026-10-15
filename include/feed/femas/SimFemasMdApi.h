#pragma once
// ============================================================================
// feed/femas/SimFemasMdApi.h —— 飞马 行情 API 的进程内仿真后端（接口与 FemasMdApi 一致）
// ============================================================================
#include "feed/femas/FemasMdTypes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace md {

class SimFemasMdApi : public FemasMdApi {
public:
    void RegisterSpi(FemasMdSpi* spi) override { spi_ = spi; }
    void RegisterFront(const char* /*frontAddr*/) override {}
    void Init() override;
    void Join() override;
    void Release() override;
    int  SubscribeMarketData(const char* insts[], int nCount) override;
    int  ReqUserLogin(const char* /*brokerId*/, const char* /*userId*/,
                      const char* /*password*/, int /*nRequestID*/) override { return 0; }

private:
    void run();
    void joinThread();

    FemasMdSpi* spi_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::vector<std::string> subs_;
};

}  // namespace md
