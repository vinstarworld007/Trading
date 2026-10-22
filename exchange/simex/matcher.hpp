#pragma once

// simex's matching thread: the exchange's single sequencer. It takes commands
// from the order entry gateway and from the historical liquidity feeder, gives
// each one the next sequence number (the total order a journal replay would
// reproduce), runs it through the matching engine, and fans the results out:
// order responses to the gateway, ITCH messages to the market data publisher,
// and the sequenced command to the journal. No syscalls, locks or allocation
// per command.

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/core/spsc_ring.hpp"
#include "lab/match/engine.hpp"
#include "messages.hpp"

namespace simex {

using CommandRing = lab::SpscRing<Command, 1u << 16>;
using SessionRing = lab::SpscRing<SessionEvent, 1u << 16>;
using MdRing = lab::SpscRing<MdEvent, 1u << 18>;
using JournalRing = lab::SpscRing<Command, 1u << 16>;

struct MatcherStats {
  std::uint64_t commands = 0;
  std::uint64_t hist_orphans = 0;        // historical reduce or cancel for an order already gone
  std::uint64_t unknown_cancels = 0;     // firm cancel for an order already gone
  std::uint64_t backpressure_spins = 0;  // waits for a full output ring
  lab::LatencyHistogram command_ticks;   // producer timestamp to command done
};

class Matcher {
 public:
  struct Rings {
    CommandRing* gateway;
    CommandRing* liquidity;
    SessionRing* sessions;
    MdRing* md;
    JournalRing* journal;  // may be null
  };

  Matcher(const lab::match::EngineConfig& cfg, Rings rings, const lab::TscClock& clock);

  // Thread body: processes commands until `closing` is set and both inputs
  // are drained, then publishes the end-of-day system events and returns.
  void run(const std::atomic<bool>& closing);

  // Opens instruments before the market opens (no replay file).
  void list_symbols(const std::vector<std::string>& symbols);

  const lab::match::Engine& engine() const noexcept { return *engine_; }
  const MatcherStats& stats() const noexcept { return stats_; }
  // Locates with an instrument open, in locate order.
  std::vector<std::uint16_t> locates() const;

 private:
  struct Listener;
  friend struct Listener;

  void process(Command& c);
  void directory(std::uint16_t locate, const Symbol& symbol);
  void system_event(char code);
  std::uint64_t feed_time() const noexcept;
  template <class M>
  void publish(M& m, std::uint16_t locate);
  void to_session(const SessionEvent& e);
  template <class Ring, class T>
  void push(Ring& ring, const T& v);
  static std::uint64_t symbol_key(const Symbol& s) noexcept;

  std::unique_ptr<lab::match::Engine> engine_;
  Rings rings_;
  lab::TscClock clock_;
  std::uint64_t base_tsc_;
  std::uint64_t base_ns_;  // feed time at base_tsc_
  std::uint64_t seq_ = 0;
  std::uint64_t now_ns_ = 0;     // feed time of the command being processed
  std::vector<Symbol> symbols_;  // by locate
  std::unordered_map<std::uint64_t, std::uint16_t> by_symbol_;
  MatcherStats stats_;
};

}  // namespace simex
