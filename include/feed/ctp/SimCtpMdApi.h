#pragma once
// ============================================================================
// feed/ctp/SimCtpMdApi.h —— CTP 行情 API 的进程内仿真后端
//
// 实现与 CtpMdApi 完全一致的接口，但「假装」自己是一个柜台前置：
//   连接 → 登录成功 → 按订阅合约持续推送行情（行情由 FuturesFeed 合成生成）。
// 无需任何 SDK / 账号 / 网络，用于本地开发、演示与 CI；行为与真机一致，
// 因此 CtpMdFeed 上层的接入代码可在无柜台环境下被编译、运行与测试。
// ============================================================================
#include "feed/ctp/CtpMdTypes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace md {

class SimCtpMdApi : public CtpMdApi {
public:
    void RegisterSpi(CtpMdSpi* spi) override { spi_ = spi; }
    void RegisterFront(const char* /*frontAddr*/) override {}
    void Init() override;
    void Join() override;
    void Release() override;
    int  SubscribeMarketData(const char* insts[], int nCount) override;
    int  ReqUserLogin(const char* /*brokerId*/, const char* /*userId*/,
                      const char* /*password*/, int /*nRequestID*/) override { return 0; }

private:
    void run();   // 仿真线程主循环
    void joinThread();

    CtpMdSpi* spi_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::vector<std::string> subs_;   // 订阅合约（Login 后由上层填）
};

}  // namespace md
