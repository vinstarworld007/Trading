#pragma once

// Daily bars for the slower strategies, from a CSV written by
// research/fetch_daily.py:
//
//   date,open,high,low,close,volume
//   2019-01-02,154.89,158.85,154.23,157.92,37039737
//
// Prices are parsed from their decimal text straight into Price(4) integers,
// never through floating point.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lab::backtest {

struct DailyBar {
  std::string date;                                      // YYYY-MM-DD
  std::uint32_t open = 0, high = 0, low = 0, close = 0;  // Price(4)
  std::uint64_t volume = 0;
};

// "157.92" -> 1'579'200. Up to four decimals; nullopt on anything else.
inline std::optional<std::uint32_t> parse_price4(std::string_view s) {
  if (!s.empty() && s.front() == '$') s.remove_prefix(1);
  if (s.empty()) return std::nullopt;
  std::uint64_t v = 0;
  int decimals = -1;
  for (const char c : s) {
    if (c == '.') {
      if (decimals >= 0) return std::nullopt;
      decimals = 0;
      continue;
    }
    if (c < '0' || c > '9') return std::nullopt;
    if (decimals >= 4) return std::nullopt;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
    if (decimals >= 0) ++decimals;
    if (v > 0xFFFF'FFFFULL) return std::nullopt;
  }
  for (int d = decimals < 0 ? 0 : decimals; d < 4; ++d) v *= 10;
  if (v > 0xFFFF'FFFFULL) return std::nullopt;
  return static_cast<std::uint32_t>(v);
}

// One CSV line (without the newline) into a bar; nullopt for a bad or header line.
inline std::optional<DailyBar> parse_daily_line(std::string_view line) {
  std::string_view f[6];
  std::size_t n = 0;
  while (n < 6) {
    const std::size_t c = line.find(',');
    f[n++] = line.substr(0, c);
    if (c == std::string_view::npos) break;
    line.remove_prefix(c + 1);
  }
  if (n != 6 || f[0].size() != 10) return std::nullopt;
  DailyBar b;
  b.date = std::string{f[0]};
  const auto o = parse_price4(f[1]), h = parse_price4(f[2]), l = parse_price4(f[3]),
             c = parse_price4(f[4]);
  if (!o || !h || !l || !c || f[5].empty()) return std::nullopt;
  for (const char ch : f[5]) {
    if (ch < '0' || ch > '9') return std::nullopt;
    b.volume = b.volume * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  b.open = *o;
  b.high = *h;
  b.low = *l;
  b.close = *c;
  return b;
}

// All bars in a file, oldest first. Empty if the file cannot be read.
inline std::vector<DailyBar> read_daily(const std::string& path) {
  std::vector<DailyBar> out;
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (f == nullptr) return out;
  char buf[256];
  while (std::fgets(buf, sizeof buf, f) != nullptr) {
    std::string_view s{buf};
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    if (auto b = parse_daily_line(s)) out.push_back(std::move(*b));
  }
  std::fclose(f);
  return out;
}

}  // namespace lab::backtest
