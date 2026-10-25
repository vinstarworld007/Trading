#pragma once

// The firm's order management: every order's state, and per-instrument
// positions, cash and exposure, kept from the trading thread's requests and
// the exchange's OUCH responses. Fixed capacity, allocated once; lookups are a
// mask of the UserRefNum, which the OMS hands out in increasing order as OUCH
// requires.
//
// Exposure counts shares that could still trade: orders sent but not yet
// accepted, live orders, and the replacement of a replace in flight (together
// with the order it replaces, until the exchange says which one exists). Risk
// checks use it for worst-case positions.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lab/oms/messages.hpp"

namespace lab::oms {

enum class State : std::uint8_t {
  kFree,         // slot unused
  kPendingNew,   // sent, not yet accepted
  kLive,         // on the exchange
  kPendingSwap,  // the replacement in a replace not yet confirmed
  kDone,         // filled, canceled or rejected
};

struct Order {
  std::uint32_t user_ref = 0;
  std::uint32_t price = 0;
  std::uint32_t open = 0;         // shares that can still trade
  std::uint32_t executed = 0;     // across the whole replace chain
  std::uint32_t replacement = 0;  // replace in flight: the new UserRefNum
  std::uint32_t replaces = 0;     // a replacement: the order it replaces
  std::uint64_t number = 0;       // exchange order reference number
  std::uint16_t instrument = 0;
  char side = 'B';
  State state = State::kFree;
  bool cancel_pending = false;

  bool working() const noexcept {
    return state == State::kPendingNew || state == State::kLive || state == State::kPendingSwap;
  }
};

struct Position {
  std::int64_t shares = 0;  // long positive
  std::int64_t cash = 0;    // Price(4) x shares: sells add, buys subtract
  std::uint64_t bought = 0;
  std::uint64_t sold = 0;
  std::uint64_t fills = 0;
  std::uint64_t open_buy = 0;  // exposure, see above
  std::uint64_t open_sell = 0;
  std::int64_t max_abs_shares = 0;

  // Mark to market at `price`, in Price(4) units.
  std::int64_t pnl(std::uint32_t price) const noexcept {
    return cash + shares * static_cast<std::int64_t>(price);
  }
};

struct OmsStats {
  std::uint64_t entered = 0, replaced = 0, canceled = 0;
  std::uint64_t accepted = 0, rejected = 0, executions = 0;
  std::uint64_t full = 0;          // no free slot for a new order
  std::uint64_t inconsistent = 0;  // a response that does not fit what was sent
};

// What a response did, for the strategy.
struct Applied {
  const Order* order = nullptr;  // the order it concerned (null if unknown)
  bool fill = false;
  bool finished = false;  // the order is done
};

class Oms {
 public:
  // `capacity` is a power of two: how many UserRefNums back an order may
  // still be working.
  explicit Oms(std::size_t instruments, std::size_t capacity = 1u << 16)
      : orders_(capacity), positions_(instruments), mask_{capacity - 1} {}

  // A new order. Returns its UserRefNum, or 0 if its slot is still in use.
  std::uint32_t enter(std::uint16_t instrument, char side, std::uint32_t qty,
                      std::uint32_t price) noexcept {
    const std::uint32_t ref = next_ref_++;
    Order& o = orders_[ref & mask_];
    if (o.working()) {
      ++stats_.full;
      return 0;
    }
    o = Order{};
    o.user_ref = ref;
    o.instrument = instrument;
    o.side = side;
    o.price = price;
    o.open = qty;
    o.state = State::kPendingNew;
    exposure(o) += qty;
    ++working_;
    ++stats_.entered;
    return ref;
  }

  // Replaces a live order with one of `open` shares at `price`. Returns the
  // replacement's UserRefNum, or 0 if the order cannot be replaced now. The
  // OUCH quantity to send is replace_quantity(new ref).
  std::uint32_t replace(std::uint32_t ref, std::uint32_t open, std::uint32_t price) noexcept {
    Order* old = get(ref);
    if (old == nullptr || old->state != State::kLive || old->replacement != 0 ||
        old->cancel_pending) {
      return 0;
    }
    const std::uint32_t nref = next_ref_++;
    Order& n = orders_[nref & mask_];
    if (n.working()) {
      ++stats_.full;
      return 0;
    }
    n = Order{};
    n.user_ref = nref;
    n.instrument = old->instrument;
    n.side = old->side;
    n.price = price;
    n.open = open;
    n.executed = old->executed;
    n.replaces = ref;
    n.state = State::kPendingSwap;
    old->replacement = nref;
    exposure(n) += open;
    ++working_;
    ++stats_.replaced;
    return nref;
  }

  std::uint32_t replace_quantity(std::uint32_t nref) const noexcept {
    const Order* n = find(nref);
    return n == nullptr ? 0 : n->executed + n->open;
  }

  // Marks a full cancel as sent. False if the order is not live or a replace
  // of it is in flight (cancel once the exchange has answered that).
  bool cancel(std::uint32_t ref) noexcept {
    Order* o = get(ref);
    if (o == nullptr || o->state != State::kLive || o->cancel_pending || o->replacement != 0) {
      return false;
    }
    o->cancel_pending = true;
    ++stats_.canceled;
    return true;
  }

  Applied apply(const OrderResponse& r) noexcept {
    switch (r.type) {
      case OrderResponse::kAccepted:
        return on_accepted(r);
      case OrderResponse::kReplaced:
        return on_replaced(r);
      case OrderResponse::kCanceled:
        return on_canceled(r);
      case OrderResponse::kExecuted:
        return on_executed(r);
      case OrderResponse::kRejected:
        return on_rejected(r);
      default:
        return {};
    }
  }

  const Order* find(std::uint32_t ref) const noexcept {
    const Order& o = orders_[ref & mask_];
    return o.user_ref == ref && o.state != State::kFree ? &o : nullptr;
  }
  const Position& position(std::uint16_t instrument) const noexcept {
    return positions_[instrument];
  }
  std::size_t instruments() const noexcept { return positions_.size(); }
  std::size_t working() const noexcept { return working_; }
  const OmsStats& stats() const noexcept { return stats_; }

  // Calls f(order) for every working order.
  template <class F>
  void for_each_working(F&& f) const {
    for (const Order& o : orders_) {
      if (o.working()) f(o);
    }
  }

 private:
  Order* get(std::uint32_t ref) noexcept {
    Order& o = orders_[ref & mask_];
    return o.user_ref == ref && o.state != State::kFree ? &o : nullptr;
  }

  std::uint64_t& exposure(const Order& o) noexcept {
    Position& p = positions_[o.instrument];
    return o.side == 'B' ? p.open_buy : p.open_sell;
  }

  void take_open(Order& o, std::uint32_t q) noexcept {
    o.open -= q;
    exposure(o) -= q;
  }

  void finish(Order& o) noexcept {
    exposure(o) -= o.open;
    o.open = 0;
    o.state = State::kDone;
    --working_;
  }

  Applied inconsistent() noexcept {
    ++stats_.inconsistent;
    return {};
  }

  Applied on_accepted(const OrderResponse& r) noexcept {
    Order* o = get(r.user_ref);
    if (o == nullptr || o->state != State::kPendingNew) return inconsistent();
    ++stats_.accepted;
    o->state = State::kLive;
    o->number = r.number;
    if (r.qty != o->open) {  // the exchange may accept fewer
      if (r.qty > o->open) return inconsistent();
      take_open(*o, o->open - r.qty);
    }
    return {o, false, false};
  }

  Applied on_replaced(const OrderResponse& r) noexcept {
    Order* n = get(r.user_ref);
    Order* old = get(r.orig_ref);
    if (n == nullptr || old == nullptr || n->state != State::kPendingSwap ||
        n->replaces != r.orig_ref || old->replacement != r.user_ref) {
      return inconsistent();
    }
    ++stats_.accepted;
    n->executed = old->executed;
    finish(*old);
    n->state = State::kLive;
    n->number = r.number;
    n->replaces = 0;
    if (r.qty < n->open) {
      take_open(*n, n->open - r.qty);
    } else if (r.qty > n->open) {
      exposure(*n) += r.qty - n->open;
      n->open = r.qty;
    }
    return {n, false, false};
  }

  // Drops a replacement the exchange will never create.
  void abandon_replacement(Order& old) noexcept {
    if (old.replacement == 0) return;
    Order* n = get(old.replacement);
    old.replacement = 0;
    if (n != nullptr && n->state == State::kPendingSwap) finish(*n);
  }

  Applied on_canceled(const OrderResponse& r) noexcept {
    Order* o = get(r.user_ref);
    if (o == nullptr || o->state != State::kLive || r.qty > o->open) return inconsistent();
    o->open -= r.qty;
    if (o->open > 0) return {o, false, false};
    // A user cancel of an order with a replace in flight is how the exchange
    // answers a replace for no more than the executed shares.
    if (r.flag == 'U' && o->replacement != 0 && !o->cancel_pending) abandon_replacement(*o);
    if (o->replacement == 0) {
      finish(*o);
      return {o, false, true};
    }
    return {o, false, false};  // the replace is rejected next; that finishes it
  }

  Applied on_executed(const OrderResponse& r) noexcept {
    Order* o = get(r.user_ref);
    if (o == nullptr || o->state != State::kLive || r.qty > o->open) return inconsistent();
    ++stats_.executions;
    take_open(*o, r.qty);
    o->executed += r.qty;
    Position& p = positions_[o->instrument];
    const auto q = static_cast<std::int64_t>(r.qty);
    const auto notional = q * static_cast<std::int64_t>(r.price);
    if (o->side == 'B') {
      p.shares += q;
      p.cash -= notional;
      p.bought += r.qty;
    } else {
      p.shares -= q;
      p.cash += notional;
      p.sold += r.qty;
    }
    ++p.fills;
    const std::int64_t abs = p.shares < 0 ? -p.shares : p.shares;
    if (abs > p.max_abs_shares) p.max_abs_shares = abs;
    if (o->replacement != 0) {
      Order* n = get(o->replacement);
      if (n != nullptr) n->executed = o->executed;
    }
    const bool done = o->open == 0 && o->replacement == 0;
    if (done) finish(*o);
    return {o, true, done};
  }

  Applied on_rejected(const OrderResponse& r) noexcept {
    Order* o = get(r.user_ref);
    if (o == nullptr) return inconsistent();
    ++stats_.rejected;
    if (o->state == State::kPendingSwap) {  // the original stands, if it is still there
      Order* old = get(o->replaces);
      finish(*o);
      if (old != nullptr && old->replacement == r.user_ref) {
        old->replacement = 0;
        if (old->state == State::kLive && old->open == 0) {
          finish(*old);
          return {old, false, true};
        }
        return {old, false, false};
      }
      return {o, false, true};
    }
    if (o->state != State::kPendingNew) return inconsistent();
    finish(*o);
    return {o, false, true};
  }

  std::vector<Order> orders_;
  std::vector<Position> positions_;
  std::size_t mask_;
  std::uint32_t next_ref_ = 1;
  std::size_t working_ = 0;
  OmsStats stats_;
};

}  // namespace lab::oms
