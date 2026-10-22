#include "matcher.hpp"

#include <chrono>
#include <cstring>
#include <ctime>

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
    if (match::key_owner(o.key) == kHistOwner) return;
    SessionEvent e = base(o);
    e.type = SessionEvent::kAccepted;
    e.qty = o.remaining;
    m.to_session(e);
  }

  void on_rejected(match::OrderKey key, match::Reject r) {
    if (match::key_owner(key) == kHistOwner) return;
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
    for (const auto* o : {&rest, &aggr}) {
      if (match::key_owner(o->key) == kHistOwner) continue;
      SessionEvent e = base(*o);
      e.type = SessionEvent::kExecuted;
      e.qty = q;
      e.price = price;
      e.match = match_no;
      e.liquidity = o == &rest ? lab::ouch::liquidity::kAdded : lab::ouch::liquidity::kRemoved;
      m.to_session(e);
    }
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
    if (match::key_owner(o.key) == kHistOwner) return;
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
      symbols_(cfg.max_instruments) {
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
  system_event('Q');  // start of market hours
  Command c;
  for (;;) {
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
  system_event('M');  // end of market hours
  system_event('E');  // end of system hours
  system_event('C');  // end of messages
}

}  // namespace simex
