#pragma once

// Statistical arbitrage on one pair of related stocks. Each sample is the two
// mid prices; the signal is the log price ratio's z-score over a rolling
// window. Past entry_z the pair is sold rich / bought cheap with equal dollars
// in each leg, and closed when the z-score comes back inside exit_z (or
// crosses through zero). Past stop_z the relationship is assumed broken: the
// position is closed and the trader waits for the z-score to come back inside
// exit_z before it trades again.
//
// The window is allocated once, in the constructor; on_sample() does not
// allocate. Prices are Price(4) integers; only the statistics are floating
// point.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lab::strategy {

struct PairsParams {
  std::size_t window = 156;      // samples in the rolling mean and deviation
  std::size_t min_samples = 60;  // no trading until the window has this many
  double entry_z = 2.0;
  double exit_z = 0.3;
  double stop_z = 4.0;
  std::int64_t leg_notional = 200'000;  // dollars per leg
  std::uint32_t lot = 100;              // leg sizes round down to whole lots
};

// -1: short the spread (sell A, buy B). +1: long the spread (buy A, sell B).
enum class PairState : std::int8_t { kShort = -1, kFlat = 0, kLong = 1 };

struct PairSignal {
  double z = 0;
  PairState state = PairState::kFlat;
  bool entered = false;
  bool exited = false;   // closed because the gap came back
  bool stopped = false;  // closed because the gap kept growing
};

class PairsTrader {
 public:
  explicit PairsTrader(const PairsParams& p) : p_{p}, ring_(p.window, 0.0) {}

  PairSignal on_sample(std::uint32_t mid_a, std::uint32_t mid_b) noexcept {
    PairSignal s;
    s.state = state_;
    if (mid_a == 0 || mid_b == 0) return s;
    const double x = std::log(static_cast<double>(mid_a)) - std::log(static_cast<double>(mid_b));
    if (count_ == ring_.size()) {
      const double old = ring_[head_];
      sum_ -= old;
      sum2_ -= old * old;
    } else {
      ++count_;
    }
    ring_[head_] = x;
    head_ = (head_ + 1) % ring_.size();
    sum_ += x;
    sum2_ += x * x;
    const double n = static_cast<double>(count_);
    const double mean = sum_ / n;
    const double var = sum2_ / n - mean * mean;
    if (var <= 1e-14) return s;
    s.z = (x - mean) / std::sqrt(var);
    last_z_ = s.z;
    if (count_ < p_.min_samples) return s;

    const double az = std::fabs(s.z);
    if (state_ == PairState::kFlat) {
      if (!armed_) {
        armed_ = az < p_.exit_z;
      } else if (az > p_.entry_z) {
        state_ = s.z > 0 ? PairState::kShort : PairState::kLong;
        s.entered = true;
      }
    } else if (az > p_.stop_z) {
      state_ = PairState::kFlat;
      armed_ = false;
      s.stopped = true;
    } else if (az < p_.exit_z || (s.z > 0) == (state_ == PairState::kLong)) {
      state_ = PairState::kFlat;
      s.exited = true;
    }
    s.state = state_;
    return s;
  }

  // Target shares in each leg at these prices (Price(4)), for the current state.
  std::int64_t target_a(std::uint32_t px_a) const noexcept {
    return static_cast<std::int64_t>(state_) * shares(px_a);
  }
  std::int64_t target_b(std::uint32_t px_b) const noexcept {
    return -static_cast<std::int64_t>(state_) * shares(px_b);
  }

  PairState state() const noexcept { return state_; }
  double z() const noexcept { return last_z_; }
  const PairsParams& params() const noexcept { return p_; }

 private:
  std::int64_t shares(std::uint32_t px) const noexcept {
    if (px == 0) return 0;
    const std::int64_t raw = p_.leg_notional * 10'000 / static_cast<std::int64_t>(px);
    return raw / p_.lot * p_.lot;
  }

  PairsParams p_;
  std::vector<double> ring_;
  std::size_t head_ = 0, count_ = 0;
  double sum_ = 0, sum2_ = 0, last_z_ = 0;
  PairState state_ = PairState::kFlat;
  bool armed_ = true;
};

}  // namespace lab::strategy
