#pragma once

// Rebuilds every Nasdaq order book from an ITCH 5.0 stream and checks the
// stream's integrity as it goes. This is the core of itch-dump and, later, of
// the live feed handler.
//
//   lab::book::BookBuilder builder;
//   while (auto msg = reader.next()) builder.apply(msg->data(), msg->size());
//   builder.checks();            // data quality counters
//   builder.book(locate);        // price levels for one instrument
//
// Book rules (ITCH 5.0 section 4.3): A/F add; E and C reduce the order, and an
// order executed down to zero leaves the book without a delete; X reduces;
// D removes; U removes the original and adds the new reference with the same
// side and instrument. P, Q and B are trades that never touch the visible book.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lab/book/order_book.hpp"
#include "lab/book/order_map.hpp"
#include "lab/itch/codec.hpp"
#include "lab/itch/messages.hpp"

namespace lab::book {

// Data quality counters. Everything except the informational fields at the
// bottom should be zero on a clean full-day file.
struct IntegrityChecks {
  std::uint64_t unknown_message_type = 0;
  std::uint64_t truncated_message = 0;
  std::uint64_t unknown_order_reference = 0;    // E, C, X, D or U for an order not on the book
  std::uint64_t duplicate_order_reference = 0;  // A, F or U reusing a live reference
  std::uint64_t over_execution = 0;             // executed or cancelled more shares than remained
  std::uint64_t locate_without_directory = 0;   // book message before its R message
  std::uint64_t locate_symbol_mismatch = 0;     // symbol in A/F differs from the R message
  std::uint64_t book_inconsistency = 0;         // level totals disagree with the orders (a bug)
  std::uint64_t timestamp_regressions = 0;      // timestamp lower than the previous message
  std::uint64_t system_event_out_of_order = 0;  // O S Q M E C sequence broken

  // Informational: not errors by themselves.
  std::uint64_t crossed_book_events = 0;  // a book became crossed (bid > ask)
  std::uint64_t locked_book_events = 0;   // a book became locked (bid == ask)

  std::uint64_t error_count() const noexcept {
    return unknown_message_type + truncated_message + unknown_order_reference +
           duplicate_order_reference + over_execution + locate_without_directory +
           locate_symbol_mismatch + book_inconsistency + timestamp_regressions +
           system_event_out_of_order;
  }
};

struct InstrumentStats {
  std::uint64_t adds = 0;
  std::uint64_t executions = 0;
  std::uint64_t cancels = 0;
  std::uint64_t deletes = 0;
  std::uint64_t replaces = 0;
  std::uint64_t executed_shares = 0;       // E plus printable C
  std::uint64_t non_printable_shares = 0;  // C with printable = N
  std::uint64_t hidden_trade_shares = 0;   // P
  std::uint64_t cross_shares = 0;          // Q
  std::uint64_t broken_trades = 0;         // B; shares not netted out
  std::uint32_t last_trade_price = 0;
  std::uint32_t opening_cross_price = 0;
  std::uint32_t closing_cross_price = 0;

  // All shares Nasdaq matched for this instrument (E + printable C + P + Q).
  std::uint64_t matched_volume() const noexcept {
    return executed_shares + hidden_trade_shares + cross_shares;
  }
};

struct Instrument {
  bool listed = false;  // saw its stock directory message
  itch::Symbol symbol{};
  std::uint32_t round_lot_size = 0;
  char trading_state = ' ';  // from H: H, P, Q, T
  char reg_sho_action = ' ';
  bool crossed = false;
  bool locked = false;
  OrderBook book;
  InstrumentStats stats;
};

class BookBuilder : public itch::HandlerBase {
 public:
  explicit BookBuilder(std::size_t expected_live_orders = 1u << 22);

  // Applies one raw ITCH message.
  void apply(const std::byte* msg, std::size_t len) {
    switch (itch::dispatch(msg, len, *this)) {
      case itch::DispatchResult::kOk:
        break;
      case itch::DispatchResult::kUnknownType:
        ++checks_.unknown_message_type;
        break;
      case itch::DispatchResult::kTooShort:
        ++checks_.truncated_message;
        break;
    }
    ++messages_;
    if (len > 0) type_counts_[static_cast<unsigned char>(msg[0])]++;
  }
  void apply(std::span<const std::byte> msg) { apply(msg.data(), msg.size()); }

  // Message handlers, called by dispatch().
  using HandlerBase::on;
  void on(const itch::SystemEvent& m);
  void on(const itch::StockDirectory& m);
  void on(const itch::StockTradingAction& m);
  void on(const itch::RegShoRestriction& m);
  void on(const itch::AddOrder& m);
  void on(const itch::AddOrderMpid& m);
  void on(const itch::OrderExecuted& m);
  void on(const itch::OrderExecutedWithPrice& m);
  void on(const itch::OrderCancel& m);
  void on(const itch::OrderDelete& m);
  void on(const itch::OrderReplace& m);
  void on(const itch::Trade& m);
  void on(const itch::CrossTrade& m);
  void on(const itch::BrokenTrade& m);

  const IntegrityChecks& checks() const noexcept { return checks_; }
  const Instrument& instrument(std::uint16_t locate) const noexcept { return instruments_[locate]; }
  const OrderBook& book(std::uint16_t locate) const noexcept { return instruments_[locate].book; }
  // Locate code for a symbol, or 0 if not listed. Linear scan; not for hot paths.
  std::uint16_t find_locate(std::string_view symbol) const noexcept;

  const OrderMap& orders() const noexcept { return orders_; }
  std::uint64_t messages() const noexcept { return messages_; }
  std::uint64_t type_count(char type) const noexcept {
    return type_counts_[static_cast<unsigned char>(type)];
  }
  const std::string& system_events() const noexcept { return system_events_; }
  std::uint64_t last_timestamp() const noexcept { return last_timestamp_; }

  // Locates with a stock directory message, in locate order.
  std::vector<std::uint16_t> listed_locates() const;

 private:
  void see(const itch::Header& h) noexcept {
    if (h.timestamp < last_timestamp_)
      ++checks_.timestamp_regressions;
    else
      last_timestamp_ = h.timestamp;
  }
  Instrument* listed(std::uint16_t locate) noexcept;
  void add_order(const itch::Header& h, std::uint64_t ref, char side, std::uint32_t shares,
                 const itch::Symbol& stock, std::uint32_t price);
  // Removes `shares` from a live order (all of it if `remove_all`); erases it
  // at zero. Returns its instrument and, if asked, the order's price.
  Instrument* reduce_order(std::uint64_t ref, std::uint32_t shares, bool remove_all,
                           std::uint32_t* price_out);
  void check_cross(Instrument& inst) noexcept;

  std::vector<Instrument> instruments_;  // indexed by stock locate (0..65535)
  OrderMap orders_;
  IntegrityChecks checks_;
  std::array<std::uint64_t, 256> type_counts_{};
  std::uint64_t messages_ = 0;
  std::uint64_t last_timestamp_ = 0;
  std::string system_events_;
};

}  // namespace lab::book
