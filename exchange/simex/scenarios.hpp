#pragma once

// Market scenarios simex can run on a day: opening and closing crosses with
// imbalance messages, news halts with a quotation period and a halt cross,
// LULD price bands with limit states, trading pauses and reopening auctions,
// market-wide circuit breakers, an IPO release, an operational halt, a
// broken trade and the Reg SHO short sale price test. Times are seconds
// after the matcher starts. Everything is off unless configured; --scenario
// all turns on a preset sized to --duration.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simex {

// "SYM@T+D" or "SYM@T+D:REASON": from T for D seconds.
struct TimedSymbol {
  std::string symbol;
  double at_s = 0;
  double for_s = 0;
  std::string reason;  // ITCH trading action reason, e.g. T1
};

struct MwcbEvent {
  int level = 1;  // 1, 2 or 3; level 3 halts for the rest of the day
  double at_s = 0;
  double for_s = 0;
};

struct ScenarioConfig {
  double open_auction_s = 0;        // pre-open: orders rest unmatched, then the opening cross
  double close_at_s = 0;            // closing auction starts (0: only when the market closes)
  double close_auction_s = 0;       // closing auction length (0: no closing cross)
  double noii_interval_s = 1;       // imbalance messages during every auction
  double quote_period_s = 1;        // halts: quotation period before the halt cross
  bool luld = false;                // price bands, limit states and pauses
  std::uint32_t luld_band_pct = 0;  // 0: the plan's 5% for Tier 1 above $3
  double luld_limit_s = 15;         // limit state lasts this long -> pause (plan: 15 s)
  double luld_pause_s = 300;        // pause length and each extension (plan: 5 minutes)
  bool reg_sho = false;             // short sale price test at a 10% drop
  std::vector<TimedSymbol> halts;
  std::vector<TimedSymbol> op_halts;
  std::vector<TimedSymbol> ipos;  // released for quotation at T, crossed at T+D
  std::optional<MwcbEvent> mwcb;
  std::vector<double> breaks;  // break the latest trade at these times
  std::vector<std::string> ipo_symbols() const {
    std::vector<std::string> out;
    for (const auto& i : ipos) out.push_back(i.symbol);
    return out;
  }
  bool any() const {
    return open_auction_s > 0 || close_auction_s > 0 || luld || reg_sho || !halts.empty() ||
           !op_halts.empty() || !ipos.empty() || mwcb || !breaks.empty();
  }
};

// Parses "SYM@T+D[:REASON]"; nullopt if malformed.
inline std::optional<TimedSymbol> parse_timed(const std::string& s) {
  TimedSymbol t;
  const auto at = s.find('@');
  const auto plus = s.find('+', at);
  if (at == std::string::npos || plus == std::string::npos || at == 0) return std::nullopt;
  t.symbol = s.substr(0, at);
  const auto colon = s.find(':', plus);
  try {
    t.at_s = std::stod(s.substr(at + 1, plus - at - 1));
    t.for_s = std::stod(s.substr(plus + 1, colon == std::string::npos ? colon : colon - plus - 1));
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (colon != std::string::npos) t.reason = s.substr(colon + 1);
  if (t.at_s < 0 || t.for_s <= 0 || t.reason.size() > 4) return std::nullopt;
  return t;
}

// The --scenario all preset for a session of `duration` seconds over
// `symbols`: every scenario once, spaced through the day. The LULD band is
// narrowed to 2% and its timers shortened so agents reach them.
inline ScenarioConfig all_scenarios(double duration, const std::vector<std::string>& symbols) {
  ScenarioConfig c;
  const double d = duration > 0 ? duration : 30;
  auto sym = [&](std::size_t i) {
    return symbols.empty() ? std::string{} : symbols[i % symbols.size()];
  };
  c.open_auction_s = d * 0.08;
  c.close_auction_s = d * 0.08;
  c.close_at_s = d * 0.88;
  c.noii_interval_s = d / 60 > 0.25 ? d / 60 : 0.25;
  c.quote_period_s = d * 0.03;
  c.luld = true;
  c.luld_band_pct = 2;
  c.luld_limit_s = d * 0.02;
  c.luld_pause_s = d * 0.05;
  c.reg_sho = true;
  c.halts.push_back({sym(0), d * 0.20, d * 0.10, "T1"});
  c.op_halts.push_back({sym(1), d * 0.35, d * 0.06, ""});
  c.ipos.push_back({"IPO0001", d * 0.15, d * 0.15, ""});
  c.mwcb = MwcbEvent{1, d * 0.55, d * 0.10};
  c.breaks.push_back(d * 0.45);
  return c;
}

}  // namespace simex
