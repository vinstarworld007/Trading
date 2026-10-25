#pragma once

// Pre-trade risk, checked inline on the trading thread before an order or a
// replace leaves the firm. Every check is a few compares against the OMS's
// positions and exposure; nothing allocates or blocks. Cancels are never
// blocked: they only reduce risk.
//
// Checks, in order:
//   kill switch     set by an operator or by a breach elsewhere; blocks all
//   quantity        shares per order
//   notional        price x shares per order
//   price collar    limit price within collar_bps of the reference (the
//                   current mid), so a bad price never reaches the market
//   position        position plus every working share on the order's side,
//                   plus this order, stays within max_position either way
//   open orders     working orders across the firm
//   order rate      a token bucket of new orders and replaces per second

#include <atomic>
#include <cstdint>

#include "lab/oms/oms.hpp"

namespace lab::risk {

struct Limits {
  std::uint32_t max_order_qty = 1'000;
  std::uint64_t max_order_notional = 100'000ULL * 10'000;  // $100,000 in Price(4)
  std::uint32_t collar_bps = 500;                          // 5% from the reference
  std::int64_t max_position = 2'000;                       // shares, long or short
  std::uint32_t max_open_orders = 200;
  double max_orders_per_second = 2'000;
  double burst = 100;  // orders the bucket holds
};

enum class Verdict : std::uint8_t {
  kOk,
  kKill,
  kQuantity,
  kNotional,
  kCollar,
  kNoReference,
  kPosition,
  kOpenOrders,
  kRate,
  kCount
};

inline const char* verdict_name(Verdict v) noexcept {
  switch (v) {
    case Verdict::kOk:
      return "ok";
    case Verdict::kKill:
      return "kill switch";
    case Verdict::kQuantity:
      return "order quantity";
    case Verdict::kNotional:
      return "order notional";
    case Verdict::kCollar:
      return "price collar";
    case Verdict::kNoReference:
      return "no reference price";
    case Verdict::kPosition:
      return "position limit";
    case Verdict::kOpenOrders:
      return "open orders";
    case Verdict::kRate:
      return "order rate";
    case Verdict::kCount:
      break;
  }
  return "?";
}

class Risk {
 public:
  // `ticks_per_second` converts rdtsc ticks for the rate limit.
  Risk(const Limits& limits, double ticks_per_second) noexcept
      : limits_{limits},
        tokens_per_tick_{limits.max_orders_per_second / ticks_per_second},
        tokens_{limits.burst} {}

  // A new order of `qty` at `price`. `reference` is the current mid (0 if
  // there is no two-sided market).
  Verdict check_new(const oms::Oms& oms, std::uint16_t instrument, char side, std::uint32_t qty,
                    std::uint32_t price, std::uint32_t reference, std::uint64_t now_tsc) noexcept {
    return count(check(oms, instrument, side, qty, qty, price, reference, now_tsc));
  }

  // Replacing working order `old` with `open` shares at `price`. OUCH counts
  // a replace's quantity across the chain, so the chain can never fill more
  // than the larger of the two: only growth adds to the worst case.
  Verdict check_replace(const oms::Oms& oms, const oms::Order& old, std::uint32_t open,
                        std::uint32_t price, std::uint32_t reference,
                        std::uint64_t now_tsc) noexcept {
    const std::uint32_t growth = open > old.open ? open - old.open : 0;
    return count(check(oms, old.instrument, old.side, open, growth, price, reference, now_tsc));
  }

  void kill() noexcept { killed_.store(true, std::memory_order_relaxed); }
  bool killed() const noexcept { return killed_.load(std::memory_order_relaxed); }
  std::uint64_t blocked(Verdict v) const noexcept { return blocked_[static_cast<int>(v)]; }
  const Limits& limits() const noexcept { return limits_; }

 private:
  // `added` is how much the order can add to the worst-case position.
  Verdict check(const oms::Oms& oms, std::uint16_t instrument, char side, std::uint32_t qty,
                std::uint32_t added, std::uint32_t price, std::uint32_t reference,
                std::uint64_t now_tsc) noexcept {
    if (killed()) return Verdict::kKill;
    if (qty == 0 || qty > limits_.max_order_qty) return Verdict::kQuantity;
    if (static_cast<std::uint64_t>(qty) * price > limits_.max_order_notional) {
      return Verdict::kNotional;
    }
    if (reference == 0) return Verdict::kNoReference;
    const std::uint64_t band = static_cast<std::uint64_t>(reference) * limits_.collar_bps / 10'000;
    if (price + band < reference || price > reference + band) return Verdict::kCollar;
    const oms::Position& p = oms.position(instrument);
    if (side == 'B') {
      if (p.shares + static_cast<std::int64_t>(p.open_buy + added) > limits_.max_position) {
        return Verdict::kPosition;
      }
    } else if (p.shares - static_cast<std::int64_t>(p.open_sell + added) < -limits_.max_position) {
      return Verdict::kPosition;
    }
    if (oms.working() >= limits_.max_open_orders) return Verdict::kOpenOrders;
    refill(now_tsc);
    if (tokens_ < 1.0) return Verdict::kRate;
    tokens_ -= 1.0;
    return Verdict::kOk;
  }

  void refill(std::uint64_t now_tsc) noexcept {
    if (last_tsc_ != 0 && now_tsc > last_tsc_) {
      tokens_ += static_cast<double>(now_tsc - last_tsc_) * tokens_per_tick_;
      if (tokens_ > limits_.burst) tokens_ = limits_.burst;
    }
    last_tsc_ = now_tsc;
  }

  Verdict count(Verdict v) noexcept {
    ++blocked_[static_cast<int>(v)];
    return v;
  }

  Limits limits_;
  double tokens_per_tick_;
  double tokens_;
  std::uint64_t last_tsc_ = 0;
  std::atomic<bool> killed_{false};
  std::uint64_t blocked_[static_cast<int>(Verdict::kCount)] = {};
};

}  // namespace lab::risk
