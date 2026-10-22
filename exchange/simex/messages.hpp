#pragma once

// Fixed-size messages between simex's threads. All are trivially copyable,
// 64 bytes (one cache line), and carry no pointers, so the rings that hold
// them could live in shared memory.
//
//   gateway ----Command----> matcher ----SessionEvent----> gateway
//   liquidity --Command----> matcher ----MdEvent---------> market data publisher
//                                    ----Command---------> journal (with seq and time)

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace simex {

using Symbol = std::array<char, 8>;

// Owner 0 is the historical liquidity replayed from an ITCH day; firm
// accounts are 1..N in the order given to --accounts.
inline constexpr std::uint16_t kHistOwner = 0;

struct Command {
  enum Type : std::uint8_t {
    kEnter,        // ref, symbol or locate, side, tif, qty, price
    kCancel,       // ref, qty = new remaining (0 cancels)
    kReduce,       // ref, qty = shares to take off (historical E, C, X)
    kReplace,      // orig_ref -> ref, qty, price; tif
    kReplaceOpen,  // as kReplace but qty is the new open size (historical U)
    kDirectory,    // locate, symbol: open an instrument
    kCancelOwner,  // owner: cancel everything (disconnect, kill switch)
  };

  std::uint64_t seq;    // set by the matcher: position in the total order
  std::uint64_t ts_ns;  // set by the matcher: feed time (ns since midnight)
  std::uint64_t ref;    // UserRefNum, or ITCH order reference for owner 0
  std::uint64_t orig_ref;
  std::uint64_t rx_tsc;  // when the gateway or liquidity thread produced it
  std::uint32_t qty;
  std::uint32_t price;  // Price(4)
  Symbol symbol;
  std::uint16_t owner;
  std::uint16_t locate;  // 0: look the symbol up
  Type type;
  char side;  // 'B' or 'S'
  char tif;   // '0' day, '3' IOC
  char reserved;
};
static_assert(sizeof(Command) == 64);
static_assert(std::is_trivially_copyable_v<Command>);

struct SessionEvent {
  enum Type : std::uint8_t { kAccepted, kReplaced, kCanceled, kExecuted, kRejected };

  std::uint64_t ts_ns;
  std::uint64_t orn;    // order reference number on the public feed
  std::uint64_t match;  // match number, executions only
  std::uint32_t user_ref;
  std::uint32_t orig_user_ref;  // replaced only
  std::uint32_t qty;            // accepted/replaced: open shares; canceled/executed: shares
  std::uint32_t price;          // Price(4)
  std::uint32_t remaining;      // shares still open after this event
  std::uint16_t owner;
  std::uint16_t reject_reason;  // OUCH reject code
  Symbol symbol;
  Type type;
  char side;
  char tif;
  char reason;     // OUCH cancel reason
  char liquidity;  // 'A' added, 'R' removed
  char reserved[3];
};
static_assert(sizeof(SessionEvent) == 64);
static_assert(std::is_trivially_copyable_v<SessionEvent>);

// One encoded ITCH 5.0 message for the public feed.
struct MdEvent {
  std::uint16_t len;
  std::byte data[62];
};
static_assert(sizeof(MdEvent) == 64);
static_assert(std::is_trivially_copyable_v<MdEvent>);

}  // namespace simex
