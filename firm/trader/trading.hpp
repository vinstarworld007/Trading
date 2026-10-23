#pragma once

// The trading thread: market maker, inline pre-trade risk and the OMS. It
// reads top-of-book updates and order responses from rings and writes order
// requests to a ring; no syscalls, locks or allocation once running.
//
// Each instrument has at most one working order per side. On every change to
// the market or to its own orders it recomputes the quote and moves its
// orders toward it: enter where there is none, replace on a new price, cancel
// a side the strategy no longer quotes. Only one request per order is ever in
// flight. When told to stop it cancels everything and waits for the answers.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/oms/oms.hpp"
#include "lab/risk/risk.hpp"
#include "lab/strategy/market_maker.hpp"
#include "order_entry.hpp"

namespace trader {

struct TradingConfig {
  std::vector<lab::oms::Symbol> symbols;
  lab::strategy::MakerParams maker;
  lab::risk::Limits limits;
  int cpu = -1;
  double cancel_wait_seconds = 3;     // at stop, how long to wait for cancels
  std::size_t fill_log_capacity = 0;  // fills kept for write_fills(); 0: none
};

// One fill as the OMS saw it, with the market (ours excluded) at the time.
struct FillRecord {
  std::uint64_t exchange_ns;
  std::uint64_t match;
  std::uint32_t user_ref;
  std::uint32_t qty;
  std::uint32_t price;
  std::uint32_t bid;
  std::uint32_t ask;
  std::uint16_t instrument;
  char side;
  char liquidity;
};

struct TradingStats {
  std::uint64_t tops = 0;
  std::uint64_t requests = 0;
  std::uint64_t ring_full_spins = 0;
  bool session_ended = false;  // the exchange ended the session first
  bool cancel_timeout = false;
  std::uint64_t self_trades = 0;
  std::uint64_t halts = 0;  // an instrument stopped trading (halt, pause, quotation only)
};

class Trading {
 public:
  Trading(const TradingConfig& cfg, TopRing& tops, RequestRing& requests, ResponseRing& responses,
          const lab::TscClock& clock);

  // Thread body: trades until `stop` is set or the session ends.
  void run(const std::atomic<bool>& stop);

  // After run(): positions, PnL, risk and OMS counters. Returns false if a
  // check failed (an inconsistent response, a position past its limit, or
  // orders left working).
  bool report(std::FILE* out) const;

  lab::risk::Risk& risk() noexcept { return risk_; }

  // After run(): writes the fill log as CSV (see research/lab_research/io.py).
  bool write_fills(const std::string& path) const;

 private:
  struct Instrument {
    lab::oms::Symbol symbol{};
    lab::oms::TopOfBook top{};
    bool have_top = false;
    bool dirty = false;
    std::uint32_t bid_ref = 0;
    std::uint32_t ask_ref = 0;
    std::uint32_t last_mid = 0;
    lab::strategy::Market last_market{};
    lab::strategy::Quote last_quote{};
  };

  void on_response(const lab::oms::OrderResponse& r);
  void reconcile(std::uint16_t i);
  void work_side(Instrument& in, std::uint16_t i, char side, std::uint32_t& ref, std::uint32_t px,
                 std::uint32_t qty, std::uint32_t reference);
  void cancel_all();
  void mark(std::uint16_t i);
  void send(const lab::oms::OrderRequest& r);

  TradingConfig cfg_;
  TopRing& tops_;
  RequestRing& requests_;
  ResponseRing& responses_;
  lab::TscClock clock_;
  lab::oms::Oms oms_;
  lab::risk::Risk risk_;
  lab::strategy::MarketMaker maker_;
  std::vector<Instrument> instruments_;
  std::vector<std::uint16_t> dirty_;
  std::uint64_t tick_tsc_ = 0;  // market data behind the current decision
  bool stopping_ = false;
  std::array<std::uint64_t, 256> recent_matches_{};  // to spot self-trades
  std::size_t next_match_ = 0;
  std::vector<FillRecord> fills_;  // reserved up front; never grows on the hot path
  TradingStats stats_;
};

}  // namespace trader
