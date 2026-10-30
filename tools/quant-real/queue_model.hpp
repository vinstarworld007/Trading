#pragma once

// Fills for orders a backtest places against a historical feed. The orders
// never reach the real book, so the replay cannot fill them; this model
// decides when they would have filled, from the feed alone.
//
// A resting order joins the back of its price level: `ahead` starts at the
// shares displayed there. Nasdaq fills a level in time priority, so every
// execution at our price comes out of `ahead` first and only the rest fills
// us. Cancels can come from anywhere in the queue; we assume the worst case
// (behind us) unless the level shrinks below `ahead`, which caps it. An
// execution at a price worse than ours, or a resting order on the other side
// at or through our price, means the market traded through us: we fill in full.
//
// The model ignores our own impact: real orders never trade against ours, and
// our marketable orders take displayed size without removing it.

#include <algorithm>
#include <cstdint>

namespace lab::backtest {

struct ShadowOrder {
  std::uint32_t ref = 0;  // OMS UserRefNum
  char side = 0;          // 'B' or 'S'
  std::uint32_t price = 0;
  std::uint32_t open = 0;   // shares still working
  std::uint64_t ahead = 0;  // displayed shares in front of us at our price
};

struct ShadowFill {
  std::uint32_t qty = 0;
  std::uint32_t price = 0;
};

// `ahead` for an order placed now, given the displayed shares at its price.
inline ShadowOrder place(std::uint32_t ref, char side, std::uint32_t price, std::uint32_t qty,
                         std::uint64_t level_shares) noexcept {
  return {ref, side, price, qty, level_shares};
}

// An execution of `shares` against a resting order on `resting_side` at `price`.
inline ShadowFill on_execution(ShadowOrder& o, char resting_side, std::uint32_t price,
                               std::uint32_t shares) noexcept {
  if (o.open == 0 || resting_side != o.side) return {};
  const bool through = o.side == 'B' ? price < o.price : price > o.price;
  if (through) {
    const ShadowFill f{o.open, o.price};
    o.open = 0;
    o.ahead = 0;
    return f;
  }
  if (price != o.price) return {};
  if (shares <= o.ahead) {
    o.ahead -= shares;
    return {};
  }
  const auto q = static_cast<std::uint32_t>(std::min<std::uint64_t>(o.open, shares - o.ahead));
  o.ahead = 0;
  o.open -= q;
  return {q, o.price};
}

// After any change to our level: the queue ahead of us cannot exceed what is there.
inline void on_level(ShadowOrder& o, std::uint64_t level_shares) noexcept {
  o.ahead = std::min(o.ahead, level_shares);
}

// The best price on the other side reached ours: we would have traded with it.
inline ShadowFill on_opposite_best(ShadowOrder& o, std::uint32_t best_px,
                                   std::uint64_t best_shares) noexcept {
  if (o.open == 0 || best_px == 0 || best_shares == 0) return {};
  const bool crossed = o.side == 'B' ? best_px <= o.price : best_px >= o.price;
  if (!crossed) return {};
  const auto q = static_cast<std::uint32_t>(std::min<std::uint64_t>(o.open, best_shares));
  o.open -= q;
  return {q, best_px};
}

}  // namespace lab::backtest
