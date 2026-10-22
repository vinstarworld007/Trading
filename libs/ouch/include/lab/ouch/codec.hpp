#pragma once

// Encode, decode and dispatch for OUCH 5.0, driven by each message's visit()
// field list (see messages.hpp). Inbound and outbound messages reuse type
// letters ('U' is Replace Order one way and Order Replaced the other), so each
// direction has its own dispatch.
//
//   struct Exchange : lab::ouch::HandlerBase {
//     using HandlerBase::on;
//     void on(const lab::ouch::EnterOrder& m) { ... }
//   };
//   lab::ouch::dispatch_inbound(bytes, len, exchange);

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "lab/core/compiler.hpp"
#include "lab/core/endian.hpp"
#include "lab/ouch/messages.hpp"

namespace lab::ouch {

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
    static_assert(sizeof(T) == 0, "unsupported OUCH field type");
}

template <class M>
consteval std::size_t layout_end() {
  M m{};
  std::size_t end = 1;  // message type
  M::visit(m, [&end](std::size_t offset, auto& field) {
    end = std::max(end, offset + wire_size<std::remove_cvref_t<decltype(field)>>());
  });
  return end;
}

}  // namespace detail

template <class M>
LAB_ALWAYS_INLINE void decode(const std::byte* p, M& m) noexcept {
  static_assert(detail::layout_end<M>() == M::kSize, "field list does not match spec length");
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

// Writes exactly M::kSize bytes; the appendage length is written as given
// (normally 0, with no appendage).
template <class M>
LAB_ALWAYS_INLINE std::size_t encode(const M& m, std::byte* p) noexcept {
  static_assert(detail::layout_end<M>() == M::kSize, "field list does not match spec length");
  p[0] = static_cast<std::byte>(M::kType);
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
  return M::kSize;
}

struct HandlerBase {
  template <class M>
  void on(const M&) noexcept {}
};

enum class DispatchResult : std::uint8_t { kOk, kUnknownType, kTooShort };

namespace detail {

template <class M, class H>
LAB_ALWAYS_INLINE DispatchResult decode_and_call(const std::byte* p, std::size_t len, H& h) {
  if (LAB_UNLIKELY(len < M::kSize)) return DispatchResult::kTooShort;
  M m;
  decode(p, m);
  if constexpr (requires { m.appendage_length; }) {
    if (LAB_UNLIKELY(len < M::kSize + m.appendage_length)) return DispatchResult::kTooShort;
  }
  h.on(m);
  return DispatchResult::kOk;
}

}  // namespace detail

// Messages a firm sends to the exchange.
template <class H>
LAB_ALWAYS_INLINE DispatchResult dispatch_inbound(const std::byte* p, std::size_t len, H& h) {
  if (LAB_UNLIKELY(len == 0)) return DispatchResult::kTooShort;
  switch (static_cast<char>(p[0])) {
    case EnterOrder::kType:
      return detail::decode_and_call<EnterOrder>(p, len, h);
    case CancelOrder::kType:
      return detail::decode_and_call<CancelOrder>(p, len, h);
    case ReplaceOrder::kType:
      return detail::decode_and_call<ReplaceOrder>(p, len, h);
    case AccountQuery::kType:
      return detail::decode_and_call<AccountQuery>(p, len, h);
    default:
      return DispatchResult::kUnknownType;
  }
}

// Messages the exchange sends to a firm.
template <class H>
LAB_ALWAYS_INLINE DispatchResult dispatch_outbound(const std::byte* p, std::size_t len, H& h) {
  if (LAB_UNLIKELY(len == 0)) return DispatchResult::kTooShort;
  switch (static_cast<char>(p[0])) {
    case OrderExecuted::kType:
      return detail::decode_and_call<OrderExecuted>(p, len, h);
    case OrderAccepted::kType:
      return detail::decode_and_call<OrderAccepted>(p, len, h);
    case OrderCanceled::kType:
      return detail::decode_and_call<OrderCanceled>(p, len, h);
    case OrderReplaced::kType:
      return detail::decode_and_call<OrderReplaced>(p, len, h);
    case Rejected::kType:
      return detail::decode_and_call<Rejected>(p, len, h);
    case BrokenTrade::kType:
      return detail::decode_and_call<BrokenTrade>(p, len, h);
    case SystemEvent::kType:
      return detail::decode_and_call<SystemEvent>(p, len, h);
    case AccountQueryResponse::kType:
      return detail::decode_and_call<AccountQueryResponse>(p, len, h);
    default:
      return DispatchResult::kUnknownType;
  }
}

}  // namespace lab::ouch
