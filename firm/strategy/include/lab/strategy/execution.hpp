#pragma once

// Execution helpers shared by the slower strategies, which decide a target
// position and leave it to these to get there.
//
// work_toward() turns the gap between a target and what the firm holds plus
// what it has working into one child order of at most max_child shares.
// pov_child() caps a child at a fraction of the volume the market traded since
// the last call: the participation-of-volume schedule a fundamental desk uses
// so its own buying does not move the price it pays.

#include <algorithm>
#include <cstdint>

namespace lab::strategy {

struct Child {
  char side = 0;  // 'B', 'S', or 0 for nothing to do
  std::uint32_t qty = 0;
};

// `position` is shares held; `open_buy` and `open_sell` are shares working.
inline Child work_toward(std::int64_t target, std::int64_t position, std::uint64_t open_buy,
                         std::uint64_t open_sell, std::uint32_t max_child,
                         std::uint32_t lot) noexcept {
  const std::int64_t if_all_fill_buy = position + static_cast<std::int64_t>(open_buy);
  const std::int64_t if_all_fill_sell = position - static_cast<std::int64_t>(open_sell);
  Child c;
  if (target > if_all_fill_buy) {
    c.side = 'B';
    c.qty = static_cast<std::uint32_t>(std::min<std::int64_t>(target - if_all_fill_buy, max_child));
  } else if (target < if_all_fill_sell) {
    c.side = 'S';
    c.qty =
        static_cast<std::uint32_t>(std::min<std::int64_t>(if_all_fill_sell - target, max_child));
  }
  c.qty = c.qty / lot * lot;
  if (c.qty == 0) c.side = 0;
  return c;
}

struct PovParams {
  std::uint32_t participation_bps = 600;  // 6% of the market's volume
  std::uint32_t lot = 100;
};

// Shares this interval may trade, given the market's volume in the last one.
inline std::uint32_t pov_child(const PovParams& p, std::uint64_t market_volume,
                               std::uint64_t remaining) noexcept {
  const std::uint64_t allowed = market_volume * p.participation_bps / 10'000;
  const std::uint64_t q = std::min(allowed, remaining);
  return static_cast<std::uint32_t>(q / p.lot * p.lot);
}

}  // namespace lab::strategy
