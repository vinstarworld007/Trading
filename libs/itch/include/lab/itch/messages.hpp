#pragma once

// Nasdaq TotalView-ITCH 5.0 messages, decoded into host-order structs.
//
// Layouts follow the official specification (see docs/specs/itch50-layout.md
// for the field tables and source). Each struct lists its wire fields once in
// visit(); codec.hpp uses that single list to both encode and decode, and a
// static_assert checks the list ends exactly at the message's spec length.
//
// Prices are Price(4) integers (1/10000 dollar) unless noted; MWCB levels are
// Price(8) (1/100000000 dollar). Timestamps are nanoseconds since midnight.
// Alpha fields are space-padded on the right.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lab::itch {

using Symbol = std::array<char, 8>;
using Mpid = std::array<char, 4>;

// Symbol without its right padding.
inline std::string_view trimmed(const Symbol& s) noexcept {
  std::size_t n = s.size();
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\0')) --n;
  return {s.data(), n};
}

// Builds a space-padded symbol from text; longer input is cut to 8 chars.
inline Symbol make_symbol(std::string_view text) noexcept {
  Symbol s;
  s.fill(' ');
  for (std::size_t i = 0; i < s.size() && i < text.size(); ++i) s[i] = text[i];
  return s;
}

// Fields every message carries at offsets 1, 3 and 5.
struct Header {
  std::uint16_t stock_locate = 0;
  std::uint16_t tracking_number = 0;
  std::uint64_t timestamp = 0;  // 6 bytes on the wire
};

#define LAB_ITCH_MESSAGE(type_char, size)    \
  static constexpr char kType = type_char;   \
  static constexpr std::size_t kSize = size; \
  Header header

struct SystemEvent {
  LAB_ITCH_MESSAGE('S', 12);
  char event_code = ' ';  // O, S, Q, M, E, C

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.event_code);
  }
};

struct StockDirectory {
  LAB_ITCH_MESSAGE('R', 39);
  Symbol stock{};
  char market_category = ' ';
  char financial_status_indicator = ' ';
  std::uint32_t round_lot_size = 0;
  char round_lots_only = ' ';
  char issue_classification = ' ';
  std::array<char, 2> issue_sub_type{};
  char authenticity = ' ';
  char short_sale_threshold_indicator = ' ';
  char ipo_flag = ' ';
  char luld_reference_price_tier = ' ';
  char etp_flag = ' ';
  std::uint32_t etp_leverage_factor = 0;
  char inverse_indicator = ' ';

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.market_category);
    f(20, m.financial_status_indicator);
    f(21, m.round_lot_size);
    f(25, m.round_lots_only);
    f(26, m.issue_classification);
    f(27, m.issue_sub_type);
    f(29, m.authenticity);
    f(30, m.short_sale_threshold_indicator);
    f(31, m.ipo_flag);
    f(32, m.luld_reference_price_tier);
    f(33, m.etp_flag);
    f(34, m.etp_leverage_factor);
    f(38, m.inverse_indicator);
  }
};

struct StockTradingAction {
  LAB_ITCH_MESSAGE('H', 25);
  Symbol stock{};
  char trading_state = ' ';  // H halted, P paused, Q quotation only, T trading
  char reserved = ' ';
  std::array<char, 4> reason{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.trading_state);
    f(20, m.reserved);
    f(21, m.reason);
  }
};

struct RegShoRestriction {
  LAB_ITCH_MESSAGE('Y', 20);
  Symbol stock{};
  char reg_sho_action = ' ';  // 0 none, 1 intraday drop, 2 restriction in effect

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.reg_sho_action);
  }
};

struct MarketParticipantPosition {
  LAB_ITCH_MESSAGE('L', 26);
  Mpid mpid{};
  Symbol stock{};
  char primary_market_maker = ' ';
  char market_maker_mode = ' ';
  char market_participant_state = ' ';

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.mpid);
    f(15, m.stock);
    f(23, m.primary_market_maker);
    f(24, m.market_maker_mode);
    f(25, m.market_participant_state);
  }
};

struct MwcbDeclineLevel {
  LAB_ITCH_MESSAGE('V', 35);
  std::uint64_t level1 = 0;  // Price(8)
  std::uint64_t level2 = 0;
  std::uint64_t level3 = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.level1);
    f(19, m.level2);
    f(27, m.level3);
  }
};

struct MwcbStatus {
  LAB_ITCH_MESSAGE('W', 12);
  char breached_level = ' ';  // 1, 2, 3

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.breached_level);
  }
};

struct IpoQuotingPeriod {
  LAB_ITCH_MESSAGE('K', 28);
  Symbol stock{};
  std::uint32_t ipo_quotation_release_time = 0;  // seconds since midnight
  char ipo_quotation_release_qualifier = ' ';
  std::uint32_t ipo_price = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.ipo_quotation_release_time);
    f(23, m.ipo_quotation_release_qualifier);
    f(24, m.ipo_price);
  }
};

struct LuldAuctionCollar {
  LAB_ITCH_MESSAGE('J', 35);
  Symbol stock{};
  std::uint32_t reference_price = 0;
  std::uint32_t upper_collar_price = 0;
  std::uint32_t lower_collar_price = 0;
  std::uint32_t collar_extension = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.reference_price);
    f(23, m.upper_collar_price);
    f(27, m.lower_collar_price);
    f(31, m.collar_extension);
  }
};

struct OperationalHalt {
  LAB_ITCH_MESSAGE('h', 21);
  Symbol stock{};
  char market_code = ' ';
  char operational_halt_action = ' ';  // H halted, T resumed

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.market_code);
    f(20, m.operational_halt_action);
  }
};

struct AddOrder {
  LAB_ITCH_MESSAGE('A', 36);
  std::uint64_t order_reference_number = 0;
  char buy_sell_indicator = ' ';  // B or S
  std::uint32_t shares = 0;
  Symbol stock{};
  std::uint32_t price = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
    f(19, m.buy_sell_indicator);
    f(20, m.shares);
    f(24, m.stock);
    f(32, m.price);
  }
};

struct AddOrderMpid {
  LAB_ITCH_MESSAGE('F', 40);
  std::uint64_t order_reference_number = 0;
  char buy_sell_indicator = ' ';
  std::uint32_t shares = 0;
  Symbol stock{};
  std::uint32_t price = 0;
  Mpid attribution{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
    f(19, m.buy_sell_indicator);
    f(20, m.shares);
    f(24, m.stock);
    f(32, m.price);
    f(36, m.attribution);
  }
};

struct OrderExecuted {
  LAB_ITCH_MESSAGE('E', 31);
  std::uint64_t order_reference_number = 0;
  std::uint32_t executed_shares = 0;
  std::uint64_t match_number = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
    f(19, m.executed_shares);
    f(23, m.match_number);
  }
};

struct OrderExecutedWithPrice {
  LAB_ITCH_MESSAGE('C', 36);
  std::uint64_t order_reference_number = 0;
  std::uint32_t executed_shares = 0;
  std::uint64_t match_number = 0;
  char printable = ' ';  // Y counts toward volume, N does not
  std::uint32_t execution_price = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
    f(19, m.executed_shares);
    f(23, m.match_number);
    f(31, m.printable);
    f(32, m.execution_price);
  }
};

struct OrderCancel {
  LAB_ITCH_MESSAGE('X', 23);
  std::uint64_t order_reference_number = 0;
  std::uint32_t cancelled_shares = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
    f(19, m.cancelled_shares);
  }
};

struct OrderDelete {
  LAB_ITCH_MESSAGE('D', 19);
  std::uint64_t order_reference_number = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
  }
};

struct OrderReplace {
  LAB_ITCH_MESSAGE('U', 35);
  std::uint64_t original_order_reference_number = 0;
  std::uint64_t new_order_reference_number = 0;
  std::uint32_t shares = 0;
  std::uint32_t price = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.original_order_reference_number);
    f(19, m.new_order_reference_number);
    f(27, m.shares);
    f(31, m.price);
  }
};

// Execution against a non-displayed order; does not change the visible book.
struct Trade {
  LAB_ITCH_MESSAGE('P', 44);
  std::uint64_t order_reference_number = 0;
  char buy_sell_indicator = ' ';
  std::uint32_t shares = 0;
  Symbol stock{};
  std::uint32_t price = 0;
  std::uint64_t match_number = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.order_reference_number);
    f(19, m.buy_sell_indicator);
    f(20, m.shares);
    f(24, m.stock);
    f(32, m.price);
    f(36, m.match_number);
  }
};

struct CrossTrade {
  LAB_ITCH_MESSAGE('Q', 40);
  std::uint64_t shares = 0;
  Symbol stock{};
  std::uint32_t cross_price = 0;
  std::uint64_t match_number = 0;
  char cross_type = ' ';  // O opening, C closing, H halt/IPO, I intraday

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.shares);
    f(19, m.stock);
    f(27, m.cross_price);
    f(31, m.match_number);
    f(39, m.cross_type);
  }
};

struct BrokenTrade {
  LAB_ITCH_MESSAGE('B', 19);
  std::uint64_t match_number = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.match_number);
  }
};

struct Noii {
  LAB_ITCH_MESSAGE('I', 50);
  std::uint64_t paired_shares = 0;
  std::uint64_t imbalance_shares = 0;
  char imbalance_direction = ' ';
  Symbol stock{};
  std::uint32_t far_price = 0;
  std::uint32_t near_price = 0;
  std::uint32_t current_reference_price = 0;
  char cross_type = ' ';
  char price_variation_indicator = ' ';

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.paired_shares);
    f(19, m.imbalance_shares);
    f(27, m.imbalance_direction);
    f(28, m.stock);
    f(36, m.far_price);
    f(40, m.near_price);
    f(44, m.current_reference_price);
    f(48, m.cross_type);
    f(49, m.price_variation_indicator);
  }
};

struct Rpii {
  LAB_ITCH_MESSAGE('N', 20);
  Symbol stock{};
  char interest_flag = ' ';

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.interest_flag);
  }
};

struct DirectListingCapitalRaise {
  LAB_ITCH_MESSAGE('O', 48);
  Symbol stock{};
  char open_eligibility_status = ' ';
  std::uint32_t minimum_allowable_price = 0;
  std::uint32_t maximum_allowable_price = 0;
  std::uint32_t near_execution_price = 0;
  std::uint64_t near_execution_time = 0;
  std::uint32_t lower_price_range_collar = 0;
  std::uint32_t upper_price_range_collar = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(11, m.stock);
    f(19, m.open_eligibility_status);
    f(20, m.minimum_allowable_price);
    f(24, m.maximum_allowable_price);
    f(28, m.near_execution_price);
    f(32, m.near_execution_time);
    f(40, m.lower_price_range_collar);
    f(44, m.upper_price_range_collar);
  }
};

#undef LAB_ITCH_MESSAGE

// Largest message in the spec; a reader's scratch buffer must hold at least this.
inline constexpr std::size_t kMaxMessageSize = Noii::kSize;

}  // namespace lab::itch
