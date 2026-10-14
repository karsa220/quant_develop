#pragma once
// ============================================================================
// net/JsonBook.h —— 极简 JSON 序列化（header-only，零依赖）
//
// 用于 MdServer 的 HTTP REST 接口对外暴露盘口/快照。刻意不引入第三方库，
// 只做「对象数组 → JSON」这一件小事，面试里也能讲清「为什么不直接用 nlohmann/json」。
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace md { namespace net {

// 一条合约的最新状态（供快照/REST 输出）
struct BookRow {
  std::string symbol;
  int64_t     lastPx;    // 最新成交价（tick 计数）
  uint32_t    lastQty;   // 最新成交量（手）
  uint8_t     lastSide;  // 0=BUY(主动买) 1=SELL(主动卖)
  uint64_t    count;     // 累计成交笔数
};

inline std::string jsonString(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else o += c;
  }
  o += "\"";
  return o;
}

inline std::string bookRowToJson(const BookRow& r) {
  std::string side = (r.lastSide == 0) ? "\"BUY\"" : "\"SELL\"";
  std::string s = "{";
  s += "\"symbol\":" + jsonString(r.symbol) + ",";
  s += "\"last\":" + std::to_string(r.lastPx) + ",";
  s += "\"qty\":" + std::to_string(r.lastQty) + ",";
  s += "\"side\":" + side + ",";
  s += "\"count\":" + std::to_string(r.count);
  s += "}";
  return s;
}

inline std::string bookRowsToJson(const std::vector<BookRow>& rows) {
  std::string s = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i) s += ",";
    s += bookRowToJson(rows[i]);
  }
  s += "]";
  return s;
}

}}  // namespace md::net
