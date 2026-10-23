#pragma once

// Messages between the firm's threads: market data thread -> trading thread
// (TopOfBook), trading thread -> order entry thread (OrderRequest) and back
// (OrderResponse). Fixed-size and trivially copyable, so they cross SPSC rings
// (or shared memory) as plain bytes.

#include <array>
#include <cstdint>
#include <type_traits>

namespace lab::oms {

using Symbol = std::array<char, 8>;

// The two best price levels per side of one instrument, sent when they change.
// Two levels let the strategy see the market without its own order in it.
struct alignas(64) TopOfBook {
  std::uint64_t rx_tsc;   // when the packet that changed it arrived
  std::uint64_t feed_ns;  // ITCH timestamp of that message
  std::uint32_t bid_px[2];
  std::uint32_t bid_qty[2];  // 0: no level
  std::uint32_t ask_px[2];
  std::uint32_t ask_qty[2];
  std::uint16_t instrument;  // index into the trader's symbol list
  std::uint16_t locate;
  char trading_state;  // ITCH H: T trading, H halted, P paused, Q quotation only
  std::uint8_t reserved[11];
};
static_assert(sizeof(TopOfBook) == 64);
static_assert(std::is_trivially_copyable_v<TopOfBook>);

struct alignas(64) OrderRequest {
  enum Type : char { kEnter = 'O', kReplace = 'U', kCancel = 'X' };
  std::uint64_t tick_tsc;    // market data that triggered it (0: none)
  std::uint64_t decide_tsc;  // when the trading thread decided
  std::uint32_t user_ref;
  std::uint32_t orig_ref;  // replace: the order being replaced
  std::uint32_t qty;       // enter/replace: shares; cancel: remaining size wanted
  std::uint32_t price;     // Price(4)
  Symbol symbol;
  char type;
  char side;  // B or S
  char tif;   // OUCH time in force
  std::uint8_t reserved[17];
};
static_assert(sizeof(OrderRequest) == 64);
static_assert(std::is_trivially_copyable_v<OrderRequest>);

struct alignas(64) OrderResponse {
  enum Type : char {
    kAccepted = 'A',
    kReplaced = 'U',
    kCanceled = 'C',
    kExecuted = 'E',
    kRejected = 'J',
    kSessionEnd = 'Z',  // the exchange ended the session
  };
  std::uint64_t rx_tsc;
  std::uint64_t exchange_ns;  // OUCH timestamp
  std::uint64_t number;       // accepted/replaced: order reference; executed: match number
  std::uint32_t user_ref;
  std::uint32_t orig_ref;  // replaced
  std::uint32_t qty;       // accepted/replaced: open shares; canceled/executed: shares
  std::uint32_t price;
  std::uint16_t reason;  // rejected: reason code
  char type;
  char flag;  // executed: liquidity flag; canceled: reason
  std::uint8_t reserved[16];
};
static_assert(sizeof(OrderResponse) == 64);
static_assert(std::is_trivially_copyable_v<OrderResponse>);

}  // namespace lab::oms
