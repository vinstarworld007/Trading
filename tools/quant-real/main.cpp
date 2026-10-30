// quant-real: the Quant Research Lab's four strategies backtested on real
// Nasdaq data, written out as JSON for the page's real-data version.
//
// The intraday strategies replay one Nasdaq TotalView-ITCH 5.0 day (a sample
// file from emi.nasdaq.com) through BookBuilder, exactly as the trader's feed
// thread would, and trade from the rebuilt books:
//
//   --mm SYM       market making   market_maker.hpp, quotes on every book change
//   --pair A,B     statistical arb pairs.hpp, 1-minute samples
//
// The firm's orders cannot reach a historical book, so queue_model.hpp decides
// when they would have filled: resting orders join the back of their level and
// fill once the executions at that price have used up the queue ahead of them;
// marketable orders take the displayed size. Fills go back through the OMS as
// order responses, and every order passes pre-trade risk first.
//
// The slower strategies need more than one day, so they run on daily bars
// (research/fetch_daily.py writes them from Nasdaq's historical quotes):
//
//   --trend SYM    trend following trend.hpp; decided at the close, filled at
//                  the next open plus 2 bps
//   --fund SYM     fundamental     thesis.hpp; fair value = trailing EPS x P/E,
//                  both known before the test starts; bought and sold at 6% of
//                  each day's volume at the day's typical price plus 5 bps
//
// No fees or rebates are counted, and the firm's orders have no market impact.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "daily.hpp"
#include "lab/book/book_builder.hpp"
#include "lab/core/endian.hpp"
#include "lab/itch/codec.hpp"
#include "lab/itch/file_reader.hpp"
#include "lab/itch/messages.hpp"
#include "lab/oms/oms.hpp"
#include "lab/risk/risk.hpp"
#include "lab/strategy/execution.hpp"
#include "lab/strategy/market_maker.hpp"
#include "lab/strategy/pairs.hpp"
#include "lab/strategy/thesis.hpp"
#include "lab/strategy/trend.hpp"
#include "queue_model.hpp"

using namespace lab;

namespace {

constexpr std::uint32_t kTick = 100;  // one cent in Price(4)
constexpr int kMinutes = 390;         // 09:30 to 16:00
constexpr std::uint64_t kOpenNs = 34'200'000'000'000ULL;
constexpr std::uint64_t kMinuteNs = 60'000'000'000ULL;
constexpr std::uint64_t kCloseNs = kOpenNs + kMinutes * kMinuteNs;

// OMS instruments: one per strategy leg.
enum Inst : std::uint16_t { kMm = 1, kPairA, kPairB, kTrend, kFund, kInsts };

double dollars(std::int64_t price4) { return static_cast<double>(price4) / 10'000.0; }

struct Args {
  std::string itch, itch_date, daily_dir, out = "quant-real.json";
  std::string mm = "AAPL", pair_a = "GOOGL", pair_b = "GOOG", trend = "QQQ", fund = "AAPL";
  std::string from = "2019-01-02", to = "2019-12-31";
  std::uint32_t fund_eps = 119'100;  // $11.91, Apple's fiscal 2018 diluted EPS (10-K, Nov 2018)
  std::uint32_t fund_pe = 15;
  int mm_frames = 900;
  int mm_from_minute = 30;  // frames start at 10:00
  std::uint64_t limit = 0;
};

// "01302019.NASDAQ_ITCH50.gz" -> "2019-01-30"; empty if the name has no date.
std::string date_from_name(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  if (base.size() < 8) return {};
  for (int i = 0; i < 8; ++i)
    if (base[static_cast<std::size_t>(i)] < '0' || base[static_cast<std::size_t>(i)] > '9')
      return {};
  return base.substr(4, 4) + "-" + base.substr(0, 2) + "-" + base.substr(2, 2);
}

// Running stats for one strategy's P&L series.
struct Track {
  std::vector<double> pnl;  // cumulative, dollars, one per period
  bool intraday = false;    // per-minute series of one day: no annualized Sharpe
  std::uint64_t orders = 0;
  std::uint64_t blocked = 0;
  double sharpe() const {
    if (intraday || pnl.size() < 3) return 0;
    std::vector<double> d;
    for (std::size_t i = 1; i < pnl.size(); ++i) d.push_back(pnl[i] - pnl[i - 1]);
    double m = 0, v = 0;
    for (double x : d) m += x;
    m /= static_cast<double>(d.size());
    for (double x : d) v += (x - m) * (x - m);
    v /= static_cast<double>(d.size());
    return v > 0 ? m / std::sqrt(v) * std::sqrt(252.0) : 0;
  }
  double max_drawdown() const {
    double peak = 0, dd = 0;
    for (double x : pnl) {
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

std::string join(const std::vector<std::string>& v) {
  std::string o = "[";
  for (std::size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + v[i];
  return o + "]";
}

std::string quoted(const std::string& s) {
  std::string o = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

struct Tob {
  std::uint32_t bid = 0, ask = 0;
  std::uint32_t mid() const { return bid && ask > bid ? (bid + ask) / 2 : 0; }
};

// ------------------------------------------------------------------ the backtest
class Backtest {
 public:
  explicit Backtest(const Args& a)
      : args_{a},
        oms_{kInsts},
        mm_{strategy::MakerParams{}},
        pairs_{pairs_params()},
        trend_{strategy::TrendParams{}},
        thesis_{thesis_params(a)},
        risk_mm_{mm_limits(), 1e9},
        risk_pairs_{pairs_limits(), 1e9},
        risk_trend_{trend_limits(), 1e9},
        risk_fund_{fund_limits(), 1e9} {
    mm_track_.intraday = pairs_track_.intraday = true;
  }

  // Returns false when an input cannot be read.
  bool run() {
    const auto t0 = std::chrono::steady_clock::now();
    if (!replay_itch()) return false;
    if (!run_daily()) return false;
    wall_s_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
  }
  bool clean() const { return integrity_errors_ == 0 && oms_.stats().inconsistent == 0; }

  std::string json() const;
  void print_summary() const;

 private:
  static strategy::PairsParams pairs_params() {
    strategy::PairsParams p;
    p.window = 60;  // one hour of 1-minute samples
    p.min_samples = 30;
    p.leg_notional = 300'000;
    return p;
  }
  static strategy::ThesisParams thesis_params(const Args& a) {
    strategy::ThesisParams p;
    p.fair_value = a.fund_eps * a.fund_pe;
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
    l.max_order_notional = 1'000'000ULL * 10'000;
    return l;
  }
  static risk::Limits pairs_limits() {
    risk::Limits l = base_limits();
    l.max_order_notional = 1'500'000ULL * 10'000;
    l.max_position = 4'000;
    return l;
  }
  static risk::Limits trend_limits() {
    risk::Limits l = base_limits();
    l.max_order_qty = 40'000;
    l.max_order_notional = 2'500'000ULL * 10'000;
    l.max_position = 40'000;
    return l;
  }
  static risk::Limits fund_limits() {
    risk::Limits l = base_limits();
    l.max_order_qty = 650'000;
    l.max_order_notional = 150'000'000ULL * 10'000;
    l.max_position = 650'000;
    return l;
  }

  // -------------------------------------------------------------- OMS plumbing
  void respond(char type, std::uint32_t ref, std::uint32_t qty, std::uint32_t px, char flag = ' ') {
    oms::OrderResponse r{};
    r.type = type;
    r.user_ref = ref;
    r.qty = qty;
    r.price = px;
    r.number = ++exchange_seq_;
    r.flag = flag;
    oms_.apply(r);
  }

  // Risk check and OMS entry, acknowledged at once. Returns the UserRefNum, 0 if blocked.
  std::uint32_t enter(risk::Risk& rk, Track& tr, std::uint16_t inst, char side, std::uint32_t qty,
                      std::uint32_t px, std::uint32_t reference, std::uint64_t now_ns) {
    if (rk.check_new(oms_, inst, side, qty, px, reference, now_ns) != risk::Verdict::kOk) {
      ++tr.blocked;
      return 0;
    }
    const std::uint32_t ref = oms_.enter(inst, side, qty, px);
    if (ref == 0) return 0;
    ++tr.orders;
    respond(oms::OrderResponse::kAccepted, ref, qty, px);
    return ref;
  }

  // A filled order: execution and done, at `px`.
  void fill_all(std::uint32_t ref, std::uint32_t qty, std::uint32_t px) {
    respond(oms::OrderResponse::kExecuted, ref, qty, px, 'R');
  }

  // -------------------------------------------------------------- ITCH replay
  Tob top(std::uint16_t locate) const {
    Tob t;
    if (locate == 0) return t;
    const auto& b = bb_->book(locate);
    if (const auto* l = b.bids().best()) t.bid = l->price;
    if (const auto* l = b.asks().best()) t.ask = l->price;
    return t;
  }
  std::uint64_t level_shares(std::uint16_t locate, char side, std::uint32_t px) const {
    const auto& b = bb_->book(locate);
    const auto& s = side == 'B' ? b.bids() : b.asks();
    for (std::size_t d = 0;; ++d) {
      const auto* l = s.level(d);
      if (l == nullptr) return 0;
      if (l->price == px) return l->shares;
      if (side == 'B' ? l->price < px : l->price > px) return 0;
    }
  }

  bool replay_itch() {
    itch::FileReader reader{args_.itch};
    if (!reader.ok()) {
      std::fprintf(stderr, "quant-real: %s\n", reader.error().c_str());
      return false;
    }
    if (itch_date_.empty())
      itch_date_ = args_.itch_date.empty() ? date_from_name(args_.itch) : args_.itch_date;
    bb_ = std::make_unique<book::BookBuilder>();
    next_minute_ns_ = kOpenNs + kMinuteNs;
    while (auto msg = reader.next()) {
      on_message(msg->data(), msg->size());
      if (args_.limit != 0 && bb_->messages() >= args_.limit) break;
    }
    if (!reader.ok()) {
      std::fprintf(stderr, "quant-real: %s\n", reader.error().c_str());
      return false;
    }
    while (minute_ < kMinutes) end_minute();
    integrity_errors_ = bb_->checks().error_count();
    if (loc_fund_ != 0) itch_close_ = bb_->instrument(loc_fund_).stats.closing_cross_price;
    messages_ = bb_->messages();
    if (loc_mm_ == 0 || loc_a_ == 0 || loc_b_ == 0) {
      std::fprintf(stderr, "quant-real: %s, %s or %s is not in %s\n", args_.mm.c_str(),
                   args_.pair_a.c_str(), args_.pair_b.c_str(), args_.itch.c_str());
      return false;
    }
    return true;
  }

  int watched(std::uint16_t locate) const {
    if (locate == 0) return -1;
    if (locate == loc_mm_) return 0;
    if (locate == loc_a_) return 1;
    if (locate == loc_b_) return 2;
    if (locate == loc_fund_) return 3;
    return -1;
  }

  void on_message(const std::byte* p, std::size_t n) {
    const char type = static_cast<char>(p[0]);
    const std::uint16_t locate = n >= 3 ? load_be16(p + 1) : 0;
    const int w = watched(locate);
    // Executions are decoded before the book forgets the order's side and price.
    if (w >= 0) count(w, type);
    if (locate != 0 && locate == loc_fund_ && (type == 'E' || type == 'C' || type == 'P'))
      fund_volume(type, p);
    if (w >= 0 && (type == 'E' || type == 'C' || type == 'P')) on_trade_message(type, p, w);
    if (w == 0 && in_tape_window()) tape_message(type, p);
    bb_->apply(p, n);
    if (type == 'R') on_directory(p, locate);
    const std::uint64_t ts = bb_->last_timestamp();
    while (minute_ < kMinutes && ts >= next_minute_ns_) end_minute();
    if (w < 0 || ts < kOpenNs || ts >= kCloseNs) return;
    if (w == 0 || w == 1 || w == 2) recheck_shadows(locate);
    if (w == 0) mm_event(ts);
  }

  void on_directory(const std::byte* p, std::uint16_t locate) {
    itch::StockDirectory sd;
    itch::decode(p, sd);
    const std::string_view s = itch::trimmed(sd.stock);
    if (s == args_.mm) loc_mm_ = locate;
    if (s == args_.pair_a) loc_a_ = locate;
    if (s == args_.pair_b) loc_b_ = locate;
    if (s == args_.fund) loc_fund_ = locate;
    if (s == args_.mm || s == args_.pair_a || s == args_.pair_b || s == args_.fund) {
      feed_[s == args_.mm       ? 0
            : s == args_.pair_a ? 1
            : s == args_.pair_b ? 2
                                : 3]
          .symbol = std::string{s};
    }
  }

  struct FeedCount {
    std::string symbol;
    std::array<std::uint64_t, 4> by_type{};  // A E X D (F counts as A, C as E, U as D+A)
    std::uint64_t volume = 0;                // printable shares in regular hours
  };

  void count(int w, char type) {
    auto& c = feed_[static_cast<std::size_t>(w)].by_type;
    switch (type) {
      case 'A':
      case 'F':
        ++c[0];
        break;
      case 'E':
      case 'C':
        ++c[1];
        break;
      case 'X':
        ++c[2];
        break;
      case 'D':
        ++c[3];
        break;
      case 'U':
        ++c[3];
        ++c[0];
        break;
      default:
        break;
    }
  }

  void on_trade_message(char type, const std::byte* p, int w) {
    char side = 0;
    std::uint32_t px = 0, shares = 0;
    bool printable = true;
    if (type == 'P') {
      itch::Trade t;
      itch::decode(p, t);
      shares = t.shares;  // hidden: no resting displayed order, so no queue effect
      px = t.price;
    } else {
      std::uint64_t ref = 0;
      if (type == 'E') {
        itch::OrderExecuted e;
        itch::decode(p, e);
        ref = e.order_reference_number;
        shares = e.executed_shares;
      } else {
        itch::OrderExecutedWithPrice c;
        itch::decode(p, c);
        ref = c.order_reference_number;
        shares = c.executed_shares;
        printable = c.printable == 'Y';
      }
      const auto* o = bb_->orders().find(ref);
      if (o == nullptr) return;
      side = o->side == book::Side::kBuy ? 'B' : 'S';
      px = o->price;
    }
    const std::uint64_t ts = bb_->last_timestamp();
    if (printable && ts >= kOpenNs && ts < kCloseNs)
      feed_[static_cast<std::size_t>(w)].volume += shares;
    if (side == 0 || ts < kOpenNs || ts >= kCloseNs) return;
    // Our resting orders on that side: does this trade reach them?
    for (auto& s : shadows_) {
      if (s.inst != inst_of(w)) continue;
      const auto f = backtest::on_execution(s.o, side, px, shares);
      if (f.qty > 0) shadow_fill(s, f);
    }
    if (w == 0) trades_this_event_ = true;
  }

  // The fundamental symbol's printable volume per half hour, for the ITCH day's row.
  void fund_volume(char type, const std::byte* p) {
    std::uint32_t shares = 0;
    if (type == 'P') {
      itch::Trade t;
      itch::decode(p, t);
      shares = t.shares;
    } else if (type == 'E') {
      itch::OrderExecuted e;
      itch::decode(p, e);
      shares = e.executed_shares;
    } else {
      itch::OrderExecutedWithPrice c;
      itch::decode(p, c);
      if (c.printable != 'Y') return;
      shares = c.executed_shares;
    }
    const std::uint64_t ts = bb_->last_timestamp();
    if (ts < kOpenNs || ts >= kCloseNs) return;
    fund_buckets_[static_cast<std::size_t>((ts - kOpenNs) / (30 * kMinuteNs))] += shares;
    fund_day_volume_ += shares;
  }

  static std::uint16_t inst_of(int w) {
    return w == 0 ? kMm : w == 1 ? kPairA : w == 2 ? kPairB : kFund;
  }
  std::uint16_t locate_of(std::uint16_t inst) const {
    return inst == kMm ? loc_mm_ : inst == kPairA ? loc_a_ : inst == kPairB ? loc_b_ : loc_fund_;
  }

  struct Shadow {
    std::uint16_t inst = 0;
    backtest::ShadowOrder o;
  };

  void shadow_fill(Shadow& s, const backtest::ShadowFill& f) {
    respond(oms::OrderResponse::kExecuted, s.o.ref, f.qty, f.price, 'A');
    if (s.inst == kMm && in_tape_window()) {
      char t[96];
      std::snprintf(t, sizeof t, "E|%c|%u|%u|1", s.o.side, f.qty, f.price);
      tape_buf_.push_back(t);
    }
  }

  void recheck_shadows(std::uint16_t locate) {
    for (auto& s : shadows_) {
      if (locate_of(s.inst) != locate || s.o.open == 0) continue;
      backtest::on_level(s.o, level_shares(locate, s.o.side, s.o.price));
      const auto& b = bb_->book(locate);
      const auto* opp = s.o.side == 'B' ? b.asks().best() : b.bids().best();
      if (opp != nullptr) {
        const auto f = backtest::on_opposite_best(s.o, opp->price, opp->shares);
        if (f.qty > 0) shadow_fill(s, f);
      }
    }
    std::erase_if(shadows_, [](const Shadow& s) { return s.o.open == 0; });
  }

  // A resting (day) order: risk, OMS, then a shadow at the back of its level.
  std::uint32_t rest(risk::Risk& rk, Track& tr, std::uint16_t inst, char side, std::uint32_t qty,
                     std::uint32_t px, std::uint64_t now) {
    const std::uint16_t loc = locate_of(inst);
    const std::uint32_t ref = enter(rk, tr, inst, side, qty, px, top(loc).mid(), now);
    if (ref == 0) return 0;
    shadows_.push_back({inst, backtest::place(ref, side, px, qty, level_shares(loc, side, px))});
    recheck_shadows(loc);  // marketable on arrival: fills against the other side
    return ref;
  }

  void cancel(std::uint32_t ref) {
    if (ref == 0 || !oms_.cancel(ref)) return;
    for (auto& s : shadows_) {
      if (s.o.ref != ref) continue;
      respond(oms::OrderResponse::kCanceled, ref, s.o.open, s.o.price, 'U');
      s.o.open = 0;
    }
    std::erase_if(shadows_, [](const Shadow& s) { return s.o.open == 0; });
  }

  // A marketable IOC: takes displayed size up to `reach` ticks through the touch.
  void take(risk::Risk& rk, Track& tr, std::uint16_t inst, char side, std::uint32_t qty,
            std::uint32_t reach, std::uint64_t now) {
    const std::uint16_t loc = locate_of(inst);
    const Tob t = top(loc);
    if (!t.mid()) return;
    const std::uint32_t limit = side == 'B' ? t.ask + reach * kTick : t.bid - reach * kTick;
    const std::uint32_t ref = enter(rk, tr, inst, side, qty, limit, t.mid(), now);
    if (ref == 0) return;
    const auto& b = bb_->book(loc);
    const auto& opp = side == 'B' ? b.asks() : b.bids();
    std::uint32_t left = qty;
    for (std::size_t d = 0; left > 0; ++d) {
      const auto* l = opp.level(d);
      if (l == nullptr || (side == 'B' ? l->price > limit : l->price < limit)) break;
      const auto q = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, l->shares));
      fill_all(ref, q, l->price);
      left -= q;
    }
    if (left > 0) respond(oms::OrderResponse::kCanceled, ref, left, limit, 'I');
  }

  void work(risk::Risk& rk, Track& tr, std::uint16_t inst, std::int64_t target,
            std::uint32_t max_child, std::uint32_t reach, std::uint64_t now) {
    const auto& p = oms_.position(inst);
    const auto c = strategy::work_toward(target, p.shares, p.open_buy, p.open_sell, max_child, 100);
    if (c.side != 0) take(rk, tr, inst, c.side, c.qty, reach, now);
  }

  // -------------------------------------------------------------- market maker
  bool in_tape_window() const {
    return bb_->last_timestamp() >=
               kOpenNs + static_cast<std::uint64_t>(args_.mm_from_minute) * kMinuteNs &&
           mm_frames_.size() < static_cast<std::size_t>(args_.mm_frames);
  }

  void tape_message(char type, const std::byte* p) {
    char t[96];
    auto push = [&](const char* k, char side, std::uint32_t q, std::uint32_t px) {
      std::snprintf(t, sizeof t, "%s|%c|%u|%u|0", k, side, q, px);
      tape_buf_.push_back(t);
    };
    auto order = [&](std::uint64_t ref) { return bb_->orders().find(ref); };
    auto sd = [](const book::Order* o) { return o->side == book::Side::kBuy ? 'B' : 'S'; };
    switch (type) {
      case 'A': {
        itch::AddOrder a;
        itch::decode(p, a);
        push("A", a.buy_sell_indicator, a.shares, a.price);
        break;
      }
      case 'F': {
        itch::AddOrderMpid a;
        itch::decode(p, a);
        push("A", a.buy_sell_indicator, a.shares, a.price);
        break;
      }
      case 'E': {
        itch::OrderExecuted e;
        itch::decode(p, e);
        if (const auto* o = order(e.order_reference_number))
          push("E", sd(o), e.executed_shares, o->price);
        break;
      }
      case 'C': {
        itch::OrderExecutedWithPrice e;
        itch::decode(p, e);
        if (const auto* o = order(e.order_reference_number))
          push("E", sd(o), e.executed_shares, e.execution_price);
        break;
      }
      case 'X': {
        itch::OrderCancel x;
        itch::decode(p, x);
        if (const auto* o = order(x.order_reference_number))
          push("X", sd(o), x.cancelled_shares, o->price);
        break;
      }
      case 'D': {
        itch::OrderDelete d;
        itch::decode(p, d);
        if (const auto* o = order(d.order_reference_number)) push("D", sd(o), o->shares, o->price);
        break;
      }
      case 'U': {
        itch::OrderReplace u;
        itch::decode(p, u);
        if (const auto* o = order(u.original_order_reference_number)) {
          push("D", sd(o), o->shares, o->price);
          push("A", sd(o), u.shares, u.price);
        }
        break;
      }
      default:
        break;
    }
  }

  void mm_event(std::uint64_t now) {
    const oms::Order* bo = bid_ref_ ? oms_.find(bid_ref_) : nullptr;
    const oms::Order* ao = ask_ref_ ? oms_.find(ask_ref_) : nullptr;
    if (bo && !bo->working()) bo = nullptr, bid_ref_ = 0;
    if (ao && !ao->working()) ao = nullptr, ask_ref_ = 0;
    const bool quoting = now >= kOpenNs + 30'000'000'000ULL && now < kCloseNs - kMinuteNs;
    if (quoting) {
      oms::TopOfBook t{};
      const auto& b = bb_->book(loc_mm_);
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
      // The real book never holds our orders, so there is nothing to subtract.
      const auto m = strategy::others(t, 0, 0, 0, 0);
      if (m.mid() != 0) {
        const auto q = mm_.quote(m, oms_.position(kMm).shares);
        requote('B', bid_ref_, bo, q.bid_px, q.bid_qty, now);
        requote('S', ask_ref_, ao, q.ask_px, q.ask_qty, now);
      }
    } else if (now >= kCloseNs - kMinuteNs) {
      if (bid_ref_) cancel(bid_ref_), bid_ref_ = 0;
      if (ask_ref_) cancel(ask_ref_), ask_ref_ = 0;
    }
    const bool changed = top_changed();
    if (in_tape_window() && (trades_this_event_ || changed)) record_mm_frame(now);
    trades_this_event_ = false;
  }

  bool top_changed() {
    const auto& b = bb_->book(loc_mm_);
    const auto* bl = b.bids().best();
    const auto* al = b.asks().best();
    const std::array<std::uint64_t, 4> now{bl ? bl->price : 0, bl ? bl->shares : 0,
                                           al ? al->price : 0, al ? al->shares : 0};
    const bool changed = now != last_top_;
    last_top_ = now;
    return changed;
  }

  void requote(char side, std::uint32_t& ref, const oms::Order* o, std::uint32_t px,
               std::uint32_t qty, std::uint64_t now) {
    if (o != nullptr && (qty == 0 || strategy::should_move(mm_.params(), o->price, px))) {
      cancel(ref);
      ref = 0;
      o = nullptr;
    }
    if (o == nullptr && qty > 0 && ref == 0) {
      const std::uint32_t r = rest(risk_mm_, mm_track_, kMm, side, qty, px, now);
      const oms::Order* n = r ? oms_.find(r) : nullptr;
      ref = n != nullptr && n->working() ? r : 0;
    }
  }

  void record_mm_frame(std::uint64_t now) {
    const auto& b = bb_->book(loc_mm_);
    std::string f = "{\"t\":" + std::to_string(now) + ",\"b\":[";
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
    // Our orders are not in the real book: the page draws them beside the level.
    auto ours = [&](std::uint32_t ref) -> std::string {
      const oms::Order* o = ref ? oms_.find(ref) : nullptr;
      if (o == nullptr || !o->working()) return "null";
      std::uint64_t ahead = 0;
      for (const auto& s : shadows_)
        if (s.o.ref == ref) ahead = s.o.ahead;
      return "[" + std::to_string(o->price / 100) + "," + std::to_string(o->open) + "," +
             std::to_string(ahead) + "]";
    };
    const auto& p = oms_.position(kMm);
    f += "],\"ub\":" + ours(bid_ref_) + ",\"ua\":" + ours(ask_ref_);
    f += ",\"pos\":" + std::to_string(p.shares) + ",\"pnl\":" + num(pnl_of(kMm));
    f += ",\"fills\":" + std::to_string(p.fills) + ",\"m\":[";
    for (std::size_t i = 0; i < tape_buf_.size() && i < 6; ++i)
      f += (i ? ",\"" : "\"") + tape_buf_[i] + "\"";
    f += "]}";
    mm_frames_.push_back(std::move(f));
    tape_buf_.clear();
  }

  double pnl_of(std::uint16_t inst) const {
    const std::uint16_t loc = locate_of(inst);
    const Tob t = top(loc);
    const auto mark = t.mid() ? t.mid() : last_mark_[inst];
    return dollars(oms_.position(inst).pnl(mark));
  }

  // -------------------------------------------------------------- each minute
  void end_minute() {
    const std::uint64_t now = next_minute_ns_;
    for (std::uint16_t i = kMm; i <= kPairB; ++i)
      if (const auto m = top(locate_of(i)).mid()) last_mark_[i] = m;
    // Stat arb: one sample a minute, legs worked at once with marketable orders.
    const Tob a = top(loc_a_), b = top(loc_b_);
    const auto sig = pairs_.on_sample(a.mid(), b.mid());
    const char* ev = sig.entered ? "in" : sig.exited ? "out" : sig.stopped ? "stop" : "";
    if (sig.exited || sig.stopped) (pairs_pnl() - pairs_mark_ >= 0 ? pairs_wins_ : pairs_losses_)++;
    if (minute_ < kMinutes - 1) {
      work(risk_pairs_, pairs_track_, kPairA, pairs_.target_a(a.mid()), 1'000, 2, now);
      work(risk_pairs_, pairs_track_, kPairB, pairs_.target_b(b.mid()), 1'000, 2, now);
    } else {  // flat into the close
      work(risk_pairs_, pairs_track_, kPairA, 0, 1'000, 2, now);
      work(risk_pairs_, pairs_track_, kPairB, 0, 1'000, 2, now);
    }
    if (sig.entered) pairs_mark_ = pairs_pnl();
    char row[200];
    std::snprintf(row, sizeof row, "[%.2f,%.2f,%.3f,%d,%lld,%lld,%.0f,\"%s\"]", a.mid() / 1e4,
                  b.mid() / 1e4, sig.z, static_cast<int>(pairs_.state()),
                  static_cast<long long>(oms_.position(kPairA).shares),
                  static_cast<long long>(oms_.position(kPairB).shares), pairs_pnl(), ev);
    pairs_rows_.emplace_back(row);
    mm_track_.pnl.push_back(pnl_of(kMm));
    pairs_track_.pnl.push_back(pairs_pnl());
    ++minute_;
    next_minute_ns_ += kMinuteNs;
  }
  double pairs_pnl() const { return pnl_of(kPairA) + pnl_of(kPairB); }

  // -------------------------------------------------------------- daily bars
  std::vector<backtest::DailyBar> load(const std::string& sym) {
    auto bars = backtest::read_daily(args_.daily_dir + "/" + sym + ".csv");
    std::sort(bars.begin(), bars.end(),
              [](const auto& x, const auto& y) { return x.date < y.date; });
    if (bars.empty())
      std::fprintf(stderr, "quant-real: no bars in %s/%s.csv\n", args_.daily_dir.c_str(),
                   sym.c_str());
    return bars;
  }

  // Next open plus or minus `bps`, in whole cents.
  static std::uint32_t slipped(std::uint32_t px, char side, std::uint32_t bps) {
    const std::uint64_t adj = static_cast<std::uint64_t>(px) * bps / 10'000;
    const std::uint64_t raw = side == 'B' ? px + adj : px - adj;
    return static_cast<std::uint32_t>((raw + kTick / 2) / kTick * kTick);
  }

  void daily_fill(risk::Risk& rk, Track& tr, std::uint16_t inst, char side, std::uint32_t qty,
                  std::uint32_t px, std::uint32_t reference, std::uint64_t day_ns) {
    const std::uint32_t ref = enter(rk, tr, inst, side, qty, px, reference, day_ns);
    if (ref != 0) fill_all(ref, qty, px);
  }

  bool run_daily() {
    const auto tb = load(args_.trend), fb = load(args_.fund);
    if (tb.empty() || fb.empty()) return false;
    // Trend: warm up on everything before `from`, trade from `from` to `to`.
    std::int64_t target = 0;
    std::uint64_t day_ns = 0;  // a clock for the risk rate limit: a day per bar
    for (const auto& bar : tb) {
      if (bar.date > args_.to) break;
      day_ns += 86'400'000'000'000ULL;
      const bool live = bar.date >= args_.from;
      if (live) {
        const std::int64_t pos = oms_.position(kTrend).shares;
        if (target != pos) {
          const char side = target > pos ? 'B' : 'S';
          const auto qty = static_cast<std::uint32_t>(std::llabs(target - pos));
          daily_fill(risk_trend_, trend_track_, kTrend, side, qty, slipped(bar.open, side, 2),
                     bar.open, day_ns);
        }
      }
      const auto sig = trend_.on_close(bar.close);
      target = trend_.target(bar.close);
      if (!live) continue;
      if (sig.changed) ++trend_signals_;
      const double pnl = dollars(oms_.position(kTrend).pnl(bar.close));
      trend_track_.pnl.push_back(pnl);
      trend_dates_.push_back(quoted(bar.date));
      char row[220];
      std::snprintf(row, sizeof row, "[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%lld,%.0f,%d]",
                    bar.open / 1e4, bar.high / 1e4, bar.low / 1e4, bar.close / 1e4,
                    sig.fast_ma / 1e4, sig.slow_ma / 1e4, sig.direction,
                    static_cast<long long>(oms_.position(kTrend).shares), pnl, sig.changed ? 1 : 0);
      trend_rows_.emplace_back(row);
    }

    // Fundamental: day 1 is `from`. Work the target at 6% of each day's volume.
    std::uint32_t day = 0;
    std::int64_t fund_target = 0;
    std::uint32_t avg = 0;
    for (const auto& bar : fb) {
      if (bar.date < args_.from) continue;
      if (bar.date > args_.to) break;
      ++day;
      day_ns += 86'400'000'000'000ULL;
      const auto& p0 = oms_.position(kFund);
      const std::int64_t gap = fund_target - p0.shares;
      std::uint64_t ours = 0;
      if (gap != 0) {
        const char side = gap > 0 ? 'B' : 'S';
        const std::uint64_t allowed =
            strategy::pov_child(pov_, bar.volume, static_cast<std::uint64_t>(gap > 0 ? gap : -gap));
        const std::uint32_t typical = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(bar.high) + bar.low + bar.close) / 3);
        if (allowed > 0) {
          daily_fill(risk_fund_, fund_track_, kFund, side, static_cast<std::uint32_t>(allowed),
                     slipped(typical, side, 5), typical, day_ns);
          ours = allowed;
        }
      }
      const auto& p = oms_.position(kFund);
      if (p.sold == 0 && p.shares > 0) avg = static_cast<std::uint32_t>(-p.cash / p.shares);
      const std::uint32_t avg_now = p.shares > 0 ? avg : 0;
      const auto d = thesis_.on_day(day, bar.close, p.shares, avg_now);
      fund_target = d.target;
      const double pnl = dollars(p.pnl(bar.close));
      fund_track_.pnl.push_back(pnl);
      fund_dates_.push_back(quoted(bar.date));
      if (bar.date == itch_date_) {
        fund_event_day_ = day;
        daily_close_ = bar.close;
        // The daily bars must be in the prices that traded that day (no later split).
        const auto lo = static_cast<std::uint64_t>(itch_close_) * 98 / 100;
        const auto hi = static_cast<std::uint64_t>(itch_close_) * 102 / 100;
        if (itch_close_ != 0 && (bar.close < lo || bar.close > hi)) {
          std::fprintf(stderr,
                       "quant-real: warning: %s daily close %.2f on %s is not the ITCH closing "
                       "cross %.2f; are the bars adjusted for a later split?\n",
                       args_.fund.c_str(), bar.close / 1e4, bar.date.c_str(), itch_close_ / 1e4);
        }
      }
      std::string row = "[" + num(bar.close / 1e4) + "," + num(thesis_.params().fair_value / 1e4) +
                        "," + std::to_string(p.shares) + "," + num(avg_now / 1e4) + "," +
                        num(pnl, 0) + ",\"" + strategy::thesis_phase_name(d.phase) + "\",[";
      // Half-hour volume only exists for the ITCH day; our share follows it at POV.
      if (bar.date == itch_date_ && loc_fund_ != 0) {
        const std::uint64_t total = fund_day_volume_;
        for (std::size_t i = 0; i < fund_buckets_.size(); ++i) {
          const std::uint64_t us = total ? ours * fund_buckets_[i] / total : 0;
          row += (i ? "," : "") + std::string("[") + std::to_string(fund_buckets_[i]) + "," +
                 std::to_string(us) + "]";
        }
      }
      fund_rows_.push_back(row + "]," + std::to_string(bar.volume) + "," + std::to_string(ours) +
                           "]");
    }
    return true;
  }

  std::string summary(const Track& t, std::initializer_list<std::uint16_t> insts) const;

  Args args_;
  oms::Oms oms_;
  std::unique_ptr<book::BookBuilder> bb_;
  std::uint16_t loc_mm_ = 0, loc_a_ = 0, loc_b_ = 0, loc_fund_ = 0;
  std::array<FeedCount, 4> feed_{};
  std::array<std::uint64_t, 13> fund_buckets_{};
  std::uint64_t fund_day_volume_ = 0;
  std::array<std::uint32_t, kInsts> last_mark_{};
  std::vector<Shadow> shadows_;
  std::uint64_t exchange_seq_ = 0;
  std::uint64_t next_minute_ns_ = 0;
  int minute_ = 0;
  std::string itch_date_;

  strategy::MarketMaker mm_;
  strategy::PairsTrader pairs_;
  strategy::TrendFollower trend_;
  strategy::ThesisTrader thesis_;
  strategy::PovParams pov_{};
  risk::Risk risk_mm_, risk_pairs_, risk_trend_, risk_fund_;
  Track mm_track_, pairs_track_, trend_track_, fund_track_;

  std::uint32_t bid_ref_ = 0, ask_ref_ = 0;
  std::vector<std::string> tape_buf_;
  bool trades_this_event_ = false;
  std::array<std::uint64_t, 4> last_top_{};
  std::vector<std::string> mm_frames_, pairs_rows_, trend_rows_, trend_dates_, fund_rows_,
      fund_dates_;
  std::uint64_t pairs_wins_ = 0, pairs_losses_ = 0, trend_signals_ = 0;
  double pairs_mark_ = 0;
  std::uint32_t fund_event_day_ = 0;
  std::uint32_t itch_close_ = 0, daily_close_ = 0;  // the fund symbol on the ITCH day

  std::uint64_t integrity_errors_ = 0, messages_ = 0;
  double wall_s_ = 0;
};

std::string Backtest::summary(const Track& t, std::initializer_list<std::uint16_t> insts) const {
  std::uint64_t fills = 0, shares = 0;
  for (auto i : insts) {
    fills += oms_.position(i).fills;
    shares += oms_.position(i).bought + oms_.position(i).sold;
  }
  const double pnl = t.pnl.empty() ? 0 : t.pnl.back();
  std::string daily;
  for (std::size_t i = 0; i < t.pnl.size(); ++i) daily += (i ? "," : "") + num(t.pnl[i], 0);
  return "{\"pnl\":" + num(pnl) + ",\"sharpe\":" + num(t.sharpe()) +
         ",\"intraday\":" + (t.intraday ? "true" : "false") +
         ",\"max_drawdown\":" + num(t.max_drawdown()) + ",\"orders\":" + std::to_string(t.orders) +
         ",\"fills\":" + std::to_string(fills) + ",\"shares\":" + std::to_string(shares) +
         ",\"risk_blocked\":" + std::to_string(t.blocked) + ",\"daily\":[" + daily + "]}";
}

std::string Backtest::json() const {
  std::string feed = "{";
  for (std::size_t i = 0; i < feed_.size(); ++i) {
    const auto& c = feed_[i];
    if (c.symbol.empty()) continue;
    feed += (feed.size() > 1 ? "," : "") + quoted(c.symbol) +
            ":{\"A\":" + std::to_string(c.by_type[0]) + ",\"E\":" + std::to_string(c.by_type[1]) +
            ",\"X\":" + std::to_string(c.by_type[2]) + ",\"D\":" + std::to_string(c.by_type[3]) +
            ",\"volume\":" + std::to_string(c.volume) + "}";
  }
  feed += "}";
  const std::size_t slash = args_.itch.find_last_of('/');
  const std::string file = slash == std::string::npos ? args_.itch : args_.itch.substr(slash + 1);
  std::string o = "{\"meta\":{\"mode\":\"real\",\"itch_file\":" + quoted(file) +
                  ",\"itch_date\":" + quoted(itch_date_) + ",\"from\":" + quoted(args_.from) +
                  ",\"to\":" + quoted(args_.to) +
                  ",\"days\":" + std::to_string(trend_rows_.size()) +
                  ",\"wall_s\":" + num(wall_s_) + ",\"messages\":" + std::to_string(messages_) +
                  ",\"integrity_errors\":" + std::to_string(integrity_errors_) +
                  ",\"oms_inconsistent\":" + std::to_string(oms_.stats().inconsistent) +
                  ",\"oms_executions\":" + std::to_string(oms_.stats().executions) +
                  ",\"itch_close\":" + num(itch_close_ / 1e4) +
                  ",\"daily_close\":" + num(daily_close_ / 1e4) + ",\"feed\":" + feed + "}";
  o += ",\"mm\":{\"symbol\":" + quoted(args_.mm) + ",\"frames\":" + join(mm_frames_) +
       ",\"from_minute\":" + std::to_string(args_.mm_from_minute) +
       ",\"summary\":" + summary(mm_track_, {kMm}) + "}";
  o += ",\"pairs\":{\"symbols\":[" + quoted(args_.pair_a) + "," + quoted(args_.pair_b) +
       "],\"rows\":" + join(pairs_rows_) + ",\"window\":" + std::to_string(pairs_.params().window) +
       ",\"leg_notional\":" + std::to_string(pairs_.params().leg_notional) +
       ",\"round_trips\":" + std::to_string(pairs_wins_ + pairs_losses_) +
       ",\"wins\":" + std::to_string(pairs_wins_) + ",\"losses\":" + std::to_string(pairs_losses_) +
       ",\"summary\":" + summary(pairs_track_, {kPairA, kPairB}) + "}";
  o += ",\"trend\":{\"symbol\":" + quoted(args_.trend) + ",\"rows\":" + join(trend_rows_) +
       ",\"dates\":" + join(trend_dates_) + ",\"signals\":" + std::to_string(trend_signals_) +
       ",\"summary\":" + summary(trend_track_, {kTrend}) + "}";
  o += ",\"fund\":{\"symbol\":" + quoted(args_.fund) +
       ",\"fair_value\":" + num(thesis_.params().fair_value / 1e4) +
       ",\"eps\":" + num(args_.fund_eps / 1e4) + ",\"pe\":" + std::to_string(args_.fund_pe) +
       ",\"catalyst_day\":" + std::to_string(fund_event_day_) +
       ",\"target_shares\":" + std::to_string(thesis_.params().target_shares) +
       ",\"rows\":" + join(fund_rows_) + ",\"dates\":" + join(fund_dates_) +
       ",\"summary\":" + summary(fund_track_, {kFund}) + "}";
  return o + "}\n";
}

void Backtest::print_summary() const {
  std::printf("quant-real: %s (%s), %llu ITCH messages, daily bars %s to %s, %.1f s\n",
              args_.itch.c_str(), itch_date_.c_str(), static_cast<unsigned long long>(messages_),
              args_.from.c_str(), args_.to.c_str(), wall_s_);
  std::printf("  integrity errors %llu, OMS inconsistent %llu\n",
              static_cast<unsigned long long>(integrity_errors_),
              static_cast<unsigned long long>(oms_.stats().inconsistent));
  const struct {
    std::string name;
    const Track* t;
  } rows[] = {{"market making (" + args_.mm + ")", &mm_track_},
              {"stat arb (" + args_.pair_a + "/" + args_.pair_b + ")", &pairs_track_},
              {"trend (" + args_.trend + ")", &trend_track_},
              {"fundamental (" + args_.fund + ")", &fund_track_}};
  for (const auto& r : rows) {
    std::printf("  %-26s P&L %14.2f  Sharpe %6.2f  max DD %12.2f  orders %8llu  blocked %llu\n",
                r.name.c_str(), r.t->pnl.empty() ? 0.0 : r.t->pnl.back(), r.t->sharpe(),
                r.t->max_drawdown(), static_cast<unsigned long long>(r.t->orders),
                static_cast<unsigned long long>(r.t->blocked));
  }
}

void usage() {
  std::fprintf(
      stderr,
      "usage: quant-real --itch DAY.gz --daily DIR [--out FILE]\n"
      "         [--mm SYM] [--pair A,B] [--trend SYM] [--fund SYM]\n"
      "         [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--itch-date YYYY-MM-DD]\n"
      "         [--fund-eps DOLLARS] [--fund-pe N] [--mm-frames N] [--mm-from MINUTE] [--limit N]\n"
      "Backtests the Quant Research Lab strategies on a real Nasdaq ITCH day (market making,\n"
      "stat arb) and on daily bars in DIR/SYM.csv (trend, fundamental); writes JSON.\n"
      "Defaults: AAPL, GOOGL/GOOG, QQQ, AAPL; 2019; fair value $11.91 x 15.\n");
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    const std::string v = argv[++i];
    if (flag == "--itch") {
      a.itch = v;
    } else if (flag == "--daily") {
      a.daily_dir = v;
    } else if (flag == "--out") {
      a.out = v;
    } else if (flag == "--mm") {
      a.mm = v;
    } else if (flag == "--pair" && v.find(',') != std::string::npos) {
      a.pair_a = v.substr(0, v.find(','));
      a.pair_b = v.substr(v.find(',') + 1);
    } else if (flag == "--trend") {
      a.trend = v;
    } else if (flag == "--fund") {
      a.fund = v;
    } else if (flag == "--from") {
      a.from = v;
    } else if (flag == "--to") {
      a.to = v;
    } else if (flag == "--itch-date") {
      a.itch_date = v;
    } else if (flag == "--fund-eps" && backtest::parse_price4(v)) {
      a.fund_eps = *backtest::parse_price4(v);
    } else if (flag == "--fund-pe") {
      a.fund_pe = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (flag == "--mm-frames") {
      a.mm_frames = std::atoi(v.c_str());
    } else if (flag == "--mm-from") {
      a.mm_from_minute = std::atoi(v.c_str());
    } else if (flag == "--limit") {
      a.limit = std::strtoull(v.c_str(), nullptr, 10);
    } else {
      usage();
      return 2;
    }
  }
  if (a.itch.empty() || a.daily_dir.empty()) {
    usage();
    return 2;
  }
  auto bt = std::make_unique<Backtest>(a);
  if (!bt->run()) return 2;
  bt->print_summary();
  std::FILE* f = std::fopen(a.out.c_str(), "w");
  if (f == nullptr) {
    std::perror(a.out.c_str());
    return 2;
  }
  const std::string j = bt->json();
  std::fwrite(j.data(), 1, j.size(), f);
  std::fclose(f);
  std::printf("  wrote %s (%zu bytes)\n", a.out.c_str(), j.size());
  return bt->clean() ? 0 : 1;
}
