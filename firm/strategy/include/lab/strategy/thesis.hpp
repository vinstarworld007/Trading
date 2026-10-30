#pragma once

// A fundamental position as a state machine. The idea comes from outside the
// feed: an analyst's fair value for the stock. After a research period the
// strategy builds target_shares while the price is at least entry_bps below
// fair value, holds, and sells once the price is within exit_bps of fair
// value or the holding period runs out. A drop of stop_bps below the average
// cost means the thesis was wrong, and it sells. Fed one close per day; the
// desk works the target with a participation schedule (see execution.hpp).

#include <cstdint>

namespace lab::strategy {

struct ThesisParams {
  std::uint32_t fair_value = 0;     // Price(4)
  std::uint32_t entry_bps = 1'000;  // buy only while 10%+ below fair value
  std::uint32_t exit_bps = 300;     // sell once within 3% of fair value
  std::uint32_t stop_bps = 2'500;   // sell if 25% below the average cost
  std::int64_t target_shares = 600'000;
  std::uint32_t research_days = 10;
  std::uint32_t max_hold_days = 230;  // from the first day of building
};

enum class ThesisPhase : std::uint8_t { kResearch, kBuild, kHold, kExit, kDone };

inline const char* thesis_phase_name(ThesisPhase p) noexcept {
  switch (p) {
    case ThesisPhase::kResearch:
      return "research";
    case ThesisPhase::kBuild:
      return "build";
    case ThesisPhase::kHold:
      return "hold";
    case ThesisPhase::kExit:
      return "exit";
    case ThesisPhase::kDone:
      return "done";
  }
  return "?";
}

struct ThesisDecision {
  ThesisPhase phase = ThesisPhase::kResearch;
  std::int64_t target = 0;  // shares to hold
  bool stopped = false;     // exiting because the thesis broke
};

class ThesisTrader {
 public:
  explicit ThesisTrader(const ThesisParams& p) noexcept : p_{p} {}

  // `day` counts from 1. `avg_cost` is Price(4), 0 when flat.
  ThesisDecision on_day(std::uint32_t day, std::uint32_t close, std::int64_t position,
                        std::uint32_t avg_cost) noexcept {
    const auto fv = static_cast<std::uint64_t>(p_.fair_value);
    const std::uint64_t entry_below = fv * (10'000 - p_.entry_bps) / 10'000;
    const std::uint64_t exit_at = fv * (10'000 - p_.exit_bps) / 10'000;
    switch (phase_) {
      case ThesisPhase::kResearch:
        if (day > p_.research_days && close <= entry_below) {
          phase_ = ThesisPhase::kBuild;
          start_day_ = day;
        }
        break;
      case ThesisPhase::kBuild:
        if (position >= p_.target_shares) phase_ = ThesisPhase::kHold;
        [[fallthrough]];
      case ThesisPhase::kHold:
        if (broken(close, avg_cost)) {
          phase_ = ThesisPhase::kExit;
          stopped_ = true;
        } else if (close >= exit_at || day - start_day_ >= p_.max_hold_days) {
          phase_ = ThesisPhase::kExit;
        } else if (phase_ == ThesisPhase::kBuild && close > entry_below) {
          phase_ = ThesisPhase::kHold;  // too close to fair value to keep adding
        }
        break;
      case ThesisPhase::kExit:
        if (position <= 0) phase_ = ThesisPhase::kDone;
        break;
      case ThesisPhase::kDone:
        break;
    }
    ThesisDecision d;
    d.phase = phase_;
    d.stopped = stopped_;
    if (phase_ == ThesisPhase::kBuild) d.target = p_.target_shares;
    if (phase_ == ThesisPhase::kHold) d.target = position;
    return d;
  }

  ThesisPhase phase() const noexcept { return phase_; }
  const ThesisParams& params() const noexcept { return p_; }

 private:
  bool broken(std::uint32_t close, std::uint32_t avg_cost) const noexcept {
    return avg_cost != 0 && static_cast<std::uint64_t>(close) * 10'000 <
                                static_cast<std::uint64_t>(avg_cost) * (10'000 - p_.stop_bps);
  }

  ThesisParams p_;
  ThesisPhase phase_ = ThesisPhase::kResearch;
  std::uint32_t start_day_ = 0;
  bool stopped_ = false;
};

}  // namespace lab::strategy
