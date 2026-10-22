#include "matcher.hpp"

#include <chrono>
#include <cstring>
#include <ctime>

#include "lab/book/luld.hpp"
#include "lab/core/compiler.hpp"
#include "lab/itch/codec.hpp"
#include "lab/itch/messages.hpp"
#include "lab/ouch/messages.hpp"

namespace simex {

namespace itch = lab::itch;
namespace match = lab::match;

namespace {

// Wall-clock nanoseconds since local midnight: ITCH timestamps are feed time.
std::uint64_t ns_since_midnight() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&t, &local);
  const auto since_epoch =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  const std::uint64_t sub_second = static_cast<std::uint64_t>(since_epoch % 1'000'000'000);
  return (static_cast<std::uint64_t>(local.tm_hour) * 3600 +
          static_cast<std::uint64_t>(local.tm_min) * 60 +
          static_cast<std::uint64_t>(local.tm_sec)) *
             1'000'000'000ULL +
         sub_second;
}

std::uint16_t reject_code(match::Reject r) {
  namespace rj = lab::ouch::reject;
  switch (r) {
    case match::Reject::kUnknownInstrument:
      return rj::kInvalidSymbol;
    case match::Reject::kInvalidQuantity:
      return rj::kInvalidQuantity;
    case match::Reject::kInvalidPrice:
      return rj::kInvalidPrice;
    case match::Reject::kUnknownOrder:
      return rj::kReplaceNotAllowed;
    case match::Reject::kHalted:
      return rj::kHalted;
    case match::Reject::kBookFull:
      return rj::kProcessingError;
    case match::Reject::kDuplicateKey:
      return rj::kOther;
  }
  return rj::kOther;
}

char cancel_code(match::CancelReason r) {
  namespace cr = lab::ouch::cancel_reason;
  switch (r) {
    case match::CancelReason::kUser:
      return cr::kUser;
    case match::CancelReason::kIoc:
      return cr::kIoc;
    case match::CancelReason::kSystem:
      return cr::kSystem;
  }
  return cr::kSystem;
}

char side_char(match::Side s) { return s == match::Side::kBuy ? 'B' : 'S'; }

// Orders whose owner gets no order responses: historical liquidity and the
// matcher's own implied leg orders.
bool silent(match::OrderKey key) {
  const std::uint16_t o = match::key_owner(key);
  return o == kHistOwner || o == kImpliedOwner;
}

}  // namespace

// Turns engine events into order responses and public ITCH messages.
struct Matcher::Listener {
  Matcher& m;
  char tif = '0';

  SessionEvent base(const match::OrderView& o) const {
    SessionEvent e{};
    e.ts_ns = m.now_ns_;
    e.orn = o.orn;
    e.user_ref = static_cast<std::uint32_t>(match::key_ref(o.key));
    e.owner = match::key_owner(o.key);
    e.price = o.price;
    e.remaining = o.remaining;
    e.symbol = m.symbols_[o.instrument];
    e.side = side_char(o.side);
    e.tif = tif;
    return e;
  }

  void on_accepted(const match::OrderView& o) {
    if (silent(o.key) || m.strip_quiet_accept_) return;
    SessionEvent e = base(o);
    if (m.strip_mode_)
      e.price = m.strip_price_;  // its first pass may be capped at an implied price
    e.type = SessionEvent::kAccepted;
    e.qty = o.remaining;
    m.to_session(e);
  }

  void on_rejected(match::OrderKey key, match::Reject r) {
    if (r == match::Reject::kHalted) ++m.sstats_.halted_rejects;
    if (silent(key)) return;
    if (m.strip_mode_) m.strip_rejected_ = true;
    SessionEvent e{};
    e.type = SessionEvent::kRejected;
    e.ts_ns = m.now_ns_;
    e.owner = match::key_owner(key);
    e.user_ref = static_cast<std::uint32_t>(match::key_ref(key));
    e.reject_reason = reject_code(r);
    m.to_session(e);
  }

  void on_execution(const match::OrderView& rest, const match::OrderView& aggr, std::uint32_t q,
                    std::uint32_t price, std::uint64_t match_no) {
    itch::OrderExecuted x;
    x.order_reference_number = rest.orn;
    x.executed_shares = q;
    x.match_number = match_no;
    m.publish(x, rest.instrument);
    m.on_trade(rest.instrument, price, match_no);
    for (const auto* o : {&rest, &aggr}) {
      if (silent(o->key)) continue;
      SessionEvent e = base(*o);
      e.type = SessionEvent::kExecuted;
      e.qty = q;
      e.price = price;
      e.match = match_no;
      e.liquidity = o == &rest ? lab::ouch::liquidity::kAdded : lab::ouch::liquidity::kRemoved;
      m.to_session(e);
    }
  }

  // One order's fill in an auction: a non-printable C (the Q message prints
  // the whole cross), and an execution report with the cross's liquidity flag.
  void on_cross_execution(const match::OrderView& o, std::uint32_t q, std::uint32_t price,
                          std::uint64_t match_no) {
    itch::OrderExecutedWithPrice c;
    c.order_reference_number = o.orn;
    c.executed_shares = q;
    c.match_number = match_no;
    c.printable = 'N';
    c.execution_price = price;
    m.publish(c, o.instrument);
    if (silent(o.key)) return;
    SessionEvent e = base(o);
    e.type = SessionEvent::kExecuted;
    e.qty = q;
    e.price = price;
    e.match = match_no;
    e.liquidity = m.cross_liquidity_;
    m.to_session(e);
  }

  void on_rested(const match::OrderView& o) {
    itch::AddOrder a;
    a.order_reference_number = o.orn;
    a.buy_sell_indicator = side_char(o.side);
    a.shares = o.remaining;
    a.stock = m.symbols_[o.instrument];
    a.price = o.price;
    m.publish(a, o.instrument);
  }

  void on_canceled(const match::OrderView& o, std::uint32_t q, match::CancelReason why) {
    if (m.strip_mode_ && why == match::CancelReason::kIoc && !silent(o.key)) {
      m.strip_left_ = q;  // a strip pass's remainder: implied liquidity may take it
      return;
    }
    if (o.on_book) {
      if (o.remaining == 0) {
        itch::OrderDelete d;
        d.order_reference_number = o.orn;
        m.publish(d, o.instrument);
      } else {
        itch::OrderCancel x;
        x.order_reference_number = o.orn;
        x.cancelled_shares = q;
        m.publish(x, o.instrument);
      }
    }
    if (silent(o.key)) return;
    SessionEvent e = base(o);
    e.type = SessionEvent::kCanceled;
    e.qty = q;
    e.reason = cancel_code(why);
    m.to_session(e);
  }

  void on_replaced(const match::OrderView& old, const match::OrderView& n) {
    if (old.on_book) {  // the replacement may trade before it rests: delete, then add
      itch::OrderDelete d;
      d.order_reference_number = old.orn;
      m.publish(d, old.instrument);
    }
    if (match::key_owner(n.key) == kHistOwner) return;
    SessionEvent e = base(n);
    e.type = SessionEvent::kReplaced;
    e.orig_user_ref = static_cast<std::uint32_t>(match::key_ref(old.key));
    e.qty = n.remaining;
    m.to_session(e);
  }
};

Matcher::Matcher(const match::EngineConfig& cfg, Rings rings, const lab::TscClock& clock)
    : engine_{std::make_unique<match::Engine>(cfg)},
      rings_{rings},
      clock_{clock},
      base_tsc_{lab::rdtsc()},
      base_ns_{ns_since_midnight()},
      symbols_(cfg.max_instruments),
      inst_(cfg.max_instruments) {
  by_symbol_.reserve(16384);
}

std::uint64_t Matcher::feed_time() const noexcept {
  return base_ns_ + static_cast<std::uint64_t>(clock_.to_ns(lab::rdtsc() - base_tsc_));
}

std::uint64_t Matcher::symbol_key(const Symbol& s) noexcept {
  std::uint64_t k;
  std::memcpy(&k, s.data(), sizeof k);
  return k;
}

template <class Ring, class T>
void Matcher::push(Ring& ring, const T& v) {
  while (LAB_UNLIKELY(!ring.try_push(v))) ++stats_.backpressure_spins;
}

void Matcher::to_session(const SessionEvent& e) { push(*rings_.sessions, e); }

template <class M>
void Matcher::publish(M& m, std::uint16_t locate) {
  m.header.stock_locate = locate;
  m.header.tracking_number = 0;
  m.header.timestamp = now_ns_;
  MdEvent* slot;
  while (LAB_UNLIKELY((slot = rings_.md->try_claim()) == nullptr)) ++stats_.backpressure_spins;
  itch::encode(m, slot->data);
  slot->len = static_cast<std::uint16_t>(M::kSize);
  rings_.md->commit();
}

void Matcher::system_event(char code) {
  now_ns_ = feed_time();
  itch::SystemEvent s;
  s.event_code = code;
  publish(s, 0);
}

void Matcher::directory(std::uint16_t locate, const Symbol& symbol) {
  if (locate == 0 || engine_->has_instrument(locate)) return;
  engine_->add_instrument(locate);
  symbols_[locate] = symbol;
  if (pre_open_) hold(locate, Hold::kPreOpen, 0);
  by_symbol_[symbol_key(symbol)] = locate;
  itch::StockDirectory r;
  r.stock = symbol;
  r.market_category = 'Q';
  r.financial_status_indicator = 'N';
  r.round_lot_size = 100;
  r.round_lots_only = 'N';
  r.issue_classification = 'C';
  r.issue_sub_type = {'Z', ' '};
  r.authenticity = 'P';
  r.short_sale_threshold_indicator = 'N';
  r.ipo_flag = 'N';
  r.luld_reference_price_tier = '1';
  r.etp_flag = 'N';
  r.inverse_indicator = 'N';
  publish(r, locate);
}

void Matcher::list_symbols(const std::vector<std::string>& symbols) {
  now_ns_ = feed_time();
  std::uint16_t locate = 1;
  for (const auto& s : symbols) directory(locate++, itch::make_symbol(s));
}

std::vector<std::uint16_t> Matcher::locates() const {
  std::vector<std::uint16_t> out;
  for (std::size_t l = 1; l < symbols_.size(); ++l) {
    if (engine_->has_instrument(static_cast<std::uint16_t>(l))) {
      out.push_back(static_cast<std::uint16_t>(l));
    }
  }
  return out;
}

void Matcher::process(Command& c) {
  c.seq = ++seq_;
  c.ts_ns = now_ns_ = feed_time();
  Listener l{*this, c.tif};
  const match::OrderKey key = match::make_key(c.owner, c.ref);
  const match::Tif tif = c.tif == lab::ouch::tif::kIoc ? match::Tif::kIoc : match::Tif::kDay;
  const match::Side side = c.side == 'B' ? match::Side::kBuy : match::Side::kSell;
  switch (c.type) {
    case Command::kEnter: {
      std::uint16_t locate = c.locate;
      if (locate == 0) {
        const auto it = by_symbol_.find(symbol_key(c.symbol));
        locate = it == by_symbol_.end() ? 0 : it->second;  // 0: rejected as unknown
      }
      const Strip* strip = nullptr;
      for (const Strip& s : strips_) {
        if (locate != 0 && s.locate == locate) strip = &s;
      }
      if (strip != nullptr) {
        c.locate = locate;
        enter_strip(c, *strip);
        break;
      }
      engine_->add(match::NewOrder{key, locate, side, tif, c.qty, c.price}, l);
      break;
    }
    case Command::kCancel:
      if (!engine_->cancel(key, c.qty, l)) {
        ++(c.owner == kHistOwner ? stats_.hist_orphans : stats_.unknown_cancels);
      }
      break;
    case Command::kReduce:
      if (!engine_->reduce_by(key, c.qty, l)) ++stats_.hist_orphans;
      break;
    case Command::kReplace:
    case Command::kReplaceOpen: {
      const match::OrderKey old_key = match::make_key(c.owner, c.orig_ref);
      if (c.owner == kHistOwner && engine_->find(old_key) == nullptr) {
        ++stats_.hist_orphans;  // traded away by a firm order; nothing to replace
        break;
      }
      engine_->replace(old_key, match::NewOrder{key, 0, side, tif, c.qty, c.price}, l,
                       c.type == Command::kReplaceOpen);
      break;
    }
    case Command::kDirectory:
      directory(c.locate, c.symbol);
      break;
    case Command::kCancelOwner:
      engine_->cancel_owner(c.owner, match::CancelReason::kSystem, l);
      break;
  }
  ++stats_.commands;
  if (rings_.journal != nullptr) push(*rings_.journal, c);
  stats_.command_ticks.record(lab::rdtscp() - c.rx_tsc);
}

void Matcher::run(const std::atomic<bool>& closing) {
  system_event('O');  // start of messages
  system_event('S');  // start of system hours
  scenarios_ = sc_.any();
  if (scenarios_) start_scenarios();
  if (!pre_open_) system_event('Q');  // start of market hours
  Command c;
  for (;;) {
    if (scenarios_ && lab::rdtsc() >= next_tick_tsc_) tick();
    bool any = false;
    // Firm orders first: a waiting order should not queue behind a burst of
    // historical liquidity.
    while (rings_.gateway->try_pop(c)) {
      process(c);
      any = true;
    }
    if (rings_.liquidity->try_pop(c)) {
      process(c);
      any = true;
    }
    if (!any && closing.load(std::memory_order_acquire) && rings_.gateway->empty_approx() &&
        rings_.liquidity->empty_approx()) {
      break;
    }
  }
  if (scenarios_) close_market();
  system_event('M');  // end of market hours
  system_event('E');  // end of system hours
  system_event('C');  // end of messages
}

// ---------------------------------------------------------------- scenarios
//
// The timeline runs on the matching thread between commands, at most once a
// millisecond (tick), so a scenario step is sequenced with the orders around
// it like any other event. Every step publishes the ITCH messages Nasdaq
// would: H trading actions, I imbalances, J collars, V/W circuit breakers,
// K IPO release, h operational halts, Y Reg SHO, B broken trades, and Q cross
// trades with non-printable C executions.

namespace {

void put_reason(std::array<char, 4>& out, const char* reason) {
  out = {' ', ' ', ' ', ' '};
  for (std::size_t i = 0; i < 4 && reason[i] != '\0'; ++i) out[i] = reason[i];
}

std::uint64_t secs(double s) { return static_cast<std::uint64_t>(s * 1e9); }

// NOII price variation indicator: |near - reference| / reference.
char variation(std::uint32_t near, std::uint32_t ref) {
  if (near == 0 || ref == 0) return ' ';
  const std::uint64_t diff = near > ref ? near - ref : ref - near;
  const std::uint64_t pct100 = diff * 10'000 / ref;  // hundredths of a percent
  if (pct100 < 100) return 'L';
  if (pct100 < 1'000) return static_cast<char>('0' + pct100 / 100);
  if (pct100 < 2'000) return 'A';
  if (pct100 < 3'000) return 'B';
  return 'C';
}

}  // namespace

std::uint16_t Matcher::locate_of(const std::string& symbol) const {
  const auto it = by_symbol_.find(symbol_key(itch::make_symbol(symbol)));
  return it == by_symbol_.end() ? 0 : it->second;
}

void Matcher::start_scenarios() {
  now_ns_ = feed_time();
  start_ns_ = now_ns_;
  tick_tsc_ = static_cast<std::uint64_t>(clock_.ticks_per_ns() * 1e6);  // 1 ms
  next_tick_tsc_ = lab::rdtsc();
  next_noii_ns_ = now_ns_;
  auto at = [&](double s) { return start_ns_ + secs(s); };
  auto add = [&](double s, ActionKind k, std::size_t i) { timeline_.push_back({at(s), k, i}); };

  if (sc_.mwcb) {
    // The lab's index stands at 5,000.00; levels 7%, 13% and 20% below.
    itch::MwcbDeclineLevel v;
    v.level1 = 465'000'000'000;
    v.level2 = 435'000'000'000;
    v.level3 = 400'000'000'000;
    publish(v, 0);
    add(sc_.mwcb->at_s, ActionKind::kMwcb, 0);
  }
  for (std::size_t i = 0; i < sc_.ipos.size(); ++i) {
    const std::uint16_t l = locate_of(sc_.ipos[i].symbol);
    if (l == 0) continue;
    itch::IpoQuotingPeriod k;
    k.stock = symbols_[l];
    k.ipo_quotation_release_time = static_cast<std::uint32_t>(at(sc_.ipos[i].at_s) / 1'000'000'000);
    k.ipo_quotation_release_qualifier = 'A';
    k.ipo_price = 0;
    publish(k, l);
    put_reason(inst_[l].reason, "IPO1");
    hold(l, Hold::kHalted, 0);
    trading_action(l, 'H', "IPO1");
    add(sc_.ipos[i].at_s, ActionKind::kIpoRelease, i);
  }
  if (sc_.open_auction_s > 0) {
    pre_open_ = true;
    for (const std::uint16_t l : locates()) {
      if (inst_[l].hold == Hold::kNone) hold(l, Hold::kPreOpen, 0);
    }
    add(sc_.open_auction_s, ActionKind::kOpenCross, 0);
  }
  for (std::size_t i = 0; i < sc_.halts.size(); ++i) add(sc_.halts[i].at_s, ActionKind::kHalt, i);
  for (std::size_t i = 0; i < sc_.op_halts.size(); ++i)
    add(sc_.op_halts[i].at_s, ActionKind::kOpHalt, i);
  for (std::size_t i = 0; i < sc_.breaks.size(); ++i) add(sc_.breaks[i], ActionKind::kBreak, i);
  if (sc_.close_auction_s > 0 && sc_.close_at_s > 0) {
    add(sc_.close_at_s, ActionKind::kCloseStart, 0);
    add(sc_.close_at_s + sc_.close_auction_s, ActionKind::kCloseCross, 0);
  }
  std::stable_sort(timeline_.begin(), timeline_.end(),
                   [](const Action& a, const Action& b) { return a.at_ns < b.at_ns; });
}

void Matcher::trading_action(std::uint16_t locate, char state, const char* reason) {
  itch::StockTradingAction h;
  h.stock = symbols_[locate];
  h.trading_state = state;
  h.reserved = ' ';
  std::array<char, 4> r;
  put_reason(r, reason);
  h.reason = r;
  publish(h, locate);
}

void Matcher::hold(std::uint16_t locate, Hold h, std::uint64_t until_ns) {
  InstState& st = inst_[locate];
  if (st.hold == Hold::kNone) held_.push_back(locate);
  st.hold = h;
  st.until_ns = until_ns;
  st.limit_since_ns = 0;
  const bool refuse = h == Hold::kHalted || h == Hold::kOpHalt || h == Hold::kClosed;
  engine_->set_phase(locate, refuse ? match::Phase::kHalted : match::Phase::kAuction);
  engine_->set_band(locate, 0, 0);  // auctions find their own price
}

void Matcher::release(std::uint16_t locate) {
  InstState& st = inst_[locate];
  st.hold = Hold::kNone;
  st.until_ns = 0;
  st.cross_at_ns = 0;
  for (auto& x : held_) {
    if (x == locate) {
      x = held_.back();
      held_.pop_back();
      break;
    }
  }
  engine_->set_phase(locate, match::Phase::kContinuous);
  set_reference(locate, st.reference);  // restores the band
}

std::uint32_t Matcher::reference_price(std::uint16_t locate) const {
  if (inst_[locate].reference != 0) return inst_[locate].reference;
  const auto b = engine_->best(locate, match::Side::kBuy);
  const auto a = engine_->best(locate, match::Side::kSell);
  if (b && a) return static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 200 * 100);
  if (b) return b->price;
  if (a) return a->price;
  return 0;
}

void Matcher::set_reference(std::uint16_t locate, std::uint32_t price) {
  InstState& st = inst_[locate];
  if (price == 0) return;
  st.reference = price;
  if (st.open_reference == 0) st.open_reference = price;
  if (!sc_.luld) return;
  if (!st.luld_watch) {
    st.luld_watch = true;
    luld_.push_back(locate);
  }
  if (st.hold == Hold::kNone) {
    const auto band = lab::book::luld_band(price, sc_.luld_band_pct);
    engine_->set_band(locate, band.lower, band.upper);
  }
}

void Matcher::on_trade(std::uint16_t locate, std::uint32_t price, std::uint64_t match_no) {
  last_match_ = match_no;
  last_match_locate_ = locate;
  if (!scenarios_) return;
  InstState& st = inst_[locate];
  if (st.reference == 0) set_reference(locate, price);  // first trade of the day
  if (sc_.reg_sho && !st.reg_sho && st.open_reference != 0 &&
      std::uint64_t{price} * 10 <= std::uint64_t{st.open_reference} * 9) {
    st.reg_sho = true;
    itch::RegShoRestriction y;
    y.stock = symbols_[locate];
    y.reg_sho_action = '1';
    publish(y, locate);
    ++sstats_.reg_sho;
  }
}

void Matcher::noii(std::uint16_t locate) {
  const InstState& st = inst_[locate];
  const std::uint32_t ref = reference_price(locate);
  const match::Indicative ind = engine_->indicative(locate, ref);
  itch::Noii n;
  n.paired_shares = ind.paired;
  n.imbalance_shares = ind.imbalance;
  n.imbalance_direction = st.hold == Hold::kPaused && ind.paired == 0 ? 'P' : ind.direction;
  n.stock = symbols_[locate];
  n.far_price = ind.price;  // no auction-only orders in the lab: far = near
  n.near_price = ind.price;
  n.current_reference_price = ref;
  n.cross_type = st.cross_type;
  n.price_variation_indicator = variation(ind.price, ref);
  publish(n, locate);
  ++sstats_.noii;
}

void Matcher::cross(std::uint16_t locate, char type) {
  InstState& st = inst_[locate];
  const std::uint32_t ref = reference_price(locate);
  const match::Indicative ind = engine_->indicative(locate, ref);
  std::uint32_t price = ind.price != 0 ? ind.price : ref;
  if (st.collar_upper != 0) {  // LULD reopening: never outside the collar
    if (price > st.collar_upper) price = st.collar_upper;
    if (price < st.collar_lower) price = st.collar_lower;
  }
  cross_liquidity_ = type == 'O'   ? lab::ouch::liquidity::kOpeningCross
                     : type == 'C' ? lab::ouch::liquidity::kClosingCross
                                   : lab::ouch::liquidity::kHaltCross;
  Listener l{*this, '0'};
  const std::uint64_t shares = ind.paired != 0 ? engine_->uncross(locate, price, l) : 0;
  itch::CrossTrade q;
  q.shares = shares;
  q.stock = symbols_[locate];
  q.cross_price = price;
  q.match_number = engine_->next_match();
  q.cross_type = type;
  publish(q, locate);
  ++sstats_.crosses;
  sstats_.cross_shares += shares;
  st.collar_ref = st.collar_lower = st.collar_upper = st.extensions = 0;
  // The new LULD reference (set_reference() runs on release): the cross
  // price, or after a reopening that printed nothing the book's midpoint,
  // so a security that gapped while paused does not pause again at once.
  if (shares > 0) {
    st.reference = price;
  } else if (type == 'H') {
    const auto b = engine_->best(locate, match::Side::kBuy);
    const auto a = engine_->best(locate, match::Side::kSell);
    if (b && a && b->price < a->price) {
      st.reference = static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 200 * 100);
    }
  }
}

void Matcher::check_limit(std::uint16_t locate) {
  InstState& st = inst_[locate];
  if (st.hold != Hold::kNone || st.reference == 0) return;
  const auto band = lab::book::luld_band(st.reference, sc_.luld_band_pct);
  const auto b = engine_->best(locate, match::Side::kBuy);
  const auto a = engine_->best(locate, match::Side::kSell);
  std::uint32_t limit = 0;
  if (b && b->price >= band.upper)
    limit = band.upper;  // limit up
  else if (a && a->price <= band.lower)
    limit = band.lower;  // limit down
  if (limit == 0) {
    st.limit_since_ns = 0;
    return;
  }
  if (st.limit_since_ns == 0) {
    st.limit_since_ns = now_ns_;
    ++sstats_.limit_states;
    return;
  }
  if (now_ns_ - st.limit_since_ns >= secs(sc_.luld_limit_s)) pause(locate, limit);
}

void Matcher::pause(std::uint16_t locate, std::uint32_t limit_price) {
  InstState& st = inst_[locate];
  hold(locate, Hold::kPaused, now_ns_ + secs(sc_.luld_pause_s));
  st.cross_type = 'H';
  put_reason(st.reason, "LUDP");
  trading_action(locate, 'P', "LUDP");
  const std::uint32_t w = lab::book::luld_width(limit_price, sc_.luld_band_pct) / 100 * 100;
  st.collar_ref = limit_price;
  st.collar_upper = limit_price + w;
  st.collar_lower = limit_price > w ? limit_price - w : 100;
  st.extensions = 0;
  itch::LuldAuctionCollar j;
  j.stock = symbols_[locate];
  j.reference_price = st.collar_ref;
  j.upper_collar_price = st.collar_upper;
  j.lower_collar_price = st.collar_lower;
  j.collar_extension = 0;
  publish(j, locate);
  ++sstats_.pauses;
  noii(locate);
}

void Matcher::reopen(std::uint16_t locate) {
  InstState& st = inst_[locate];
  const match::Indicative ind = engine_->indicative(locate, reference_price(locate));
  const bool outside =
      ind.paired != 0 && (ind.price > st.collar_upper || ind.price < st.collar_lower);
  if (outside && st.extensions < 3) {
    // Extend the pause and widen the collar by one band on each side.
    const std::uint32_t w = lab::book::luld_width(st.collar_ref, sc_.luld_band_pct) / 100 * 100;
    ++st.extensions;
    st.collar_upper += w;
    st.collar_lower = st.collar_lower > w + 100 ? st.collar_lower - w : 100;
    itch::LuldAuctionCollar j;
    j.stock = symbols_[locate];
    j.reference_price = st.collar_ref;
    j.upper_collar_price = st.collar_upper;
    j.lower_collar_price = st.collar_lower;
    j.collar_extension = st.extensions;
    publish(j, locate);
    st.until_ns = now_ns_ + secs(sc_.luld_pause_s);
    ++sstats_.extensions;
    return;
  }
  cross(locate, 'H');
  release(locate);
  trading_action(locate, 'T', "");
}

void Matcher::act(const Action& a) {
  switch (a.kind) {
    case ActionKind::kOpenCross: {
      pre_open_ = false;
      system_event('Q');  // start of market hours, then the opening cross
      for (const std::uint16_t l : locates()) {
        if (inst_[l].hold != Hold::kPreOpen) continue;
        cross(l, 'O');
        release(l);
      }
      break;
    }
    case ActionKind::kCloseStart:
      closing_started_ = true;
      for (const std::uint16_t l : locates()) {
        if (inst_[l].hold != Hold::kNone) continue;
        hold(l, Hold::kClosing, 0);
        inst_[l].cross_type = 'C';
      }
      break;
    case ActionKind::kCloseCross:
      for (const std::uint16_t l : locates()) {
        if (inst_[l].hold != Hold::kClosing) continue;
        cross(l, 'C');
        hold(l, Hold::kClosed, 0);
      }
      close_done_ = true;
      break;
    case ActionKind::kHalt: {
      const TimedSymbol& h = sc_.halts[a.index];
      const std::uint16_t l = locate_of(h.symbol);
      if (l == 0 || inst_[l].hold != Hold::kNone) break;
      const char* reason = h.reason.empty() ? "T1" : h.reason.c_str();
      put_reason(inst_[l].reason, reason);
      const std::uint64_t end = now_ns_ + secs(h.for_s);
      const std::uint64_t quote = secs(sc_.quote_period_s) < secs(h.for_s)
                                      ? end - secs(sc_.quote_period_s)
                                      : now_ns_ + secs(h.for_s) / 2;
      hold(l, Hold::kHalted, quote);
      inst_[l].cross_at_ns = end;
      inst_[l].cross_type = 'H';
      trading_action(l, 'H', reason);
      ++sstats_.halts;
      break;
    }
    case ActionKind::kIpoRelease: {
      const TimedSymbol& ipo = sc_.ipos[a.index];
      const std::uint16_t l = locate_of(ipo.symbol);
      if (l == 0 || inst_[l].hold != Hold::kHalted) break;
      hold(l, Hold::kQuoting, now_ns_ + secs(ipo.for_s));
      inst_[l].cross_type = 'H';
      put_reason(inst_[l].reason, "IPOQ");
      trading_action(l, 'Q', "IPOQ");
      ++sstats_.halts;
      break;
    }
    case ActionKind::kOpHalt: {
      const TimedSymbol& h = sc_.op_halts[a.index];
      const std::uint16_t l = locate_of(h.symbol);
      if (l == 0 || inst_[l].hold != Hold::kNone) break;
      hold(l, Hold::kOpHalt, now_ns_ + secs(h.for_s));
      itch::OperationalHalt oh;
      oh.stock = symbols_[l];
      oh.market_code = 'Q';
      oh.operational_halt_action = 'H';
      publish(oh, l);
      ++sstats_.op_halts;
      break;
    }
    case ActionKind::kMwcb: {
      const MwcbEvent& e = *sc_.mwcb;
      itch::MwcbStatus w;
      w.breached_level = static_cast<char>('0' + e.level);
      publish(w, 0);
      const char reason[5] = {'M', 'W', 'C', static_cast<char>('0' + e.level), '\0'};
      const std::uint64_t end = now_ns_ + secs(e.for_s);
      const std::uint64_t quote = secs(sc_.quote_period_s) < secs(e.for_s)
                                      ? end - secs(sc_.quote_period_s)
                                      : now_ns_ + secs(e.for_s) / 2;
      if (e.level == 3) mwcb3_ = true;
      for (const std::uint16_t l : locates()) {
        if (inst_[l].hold != Hold::kNone) continue;
        put_reason(inst_[l].reason, reason);
        hold(l, Hold::kHalted, e.level == 3 ? 0 : quote);  // level 3: rest of the day
        inst_[l].cross_at_ns = end;
        inst_[l].cross_type = 'H';
        trading_action(l, 'H', reason);
        ++sstats_.halts;
      }
      break;
    }
    case ActionKind::kBreak:
      if (last_match_ != 0) {
        itch::BrokenTrade b;
        b.match_number = last_match_;
        publish(b, last_match_locate_);
        ++sstats_.broken_trades;
        last_match_ = 0;
      }
      break;
  }
}

void Matcher::tick() {
  next_tick_tsc_ = lab::rdtsc() + tick_tsc_;
  now_ns_ = feed_time();
  while (next_action_ < timeline_.size() && timeline_[next_action_].at_ns <= now_ns_) {
    act(timeline_[next_action_++]);
  }
  // Timed holds, back to front: release() moves the last entry into the
  // released one's place, which has then been visited already.
  for (std::size_t i = held_.size(); i-- > 0;) {
    const std::uint16_t l = held_[i];
    InstState& st = inst_[l];
    if (st.until_ns == 0 || now_ns_ < st.until_ns) continue;
    switch (st.hold) {
      case Hold::kHalted:  // quotation period before the halt cross
        hold(l, Hold::kQuoting, st.cross_at_ns);
        trading_action(l, 'Q', st.reason[0] == 'M' ? "MWCQ" : "T3");
        break;
      case Hold::kQuoting:
        cross(l, 'H');
        release(l);
        trading_action(l, 'T', "");
        break;
      case Hold::kPaused:
        reopen(l);
        break;
      case Hold::kOpHalt: {
        itch::OperationalHalt oh;
        oh.stock = symbols_[l];
        oh.market_code = 'Q';
        oh.operational_halt_action = 'T';
        publish(oh, l);
        release(l);
        break;
      }
      default:
        st.until_ns = 0;
        break;
    }
  }
  if (sc_.luld) {
    for (const std::uint16_t l : luld_) check_limit(l);
  }
  if (now_ns_ >= next_noii_ns_) {
    next_noii_ns_ = now_ns_ + secs(sc_.noii_interval_s);
    for (const std::uint16_t l : held_) {
      const Hold h = inst_[l].hold;
      if (h == Hold::kPreOpen) inst_[l].cross_type = 'O';
      if (h == Hold::kPreOpen || h == Hold::kQuoting || h == Hold::kPaused || h == Hold::kClosing)
        noii(l);
    }
  }
}

void Matcher::close_market() {
  now_ns_ = feed_time();
  if (pre_open_) act({now_ns_, ActionKind::kOpenCross, 0});  // closed before it opened
  if (sc_.close_auction_s > 0 && !close_done_ && !mwcb3_) {
    if (!closing_started_) act({now_ns_, ActionKind::kCloseStart, 0});
    act({now_ns_, ActionKind::kCloseCross, 0});
  }
}

// ---------------------------------------------------------------- strips

void Matcher::set_strips(const std::vector<lab::book::StripDef>& strips) {
  for (const auto& d : strips) {
    const Strip s{locate_of(d.name), locate_of(d.leg_a), locate_of(d.leg_b)};
    if (s.locate != 0 && s.a != 0 && s.b != 0) strips_.push_back(s);
  }
}

void Matcher::send_cancel(const Command& c, std::uint32_t qty) {
  SessionEvent e{};
  e.type = SessionEvent::kCanceled;
  e.ts_ns = now_ns_;
  e.owner = c.owner;
  e.user_ref = static_cast<std::uint32_t>(c.ref);
  e.qty = qty;
  e.price = c.price;
  e.symbol = symbols_[c.locate];
  e.side = c.side;
  e.tif = c.tif;
  e.reason = lab::ouch::cancel_reason::kIoc;
  to_session(e);
}

// A strip order: it takes the strip's own book at prices up to the best
// implied price first (resting orders have priority at equal prices), then
// implied liquidity, then the book again, until it is filled or nothing
// crosses its limit. Each implied fill lifts both legs at their best prices
// with IOC orders of the matcher's own; the legs' ITCH executions are the
// public record, and a P message prints the strip trade. What is left rests
// on the strip's book (day) or is canceled (IOC).
void Matcher::enter_strip(const Command& c, const Strip& s) {
  const match::OrderKey key = match::make_key(c.owner, c.ref);
  const match::Side side = c.side == 'B' ? match::Side::kBuy : match::Side::kSell;
  const bool ioc = c.tif == lab::ouch::tif::kIoc;
  Listener l{*this, c.tif};
  // Implied-out top: both legs' offers for a buy, bids for a sell.
  auto implied = [&]() -> std::optional<lab::book::ImpliedLevel> {
    if (engine_->phase(s.a) != match::Phase::kContinuous ||
        engine_->phase(s.b) != match::Phase::kContinuous ||
        engine_->phase(s.locate) != match::Phase::kContinuous)
      return std::nullopt;
    const match::Side leg_side = side == match::Side::kBuy ? match::Side::kSell : match::Side::kBuy;
    const auto x = engine_->best(s.a, leg_side);
    const auto y = engine_->best(s.b, leg_side);
    // A leg order beyond its band would be repriced short of the level.
    if (!x || !y || !engine_->in_band(s.a, x->price) || !engine_->in_band(s.b, y->price))
      return std::nullopt;
    return lab::book::ImpliedLevel{x->price + y->price,
                                   x->shares < y->shares ? x->shares : y->shares};
  };
  auto crosses = [&](std::uint32_t p) {
    return side == match::Side::kBuy ? p <= c.price : p >= c.price;
  };

  strip_mode_ = true;
  strip_quiet_accept_ = false;
  strip_rejected_ = false;
  strip_price_ = c.price;
  std::uint32_t remaining = c.qty;
  for (;;) {
    const auto imp = implied();
    const bool use_implied = imp && crosses(imp->price);
    strip_left_ = 0;
    engine_->add(match::NewOrder{key, s.locate, side, match::Tif::kIoc, remaining,
                                 use_implied ? imp->price : c.price},
                 l);
    strip_quiet_accept_ = true;  // later passes are the same order to its owner
    if (strip_rejected_) break;
    remaining = strip_left_;
    if (remaining == 0 || !use_implied) break;
    const std::uint32_t q =
        static_cast<std::uint32_t>(remaining < imp->shares ? remaining : imp->shares);
    const match::Side leg_side = side;  // buying the strip buys both legs
    const auto x =
        engine_->best(s.a, leg_side == match::Side::kBuy ? match::Side::kSell : match::Side::kBuy);
    const auto y =
        engine_->best(s.b, leg_side == match::Side::kBuy ? match::Side::kSell : match::Side::kBuy);
    Listener legs{*this, lab::ouch::tif::kIoc};
    engine_->add(match::NewOrder{match::make_key(kImpliedOwner, ++implied_ref_), s.a, leg_side,
                                 match::Tif::kIoc, q, x->price},
                 legs);
    engine_->add(match::NewOrder{match::make_key(kImpliedOwner, ++implied_ref_), s.b, leg_side,
                                 match::Tif::kIoc, q, y->price},
                 legs);
    const std::uint64_t match_no = engine_->next_match();
    itch::Trade p;
    p.order_reference_number = 0;
    p.buy_sell_indicator = c.side;
    p.shares = q;
    p.stock = symbols_[s.locate];
    p.price = imp->price;
    p.match_number = match_no;
    publish(p, s.locate);
    remaining -= q;
    SessionEvent e{};
    e.type = SessionEvent::kExecuted;
    e.ts_ns = now_ns_;
    e.owner = c.owner;
    e.user_ref = static_cast<std::uint32_t>(c.ref);
    e.qty = q;
    e.price = imp->price;
    e.match = match_no;
    e.remaining = remaining;
    e.symbol = symbols_[s.locate];
    e.side = c.side;
    e.tif = c.tif;
    e.liquidity = lab::ouch::liquidity::kRemoved;
    to_session(e);
    ++sstats_.implied_fills;
    sstats_.implied_shares += q;
    if (remaining == 0) break;
  }
  strip_mode_ = false;
  if (remaining > 0 && !strip_rejected_) {
    if (ioc) {
      send_cancel(c, remaining);
    } else {
      engine_->add(match::NewOrder{key, s.locate, side, match::Tif::kDay, remaining, c.price}, l);
    }
  }
  strip_quiet_accept_ = false;
}

}  // namespace simex
