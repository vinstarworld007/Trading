#pragma once

// simex's matching thread: the exchange's single sequencer. It takes commands
// from the order entry gateway and from the historical liquidity feeder, gives
// each one the next sequence number (the total order a journal replay would
// reproduce), runs it through the matching engine, and fans the results out:
// order responses to the gateway, ITCH messages to the market data publisher,
// and the sequenced command to the journal. No syscalls, locks or allocation
// per command.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "lab/book/implied.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/core/spsc_ring.hpp"
#include "lab/match/engine.hpp"
#include "messages.hpp"
#include "scenarios.hpp"

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

// What the scenario timeline did.
struct ScenarioStats {
  std::uint64_t crosses = 0;  // Q messages
  std::uint64_t cross_shares = 0;
  std::uint64_t noii = 0;          // I messages
  std::uint64_t halts = 0;         // news, MWCB and IPO halts
  std::uint64_t pauses = 0;        // LULD trading pauses
  std::uint64_t extensions = 0;    // LULD reopening auction extensions
  std::uint64_t limit_states = 0;  // LULD limit states entered
  std::uint64_t op_halts = 0;
  std::uint64_t broken_trades = 0;
  std::uint64_t reg_sho = 0;
  std::uint64_t halted_rejects = 0;  // orders refused while halted
  std::uint64_t implied_fills = 0;   // strip fills against the legs
  std::uint64_t implied_shares = 0;
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
  // Scenarios for the day; call before run(). IPO symbols must be listed.
  void set_scenarios(const ScenarioConfig& cfg) { sc_ = cfg; }
  // Strips NAME=A+B: every name must be listed already. Strip orders trade
  // against the strip's own book and against implied liquidity from the legs.
  void set_strips(const std::vector<lab::book::StripDef>& strips);
  const ScenarioStats& scenario_stats() const noexcept { return sstats_; }

  const lab::match::Engine& engine() const noexcept { return *engine_; }
  const MatcherStats& stats() const noexcept { return stats_; }
  // Locates with an instrument open, in locate order.
  std::vector<std::uint16_t> locates() const;

 private:
  struct Listener;
  friend struct Listener;

  // Why an instrument is not trading continuously.
  enum class Hold : std::uint8_t {
    kNone,
    kPreOpen,  // opening auction
    kHalted,   // news, MWCB or IPO not yet released: orders refused
    kQuoting,  // quotation period before a halt or IPO cross
    kPaused,   // LULD pause: reopening auction
    kOpHalt,   // operational halt: orders refused, no cross
    kClosing,  // closing auction
    kClosed,   // after the closing cross
  };
  struct InstState {
    Hold hold = Hold::kNone;
    std::uint64_t until_ns = 0;     // next transition of a timed hold (0: none)
    std::uint64_t cross_at_ns = 0;  // kHalted/kQuoting: when the cross happens
    std::array<char, 4> reason{' ', ' ', ' ', ' '};
    char cross_type = ' ';             // the cross that will end the hold
    std::uint32_t reference = 0;       // LULD reference price
    std::uint32_t open_reference = 0;  // first reference of the day, for Reg SHO
    std::uint64_t limit_since_ns = 0;
    std::uint32_t collar_ref = 0, collar_lower = 0, collar_upper = 0, extensions = 0;
    bool reg_sho = false;
    bool luld_watch = false;
  };
  enum class ActionKind : std::uint8_t {
    kOpenCross,
    kCloseStart,
    kCloseCross,
    kHalt,
    kOpHalt,
    kIpoRelease,
    kMwcb,
    kBreak
  };
  struct Action {
    std::uint64_t at_ns;
    ActionKind kind;
    std::size_t index;
  };

  void start_scenarios();
  void tick();
  void act(const Action& a);
  void hold(std::uint16_t locate, Hold h, std::uint64_t until_ns);
  void release(std::uint16_t locate);
  void trading_action(std::uint16_t locate, char state, const char* reason);
  void noii(std::uint16_t locate);
  void cross(std::uint16_t locate, char type);
  void set_reference(std::uint16_t locate, std::uint32_t price);
  std::uint32_t reference_price(std::uint16_t locate) const;
  void check_limit(std::uint16_t locate);
  void pause(std::uint16_t locate, std::uint32_t limit_price);
  void reopen(std::uint16_t locate);
  void close_market();
  void on_trade(std::uint16_t locate, std::uint32_t price, std::uint64_t match);
  std::uint16_t locate_of(const std::string& symbol) const;

  struct Strip {
    std::uint16_t locate, a, b;
  };
  void enter_strip(const Command& c, const Strip& s);
  void send_cancel(const Command& c, std::uint32_t qty);

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

  // Scenarios.
  ScenarioConfig sc_;
  ScenarioStats sstats_;
  bool scenarios_ = false;
  bool pre_open_ = false;
  bool closing_started_ = false;
  bool close_done_ = false;
  bool mwcb3_ = false;
  char cross_liquidity_ = ' ';  // OUCH liquidity flag for fills in the cross being run
  std::uint64_t start_ns_ = 0;
  std::uint64_t next_tick_tsc_ = 0;
  std::uint64_t tick_tsc_ = 0;
  std::uint64_t next_noii_ns_ = 0;
  std::vector<Action> timeline_;
  std::size_t next_action_ = 0;
  std::vector<InstState> inst_;      // by locate
  std::vector<std::uint16_t> held_;  // locates with a hold
  std::vector<std::uint16_t> luld_;  // locates with a LULD reference
  std::uint64_t last_match_ = 0;     // latest continuous trade, for a broken trade
  std::uint16_t last_match_locate_ = 0;

  // Strips, and the state of the strip order being entered (the listener
  // reads it to keep the order's several passes through the engine looking
  // like one order to its owner).
  std::vector<Strip> strips_;
  std::uint64_t implied_ref_ = 0;
  bool strip_mode_ = false;
  bool strip_quiet_accept_ = false;
  bool strip_rejected_ = false;
  std::uint32_t strip_left_ = 0;
  std::uint32_t strip_price_ = 0;
};

}  // namespace simex
