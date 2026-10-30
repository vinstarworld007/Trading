#pragma once

// Trend following on one instrument: a fast and a slow simple moving average
// of the closing price. Long while the fast average is above the slow one,
// short while it is below, sized to a fixed dollar notional. Fed one close
// per bar (a day in the quant lab), so it trades a few times a month.
//
// The window is allocated once, in the constructor; on_close() does not
// allocate. Averages are integer Price(4).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lab::strategy {

struct TrendParams {
  std::size_t fast = 20;
  std::size_t slow = 50;
  std::int64_t notional = 1'000'000;  // dollars
  std::uint32_t lot = 100;
};

struct TrendSignal {
  std::uint32_t fast_ma = 0;  // Price(4); 0 until the window is full
  std::uint32_t slow_ma = 0;
  int direction = 0;  // +1 long, -1 short, 0 not enough history
  bool changed = false;
};

class TrendFollower {
 public:
  explicit TrendFollower(const TrendParams& p) : p_{p}, ring_(p.slow, 0) {}

  TrendSignal on_close(std::uint32_t close) noexcept {
    const std::size_t n = ring_.size();
    ring_[head_] = close;
    head_ = (head_ + 1) % n;
    if (count_ < n) ++count_;
    TrendSignal s;
    s.direction = direction_;
    if (count_ < n) return s;
    std::uint64_t fast = 0, slow = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint32_t v = ring_[(head_ + n - 1 - i) % n];  // newest first
      slow += v;
      if (i < p_.fast) fast += v;
    }
    s.fast_ma = static_cast<std::uint32_t>(fast / p_.fast);
    s.slow_ma = static_cast<std::uint32_t>(slow / n);
    const int want = s.fast_ma > s.slow_ma ? 1 : s.fast_ma < s.slow_ma ? -1 : direction_;
    s.changed = want != direction_;
    direction_ = want;
    s.direction = want;
    return s;
  }

  // Target shares at `px` (Price(4)).
  std::int64_t target(std::uint32_t px) const noexcept {
    if (px == 0 || direction_ == 0) return 0;
    const std::int64_t raw = p_.notional * 10'000 / static_cast<std::int64_t>(px);
    return direction_ * (raw / p_.lot * p_.lot);
  }

  int direction() const noexcept { return direction_; }
  const TrendParams& params() const noexcept { return p_; }

 private:
  TrendParams p_;
  std::vector<std::uint32_t> ring_;
  std::size_t head_ = 0, count_ = 0;
  int direction_ = 0;
};

}  // namespace lab::strategy
