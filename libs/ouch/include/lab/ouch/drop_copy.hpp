#pragma once

// simex's drop copy: one Execution Report per firm execution, sent over
// SoupBinTCP to the drop copy account. A lab protocol, not a Nasdaq one; the
// layout is in docs/specs/dropcopy-layout.md. Encoded and decoded with the
// OUCH codec (lab/ouch/codec.hpp), which drives both from visit().

#include <array>
#include <cstddef>
#include <cstdint>

#include "lab/ouch/messages.hpp"

namespace lab::dropcopy {

struct ExecutionReport {
  static constexpr char kType = 'D';
  static constexpr std::size_t kSize = 57;
  std::uint64_t timestamp = 0;
  std::array<char, 6> account{};
  std::uint32_t user_ref_num = 0;
  char side = 'B';
  std::uint32_t quantity = 0;
  ouch::Symbol symbol{};
  std::uint64_t price = 0;
  std::uint64_t match_number = 0;
  char liquidity_flag = 'A';
  std::uint64_t order_reference_number = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.account);
    f(15, m.user_ref_num);
    f(19, m.side);
    f(20, m.quantity);
    f(24, m.symbol);
    f(32, m.price);
    f(40, m.match_number);
    f(48, m.liquidity_flag);
    f(49, m.order_reference_number);
  }
};

}  // namespace lab::dropcopy
