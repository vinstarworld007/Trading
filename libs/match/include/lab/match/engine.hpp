#pragma once

// Price-time priority limit order book matching engine for many instruments.
//
// Single-threaded by design: one matching thread owns an Engine and feeds it
// commands in sequence order, which makes every run deterministic and
// replayable from the journal. All memory (order pool, key index, price
// levels) is reserved up front; nothing allocates while matching unless an
// instrument grows past its reserved price levels.
//
// Orders are named by an OrderKey the submitter picks (see make_key), unique
// among its live orders; the engine assigns each accepted order a day-unique
// order reference number (ORN) for the public feed. Results go to a listener,
// a template parameter so the calls inline:
//
//   struct Listener {
//     void on_accepted(const OrderView&);                          // admitted, before matching
//     void on_rejected(OrderKey, Reject);
//     void on_execution(const OrderView& resting, const OrderView& aggressor,
//                       std::uint32_t qty, std::uint32_t price, std::uint64_t match);
//     void on_rested(const OrderView&);                            // remainder joined the book
//     void on_canceled(const OrderView&, std::uint32_t qty, CancelReason);
//     void on_replaced(const OrderView& old, const OrderView& replacement);
//   };
//
// Views passed to a listener show the order after the event (remaining
// already reduced), except on_book, which says whether the order was resting
// when the event happened. An order is gone after its last event.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "lab/core/compiler.hpp"
#include "lab/core/huge_pages.hpp"
#include "lab/match/key_index.hpp"

namespace lab::match {

using OrderKey = std::uint64_t;

// Owner in the top 16 bits, the owner's own reference in the low 48.
constexpr OrderKey make_key(std::uint16_t owner, std::uint64_t ref) noexcept {
  return (std::uint64_t{owner} << 48) | (ref & 0xFFFF'FFFF'FFFFULL);
}
constexpr std::uint16_t key_owner(OrderKey k) noexcept {
  return static_cast<std::uint16_t>(k >> 48);
}
constexpr std::uint64_t key_ref(OrderKey k) noexcept { return k & 0xFFFF'FFFF'FFFFULL; }

enum class Side : std::uint8_t { kBuy, kSell };
enum class Tif : std::uint8_t { kDay, kIoc };
enum class CancelReason : std::uint8_t { kUser, kIoc, kSystem };
enum class Reject : std::uint8_t {
  kDuplicateKey,
  kUnknownOrder,
  kUnknownInstrument,
  kInvalidQuantity,
  kInvalidPrice,
  kBookFull,
};

inline constexpr std::uint32_t kMaxQuantity = 999'999;       // OUCH: less than 1,000,000
inline constexpr std::uint32_t kMaxPrice = 1'999'999'900;    // 199,999.99 in Price(4)
inline constexpr std::uint32_t kMarketBuyPrice = kMaxPrice;  // a buy that takes any offer
inline constexpr std::uint32_t kMarketSellPrice = 1;         // a sell that hits any bid

struct NewOrder {
  OrderKey key = 0;
  std::uint16_t instrument = 0;
  Side side = Side::kBuy;
  Tif tif = Tif::kDay;
  std::uint32_t quantity = 0;
  std::uint32_t price = 0;  // Price(4)
};

struct OrderView {
  OrderKey key;
  std::uint64_t orn;
  std::uint16_t instrument;
  Side side;
  bool on_book;  // resting (visible on the public feed)
  std::uint32_t price;
  std::uint32_t remaining;
  std::uint32_t executed;  // over the order's whole replace chain
};

struct EngineConfig {
  std::size_t max_orders = 1u << 20;  // live orders across all instruments
  std::size_t max_instruments = 1u << 16;
  std::size_t levels_per_side = 512;          // price levels reserved per side of each instrument
  std::uint32_t max_quantity = kMaxQuantity;  // larger orders are rejected
};

struct EngineStats {
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t executions = 0;
  std::uint64_t executed_shares = 0;
  std::uint64_t canceled = 0;
  std::uint64_t replaced = 0;
};

class Engine {
 public:
  explicit Engine(const EngineConfig& cfg = {})
      : cfg_{cfg}, nodes_(cfg.max_orders), index_{cfg.max_orders}, books_(cfg.max_instruments) {
    free_.reserve(cfg.max_orders);
    for (std::size_t i = cfg.max_orders; i-- > 0;) free_.push_back(static_cast<std::uint32_t>(i));
  }

  // Opens trading in an instrument. Setup only: reserves its price levels.
  void add_instrument(std::uint16_t instrument) {
    if (instrument >= books_.size()) throw std::out_of_range("instrument beyond max_instruments");
    Book& b = books_[instrument];
    if (b.active) return;
    b.active = true;
    b.side[0].reserve(cfg_.levels_per_side);
    b.side[1].reserve(cfg_.levels_per_side);
  }
  bool has_instrument(std::uint16_t instrument) const noexcept {
    return instrument < books_.size() && books_[instrument].active;
  }

  template <class L>
  void add(const NewOrder& o, L& l) {
    if (const auto r = validate(o)) {
      reject(o.key, *r, l);
      return;
    }
    const std::uint32_t n = allocate(o.key);
    if (n == KeyIndex::kNone) {
      reject(o.key, free_.empty() ? Reject::kBookFull : Reject::kDuplicateKey, l);
      return;
    }
    Node& x = nodes_[n];
    init(x, o, 0);
    ++stats_.accepted;
    l.on_accepted(view(x));
    match_and_rest(n, o.tif, l);
  }

  // Reduces a live order to `remaining` shares (0 cancels it). Asking for
  // more than it has changes nothing. Returns false for an unknown key.
  template <class L>
  bool cancel(OrderKey key, std::uint32_t remaining, L& l) {
    const std::uint32_t n = index_.find(key);
    if (n == KeyIndex::kNone) return false;
    Node& x = nodes_[n];
    if (remaining >= x.remaining) return true;
    reduce(n, x.remaining - remaining, CancelReason::kUser, l);
    return true;
  }

  // Takes up to `qty` shares off a live order (all that is left if fewer).
  // Returns false for an unknown key.
  template <class L>
  bool reduce_by(OrderKey key, std::uint32_t qty, L& l) {
    const std::uint32_t n = index_.find(key);
    if (n == KeyIndex::kNone) return false;
    const std::uint32_t q = qty < nodes_[n].remaining ? qty : nodes_[n].remaining;
    if (q > 0) reduce(n, q, CancelReason::kUser, l);
    return true;
  }

  // Replaces a live order: the old one leaves the book, the replacement
  // keeps its side and instrument, loses time priority and may trade at once.
  // `r.quantity` counts the whole chain including shares already executed
  // (OUCH), unless `open_quantity` says it is the replacement's own size (ITCH).
  template <class L>
  void replace(OrderKey old_key, const NewOrder& r, L& l, bool open_quantity = false) {
    const std::uint32_t old_n = index_.find(old_key);
    if (old_n == KeyIndex::kNone) {
      reject(r.key, Reject::kUnknownOrder, l);
      return;
    }
    Node& old = nodes_[old_n];
    NewOrder o = r;
    o.instrument = old.instrument;
    o.side = old.side;
    if (open_quantity) o.quantity += old.executed;
    if (const auto bad = validate(o)) {
      reject(r.key, *bad, l);
      return;
    }
    if (r.key != old_key && index_.find(r.key) != KeyIndex::kNone) {
      reject(r.key, Reject::kDuplicateKey, l);
      return;
    }
    if (o.quantity <= old.executed) {  // nothing left to replace into
      reduce(old_n, old.remaining, CancelReason::kUser, l);
      return;
    }
    const OrderView before = view(old);
    const std::uint32_t executed = old.executed;
    unlink(old_n);
    release(old_n);
    const std::uint32_t n = allocate(o.key);  // cannot fail: a slot and the key were just freed
    Node& x = nodes_[n];
    o.quantity -= executed;
    init(x, o, executed);
    ++stats_.replaced;
    l.on_replaced(before, view(x));
    match_and_rest(n, o.tif, l);
  }

  // Cancels every live order of `owner` (disconnect, kill switch). Scans the
  // whole pool: an emergency path, not a hot one. Returns orders canceled.
  template <class L>
  std::size_t cancel_owner(std::uint16_t owner, CancelReason why, L& l) {
    std::size_t count = 0;
    for (std::uint32_t n = 0; n < nodes_.size(); ++n) {
      if (nodes_[n].in_use && key_owner(nodes_[n].key) == owner) {
        reduce(n, nodes_[n].remaining, why, l);
        ++count;
      }
    }
    return count;
  }

  // Book inspection, best level first. Not for hot paths.
  struct LevelView {
    std::uint32_t price;
    std::uint64_t shares;
    std::uint32_t orders;
  };
  std::vector<LevelView> depth(std::uint16_t instrument, Side side, std::size_t levels) const {
    std::vector<LevelView> out;
    const auto& v = books_[instrument].side[static_cast<int>(side)];
    for (std::size_t i = v.size(); i-- > 0 && out.size() < levels;) {
      out.push_back({v[i].price, v[i].shares, v[i].orders});
    }
    return out;
  }
  const OrderView* find(OrderKey key) noexcept {
    const std::uint32_t n = index_.find(key);
    if (n == KeyIndex::kNone) return nullptr;
    scratch_ = view(nodes_[n]);
    return &scratch_;
  }
  std::size_t live_orders() const noexcept { return index_.size(); }
  const EngineStats& stats() const noexcept { return stats_; }

 private:
  static constexpr std::uint32_t kNil = ~std::uint32_t{0};

  struct Node {
    OrderKey key = 0;
    std::uint64_t orn = 0;
    std::uint32_t price = 0;
    std::uint32_t remaining = 0;
    std::uint32_t executed = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;
    std::uint16_t instrument = 0;
    Side side = Side::kBuy;
    bool on_book = false;
    bool in_use = false;
  };

  struct Level {
    std::uint32_t price = 0;
    std::uint32_t orders = 0;
    std::uint64_t shares = 0;
    std::uint32_t head = kNil;  // oldest order, first to trade
    std::uint32_t tail = kNil;
  };

  // Each side sorted so the best price is at the back.
  struct Book {
    bool active = false;
    std::vector<Level> side[2];
  };

  static bool better(Side s, std::uint32_t a, std::uint32_t b) noexcept {
    return s == Side::kBuy ? a > b : a < b;
  }
  static bool crosses(Side aggressor, std::uint32_t limit, std::uint32_t resting) noexcept {
    return aggressor == Side::kBuy ? resting <= limit : resting >= limit;
  }

  OrderView view(const Node& x) const noexcept {
    return {x.key, x.orn, x.instrument, x.side, x.on_book, x.price, x.remaining, x.executed};
  }

  std::optional<Reject> validate(const NewOrder& o) const noexcept {
    if (!has_instrument(o.instrument)) return Reject::kUnknownInstrument;
    if (o.quantity == 0 || o.quantity > cfg_.max_quantity) return Reject::kInvalidQuantity;
    if (o.price == 0 || o.price > kMaxPrice) return Reject::kInvalidPrice;
    return std::nullopt;
  }

  template <class L>
  void reject(OrderKey key, Reject why, L& l) {
    ++stats_.rejected;
    l.on_rejected(key, why);
  }

  std::uint32_t allocate(OrderKey key) noexcept {
    if (LAB_UNLIKELY(free_.empty())) return KeyIndex::kNone;
    const std::uint32_t n = free_.back();
    if (LAB_UNLIKELY(!index_.insert(key, n))) return KeyIndex::kNone;
    free_.pop_back();
    return n;
  }

  void release(std::uint32_t n) noexcept {
    index_.erase(nodes_[n].key);
    nodes_[n].in_use = false;
    free_.push_back(n);
  }

  void init(Node& x, const NewOrder& o, std::uint32_t executed) noexcept {
    x.key = o.key;
    x.orn = ++orn_;
    x.price = o.price;
    x.remaining = o.quantity;
    x.executed = executed;
    x.prev = x.next = kNil;
    x.instrument = o.instrument;
    x.side = o.side;
    x.on_book = false;
    x.in_use = true;
  }

  template <class L>
  void match_and_rest(std::uint32_t n, Tif tif, L& l) {
    Node& x = nodes_[n];
    auto& opposite = books_[x.instrument].side[x.side == Side::kBuy ? 1 : 0];
    while (x.remaining > 0 && !opposite.empty() &&
           crosses(x.side, x.price, opposite.back().price)) {
      Level& lv = opposite.back();
      while (x.remaining > 0 && lv.head != kNil) {
        const std::uint32_t r = lv.head;
        Node& rest = nodes_[r];
        const std::uint32_t q = x.remaining < rest.remaining ? x.remaining : rest.remaining;
        rest.remaining -= q;
        rest.executed += q;
        x.remaining -= q;
        x.executed += q;
        lv.shares -= q;
        ++stats_.executions;
        stats_.executed_shares += q;
        const bool filled = rest.remaining == 0;
        l.on_execution(view(rest), view(x), q, lv.price, ++match_);
        if (filled) {
          rest.on_book = false;
          lv.head = rest.next;
          if (lv.head != kNil)
            nodes_[lv.head].prev = kNil;
          else
            lv.tail = kNil;
          --lv.orders;
          release(r);
        }
      }
      if (lv.head == kNil) opposite.pop_back();
    }
    if (x.remaining == 0) {
      release(n);
      return;
    }
    if (tif == Tif::kIoc) {
      const std::uint32_t q = x.remaining;
      x.remaining = 0;
      ++stats_.canceled;
      l.on_canceled(view(x), q, CancelReason::kIoc);
      release(n);
      return;
    }
    link(n);
    l.on_rested(view(x));
  }

  // Takes `qty` shares off a live order, removing it at zero.
  template <class L>
  void reduce(std::uint32_t n, std::uint32_t qty, CancelReason why, L& l) {
    Node& x = nodes_[n];
    const bool was_on_book = x.on_book;
    if (was_on_book) level_of(x).shares -= qty;
    x.remaining -= qty;
    const bool gone = x.remaining == 0;
    if (gone) unlink(n);
    ++stats_.canceled;
    OrderView v = view(x);
    v.on_book = was_on_book;
    l.on_canceled(v, qty, why);
    if (gone) release(n);
  }

  Level& level_of(const Node& x) noexcept {
    auto& v = books_[x.instrument].side[static_cast<int>(x.side)];
    std::size_t i = v.size();
    while (v[--i].price != x.price) {
    }
    return v[i];
  }

  // Appends a resting order at the back of its price level's queue.
  void link(std::uint32_t n) {
    Node& x = nodes_[n];
    auto& v = books_[x.instrument].side[static_cast<int>(x.side)];
    std::size_t i = v.size();
    while (i > 0 && better(x.side, v[i - 1].price, x.price)) --i;  // skip better levels
    if (i == 0 || v[i - 1].price != x.price) {
      v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), Level{x.price, 0, 0, kNil, kNil});
    } else {
      --i;
    }
    Level& lv = v[i];
    x.prev = lv.tail;
    x.next = kNil;
    if (lv.tail != kNil)
      nodes_[lv.tail].next = n;
    else
      lv.head = n;
    lv.tail = n;
    ++lv.orders;
    lv.shares += x.remaining;
    x.on_book = true;
  }

  // Removes a resting order from its level; drops the level when empty.
  void unlink(std::uint32_t n) noexcept {
    Node& x = nodes_[n];
    if (!x.on_book) return;
    auto& v = books_[x.instrument].side[static_cast<int>(x.side)];
    std::size_t i = v.size();
    while (v[--i].price != x.price) {
    }
    Level& lv = v[i];
    if (x.prev != kNil)
      nodes_[x.prev].next = x.next;
    else
      lv.head = x.next;
    if (x.next != kNil)
      nodes_[x.next].prev = x.prev;
    else
      lv.tail = x.prev;
    --lv.orders;
    lv.shares -= x.remaining;
    if (lv.orders == 0) v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    x.on_book = false;
  }

  EngineConfig cfg_;
  std::vector<Node, HugePageAllocator<Node>> nodes_;  // see lab/core/huge_pages.hpp
  std::vector<std::uint32_t> free_;
  KeyIndex index_;
  std::vector<Book> books_;
  std::uint64_t orn_ = 0;
  std::uint64_t match_ = 0;
  EngineStats stats_;
  OrderView scratch_{};
};

}  // namespace lab::match
