#pragma once

// Historical liquidity: replays a Nasdaq ITCH day into simex as resting orders
// from owner 0, so firm orders trade against a realistic book. Adds become
// orders, cancels and deletes take them off, and replaces move them. A real
// execution only ever names the resting order (the aggressor is not on the
// feed), so it becomes a reduction: the book stays the day's book, minus
// whatever firm orders took from it. Trades that never touched the visible
// book (P, Q) are ignored, and simex publishes its own system events.

#include <atomic>
#include <cstdint>
#include <string>

#include "matcher.hpp"

namespace simex {

struct LiquidityConfig {
  std::string path;
  double rate = 1'000'000;  // messages per second; 0 = unlimited
  double speed = 0;         // > 0: pace by ITCH timestamps instead
  std::uint64_t limit = 0;  // stop after this many messages
};

struct LiquidityStats {
  std::uint64_t messages = 0;
  std::uint64_t commands = 0;
  std::uint64_t ignored = 0;
  bool ok = true;
  std::string error;
};

// Thread body. Returns at end of file, at the limit, or when `stop` is set.
LiquidityStats replay_liquidity(const LiquidityConfig& cfg, CommandRing& out,
                                const std::atomic<bool>& stop);

}  // namespace simex
