#pragma once

// Generates a synthetic but internally consistent ITCH 5.0 trading day, for
// tests, benchmarks and trying the tools without a multi-GB sample file.
//
// The day has the real structure (system events, stock directory, trading
// actions, opening and closing crosses) and a random mix of adds, executions,
// cancels, deletes, replaces and hidden trades. Bids always sit below each
// symbol's fixed mid and asks above, so books never cross. The generator keeps
// its own model of every live order, so a consumer's rebuilt books can be
// compared against it exactly.

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace lab::itch {

struct SyntheticConfig {
  std::uint32_t symbols = 50;
  std::uint64_t book_messages = 1'000'000;  // order and trade messages, excluding setup
  std::uint64_t seed = 42;
};

class SyntheticDay {
 public:
  struct LiveOrder {
    std::uint16_t locate;
    char side;  // 'B' or 'S'
    std::uint32_t price;
    std::uint32_t shares;
  };

  struct SymbolTotals {
    std::uint64_t executed_shares = 0;      // E plus printable C
    std::uint64_t hidden_trade_shares = 0;  // P
    std::uint64_t cross_shares = 0;         // Q
  };

  explicit SyntheticDay(SyntheticConfig config);

  // Calls `sink` once per encoded message, in order.
  void generate(const std::function<void(std::span<const std::byte>)>& sink);

  // Model state after generate().
  const std::unordered_map<std::uint64_t, LiveOrder>& live_orders() const { return live_; }
  const SymbolTotals& totals(std::uint16_t locate) const { return totals_.at(locate); }
  std::uint64_t messages_written() const { return written_; }

 private:
  SyntheticConfig config_;
  std::unordered_map<std::uint64_t, LiveOrder> live_;
  std::vector<std::vector<std::uint64_t>> refs_by_locate_;  // live refs, for random picks
  std::vector<SymbolTotals> totals_;
  std::uint64_t written_ = 0;
};

}  // namespace lab::itch
