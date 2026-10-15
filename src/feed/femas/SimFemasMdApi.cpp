// ============================================================================
// feed/femas/SimFemasMdApi.cpp —— 飞马 仿真后端实现
// ============================================================================
#include "feed/femas/SimFemasMdApi.h"
#include "feed/FuturesFeed.h"
#include "feed/MarketFeed.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace md {

static void formatWallTime(Nanos wallNs, char* day9, char* time9, int* ms) {
    std::time_t tt = static_cast<std::time_t>(wallNs / 1'000'000'000ULL);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::snprintf(day9, 9, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    std::snprintf(time9, 9, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    *ms = static_cast<int>((wallNs % 1'000'000'000ULL) / 1'000'000ULL);
}

void SimFemasMdApi::Init() {
    stop_ = false;
    thread_ = std::thread([this]() { run(); });
}

void SimFemasMdApi::Join() { joinThread(); }

void SimFemasMdApi::Release() {
    stop_ = true;
    joinThread();
}

int SimFemasMdApi::SubscribeMarketData(const char* insts[], int nCount) {
    std::lock_guard<std::mutex> lk(mtx_);
    subs_.clear();
    for (int i = 0; i < nCount && insts && insts[i]; ++i) subs_.emplace_back(insts[i]);
    return 0;
}

void SimFemasMdApi::joinThread() {
    if (thread_.joinable()) thread_.join();
}

void SimFemasMdApi::run() {
    using namespace std::chrono;
    std::this_thread::sleep_for(milliseconds(20));
    if (spi_) spi_->OnFrontConnected();
    std::this_thread::sleep_for(milliseconds(10));

    FemasRspUserLogin rsp{};
    std::strncpy(rsp.BrokerID, "8888", sizeof(rsp.BrokerID) - 1);
    std::strncpy(rsp.UserID, "SIM", sizeof(rsp.UserID) - 1);
    FemasRspInfo info{};
    if (spi_) spi_->OnRspUserLogin(rsp, info, 1, true);

    FuturesFeed feed(0xCAFE1234ULL);
    const auto kStep = microseconds(50);
    while (!stop_) {
        MarketTick t{};
        Nanos srcTs = 0;
        if (feed.nextTick(t, srcTs)) {
            const char* sym = feed.instrument(static_cast<size_t>(t.inst)).symbol;
            bool emit = true;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!subs_.empty()) {
                    emit = false;
                    for (const auto& s : subs_) if (s == sym) { emit = true; break; }
                }
            }
            if (emit) {
                FemasDepthMarketData md{};
                std::strncpy(md.InstrumentID, sym, sizeof(md.InstrumentID) - 1);
                double u = symbolPriceUnit(sym);
                md.LastPrice  = ticksToPrice(t.price, sym);
                md.Volume     = static_cast<int32_t>(t.qty);
                md.BidPrice1  = md.LastPrice - u;
                md.AskPrice1  = md.LastPrice + u;
                md.BidVolume1 = static_cast<int32_t>(t.qty);
                md.AskVolume1 = static_cast<int32_t>(t.qty);
                auto wn = system_clock::now().time_since_epoch();
                Nanos wallNs = static_cast<Nanos>(duration_cast<nanoseconds>(wn).count());
                formatWallTime(wallNs, md.TradingDay, md.UpdateTime, &md.UpdateMillisec);
                if (spi_) spi_->OnRtnDepthMarketData(md);
            }
        }
        std::this_thread::sleep_for(kStep);
    }
}

}  // namespace md
