#pragma once
// ============================================================================
// feed/MarketFeed.h —— 统一行情源接口（合成 / 真实 CTP / 真实 飞马 / 仿真 共用）
//
// 设计目标：把「行情从哪来」与「行情怎么用」解耦。无论是合成源、还是真实柜台
// CTP/飞马的 MdApi，都对外暴露同一个 push 模型接口（行情经 OnMarketData 上送），
// 上层（网关 / 风控 / 共享内存环）无需关心背后是仿真还是真机。
//
// 真实柜台 API（CThostFtdcMdApi / CFemasMdApi）是期货公司授权的闭源 SDK，需要
// 头文件 + 动态库 + 账号 front 地址。本仓库的做法（业界标准）：
//   - 各 vendor 适配层「镜像」一份与真机一致的方法/回调契约（见 ctp/ / femas/ 下
//     的 *MdTypes.h），真实封装仅在编译期定义 HAS_CTP / HAS_FEMAS 时才被编译；
//   - 同时提供「同契约的进程内仿真后端」SimXxxMdApi，无需 SDK/账号即可在本地与
//     CI 跑通「登录 → 订阅 → 行情 → 风控」整条链路。
// 真机接入只需：把 vendor 头文件/库放到 third_party/<ctp|femas>/，定义宏并链接。
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "common/Types.h"   // MarketTick / PriceTicks / Nanos / InstrumentId

namespace md {

// 行情回调：收到一笔行情（已转换为内部 MarketTick）+ 行情源时间戳（交易所/网关，ns）
using OnMarketData = std::function<void(const MarketTick& tick, Nanos srcTs)>;

// 统一行情源配置
struct FeedConfig {
    std::vector<std::string> frontAddresses;  // 柜台前置地址，如 "tcp://180.168.146.187:10131"
    std::string brokerId;                     // 经纪商代码，如 "9999"
    std::string userId;                       // 投资者账号
    std::string password;                     // 密码
    std::vector<std::string> instruments;     // 订阅合约，如 {"IF","IC","TF"}
    bool simulation = true;                   // true=本地仿真（无需 SDK/账号）；false=接真机
};

// 统一行情源接口（push 模型）
class MarketFeed {
public:
    virtual ~MarketFeed() = default;

    // 启动并登录柜台、订阅合约。异步：登录结果与行情经 OnMarketData 回调上送。
    virtual bool start(const FeedConfig& cfg) = 0;
    virtual void stop()  = 0;

    // 后端名： "ctp" / "femas" / "ctp-sim" / "femas-sim"
    virtual const char* backend() const = 0;

    void setOnMarketData(OnMarketData cb) { onMd_ = std::move(cb); }
    bool running() const { return running_; }

protected:
    OnMarketData onMd_;
    bool         running_ = false;
    void emit(const MarketTick& t, Nanos srcTs) { if (onMd_) onMd_(t, srcTs); }
};

// ---------------------------------------------------------------------------
// 价格 / 时间转换工具（vendor double 价 → 内部 int64 tick 价）
// ---------------------------------------------------------------------------

// 每个内部 PriceTick 对应的「元」。仿真与真实封装共用，保证 round-trip 一致。
// 取合约前缀（去尾部数字）查表；未知合约默认 0.01 元/tick。
double symbolPriceUnit(const char* symbol);

// 合约代码 → 内部 InstrumentId（委托 FuturesFeed 的映射表）。
InstrumentId symbolToInst(const char* symbol);

// vendor 元价 → 内部 PriceTicks（四舍五入到最近 tick）
inline PriceTicks priceToTicks(double yuanPrice, const char* symbol) {
    double u = symbolPriceUnit(symbol);
    if (u <= 0) u = 0.01;
    return static_cast<PriceTicks>(std::llround(yuanPrice / u));
}

// 内部 PriceTicks → vendor 元价（double）
inline double ticksToPrice(PriceTicks ticks, const char* symbol) {
    return static_cast<double>(ticks) * symbolPriceUnit(symbol);
}

// 把交易所文本时间解析为 Unix ns。
//   tradingDay : "YYYYMMDD"（可空）
//   updateTime : "HH:MM:SS"（可空）
//   updateMs   : 毫秒
// 解析失败 / 字段为空 → 返回当前 steady_clock 的 ns（作为兜底）。
Nanos parseExchangeTimeNs(const char* tradingDay, const char* updateTime, int updateMs);

// 主动成交方向启发式：LastPrice 贴近卖一 → 买主动；贴近买一 → 卖主动；否则按奇偶兜底。
inline uint8_t aggressorSide(double lastPrice, double bid1, double ask1) {
    if (ask1 > 0 && lastPrice >= ask1) return kSideBuy;
    if (bid1 > 0 && lastPrice <= bid1) return kSideSell;
    return (static_cast<uint64_t>(lastPrice * 1000) & 1u) ? kSideBuy : kSideSell;
}

}  // namespace md
