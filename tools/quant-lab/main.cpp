// quant-lab: a simulated year of trading for four strategies on the lab's own
// stack, written out as JSON for the Quant Research Lab page.
//
// One process, one thread, deterministic for a seed. A market model moves a
// hidden value for each of five stocks. Liquidity providers and takers send
// orders to the matching engine around those values; every engine event goes
// out as an ITCH 5.0 message and is applied to a BookBuilder, a fresh one each
// day, exactly as the trader's feed thread would. The strategies see only the
// rebuilt books and the trades on the feed. Their orders go through the OMS
// and pre-trade risk into the same engine, and their fills come back as OMS
// order responses.
//
//   AAPL         market making       firm/strategy market_maker.hpp
//   SEMA, SEMB   statistical arb     pairs.hpp, 5-minute samples
//   TRND         trend following     trend.hpp, daily closes
//   VALU         fundamental         thesis.hpp + POV execution (execution.hpp)
//
// SEMA, SEMB, TRND and VALU are made-up symbols; all prices are simulated.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/itch/codec.hpp"
#include "lab/itch/messages.hpp"
#include "lab/match/engine.hpp"
#include "lab/oms/oms.hpp"
#include "lab/risk/risk.hpp"
#include "lab/strategy/execution.hpp"
#include "lab/strategy/market_maker.hpp"
#include "lab/strategy/pairs.hpp"
#include "lab/strategy/thesis.hpp"
#include "lab/strategy/trend.hpp"

using namespace lab;

namespace {

constexpr std::uint16_t kLp = 1, kTaker = 2, kFirm = 3;  // engine owners
constexpr std::uint32_t kTick = 100;                     // one cent in Price(4)
constexpr int kMinutes = 390;                            // 09:30 to 16:00
constexpr std::uint64_t kOpenNs = 34'200'000'000'000ULL;
constexpr std::uint64_t kMinuteNs = 60'000'000'000ULL;

enum Sym : std::uint16_t { kAapl = 1, kSema, kSemb, kTrnd, kValu, kSymbols };
constexpr const char* kNames[kSymbols] = {"", "AAPL", "SEMA", "SEMB", "TRND", "VALU"};
constexpr int kEventsPerMinute[kSymbols] = {0, 60, 20, 20, 15, 30};
constexpr int kTakerLots[kSymbols] = {0, 2, 3, 3, 4, 5};  // taker size: 100..lots*100

std::uint32_t to_px(double dollars) {
  return static_cast<std::uint32_t>(std::llround(dollars * 10'000.0));
}
double dollars(std::int64_t price4) { return static_cast<double>(price4) / 10'000.0; }

struct Args {
  int days = 252;
  std::uint64_t seed = 3;
  std::string out = "quant-lab.json";
  int mm_frames = 900;
};

// ------------------------------------------------------------------ market model
// Hidden values in dollars. The strategies never see these, only the books.
struct Model {
  std::mt19937_64 rng;
  std::normal_distribution<double> n01{0.0, 1.0};
  std::uniform_real_distribution<double> u01{0.0, 1.0};
  double aapl = 189.50;
  double factor = 0, spread = 0, spread_mean = 0;  // SEMA/SEMB: log factor and log gap
  double idio_a = 0, idio_b = 0;                   // news that only moves one name
  double trnd = 60.0, trnd_drift = 0.0035;
  int trnd_left = 50;
  bool trnd_trending = true;
  double valu = 40.0;
  static constexpr double kValuTrue = 50.0;
  static constexpr int kCatalystDay = 128;
  std::vector<std::string> notes;  // what the model did, for the page

  explicit Model(std::uint64_t seed) : rng{seed} {}
  double g() { return n01(rng); }
  double u() { return u01(rng); }

  double value(std::uint16_t s) const {
    switch (s) {
      case kAapl:
        return aapl;
      case kSema:
        return 120.0 * std::exp(factor + idio_a + spread / 2);
      case kSemb:
        return 80.0 * std::exp(factor + idio_b - spread / 2);
      case kTrnd:
        return trnd;
      case kValu:
        return valu;
    }
    return 0;
  }

  void start_day(int day) {
    if (--trnd_left <= 0) {
      trnd_trending = !trnd_trending;
      if (trnd_trending) {
        trnd_drift = (u() < 0.5 ? 1 : -1) * (0.002 + 0.0015 * u());
        trnd_left = 40 + static_cast<int>(u() * 40);
      } else {
        trnd_drift = 0;
        trnd_left = 25 + static_cast<int>(u() * 30);
      }
    }
    if (day == 18) valu *= std::exp(-0.045);           // a sector sell-off
    if (day == kCatalystDay) valu *= std::exp(0.072);  // earnings show the margins
  }

  // One event's worth of time for symbol `s` (n events a minute).
  void step(std::uint16_t s, int day) {
    const double per = 1.0 / kEventsPerMinute[s];
    const double sq = std::sqrt(per);
    switch (s) {
      case kAapl:
        aapl *= std::exp(0.00045 * sq * g());
        break;
      case kSema:
      case kSemb: {
        // Both legs share the factor; advance it on SEMA's clock only.
        if (s == kSemb) {
          idio_b += 0.00035 * sq * g();
          break;
        }
        idio_a += 0.00035 * sq * g();
        factor += 0.0009 * sq * g();
        spread += 0.007 * per * (spread_mean - spread) + 0.0007 * sq * g();
        if (u() < 0.00002 * per) {  // rarely, the relationship shifts for good
          spread_mean += (u() < 0.5 ? 1 : -1) * 0.02;
          notes.push_back("day " + std::to_string(day) + ": SEMA/SEMB relationship shifts");
        }
        break;
      }
      case kTrnd: {
        const double vol = trnd_trending ? 0.012 : 0.016;
        trnd *= std::exp(trnd_drift / kMinutes * per + vol / std::sqrt(390.0) * sq * g());
        break;
      }
      case kValu: {
        // Until the catalyst the market prices VALU near $40 and pulls back
        // toward it; after, it reprices toward the true value.
        const double anchor = day >= kCatalystDay ? kValuTrue : 40.0;
        const double drift = (day >= kCatalystDay ? 0.012 : 0.03) * std::log(anchor / valu);
        valu *= std::exp(drift / kMinutes * per + 0.013 / std::sqrt(390.0) * sq * g());
        break;
      }
    }
  }
};

// ------------------------------------------------------------------ feed
// Engine listener: turns every engine event into ITCH, applies it to the
// day's BookBuilder, and routes the firm's events to the OMS.
struct Feed {
  book::BookBuilder* bb = nullptr;
  oms::Oms* oms = nullptr;
  std::uint64_t ts = 0;
  std::uint64_t now_ns() { return ++ts; }
  std::array<std::uint64_t, kSymbols> volume{};                  // shares traded today
  std::array<std::uint64_t, kSymbols> last_trade{};              // Price(4)
  std::array<std::uint32_t, kSymbols> open{}, high{}, low{};     // today's trades
  std::array<std::uint64_t, kSymbols> messages{};                // ITCH messages, all days
  std::array<std::array<std::uint64_t, 4>, kSymbols> by_type{};  // A E X D
  std::vector<std::string>* tape = nullptr;  // when set, short text of each message
  std::uint64_t firm_fills = 0;

  template <class M>
  void publish(const M& m, int kind) {
    std::byte b[64];
    itch::encode(m, b);
    bb->apply(b, M::kSize);
    ++messages[m.header.stock_locate];
    ++by_type[m.header.stock_locate][static_cast<std::size_t>(kind)];
  }
  static char side(match::Side s) { return s == match::Side::kBuy ? 'B' : 'S'; }
  void note(const char* k, const match::OrderView& o, std::uint32_t q, std::uint32_t px) {
    if (tape == nullptr || o.instrument != kAapl) return;
    char t[96];
    std::snprintf(t, sizeof t, "%s|%c|%u|%u|%d", k, side(o.side), q, px,
                  match::key_owner(o.key) == kFirm ? 1 : 0);
    tape->push_back(t);
  }
  void respond(char type, std::uint32_t ref, std::uint32_t qty, std::uint32_t px, std::uint64_t num,
               char flag = ' ') {
    oms::OrderResponse r{};
    r.type = type;
    r.user_ref = ref;
    r.qty = qty;
    r.price = px;
    r.number = num;
    r.flag = flag;
    oms->apply(r);
  }
  static bool firm(match::OrderKey k) { return match::key_owner(k) == kFirm; }
  static std::uint32_t ref(match::OrderKey k) {
    return static_cast<std::uint32_t>(match::key_ref(k));
  }

  void on_accepted(const match::OrderView& o) {
    if (firm(o.key))
      respond(oms::OrderResponse::kAccepted, ref(o.key), o.remaining, o.price, o.orn);
  }
  void on_rejected(match::OrderKey k, match::Reject) {
    if (firm(k)) respond(oms::OrderResponse::kRejected, ref(k), 0, 0, 0);
  }
  void on_rested(const match::OrderView& o) {
    itch::AddOrder a;
    a.header = {o.instrument, 0, now_ns()};
    a.order_reference_number = o.orn;
    a.buy_sell_indicator = side(o.side);
    a.shares = o.remaining;
    a.stock = itch::make_symbol(kNames[o.instrument]);
    a.price = o.price;
    publish(a, 0);
    note("A", o, o.remaining, o.price);
  }
  void on_execution(const match::OrderView& rest, const match::OrderView& aggr, std::uint32_t q,
                    std::uint32_t px, std::uint64_t match_no) {
    itch::OrderExecuted e;
    e.header = {rest.instrument, 0, now_ns()};
    e.order_reference_number = rest.orn;
    e.executed_shares = q;
    e.match_number = match_no;
    publish(e, 1);
    const auto i = rest.instrument;
    volume[i] += q;
    last_trade[i] = px;
    if (open[i] == 0) open[i] = high[i] = low[i] = px;
    high[i] = std::max(high[i], px);
    low[i] = std::min(low[i], px);
    note("E", rest, q, px);
    if (firm(aggr.key)) {
      note("T", aggr, q, px);  // our order took liquidity
    }
    if (firm(rest.key)) {
      ++firm_fills;
      respond(oms::OrderResponse::kExecuted, ref(rest.key), q, px, match_no, 'A');
    }
    if (firm(aggr.key)) {
      ++firm_fills;
      respond(oms::OrderResponse::kExecuted, ref(aggr.key), q, px, match_no, 'R');
    }
  }
  void on_canceled(const match::OrderView& o, std::uint32_t q, match::CancelReason why) {
    if (o.on_book) {
      if (o.remaining == 0) {
        itch::OrderDelete d;
        d.header = {o.instrument, 0, now_ns()};
        d.order_reference_number = o.orn;
        publish(d, 3);
        note("D", o, q, o.price);
      } else {
        itch::OrderCancel x;
        x.header = {o.instrument, 0, now_ns()};
        x.order_reference_number = o.orn;
        x.cancelled_shares = q;
        publish(x, 2);
        note("X", o, q, o.price);
      }
    }
    if (firm(o.key)) {
      respond(oms::OrderResponse::kCanceled, ref(o.key), q, o.price, 0,
              why == match::CancelReason::kIoc ? 'I' : 'U');
    }
  }
  void on_replaced(const match::OrderView&, const match::OrderView&) {}
  void on_cross_execution(const match::OrderView&, std::uint32_t, std::uint32_t, std::uint64_t) {}
};

// ------------------------------------------------------------------ helpers
struct Tob {
  std::uint32_t bid = 0, ask = 0;
  std::uint32_t mid() const { return bid && ask > bid ? (bid + ask) / 2 : 0; }
};

Tob top(const book::BookBuilder& bb, std::uint16_t s) {
  const auto& b = bb.book(s);
  Tob t;
  if (const auto* l = b.bids().best()) t.bid = l->price;
  if (const auto* l = b.asks().best()) t.ask = l->price;
  return t;
}

oms::TopOfBook top2(const book::BookBuilder& bb, std::uint16_t s) {
  oms::TopOfBook t{};
  const auto& b = bb.book(s);
  for (std::size_t d = 0; d < 2; ++d) {
    if (const auto* l = b.bids().level(d)) {
      t.bid_px[d] = l->price;
      t.bid_qty[d] = static_cast<std::uint32_t>(l->shares);
    }
    if (const auto* l = b.asks().level(d)) {
      t.ask_px[d] = l->price;
      t.ask_qty[d] = static_cast<std::uint32_t>(l->shares);
    }
  }
  return t;
}

// Running stats for one strategy's daily P&L.
struct Track {
  std::vector<double> daily_pnl;  // cumulative, dollars
  std::uint64_t orders = 0;
  std::uint64_t blocked = 0;
  double sharpe() const {
    if (daily_pnl.size() < 3) return 0;
    std::vector<double> d;
    for (std::size_t i = 1; i < daily_pnl.size(); ++i) d.push_back(daily_pnl[i] - daily_pnl[i - 1]);
    double m = 0, v = 0;
    for (double x : d) m += x;
    m /= static_cast<double>(d.size());
    for (double x : d) v += (x - m) * (x - m);
    v /= static_cast<double>(d.size());
    return v > 0 ? m / std::sqrt(v) * std::sqrt(252.0) : 0;
  }
  double max_drawdown() const {
    double peak = 0, dd = 0;
    for (double x : daily_pnl) {
      peak = std::max(peak, x);
      dd = std::min(dd, x - peak);
    }
    return dd;
  }
};

std::string num(double v, int prec = 2) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", prec, v);
  return b;
}

// ------------------------------------------------------------------ the lab
class Lab {
 public:
  explicit Lab(const Args& a)
      : args_{a},
        model_{a.seed},
        flow_{a.seed * 7919 + 1},
        eng_{engine_config()},
        oms_{kSymbols},
        mm_{strategy::MakerParams{}},
        pairs_{strategy::PairsParams{}},
        trend_{strategy::TrendParams{}},
        thesis_{thesis_params()},
        risk_mm_{mm_limits(), 1e9},
        risk_pairs_{pairs_limits(), 1e9},
        risk_trend_{trend_limits(), 1e9},
        risk_fund_{fund_limits(), 1e9} {
    for (std::uint16_t s = 1; s < kSymbols; ++s) eng_.add_instrument(s);
    feed_.oms = &oms_;
  }

  int run() {
    const auto t0 = std::chrono::steady_clock::now();
    for (int day = 1; day <= args_.days; ++day) run_day(day);
    wall_s_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return integrity_errors_ == 0 && book_mismatches_ == 0 && oms_.stats().inconsistent == 0 ? 0
                                                                                             : 1;
  }

  std::string json() const;
  void print_summary() const;
  std::string summary(const Track& t, std::initializer_list<std::uint16_t> syms) const;

 private:
  static match::EngineConfig engine_config() {
    match::EngineConfig c;
    c.max_orders = 1u << 18;
    c.max_instruments = kSymbols;
    c.levels_per_side = 1024;
    return c;
  }
  static strategy::ThesisParams thesis_params() {
    strategy::ThesisParams p;
    p.fair_value = to_px(Model::kValuTrue);
    return p;
  }
  static risk::Limits base_limits() {
    risk::Limits l;
    l.max_open_orders = 1'000;
    l.max_orders_per_second = 50;
    l.burst = 50;
    return l;
  }
  static risk::Limits mm_limits() {
    risk::Limits l = base_limits();
    l.max_position = 1'000;
    return l;
  }
  static risk::Limits pairs_limits() {
    risk::Limits l = base_limits();
    l.max_order_notional = 250'000ULL * 10'000;
    l.max_position = 4'000;
    return l;
  }
  static risk::Limits trend_limits() {
    risk::Limits l = base_limits();
    l.max_order_notional = 250'000ULL * 10'000;
    l.max_position = 40'000;
    return l;
  }
  static risk::Limits fund_limits() {
    risk::Limits l = base_limits();
    l.max_order_qty = 5'000;
    l.max_order_notional = 500'000ULL * 10'000;
    l.max_position = 650'000;
    return l;
  }

  // -------------------------------------------------------------- one day
  void run_day(int day) {
    bb_ = std::make_unique<book::BookBuilder>(1u << 16);
    feed_.bb = bb_.get();
    feed_.ts = kOpenNs - 1'000'000'000ULL;
    feed_.volume.fill(0);
    feed_.open.fill(0);
    lp_.assign(kSymbols, {});
    model_.start_day(day);
    for (std::uint16_t s = 1; s < kSymbols; ++s) {
      itch::StockDirectory sd;
      sd.header = {s, 0, feed_.now_ns()};
      sd.stock = itch::make_symbol(kNames[s]);
      sd.market_category = 'Q';
      sd.financial_status_indicator = 'N';
      sd.round_lot_size = 100;
      sd.round_lots_only = 'N';
      std::byte b[64];
      itch::encode(sd, b);
      bb_->apply(b, itch::StockDirectory::kSize);
      for (int i = 0; i < 6; ++i) {
        lp_add(s, 'B', i);
        lp_add(s, 'S', i);
      }
    }
    std::array<std::uint64_t, 13> fund_mkt{}, fund_us{};
    for (int minute = 0; minute < kMinutes; ++minute) {
      const std::uint64_t m0 = kOpenNs + static_cast<std::uint64_t>(minute) * kMinuteNs;
      feed_.ts = std::max(feed_.ts, m0);
      const std::uint64_t valu_vol0 = feed_.volume[kValu];
      const std::int64_t valu_pos0 = oms_.position(kValu).shares;
      // Interleave the symbols' events across the minute.
      std::array<int, kSymbols> left{};
      int total = 0;
      for (std::uint16_t s = 1; s < kSymbols; ++s) total += left[s] = kEventsPerMinute[s];
      while (total > 0) {
        int pick = static_cast<int>(flow_() % static_cast<std::uint64_t>(total));
        std::uint16_t s = 1;
        while (pick >= left[s]) pick -= left[s++];
        --left[s];
        --total;
        model_.step(s, day);
        market_event(s);
        if (s == kAapl) mm_event(day, minute);
      }
      on_minute(day, minute);
      const std::size_t bucket = static_cast<std::size_t>(minute / 30);
      fund_mkt[bucket] += feed_.volume[kValu] - valu_vol0;
      const std::int64_t dpos = oms_.position(kValu).shares - valu_pos0;
      fund_us[bucket] += static_cast<std::uint64_t>(dpos < 0 ? -dpos : dpos);
    }
    end_of_day(day, fund_mkt, fund_us);
  }

  // -------------------------------------------------------------- liquidity
  void lp_add(std::uint16_t s, char side, int extra_ticks) {
    const double v = model_.value(s);
    const std::uint32_t fl = static_cast<std::uint32_t>(std::floor(v * 100.0)) * kTick;
    const std::uint32_t ce = static_cast<std::uint32_t>(std::ceil(v * 100.0)) * kTick;
    int k = extra_ticks;
    while (k < 12 && model_.u() < 0.45) ++k;
    const std::uint32_t px =
        side == 'B' ? fl - static_cast<std::uint32_t>(k) * kTick
                    : (ce == fl ? ce + kTick : ce) + static_cast<std::uint32_t>(k) * kTick;
    const auto qty = static_cast<std::uint32_t>(100 * (1 + flow_() % 5));
    const match::OrderKey key = match::make_key(kLp, ++lp_ref_);
    eng_.add(
        {key, s, side == 'B' ? match::Side::kBuy : match::Side::kSell, match::Tif::kDay, qty, px},
        feed_);
    lp_[s].push_back({key, events_[s] + 80});
  }

  void market_event(std::uint16_t s) {
    ++events_[s];
    auto& q = lp_[s];
    while (!q.empty() && q.front().second <= events_[s]) {  // stale quotes leave
      eng_.cancel(q.front().first, 0, feed_);
      q.pop_front();
    }
    const double r = model_.u();
    if (r < 0.47) {
      lp_add(s, model_.u() < 0.5 ? 'B' : 'S', 0);
    } else if (r < 0.62) {
      if (!q.empty()) {  // a provider pulls or trims a quote
        const std::size_t i = flow_() % q.size();
        if (model_.u() < 0.3) {
          eng_.reduce_by(q[i].first, 100, feed_);
        } else {
          eng_.cancel(q[i].first, 0, feed_);
        }
      }
    } else {
      // A taker. Mostly noise, leaning a little toward the hidden value.
      const Tob t = top(*bb_, s);
      if (!t.mid()) return refill(s);
      const double gap = (model_.value(s) * 10'000.0 - t.mid()) / kTick;
      const double p_buy = 0.5 + std::clamp(gap * 0.06, -0.2, 0.2);
      const bool buy = model_.u() < p_buy;
      const auto qty = static_cast<std::uint32_t>(
          100 * (1 + flow_() % static_cast<std::uint64_t>(kTakerLots[s])));
      const std::uint32_t px = buy ? t.ask + 2 * kTick : t.bid - 2 * kTick;
      eng_.add({match::make_key(kTaker, ++taker_ref_), s,
                buy ? match::Side::kBuy : match::Side::kSell, match::Tif::kIoc, qty, px},
               feed_);
    }
    refill(s);
  }

  // Providers step back in when a side is swept empty.
  void refill(std::uint16_t s) {
    for (int i = 0; i < 4; ++i) {
      const Tob t = top(*bb_, s);
      if (t.bid != 0 && t.ask != 0) return;
      if (t.bid == 0) lp_add(s, 'B', 0);
      if (t.ask == 0) lp_add(s, 'S', 0);
    }
  }

  // -------------------------------------------------------------- firm orders
  // Sends one firm order through risk to the engine. Returns its UserRefNum, 0 if blocked.
  std::uint32_t send(risk::Risk& rk, Track& tr, std::uint16_t s, char side, std::uint32_t qty,
                     std::uint32_t px, match::Tif tif) {
    const Tob t = top(*bb_, s);
    const auto v = rk.check_new(oms_, s, side, qty, px, t.mid(), feed_.ts);
    if (v != risk::Verdict::kOk) {
      ++tr.blocked;
      return 0;
    }
    const std::uint32_t ref = oms_.enter(s, side, qty, px);
    if (ref == 0) return 0;
    ++tr.orders;
    eng_.add({match::make_key(kFirm, ref), s, side == 'B' ? match::Side::kBuy : match::Side::kSell,
              tif, qty, px},
             feed_);
    refill(s);
    return ref;
  }
  void cancel(std::uint32_t ref) {
    if (ref != 0 && oms_.cancel(ref)) eng_.cancel(match::make_key(kFirm, ref), 0, feed_);
  }

  // Marketable IOC toward a target position, crossing at most `reach` ticks.
  void work(risk::Risk& rk, Track& tr, std::uint16_t s, std::int64_t target,
            std::uint32_t max_child, std::uint32_t reach) {
    const auto& p = oms_.position(s);
    const auto c = strategy::work_toward(target, p.shares, p.open_buy, p.open_sell, max_child, 100);
    if (c.side == 0) return;
    const Tob t = top(*bb_, s);
    if (!t.mid()) return;
    const std::uint32_t px = c.side == 'B' ? t.ask + reach * kTick : t.bid - reach * kTick;
    send(rk, tr, s, c.side, c.qty, px, match::Tif::kIoc);
  }

  // -------------------------------------------------------------- market maker
  void mm_event(int day, int minute) {
    const oms::Order* bo = bid_ref_ ? oms_.find(bid_ref_) : nullptr;
    const oms::Order* ao = ask_ref_ ? oms_.find(ask_ref_) : nullptr;
    if (bo && !bo->working()) bo = nullptr, bid_ref_ = 0;
    if (ao && !ao->working()) ao = nullptr, ask_ref_ = 0;
    if (minute < kMinutes - 1) {
      const auto t = top2(*bb_, kAapl);
      const auto m = strategy::others(t, bo ? bo->price : 0, bo ? bo->open : 0, ao ? ao->price : 0,
                                      ao ? ao->open : 0);
      const auto q = mm_.quote(m, oms_.position(kAapl).shares);
      requote('B', bid_ref_, bo, q.bid_px, q.bid_qty);
      requote('S', ask_ref_, ao, q.ask_px, q.ask_qty);
    }
    record_mm_frame(day, minute);
  }
  void requote(char side, std::uint32_t& ref, const oms::Order* o, std::uint32_t px,
               std::uint32_t qty) {
    if (o != nullptr && (qty == 0 || strategy::should_move(mm_.params(), o->price, px))) {
      cancel(ref);
      ref = 0;
      o = nullptr;
    }
    if (o == nullptr && qty > 0 && ref == 0) {
      const std::uint32_t r = send(risk_mm_, mm_track_, kAapl, side, qty, px, match::Tif::kDay);
      const oms::Order* n = r ? oms_.find(r) : nullptr;
      ref = n != nullptr && n->working() ? r : 0;  // it may have filled at once
    }
  }

  void record_mm_frame(int day, int minute) {
    // Day 2 from 10:00, after a day of warm-up.
    if (day != 2 || minute < 30 || mm_frames_.size() >= static_cast<std::size_t>(args_.mm_frames)) {
      tape_.clear();
      return;
    }
    const auto& b = bb_->book(kAapl);
    std::string f = "{\"t\":" + std::to_string(feed_.ts) + ",\"b\":[";
    for (std::size_t d = 0; d < 6; ++d) {
      const auto* l = b.bids().level(d);
      if (!l) break;
      f += (d ? "," : "") + std::string("[") + std::to_string(l->price / 100) + "," +
           std::to_string(l->shares) + "]";
    }
    f += "],\"a\":[";
    for (std::size_t d = 0; d < 6; ++d) {
      const auto* l = b.asks().level(d);
      if (!l) break;
      f += (d ? "," : "") + std::string("[") + std::to_string(l->price / 100) + "," +
           std::to_string(l->shares) + "]";
    }
    const oms::Order* bo = bid_ref_ ? oms_.find(bid_ref_) : nullptr;
    const oms::Order* ao = ask_ref_ ? oms_.find(ask_ref_) : nullptr;
    const auto& p = oms_.position(kAapl);
    const Tob t = top(*bb_, kAapl);
    f += "],\"ub\":" + std::string(bo && bo->working() ? "[" + std::to_string(bo->price / 100) +
                                                             "," + std::to_string(bo->open) + "]"
                                                       : "null");
    f += ",\"ua\":" + std::string(ao && ao->working() ? "[" + std::to_string(ao->price / 100) +
                                                            "," + std::to_string(ao->open) + "]"
                                                      : "null");
    f += ",\"pos\":" + std::to_string(p.shares) + ",\"pnl\":" + num(dollars(p.pnl(t.mid())));
    f += ",\"fills\":" + std::to_string(p.fills) + ",\"m\":[";
    for (std::size_t i = 0; i < tape_.size() && i < 6; ++i)
      f += (i ? ",\"" : "\"") + tape_[i] + "\"";
    f += "]}";
    mm_frames_.push_back(std::move(f));
    tape_.clear();
  }

  // -------------------------------------------------------------- slower strategies
  void on_minute(int day, int minute) {
    // AAPL tape only while recording the market maker's frames.
    feed_.tape = (day == 2 && minute >= 29) ? &tape_ : nullptr;
    // Stat arb: decide every 5 minutes, work the legs every minute.
    if (minute % 5 == 4) {
      const Tob a = top(*bb_, kSema), b = top(*bb_, kSemb);
      const auto sig = pairs_.on_sample(a.mid(), b.mid());
      const char* ev = sig.entered ? "in" : sig.exited ? "out" : sig.stopped ? "stop" : "";
      if (sig.entered || sig.exited || sig.stopped) ++pairs_trades_;
      if (sig.exited || sig.stopped) {
        const double pnl = pairs_pnl();
        (pnl - pairs_mark_ >= 0 ? pairs_wins_ : pairs_losses_)++;
      }
      if (sig.entered) pairs_mark_ = pairs_pnl();
      char row[160];
      std::snprintf(row, sizeof row, "[%.2f,%.2f,%.3f,%d,%lld,%lld,%.0f,\"%s\"]", a.mid() / 1e4,
                    b.mid() / 1e4, sig.z, static_cast<int>(sig.state),
                    static_cast<long long>(oms_.position(kSema).shares),
                    static_cast<long long>(oms_.position(kSemb).shares), pairs_pnl(), ev);
      pairs_rows_.emplace_back(row);
    }
    if (minute < kMinutes - 1) {
      const Tob a = top(*bb_, kSema), b = top(*bb_, kSemb);
      work(risk_pairs_, pairs_track_, kSema, pairs_.target_a(a.mid()), 1'000, 2);
      work(risk_pairs_, pairs_track_, kSemb, pairs_.target_b(b.mid()), 1'000, 2);
      // Trend: the target set at yesterday's close, worked through the day.
      work(risk_trend_, trend_track_, kTrnd, trend_target_, 1'000, 2);
      // Fundamental: participation of volume, at the touch.
      fund_work();
    }
  }

  void fund_work() {
    const auto& p = oms_.position(kValu);
    const std::int64_t target = fund_target_;
    const std::int64_t gap = target - p.shares;
    if (gap == 0) return;
    // Cumulative participation since this parent order started.
    const std::uint64_t cum = fund_cum_volume_ + feed_.volume[kValu];
    const std::uint64_t allowed = strategy::pov_child(
        pov_, cum, static_cast<std::uint64_t>(std::llabs(target - fund_parent_start_)));
    const auto done = static_cast<std::uint64_t>(std::llabs(p.shares - fund_parent_start_));
    if (allowed <= done) return;
    const char side = gap > 0 ? 'B' : 'S';
    const std::uint64_t working = side == 'B' ? p.open_buy : p.open_sell;
    const std::uint64_t want =
        std::min<std::uint64_t>(allowed - done, static_cast<std::uint64_t>(std::llabs(gap)));
    if (working >= want) return;
    const auto qty =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(want - working, 5'000) / 100 * 100);
    if (qty == 0) return;
    const Tob t = top(*bb_, kValu);
    if (!t.mid()) return;
    send(risk_fund_, fund_track_, kValu, side, qty, side == 'B' ? t.ask : t.bid, match::Tif::kIoc);
  }

  double pnl_of(std::uint16_t s) const {
    const Tob t = top(*bb_, s);
    const auto mark = t.mid() ? t.mid() : static_cast<std::uint32_t>(feed_.last_trade[s]);
    return dollars(oms_.position(s).pnl(mark));
  }
  double pairs_pnl() const { return pnl_of(kSema) + pnl_of(kSemb); }

  void end_of_day(int day, const std::array<std::uint64_t, 13>& fund_mkt,
                  const std::array<std::uint64_t, 13>& fund_us) {
    // Close-of-day marks and decisions, before the books are cleared.
    const double mm_pnl = pnl_of(kAapl), pp = pairs_pnl(), tp = pnl_of(kTrnd), fp = pnl_of(kValu);
    const std::uint32_t trnd_close = static_cast<std::uint32_t>(feed_.last_trade[kTrnd]);
    const std::uint32_t valu_close = static_cast<std::uint32_t>(feed_.last_trade[kValu]);
    // Book check: the rebuilt books must equal the engine's.
    for (std::uint16_t s = 1; s < kSymbols; ++s) {
      const Tob t = top(*bb_, s);
      const auto eb = eng_.best(s, match::Side::kBuy), ea = eng_.best(s, match::Side::kSell);
      if ((eb ? eb->price : 0) != t.bid || (ea ? ea->price : 0) != t.ask) ++book_mismatches_;
    }
    // Everything still resting expires at the close (day orders).
    if (bid_ref_) cancel(bid_ref_), bid_ref_ = 0;
    if (ask_ref_) cancel(ask_ref_), ask_ref_ = 0;
    eng_.cancel_owner(kLp, match::CancelReason::kSystem, feed_);
    eng_.cancel_owner(kFirm, match::CancelReason::kSystem, feed_);
    integrity_errors_ += bb_->checks().error_count();
    messages_ += bb_->messages();

    mm_track_.daily_pnl.push_back(mm_pnl);
    pairs_track_.daily_pnl.push_back(pp);
    trend_track_.daily_pnl.push_back(tp);
    fund_track_.daily_pnl.push_back(fp);
    {
      const auto& p = oms_.position(kAapl);
      char row[128];
      std::snprintf(row, sizeof row, "[%.2f,%lld,%llu,%llu]", mm_pnl,
                    static_cast<long long>(p.shares), static_cast<unsigned long long>(p.fills),
                    static_cast<unsigned long long>(feed_.volume[kAapl]));
      mm_days_.emplace_back(row);
    }
    // Trend: today's bar, then tomorrow's target.
    {
      const auto sig = trend_.on_close(trnd_close);
      if (sig.changed) ++trend_signals_;
      trend_target_ = trend_.target(trnd_close);
      char row[200];
      std::snprintf(row, sizeof row, "[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%lld,%.0f,%d]",
                    feed_.open[kTrnd] / 1e4, feed_.high[kTrnd] / 1e4, feed_.low[kTrnd] / 1e4,
                    trnd_close / 1e4, sig.fast_ma / 1e4, sig.slow_ma / 1e4, sig.direction,
                    static_cast<long long>(oms_.position(kTrnd).shares), tp, sig.changed ? 1 : 0);
      trend_rows_.emplace_back(row);
    }
    // Fundamental.
    {
      const auto& p = oms_.position(kValu);
      if (p.sold == 0 && p.shares > 0) fund_avg_ = static_cast<std::uint32_t>(-p.cash / p.shares);
      const std::uint32_t avg = p.shares > 0 ? fund_avg_ : 0;
      const auto d = thesis_.on_day(static_cast<std::uint32_t>(day), valu_close, p.shares, avg);
      if (d.target != fund_target_) {
        fund_parent_start_ = p.shares;
        fund_cum_volume_ = 0;
      } else {
        fund_cum_volume_ += feed_.volume[kValu];
      }
      fund_target_ = d.target;
      std::string row = "[" + num(valu_close / 1e4) + "," + num(model_.valu) + "," +
                        std::to_string(p.shares) + "," + num(avg / 1e4) + "," + num(fp, 0) + ",\"" +
                        strategy::thesis_phase_name(d.phase) + "\",[";
      for (std::size_t i = 0; i < 13; ++i) {
        row += (i ? "," : "") + std::string("[") + std::to_string(fund_mkt[i]) + "," +
               std::to_string(fund_us[i]) + "]";
      }
      fund_rows_.push_back(row + "]]");
    }
  }

  Args args_;
  Model model_;
  std::mt19937_64 flow_;
  match::Engine eng_;
  oms::Oms oms_;
  Feed feed_;
  std::unique_ptr<book::BookBuilder> bb_;
  std::vector<std::deque<std::pair<match::OrderKey, std::uint64_t>>> lp_;
  std::array<std::uint64_t, kSymbols> events_{};
  std::uint64_t lp_ref_ = 0, taker_ref_ = 0;

  strategy::MarketMaker mm_;
  strategy::PairsTrader pairs_;
  strategy::TrendFollower trend_;
  strategy::ThesisTrader thesis_;
  strategy::PovParams pov_{};
  risk::Risk risk_mm_, risk_pairs_, risk_trend_, risk_fund_;
  Track mm_track_, pairs_track_, trend_track_, fund_track_;

  std::uint32_t bid_ref_ = 0, ask_ref_ = 0;
  std::vector<std::string> tape_;
  std::vector<std::string> mm_frames_, mm_days_, pairs_rows_, trend_rows_, fund_rows_;
  std::uint64_t pairs_trades_ = 0, pairs_wins_ = 0, pairs_losses_ = 0, trend_signals_ = 0;
  double pairs_mark_ = 0;
  std::int64_t trend_target_ = 0, fund_target_ = 0, fund_parent_start_ = 0;
  std::uint64_t fund_cum_volume_ = 0;
  std::uint32_t fund_avg_ = 0;  // Price(4) paid while building

  std::uint64_t integrity_errors_ = 0, book_mismatches_ = 0, messages_ = 0;
  double wall_s_ = 0;
};

std::string join(const std::vector<std::string>& v) {
  std::string o = "[";
  for (std::size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + v[i];
  return o + "]";
}

std::string Lab::summary(const Track& t, std::initializer_list<std::uint16_t> syms) const {
  std::uint64_t fills = 0, shares = 0;
  for (auto s : syms) {
    fills += oms_.position(s).fills;
    shares += oms_.position(s).bought + oms_.position(s).sold;
  }
  const double pnl = t.daily_pnl.empty() ? 0 : t.daily_pnl.back();
  return "{\"pnl\":" + num(pnl) + ",\"sharpe\":" + num(t.sharpe()) +
         ",\"max_drawdown\":" + num(t.max_drawdown()) + ",\"orders\":" + std::to_string(t.orders) +
         ",\"fills\":" + std::to_string(fills) + ",\"shares\":" + std::to_string(shares) +
         ",\"risk_blocked\":" + std::to_string(t.blocked) + ",\"daily\":[" +
         [&] {
           std::string o;
           for (std::size_t i = 0; i < t.daily_pnl.size(); ++i)
             o += (i ? "," : "") + num(t.daily_pnl[i], 0);
           return o;
         }() +
         "]}";
}

std::string Lab::json() const {
  std::string feed = "{";
  for (std::uint16_t s = 1; s < kSymbols; ++s) {
    const auto& c = feed_.by_type[s];
    feed += (s > 1 ? "," : "") + std::string("\"") + kNames[s] +
            "\":{\"A\":" + std::to_string(c[0]) + ",\"E\":" + std::to_string(c[1]) +
            ",\"X\":" + std::to_string(c[2]) + ",\"D\":" + std::to_string(c[3]) + "}";
  }
  feed += "}";
  std::vector<std::string> notes;
  for (const auto& n : model_.notes) notes.push_back("\"" + n + "\"");
  std::string o = "{\"meta\":{\"seed\":" + std::to_string(args_.seed) +
                  ",\"days\":" + std::to_string(args_.days) + ",\"wall_s\":" + num(wall_s_) +
                  ",\"messages\":" + std::to_string(messages_) +
                  ",\"integrity_errors\":" + std::to_string(integrity_errors_) +
                  ",\"book_mismatches\":" + std::to_string(book_mismatches_) +
                  ",\"oms_inconsistent\":" + std::to_string(oms_.stats().inconsistent) +
                  ",\"engine\":{\"accepted\":" + std::to_string(eng_.stats().accepted) +
                  ",\"executions\":" + std::to_string(eng_.stats().executions) +
                  ",\"executed_shares\":" + std::to_string(eng_.stats().executed_shares) +
                  "},\"feed\":" + feed + ",\"notes\":" + join(notes) + "}";
  o += ",\"mm\":{\"symbol\":\"AAPL\",\"frames\":" + join(mm_frames_) +
       ",\"days\":" + join(mm_days_) + ",\"summary\":" + summary(mm_track_, {kAapl}) + "}";
  o += ",\"pairs\":{\"symbols\":[\"SEMA\",\"SEMB\"],\"rows\":" + join(pairs_rows_) +
       ",\"round_trips\":" + std::to_string(pairs_wins_ + pairs_losses_) +
       ",\"wins\":" + std::to_string(pairs_wins_) + ",\"losses\":" + std::to_string(pairs_losses_) +
       ",\"summary\":" + summary(pairs_track_, {kSema, kSemb}) + "}";
  o += ",\"trend\":{\"symbol\":\"TRND\",\"rows\":" + join(trend_rows_) +
       ",\"signals\":" + std::to_string(trend_signals_) +
       ",\"summary\":" + summary(trend_track_, {kTrnd}) + "}";
  o += ",\"fund\":{\"symbol\":\"VALU\",\"fair_value\":" + num(Model::kValuTrue) +
       ",\"catalyst_day\":" + std::to_string(Model::kCatalystDay) +
       ",\"target_shares\":" + std::to_string(thesis_.params().target_shares) +
       ",\"rows\":" + join(fund_rows_) + ",\"summary\":" + summary(fund_track_, {kValu}) + "}";
  return o + "}\n";
}

void Lab::print_summary() const {
  std::printf("quant-lab: %d days, %llu ITCH messages, %.1f s\n", args_.days,
              static_cast<unsigned long long>(messages_), wall_s_);
  std::printf("  integrity errors %llu, book mismatches %llu, OMS inconsistent %llu\n",
              static_cast<unsigned long long>(integrity_errors_),
              static_cast<unsigned long long>(book_mismatches_),
              static_cast<unsigned long long>(oms_.stats().inconsistent));
  const struct {
    const char* name;
    const Track* t;
  } rows[] = {{"market making (AAPL)", &mm_track_},
              {"stat arb (SEMA/SEMB)", &pairs_track_},
              {"trend (TRND)", &trend_track_},
              {"fundamental (VALU)", &fund_track_}};
  for (const auto& r : rows) {
    std::printf("  %-22s P&L %14.2f  Sharpe %6.2f  max DD %12.2f  orders %8llu  blocked %llu\n",
                r.name, r.t->daily_pnl.empty() ? 0.0 : r.t->daily_pnl.back(), r.t->sharpe(),
                r.t->max_drawdown(), static_cast<unsigned long long>(r.t->orders),
                static_cast<unsigned long long>(r.t->blocked));
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has = i + 1 < argc;
    if (flag == "--days" && has) {
      a.days = std::atoi(argv[++i]);
    } else if (flag == "--seed" && has) {
      a.seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (flag == "--out" && has) {
      a.out = argv[++i];
    } else if (flag == "--mm-frames" && has) {
      a.mm_frames = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: quant-lab [--days N] [--seed S] [--out FILE] [--mm-frames N]\n"
                   "Runs market making, stat arb, trend and fundamental strategies for N\n"
                   "simulated days on the lab's engine, feed, OMS and risk; writes JSON.\n");
      return 2;
    }
  }
  if (a.days < 2) a.days = 2;
  Lab lab{a};
  const int rc = lab.run();
  lab.print_summary();
  std::FILE* f = std::fopen(a.out.c_str(), "w");
  if (f == nullptr) {
    std::perror(a.out.c_str());
    return 2;
  }
  const std::string j = lab.json();
  std::fwrite(j.data(), 1, j.size(), f);
  std::fclose(f);
  std::printf("  wrote %s (%zu bytes)\n", a.out.c_str(), j.size());
  return rc;
}
