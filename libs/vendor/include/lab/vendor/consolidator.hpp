#pragma once

// The ticker plant inside every lab market data vendor: rebuilds each venue
// book from the venue's ITCH feed with book::BookBuilder and keeps one
// normalized Level 1 record per instrument (best bid and offer, last trade,
// day statistics, trading status). After each message it reports which
// fields of which record changed, so a distribution protocol can send an
// image, an update with just those fields, or nothing.
//
//   Consolidator c;
//   c.apply(msg, [&](const Record& r, std::uint32_t changed) { ... });
//
// Records are preallocated for every possible locate; apply() never
// allocates.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/core/endian.hpp"
#include "lab/itch/messages.hpp"

namespace lab::vendor {

// Bits of Record that changed.
namespace field {
inline constexpr std::uint32_t kBid = 1u << 0;
inline constexpr std::uint32_t kBidSize = 1u << 1;
inline constexpr std::uint32_t kAsk = 1u << 2;
inline constexpr std::uint32_t kAskSize = 1u << 3;
inline constexpr std::uint32_t kLast = 1u << 4;
inline constexpr std::uint32_t kLastSize = 1u << 5;
inline constexpr std::uint32_t kVolume = 1u << 6;
inline constexpr std::uint32_t kOpen = 1u << 7;
inline constexpr std::uint32_t kHigh = 1u << 8;
inline constexpr std::uint32_t kLow = 1u << 9;
inline constexpr std::uint32_t kTrades = 1u << 10;
inline constexpr std::uint32_t kStatus = 1u << 11;
inline constexpr std::uint32_t kQuote = kBid | kBidSize | kAsk | kAskSize;
inline constexpr std::uint32_t kTrade =
    kLast | kLastSize | kVolume | kOpen | kHigh | kLow | kTrades;
inline constexpr std::uint32_t kAll = kQuote | kTrade | kStatus;
}  // namespace field

// One instrument's normalized Level 1 view. Prices are ITCH Price(4); 0 means
// no value (an empty side, no trade yet).
struct Record {
  std::uint16_t locate = 0;
  itch::Symbol symbol{};
  bool listed = false;
  char status = 'T';  // T trading, H halted, P paused, Q quotation only
  std::uint32_t bid = 0;
  std::uint64_t bid_size = 0;
  std::uint32_t ask = 0;
  std::uint64_t ask_size = 0;
  std::uint32_t last = 0;
  std::uint64_t last_size = 0;  // shares in the most recent trade message
  std::uint64_t volume = 0;     // matched volume for the day
  std::uint32_t open = 0;       // first trade, or the opening cross
  std::uint32_t high = 0;
  std::uint32_t low = 0;
  std::uint64_t trades = 0;    // trade messages (E, printable C, P, Q with shares)
  std::uint64_t quote_ns = 0;  // ITCH time of the last quote change
  std::uint64_t trade_ns = 0;  // ITCH time of the last trade
  std::uint64_t exch_ns = 0;   // ITCH time of the last change of any kind
  std::uint64_t updates = 0;   // changes published, for a per-record sequence
};

class Consolidator {
 public:
  explicit Consolidator(std::size_t expected_live_orders = 1u << 22)
      : books_{std::make_unique<book::BookBuilder>(expected_live_orders)}, records_(65536) {}

  // Applies one ITCH message and calls changed(const Record&, std::uint32_t
  // mask) if it changed the record of its instrument.
  template <class Changed>
  void apply(std::span<const std::byte> msg, Changed&& changed) {
    books_->apply(msg.data(), msg.size());
    if (msg.size() < 11) return;
    const char type = static_cast<char>(msg[0]);
    const std::uint16_t locate = load_be16(msg.data() + 1);
    if (locate == 0) return;  // system events and MWCB messages
    const std::uint64_t ts = load_be48(msg.data() + 5);
    const book::Instrument& inst = books_->instrument(locate);
    Record& r = records_[locate];
    if (!r.listed) {
      if (!inst.listed) return;
      r.listed = true;
      r.locate = locate;
      r.symbol = inst.symbol;
      if (type == 'R') return;  // nothing to say until there is a quote or a trade
    }
    std::uint32_t mask = 0;
    const book::Level* b = inst.book.bids().best();
    const book::Level* a = inst.book.asks().best();
    const std::uint32_t bid = b ? b->price : 0, ask = a ? a->price : 0;
    const std::uint64_t bsz = b ? b->shares : 0, asz = a ? a->shares : 0;
    if (bid != r.bid) mask |= field::kBid;
    if (bsz != r.bid_size) mask |= field::kBidSize;
    if (ask != r.ask) mask |= field::kAsk;
    if (asz != r.ask_size) mask |= field::kAskSize;
    if (mask != 0) {
      r.bid = bid;
      r.bid_size = bsz;
      r.ask = ask;
      r.ask_size = asz;
      r.quote_ns = ts;
    }
    const std::uint64_t volume = inst.stats.matched_volume();
    if (volume != r.volume) {
      const std::uint32_t px = inst.stats.last_trade_price;
      r.last_size = volume - r.volume;
      r.volume = volume;
      mask |= field::kLastSize | field::kVolume | field::kTrades;
      ++r.trades;
      if (px != r.last) mask |= field::kLast;
      r.last = px;
      if (r.open == 0) {
        r.open = inst.stats.opening_cross_price != 0 ? inst.stats.opening_cross_price : px;
        mask |= field::kOpen;
      }
      if (px > r.high) {
        r.high = px;
        mask |= field::kHigh;
      }
      if (r.low == 0 || px < r.low) {
        r.low = px;
        mask |= field::kLow;
      }
      r.trade_ns = ts;
    }
    const char status = inst.trading_state == ' ' ? 'T' : inst.trading_state;
    if (status != r.status) {
      r.status = status;
      mask |= field::kStatus;
    }
    if (mask == 0) return;
    r.exch_ns = ts;
    ++r.updates;
    changed(static_cast<const Record&>(r), mask);
  }

  const Record& record(std::uint16_t locate) const noexcept { return records_[locate]; }
  // Locate for a symbol, or 0 if no record is listed under it. Linear scan:
  // for subscription requests, not for market data.
  std::uint16_t find(std::string_view symbol) const noexcept {
    for (std::size_t l = 1; l < records_.size(); ++l) {
      const Record& r = records_[l];
      if (r.listed && itch::trimmed(r.symbol) == symbol) return static_cast<std::uint16_t>(l);
    }
    return 0;
  }
  // Every listed record, in locate order.
  template <class F>
  void for_each(F&& f) const {
    for (const Record& r : records_) {
      if (r.listed) f(r);
    }
  }
  const book::BookBuilder& books() const noexcept { return *books_; }

 private:
  std::unique_ptr<book::BookBuilder> books_;
  std::vector<Record> records_;  // by locate
};

// Which kind of update a change mask describes, for protocols that label
// updates (OMM UpdateType, BLPAPI MKTDATA_EVENT_TYPE, ICF event type).
enum class Kind : std::uint8_t { kQuote, kTrade, kStatus };
inline Kind kind_of(std::uint32_t mask) noexcept {
  if (mask & field::kTrade) return Kind::kTrade;
  if (mask & field::kQuote) return Kind::kQuote;
  return Kind::kStatus;
}

}  // namespace lab::vendor
