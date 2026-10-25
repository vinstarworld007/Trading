#pragma once

// A one-level market maker. It quotes both sides of each instrument at the
// best prices the rest of the market shows (one tick inside when the spread is
// wide), leans both quotes against its position so fills pull it back toward
// flat, and stops quoting the side that would take it past its position cap.
// Pure functions of the market and the position: the trading thread decides
// how to turn a quote into orders.

#include <cstdint>

#include "lab/oms/messages.hpp"

namespace lab::strategy {

struct MakerParams {
  std::uint32_t quote_qty = 100;
  std::uint32_t tick = 100;                // Price(4): one cent
  std::int64_t max_position = 1'000;       // shares either way before a side stops
  std::uint32_t improve_spread_ticks = 3;  // step inside when the spread is at least this
  std::uint32_t skew_ticks = 1;            // ticks of lean per quote_qty of position
  // Leave a working order alone while it is within this many ticks of the
  // quote: every replace goes to the back of the queue.
  std::uint32_t hold_ticks = 1;
};

// Whether an order resting at `working` should move to `wanted`.
inline bool should_move(const MakerParams& p, std::uint32_t working,
                        std::uint32_t wanted) noexcept {
  const std::uint32_t diff = working > wanted ? working - wanted : wanted - working;
  return diff >= p.hold_ticks * p.tick;
}

// The best prices of everyone but us; 0 when a side is empty.
struct Market {
  std::uint32_t bid = 0;
  std::uint32_t ask = 0;
  std::uint32_t mid() const noexcept { return bid != 0 && ask > bid ? (bid + ask) / 2 : 0; }
};

struct Quote {
  std::uint32_t bid_px = 0, bid_qty = 0;  // qty 0: no bid
  std::uint32_t ask_px = 0, ask_qty = 0;
};

// Removes our own resting order (price, open shares; open 0 if none) from the
// top two levels of one side.
inline std::uint32_t best_of_others(const std::uint32_t px[2], const std::uint32_t qty[2],
                                    std::uint32_t own_px, std::uint32_t own_open) noexcept {
  if (qty[0] == 0) return 0;
  if (own_open == 0 || px[0] != own_px || qty[0] > own_open) return px[0];
  return qty[1] != 0 ? px[1] : 0;
}

inline Market others(const oms::TopOfBook& t, std::uint32_t own_bid_px, std::uint32_t own_bid_open,
                     std::uint32_t own_ask_px, std::uint32_t own_ask_open) noexcept {
  return {best_of_others(t.bid_px, t.bid_qty, own_bid_px, own_bid_open),
          best_of_others(t.ask_px, t.ask_qty, own_ask_px, own_ask_open)};
}

class MarketMaker {
 public:
  explicit MarketMaker(const MakerParams& p) noexcept : p_{p} {}

  Quote quote(const Market& m, std::int64_t position) const noexcept {
    Quote q;
    if (m.bid == 0 || m.ask == 0 || m.ask <= m.bid) return q;  // no two-sided market
    std::int64_t bid = m.bid;
    std::int64_t ask = m.ask;
    const std::int64_t tick = p_.tick;
    if (static_cast<std::uint32_t>((m.ask - m.bid) / p_.tick) >= p_.improve_spread_ticks) {
      bid += tick;
      ask -= tick;
    }
    const std::int64_t lean =
        position / static_cast<std::int64_t>(p_.quote_qty) * p_.skew_ticks * tick;
    bid -= lean;
    ask -= lean;
    // Stay passive: never cross the others' best.
    if (ask <= m.bid) ask = m.bid + tick;
    if (bid >= m.ask) bid = m.ask - tick;
    if (bid >= ask || bid <= 0) return q;
    const auto size = static_cast<std::int64_t>(p_.quote_qty);
    if (position + size <= p_.max_position) {
      q.bid_px = static_cast<std::uint32_t>(bid);
      q.bid_qty = p_.quote_qty;
    }
    if (position - size >= -p_.max_position) {
      q.ask_px = static_cast<std::uint32_t>(ask);
      q.ask_qty = p_.quote_qty;
    }
    return q;
  }

  const MakerParams& params() const noexcept { return p_; }

 private:
  MakerParams p_;
};

}  // namespace lab::strategy
