#include "trading.hpp"

#include <algorithm>

#include "lab/core/affinity.hpp"
#include "lab/core/compiler.hpp"

namespace trader {

using lab::oms::OrderRequest;
using lab::oms::OrderResponse;
using lab::oms::State;
using lab::risk::Verdict;

Trading::Trading(const TradingConfig& cfg, TopRing& tops, RequestRing& requests,
                 ResponseRing& responses, const lab::TscClock& clock)
    : cfg_{cfg},
      tops_{tops},
      requests_{requests},
      responses_{responses},
      clock_{clock},
      oms_{cfg.symbols.size()},
      risk_{cfg.limits, clock.ticks_per_ns() * 1e9},
      maker_{cfg.maker},
      instruments_(cfg.symbols.size()) {
  for (std::size_t i = 0; i < instruments_.size(); ++i) instruments_[i].symbol = cfg.symbols[i];
  dirty_.reserve(instruments_.size());
  fills_.reserve(cfg.fill_log_capacity);
}

void Trading::mark(std::uint16_t i) {
  if (instruments_[i].dirty) return;
  instruments_[i].dirty = true;
  dirty_.push_back(i);
}

void Trading::send(const OrderRequest& r) {
  while (LAB_UNLIKELY(!requests_.try_push(r))) ++stats_.ring_full_spins;
  ++stats_.requests;
}

void Trading::on_response(const OrderResponse& r) {
  if (r.type == OrderResponse::kSessionEnd) {
    stats_.session_ended = true;
    return;
  }
  if (r.type == OrderResponse::kExecuted) {  // two fills with one match number: a self-trade
    for (const std::uint64_t m : recent_matches_) {
      if (m == r.number) ++stats_.self_trades;
    }
    recent_matches_[next_match_++ % recent_matches_.size()] = r.number;
  }
  const auto applied = oms_.apply(r);
  if (applied.order == nullptr) return;
  const std::uint16_t i = applied.order->instrument;
  Instrument& in = instruments_[i];
  if (applied.fill && fills_.size() < fills_.capacity()) {
    fills_.push_back({r.exchange_ns, r.number, r.user_ref, r.qty, r.price, in.last_market.bid,
                      in.last_market.ask, i, applied.order->side, r.flag});
  }
  if (r.type == OrderResponse::kReplaced) {  // our working order is now the replacement
    if (in.bid_ref == r.orig_ref) in.bid_ref = r.user_ref;
    if (in.ask_ref == r.orig_ref) in.ask_ref = r.user_ref;
  }
  mark(i);
}

void Trading::work_side(Instrument& in, std::uint16_t i, char side, std::uint32_t& ref,
                        std::uint32_t px, std::uint32_t qty, std::uint32_t reference) {
  const lab::oms::Order* o = ref != 0 ? oms_.find(ref) : nullptr;
  if (o != nullptr && !o->working()) o = nullptr;
  if (o == nullptr) {
    ref = 0;
    if (qty == 0) return;
    const std::uint64_t now = lab::rdtsc();
    if (risk_.check_new(oms_, i, side, qty, px, reference, now) != Verdict::kOk) return;
    const std::uint32_t r = oms_.enter(i, side, qty, px);
    if (r == 0) return;
    ref = r;
    OrderRequest req{};
    req.tick_tsc = tick_tsc_;
    req.decide_tsc = now;
    req.user_ref = r;
    req.qty = qty;
    req.price = px;
    req.symbol = in.symbol;
    req.type = OrderRequest::kEnter;
    req.side = side;
    req.tif = '0';
    send(req);
    return;
  }
  // One request in flight per order: wait for the exchange to answer.
  if (o->state != State::kLive || o->replacement != 0 || o->cancel_pending) return;
  if (qty == 0) {
    if (!oms_.cancel(ref)) return;
    OrderRequest req{};
    req.tick_tsc = tick_tsc_;
    req.decide_tsc = lab::rdtsc();
    req.user_ref = ref;
    req.qty = 0;
    req.symbol = in.symbol;
    req.type = OrderRequest::kCancel;
    send(req);
    return;
  }
  if (!lab::strategy::should_move(cfg_.maker, o->price, px)) return;  // close enough: keep priority
  const std::uint64_t now = lab::rdtsc();
  if (risk_.check_replace(oms_, *o, qty, px, reference, now) != Verdict::kOk) return;
  const std::uint32_t orig = ref;
  const std::uint32_t n = oms_.replace(orig, qty, px);
  if (n == 0) return;
  OrderRequest req{};
  req.tick_tsc = tick_tsc_;
  req.decide_tsc = now;
  req.user_ref = n;
  req.orig_ref = orig;
  req.qty = oms_.replace_quantity(n);
  req.price = px;
  req.symbol = in.symbol;
  req.type = OrderRequest::kReplace;
  req.side = side;
  req.tif = '0';
  send(req);
}

void Trading::reconcile(std::uint16_t i) {
  Instrument& in = instruments_[i];
  if (!in.have_top) return;
  auto own = [&](std::uint32_t ref, std::uint32_t& px, std::uint32_t& open) {
    px = open = 0;
    if (const auto* o = ref != 0 ? oms_.find(ref) : nullptr; o != nullptr && o->working()) {
      px = o->price;
      open = o->open;
    }
  };
  // The most aggressive price a side may trade at: a replace in flight may
  // land at its new price before we hear back.
  auto reach = [&](std::uint32_t ref, char side, std::uint32_t px) {
    const auto* o = ref != 0 ? oms_.find(ref) : nullptr;
    if (o == nullptr || o->replacement == 0) return px;
    const auto* n = oms_.find(o->replacement);
    if (n == nullptr || !n->working()) return px;
    return side == 'B' ? std::max(px, n->price) : std::min(px, n->price);
  };
  std::uint32_t bpx, bopen, apx, aopen;
  own(in.bid_ref, bpx, bopen);
  own(in.ask_ref, apx, aopen);
  const auto m = lab::strategy::others(in.top, bpx, bopen, apx, aopen);
  const std::uint32_t bid_reach = bopen != 0 ? reach(in.bid_ref, 'B', bpx) : 0;
  const std::uint32_t ask_reach = aopen != 0 ? reach(in.ask_ref, 'S', apx) : 0;
  if (m.mid() != 0) in.last_mid = m.mid();
  auto q = maker_.quote(m, oms_.position(i).shares);
  // Self-match prevention: never bid at or above our own working ask, or offer
  // at or below our own bid, while the other side is still being moved.
  const std::uint32_t tick = cfg_.maker.tick;
  if (aopen != 0 && q.bid_qty != 0 && q.bid_px >= ask_reach) q.bid_px = ask_reach - tick;
  if (bopen != 0 && q.ask_qty != 0 && q.ask_px <= bid_reach) q.ask_px = bid_reach + tick;
  in.last_market = m;
  in.last_quote = q;
  work_side(in, i, 'B', in.bid_ref, q.bid_px, q.bid_qty, m.mid());
  work_side(in, i, 'S', in.ask_ref, q.ask_px, q.ask_qty, m.mid());
}

void Trading::cancel_all() {
  for (std::uint16_t i = 0; i < instruments_.size(); ++i) {
    Instrument& in = instruments_[i];
    work_side(in, i, 'B', in.bid_ref, 0, 0, 0);
    work_side(in, i, 'S', in.ask_ref, 0, 0, 0);
  }
}

void Trading::run(const std::atomic<bool>& stop) {
  lab::set_thread_name("trader");
  if (cfg_.cpu >= 0) (void)lab::pin_this_thread(cfg_.cpu);
  std::uint64_t deadline = 0;
  OrderResponse r;
  lab::oms::TopOfBook t;
  for (;;) {
    while (responses_.try_pop(r)) on_response(r);
    if (stats_.session_ended) return;  // the exchange cancels what is left
    while (tops_.try_pop(t)) {
      ++stats_.tops;
      Instrument& in = instruments_[t.instrument];
      in.top = t;
      in.have_top = true;
      tick_tsc_ = t.rx_tsc;
      mark(t.instrument);
    }
    if (!stopping_) {
      for (const std::uint16_t i : dirty_) {
        instruments_[i].dirty = false;
        reconcile(i);
      }
      dirty_.clear();
      tick_tsc_ = 0;
      if (stop.load(std::memory_order_relaxed)) {
        stopping_ = true;
        deadline = lab::rdtsc() + static_cast<std::uint64_t>(cfg_.cancel_wait_seconds * 1e9 *
                                                             clock_.ticks_per_ns());
      }
    }
    if (stopping_) {
      for (const std::uint16_t i : dirty_) instruments_[i].dirty = false;
      dirty_.clear();
      cancel_all();  // retried as replaces in flight are answered
      if (oms_.working() == 0) return;
      if (lab::rdtsc() > deadline) {
        stats_.cancel_timeout = true;
        return;
      }
    }
  }
}

bool Trading::report(std::FILE* out) const {
  bool ok = true;
  const auto& s = oms_.stats();
  std::fprintf(
      out,
      "Orders       %llu entered, %llu replaces, %llu cancels | exchange: %llu accepted, "
      "%llu rejected, %llu executions | %llu tops in, %llu requests out\n",
      static_cast<unsigned long long>(s.entered), static_cast<unsigned long long>(s.replaced),
      static_cast<unsigned long long>(s.canceled), static_cast<unsigned long long>(s.accepted),
      static_cast<unsigned long long>(s.rejected), static_cast<unsigned long long>(s.executions),
      static_cast<unsigned long long>(stats_.tops),
      static_cast<unsigned long long>(stats_.requests));
  std::fprintf(out, "Risk blocks ");
  bool any = false;
  for (int v = 1; v < static_cast<int>(Verdict::kCount); ++v) {
    const auto n = risk_.blocked(static_cast<Verdict>(v));
    if (n == 0) continue;
    std::fprintf(out, " %s %llu,", lab::risk::verdict_name(static_cast<Verdict>(v)),
                 static_cast<unsigned long long>(n));
    any = true;
  }
  std::fprintf(out, "%s\n", any ? "" : " none");

  std::fprintf(out, "\n%-8s %9s %9s %9s %7s %9s %12s\n", "Symbol", "Position", "Bought", "Sold",
               "Fills", "Max |pos|", "PnL ($)");
  std::int64_t total = 0;
  for (std::uint16_t i = 0; i < instruments_.size(); ++i) {
    const auto& p = oms_.position(i);
    const auto pnl = p.pnl(instruments_[i].last_mid);
    total += pnl;
    std::fprintf(out, "%-8.8s %9lld %9llu %9llu %7llu %9lld %12.2f\n",
                 instruments_[i].symbol.data(), static_cast<long long>(p.shares),
                 static_cast<unsigned long long>(p.bought), static_cast<unsigned long long>(p.sold),
                 static_cast<unsigned long long>(p.fills), static_cast<long long>(p.max_abs_shares),
                 static_cast<double>(pnl) / 1e4);
    if (p.max_abs_shares > cfg_.limits.max_position) {
      std::fprintf(out, "CHECK FAILED: %.8s position %lld past the %lld-share limit\n",
                   instruments_[i].symbol.data(), static_cast<long long>(p.max_abs_shares),
                   static_cast<long long>(cfg_.limits.max_position));
      ok = false;
    }
  }
  std::fprintf(out, "%-8s %66.2f  (marked at the last mid)\n\n", "Total",
               static_cast<double>(total) / 1e4);
  std::fprintf(out, "%-8s %21s %21s\n", "Symbol", "Last market", "Last quote");
  for (const auto& in : instruments_) {
    std::fprintf(out, "%-8.8s %10.4f %10.4f %10.4f %10.4f\n", in.symbol.data(),
                 in.last_market.bid / 1e4, in.last_market.ask / 1e4,
                 in.last_quote.bid_qty != 0 ? in.last_quote.bid_px / 1e4 : 0.0,
                 in.last_quote.ask_qty != 0 ? in.last_quote.ask_px / 1e4 : 0.0);
  }
  std::fprintf(out, "\n");
  if (s.inconsistent != 0) {
    std::fprintf(out, "CHECK FAILED: %llu responses did not fit what was sent\n",
                 static_cast<unsigned long long>(s.inconsistent));
    ok = false;
  }
  if (stats_.self_trades != 0) {
    std::fprintf(out, "CHECK FAILED: %llu fills traded against our own orders\n",
                 static_cast<unsigned long long>(stats_.self_trades));
    ok = false;
  }
  if (!stats_.session_ended && oms_.working() != 0) {
    std::fprintf(out, "CHECK FAILED: %zu orders still working after stop\n", oms_.working());
    ok = false;
  }
  return ok;
}

}  // namespace trader

namespace trader {

bool Trading::write_fills(const std::string& path) const {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) return false;
  std::fprintf(f, "exchange_ns,symbol,side,qty,price,match,liquidity,user_ref,bid,ask\n");
  for (const auto& r : fills_) {
    std::string sym(instruments_[r.instrument].symbol.data(), 8);
    while (!sym.empty() && sym.back() == ' ') sym.pop_back();
    std::fprintf(f, "%llu,%s,%c,%u,%u,%llu,%c,%u,%u,%u\n",
                 static_cast<unsigned long long>(r.exchange_ns), sym.c_str(), r.side, r.qty,
                 r.price, static_cast<unsigned long long>(r.match), r.liquidity, r.user_ref, r.bid,
                 r.ask);
  }
  return std::fclose(f) == 0;
}

}  // namespace trader
