#pragma once

// Implied prices for a two-leg strip (a lab extension: Nasdaq equities have
// no strips; futures exchanges do, and implied pricing is their core idea).
//
// A strip NAME=A+B is one unit of A plus one unit of B, priced as the sum,
// so its prices stay positive (a calendar spread A-B would not). Two kinds
// of implied liquidity follow from the books:
//
//   implied out (legs -> strip): buying the strip by lifting both legs' offers
//     strip ask = ask(A) + ask(B), shares = min of the two levels' shares;
//     strip bid = bid(A) + bid(B).
//   implied in (strip + one leg -> the other leg): a resting strip bid and
//     B's offer together bid for A
//     A bid = strip bid - ask(B);  A ask = strip ask - bid(B).
//
// simex trades incoming strip orders against implied-out liquidity (each
// fill lifts both legs at once); a feed handler rebuilds the same prices from
// the three books. Strip definitions are not on the ITCH feed: both sides
// take them from the same --strip NAME=A+B option.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lab/book/order_book.hpp"

namespace lab::book {

struct StripDef {
  std::string name;
  std::string leg_a;
  std::string leg_b;
};

// "NAME=A+B"; nullopt if malformed.
inline std::optional<StripDef> parse_strip(const std::string& s) {
  const auto eq = s.find('=');
  const auto plus = s.find('+', eq == std::string::npos ? 0 : eq);
  if (eq == std::string::npos || plus == std::string::npos || eq == 0 || plus == eq + 1 ||
      plus + 1 == s.size() || eq > 8)
    return std::nullopt;
  return StripDef{s.substr(0, eq), s.substr(eq + 1, plus - eq - 1), s.substr(plus + 1)};
}

struct ImpliedLevel {
  std::uint32_t price = 0;
  std::uint64_t shares = 0;
};

// Implied-out depth of one side of the strip from the legs' same sides
// (asks for the strip ask, bids for the strip bid), best first, up to
// `levels` levels. Walks both books consuming shares the way successive
// implied fills would, merging equal prices.
inline std::vector<ImpliedLevel> implied_out(const BookSide& a, const BookSide& b,
                                             std::size_t levels) {
  std::vector<ImpliedLevel> out;
  std::size_t i = 0, j = 0;
  std::uint64_t left_a = a.level(0) ? a.level(0)->shares : 0;
  std::uint64_t left_b = b.level(0) ? b.level(0)->shares : 0;
  while (i < a.depth() && j < b.depth()) {
    const std::uint32_t price = a.level(i)->price + b.level(j)->price;
    const std::uint64_t q = left_a < left_b ? left_a : left_b;
    if (!out.empty() && out.back().price == price) {
      out.back().shares += q;
    } else {
      if (out.size() == levels) break;
      out.push_back({price, q});
    }
    left_a -= q;
    left_b -= q;
    if (left_a == 0 && ++i < a.depth()) left_a = a.level(i)->shares;
    if (left_b == 0 && ++j < b.depth()) left_b = b.level(j)->shares;
  }
  return out;
}

// Implied-in top of book for one leg: its bid from the strip bid less the
// other leg's ask, its ask from the strip ask less the other leg's bid.
struct ImpliedQuote {
  std::optional<ImpliedLevel> bid;
  std::optional<ImpliedLevel> ask;
};

inline ImpliedQuote implied_in(const OrderBook& strip, const OrderBook& other) {
  ImpliedQuote q;
  const Level* sb = strip.bids().best();
  const Level* oa = other.asks().best();
  if (sb && oa && sb->price > oa->price) {
    q.bid = ImpliedLevel{sb->price - oa->price, sb->shares < oa->shares ? sb->shares : oa->shares};
  }
  const Level* sa = strip.asks().best();
  const Level* ob = other.bids().best();
  if (sa && ob && sa->price > ob->price) {
    q.ask = ImpliedLevel{sa->price - ob->price, sa->shares < ob->shares ? sa->shares : ob->shares};
  }
  return q;
}

}  // namespace lab::book
