// ============================================================================
// feed/MarketFeed.cpp —— 统一行情源接口的实现 + 价格/时间转换工具
// ============================================================================
#include "feed/MarketFeed.h"
#include "feed/FuturesFeed.h"

#include <cctype>
#include <cmath>
#include <chrono>

namespace md {

InstrumentId symbolToInst(const char* symbol) { return FuturesFeed::symbolToId(symbol); }

double symbolPriceUnit(const char* symbol) {
    if (!symbol || !symbol[0]) return 0.01;
    std::string s(symbol);
    size_t i = 0;
    while (i < s.size() && std::isalpha((unsigned char)s[i])) ++i;
    std::string p = s.substr(0, i);
    if (p == "IF" || p == "IC" || p == "IH" || p == "TF") return 0.2;  // 指数/国债类
    if (p == "RB" || p == "AU" || p == "CU")                 return 1.0;  // 商品类
    return 0.01;
}

// civil date → days since 1970-01-01 (Howard Hinnant 的经典算法)
static int days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;                                   // [0, 399]
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + doe - 719468;
}

Nanos parseExchangeTimeNs(const char* tradingDay, const char* updateTime, int updateMs) {
    int Y = 1970, M = 1, D = 1, h = 0, m = 0, s = 0;
    if (tradingDay && std::strlen(tradingDay) == 8) {
        Y = (tradingDay[0]-'0')*1000 + (tradingDay[1]-'0')*100 +
            (tradingDay[2]-'0')*10  + (tradingDay[3]-'0');
        M = (tradingDay[4]-'0')*10 + (tradingDay[5]-'0');
        D = (tradingDay[6]-'0')*10 + (tradingDay[7]-'0');
    }
    if (updateTime && std::strlen(updateTime) == 8 &&
        updateTime[2] == ':' && updateTime[5] == ':') {
        h = (updateTime[0]-'0')*10 + (updateTime[1]-'0');
        m = (updateTime[3]-'0')*10 + (updateTime[4]-'0');
        s = (updateTime[6]-'0')*10 + (updateTime[7]-'0');
    }
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 ||
        h > 23 || m > 59 || s > 59 || updateMs < 0 || updateMs > 999) {
        // 兜底：用 steady_clock（无法对应墙钟，仅用于本地仿真不校验时效的场景）
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return (Nanos)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }
    int64_t secs = (int64_t)days_from_civil(Y, M, D) * 86400LL +
                   (int64_t)h * 3600 + (int64_t)m * 60 + s;
    return (Nanos)secs * 1'000'000'000ULL + (Nanos)updateMs * 1'000'000ULL;
}

}  // namespace md
