#pragma once

// Nasdaq OUCH 5.0 messages the lab uses, decoded into host-order structs.
//
// Layouts follow docs/specs/ouch50-layout.md. As with ITCH, each struct lists
// its wire fields once in visit(), codec.hpp drives encode and decode from
// that list, and a compile-time check fails the build if the list does not end
// at the spec length. Every message ends with Appendage Length; the lab sends
// no optional fields and skips any it receives.
//
// Prices are OUCH Price: 8 bytes, 4 implied decimals.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lab::ouch {

using Symbol = std::array<char, 8>;
using ClOrdId = std::array<char, 14>;

inline constexpr std::uint64_t kMarketPrice = 0x7FFFFFFF;  // 214,748.3647: market order
inline constexpr std::uint64_t kMaxPrice = 1'999'999'900;  // 199,999.9900

template <std::size_t N>
std::array<char, N> make_alpha(std::string_view text) noexcept {
  std::array<char, N> a;
  a.fill(' ');
  for (std::size_t i = 0; i < N && i < text.size(); ++i) a[i] = text[i];
  return a;
}

template <std::size_t N>
std::string_view trimmed(const std::array<char, N>& a) noexcept {
  std::size_t n = N;
  while (n > 0 && (a[n - 1] == ' ' || a[n - 1] == '\0')) --n;
  return {a.data(), n};
}

namespace side {
inline constexpr char kBuy = 'B';
inline constexpr char kSell = 'S';
inline constexpr char kSellShort = 'T';
inline constexpr char kSellShortExempt = 'E';
}  // namespace side

namespace tif {
inline constexpr char kDay = '0';
inline constexpr char kIoc = '3';
}  // namespace tif

namespace cancel_reason {
inline constexpr char kUser = 'U';
inline constexpr char kIoc = 'I';
inline constexpr char kSupervisory = 'S';
inline constexpr char kSystem = 'Z';
}  // namespace cancel_reason

namespace liquidity {
inline constexpr char kAdded = 'A';
inline constexpr char kRemoved = 'R';
}  // namespace liquidity

namespace reject {
inline constexpr std::uint16_t kFatFinger = 0x0006;
inline constexpr std::uint16_t kHalted = 0x0007;
inline constexpr std::uint16_t kInvalidSide = 0x0009;
inline constexpr std::uint16_t kProcessingError = 0x000A;
inline constexpr std::uint16_t kOther = 0x000F;
inline constexpr std::uint16_t kInvalidQuantity = 0x0013;
inline constexpr std::uint16_t kReplaceNotAllowed = 0x0015;
inline constexpr std::uint16_t kInvalidSymbol = 0x0017;
inline constexpr std::uint16_t kInvalidPrice = 0x001D;
}  // namespace reject

#define LAB_OUCH_MESSAGE(type_char, size)    \
  static constexpr char kType = type_char;   \
  static constexpr std::size_t kSize = size; \
  std::uint16_t appendage_length = 0

// ---- Inbound -------------------------------------------------------------

struct EnterOrder {
  LAB_OUCH_MESSAGE('O', 47);
  std::uint32_t user_ref_num = 0;
  char side = side::kBuy;
  std::uint32_t quantity = 0;
  Symbol symbol{};
  std::uint64_t price = 0;
  char time_in_force = tif::kDay;
  char display = 'Y';
  char capacity = 'P';
  char intermarket_sweep = 'N';
  char cross_type = 'N';
  ClOrdId cl_ord_id{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.user_ref_num);
    f(5, m.side);
    f(6, m.quantity);
    f(10, m.symbol);
    f(18, m.price);
    f(26, m.time_in_force);
    f(27, m.display);
    f(28, m.capacity);
    f(29, m.intermarket_sweep);
    f(30, m.cross_type);
    f(31, m.cl_ord_id);
    f(45, m.appendage_length);
  }
};

struct ReplaceOrder {
  LAB_OUCH_MESSAGE('U', 40);
  std::uint32_t orig_user_ref_num = 0;
  std::uint32_t user_ref_num = 0;
  std::uint32_t quantity = 0;
  std::uint64_t price = 0;
  char time_in_force = tif::kDay;
  char display = 'Y';
  char intermarket_sweep = 'N';
  ClOrdId cl_ord_id{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.orig_user_ref_num);
    f(5, m.user_ref_num);
    f(9, m.quantity);
    f(13, m.price);
    f(21, m.time_in_force);
    f(22, m.display);
    f(23, m.intermarket_sweep);
    f(24, m.cl_ord_id);
    f(38, m.appendage_length);
  }
};

struct CancelOrder {
  LAB_OUCH_MESSAGE('X', 11);
  std::uint32_t user_ref_num = 0;
  std::uint32_t quantity = 0;  // new intended remaining size; 0 cancels

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.user_ref_num);
    f(5, m.quantity);
    f(9, m.appendage_length);
  }
};

struct AccountQuery {
  LAB_OUCH_MESSAGE('Q', 3);

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.appendage_length);
  }
};

// ---- Outbound ------------------------------------------------------------

struct SystemEvent {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 10;
  std::uint64_t timestamp = 0;
  char event_code = 'S';  // S start of day, E end of day

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.event_code);
  }
};

struct OrderAccepted {
  LAB_OUCH_MESSAGE('A', 64);
  std::uint64_t timestamp = 0;
  std::uint32_t user_ref_num = 0;
  char side = side::kBuy;
  std::uint32_t quantity = 0;
  Symbol symbol{};
  std::uint64_t price = 0;
  char time_in_force = tif::kDay;
  char display = 'Y';
  std::uint64_t order_reference_number = 0;
  char capacity = 'P';
  char intermarket_sweep = 'N';
  char cross_type = 'N';
  char order_state = 'L';  // L live, D dead
  ClOrdId cl_ord_id{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.user_ref_num);
    f(13, m.side);
    f(14, m.quantity);
    f(18, m.symbol);
    f(26, m.price);
    f(34, m.time_in_force);
    f(35, m.display);
    f(36, m.order_reference_number);
    f(44, m.capacity);
    f(45, m.intermarket_sweep);
    f(46, m.cross_type);
    f(47, m.order_state);
    f(48, m.cl_ord_id);
    f(62, m.appendage_length);
  }
};

struct OrderReplaced {
  LAB_OUCH_MESSAGE('U', 68);
  std::uint64_t timestamp = 0;
  std::uint32_t orig_user_ref_num = 0;
  std::uint32_t user_ref_num = 0;
  char side = side::kBuy;
  std::uint32_t quantity = 0;
  Symbol symbol{};
  std::uint64_t price = 0;
  char time_in_force = tif::kDay;
  char display = 'Y';
  std::uint64_t order_reference_number = 0;
  char capacity = 'P';
  char intermarket_sweep = 'N';
  char cross_type = 'N';
  char order_state = 'L';
  ClOrdId cl_ord_id{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.orig_user_ref_num);
    f(13, m.user_ref_num);
    f(17, m.side);
    f(18, m.quantity);
    f(22, m.symbol);
    f(30, m.price);
    f(38, m.time_in_force);
    f(39, m.display);
    f(40, m.order_reference_number);
    f(48, m.capacity);
    f(49, m.intermarket_sweep);
    f(50, m.cross_type);
    f(51, m.order_state);
    f(52, m.cl_ord_id);
    f(66, m.appendage_length);
  }
};

struct OrderCanceled {
  LAB_OUCH_MESSAGE('C', 20);
  std::uint64_t timestamp = 0;
  std::uint32_t user_ref_num = 0;
  std::uint32_t quantity = 0;  // shares taken off by this cancel
  char reason = cancel_reason::kUser;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.user_ref_num);
    f(13, m.quantity);
    f(17, m.reason);
    f(18, m.appendage_length);
  }
};

struct OrderExecuted {
  LAB_OUCH_MESSAGE('E', 36);
  std::uint64_t timestamp = 0;
  std::uint32_t user_ref_num = 0;
  std::uint32_t quantity = 0;
  std::uint64_t price = 0;
  char liquidity_flag = liquidity::kAdded;
  std::uint64_t match_number = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.user_ref_num);
    f(13, m.quantity);
    f(17, m.price);
    f(25, m.liquidity_flag);
    f(26, m.match_number);
    f(34, m.appendage_length);
  }
};

struct BrokenTrade {
  LAB_OUCH_MESSAGE('B', 38);
  std::uint64_t timestamp = 0;
  std::uint32_t user_ref_num = 0;
  std::uint64_t match_number = 0;
  char reason = 'E';
  ClOrdId cl_ord_id{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.user_ref_num);
    f(13, m.match_number);
    f(21, m.reason);
    f(22, m.cl_ord_id);
    f(36, m.appendage_length);
  }
};

struct Rejected {
  LAB_OUCH_MESSAGE('J', 31);
  std::uint64_t timestamp = 0;
  std::uint32_t user_ref_num = 0;
  std::uint16_t reason = reject::kOther;
  ClOrdId cl_ord_id{};

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.user_ref_num);
    f(13, m.reason);
    f(15, m.cl_ord_id);
    f(29, m.appendage_length);
  }
};

struct AccountQueryResponse {
  LAB_OUCH_MESSAGE('Q', 15);
  std::uint64_t timestamp = 0;
  std::uint32_t next_user_ref_num = 0;

  template <class S, class F>
  static constexpr void visit(S& m, F&& f) {
    f(1, m.timestamp);
    f(9, m.next_user_ref_num);
    f(13, m.appendage_length);
  }
};

#undef LAB_OUCH_MESSAGE

inline constexpr std::size_t kMaxMessageSize = 68;

}  // namespace lab::ouch
