#pragma once

// Encode, decode and dispatch for ITCH 5.0 messages.
//
//   struct MyHandler : lab::itch::HandlerBase {
//     using HandlerBase::on;                      // keep the no-op defaults
//     void on(const lab::itch::AddOrder& m) { ... }
//   };
//   lab::itch::dispatch(msg_bytes, msg_len, handler);
//
// decode() and encode() walk the same field list (each message's visit()), so
// the two directions cannot drift apart. Wire integers are big-endian.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "lab/core/compiler.hpp"
#include "lab/core/endian.hpp"
#include "lab/itch/messages.hpp"

namespace lab::itch {

namespace detail {

template <class T>
inline constexpr bool kIsCharArray = false;
template <std::size_t N>
inline constexpr bool kIsCharArray<std::array<char, N>> = true;

template <class T>
constexpr std::size_t wire_size() {
  if constexpr (std::is_same_v<T, char>)
    return 1;
  else if constexpr (std::is_same_v<T, std::uint16_t>)
    return 2;
  else if constexpr (std::is_same_v<T, std::uint32_t>)
    return 4;
  else if constexpr (std::is_same_v<T, std::uint64_t>)
    return 8;
  else if constexpr (kIsCharArray<T>)
    return std::tuple_size_v<T>;
  else
    static_assert(sizeof(T) == 0, "unsupported ITCH field type");
}

// End offset of the last field, computed from the field list at compile time.
template <class M>
consteval std::size_t layout_end() {
  M m{};
  std::size_t end = 11;  // header: type(1) + locate(2) + tracking(2) + timestamp(6)
  M::visit(m, [&end](std::size_t offset, auto& field) {
    end = std::max(end, offset + wire_size<std::remove_cvref_t<decltype(field)>>());
  });
  return end;
}

}  // namespace detail

template <class M>
LAB_ALWAYS_INLINE void decode(const std::byte* p, M& m) noexcept {
  static_assert(detail::layout_end<M>() == M::kSize, "field list does not match spec length");
  m.header.stock_locate = load_be16(p + 1);
  m.header.tracking_number = load_be16(p + 3);
  m.header.timestamp = load_be48(p + 5);
  M::visit(m, [p](std::size_t offset, auto& field) {
    using T = std::remove_cvref_t<decltype(field)>;
    if constexpr (std::is_same_v<T, char>)
      field = static_cast<char>(p[offset]);
    else if constexpr (std::is_same_v<T, std::uint16_t>)
      field = load_be16(p + offset);
    else if constexpr (std::is_same_v<T, std::uint32_t>)
      field = load_be32(p + offset);
    else if constexpr (std::is_same_v<T, std::uint64_t>)
      field = load_be64(p + offset);
    else
      std::memcpy(field.data(), p + offset, field.size());
  });
}

// Writes exactly M::kSize bytes to p.
template <class M>
void encode(const M& m, std::byte* p) noexcept {
  static_assert(detail::layout_end<M>() == M::kSize, "field list does not match spec length");
  p[0] = static_cast<std::byte>(M::kType);
  store_be16(p + 1, m.header.stock_locate);
  store_be16(p + 3, m.header.tracking_number);
  store_be48(p + 5, m.header.timestamp);
  M::visit(m, [p](std::size_t offset, const auto& field) {
    using T = std::remove_cvref_t<decltype(field)>;
    if constexpr (std::is_same_v<T, char>)
      p[offset] = static_cast<std::byte>(field);
    else if constexpr (std::is_same_v<T, std::uint16_t>)
      store_be16(p + offset, field);
    else if constexpr (std::is_same_v<T, std::uint32_t>)
      store_be32(p + offset, field);
    else if constexpr (std::is_same_v<T, std::uint64_t>)
      store_be64(p + offset, field);
    else
      std::memcpy(p + offset, field.data(), field.size());
  });
}

// Handlers derive from this and add `using HandlerBase::on;` so message types
// they do not care about fall through to a no-op.
struct HandlerBase {
  template <class M>
  void on(const M&) noexcept {}
};

enum class DispatchResult : std::uint8_t {
  kOk,
  kUnknownType,  // type byte not in the spec; skipped
  kTooShort,     // shorter than the spec length for its type; skipped
};

namespace detail {

template <class M, class H>
LAB_ALWAYS_INLINE DispatchResult decode_and_call(const std::byte* p, std::size_t len, H& h) {
  // The spec allows Nasdaq to append fields, so longer messages are accepted.
  if (LAB_UNLIKELY(len < M::kSize)) return DispatchResult::kTooShort;
  M m;
  decode(p, m);
  h.on(m);
  return DispatchResult::kOk;
}

}  // namespace detail

template <class H>
LAB_ALWAYS_INLINE DispatchResult dispatch(const std::byte* p, std::size_t len, H& h) {
  if (LAB_UNLIKELY(len == 0)) return DispatchResult::kTooShort;
  using detail::decode_and_call;
  switch (static_cast<char>(p[0])) {
    // Order book messages first: they are the vast majority of a day.
    case AddOrder::kType:
      return decode_and_call<AddOrder>(p, len, h);
    case OrderDelete::kType:
      return decode_and_call<OrderDelete>(p, len, h);
    case OrderExecuted::kType:
      return decode_and_call<OrderExecuted>(p, len, h);
    case OrderReplace::kType:
      return decode_and_call<OrderReplace>(p, len, h);
    case OrderCancel::kType:
      return decode_and_call<OrderCancel>(p, len, h);
    case AddOrderMpid::kType:
      return decode_and_call<AddOrderMpid>(p, len, h);
    case OrderExecutedWithPrice::kType:
      return decode_and_call<OrderExecutedWithPrice>(p, len, h);
    case Trade::kType:
      return decode_and_call<Trade>(p, len, h);
    case CrossTrade::kType:
      return decode_and_call<CrossTrade>(p, len, h);
    case BrokenTrade::kType:
      return decode_and_call<BrokenTrade>(p, len, h);
    case Noii::kType:
      return decode_and_call<Noii>(p, len, h);
    case Rpii::kType:
      return decode_and_call<Rpii>(p, len, h);
    case SystemEvent::kType:
      return decode_and_call<SystemEvent>(p, len, h);
    case StockDirectory::kType:
      return decode_and_call<StockDirectory>(p, len, h);
    case StockTradingAction::kType:
      return decode_and_call<StockTradingAction>(p, len, h);
    case RegShoRestriction::kType:
      return decode_and_call<RegShoRestriction>(p, len, h);
    case MarketParticipantPosition::kType:
      return decode_and_call<MarketParticipantPosition>(p, len, h);
    case MwcbDeclineLevel::kType:
      return decode_and_call<MwcbDeclineLevel>(p, len, h);
    case MwcbStatus::kType:
      return decode_and_call<MwcbStatus>(p, len, h);
    case IpoQuotingPeriod::kType:
      return decode_and_call<IpoQuotingPeriod>(p, len, h);
    case LuldAuctionCollar::kType:
      return decode_and_call<LuldAuctionCollar>(p, len, h);
    case OperationalHalt::kType:
      return decode_and_call<OperationalHalt>(p, len, h);
    case DirectListingCapitalRaise::kType:
      return decode_and_call<DirectListingCapitalRaise>(p, len, h);
    default:
      return DispatchResult::kUnknownType;
  }
}

}  // namespace lab::itch
