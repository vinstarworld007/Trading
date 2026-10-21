#pragma once

// Price-level (L2) book for one instrument, rebuilt from order-level (L3)
// events. Each side is a vector sorted so the best price is at the back:
// almost all activity happens near the touch, so the linear scan from the back
// usually stops within a few levels, and the memory is contiguous.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "lab/book/order_map.hpp"
#include "lab/core/compiler.hpp"

namespace lab::book {

struct Level {
  std::uint32_t price = 0;
  std::uint32_t orders = 0;
  std::uint64_t shares = 0;
};

class BookSide {
 public:
  explicit BookSide(Side side) : side_{side} {}

  void add(std::uint32_t price, std::uint32_t shares) {
    const std::size_t i = find_or_insert(price);
    levels_[i].shares += shares;
    ++levels_[i].orders;
  }

  // Removes `shares` at `price`; `order_gone` also removes one order from the
  // level. Returns false if the level does not exist or holds fewer shares,
  // which means the book and the order store disagree.
  bool reduce(std::uint32_t price, std::uint32_t shares, bool order_gone) {
    const std::size_t i = find(price);
    if (LAB_UNLIKELY(i == kNone)) return false;
    Level& l = levels_[i];
    if (LAB_UNLIKELY(l.shares < shares || (order_gone && l.orders == 0))) return false;
    l.shares -= shares;
    if (order_gone) --l.orders;
    if (l.orders == 0) remove_at(i);
    return true;
  }

  const Level* best() const noexcept { return levels_.empty() ? nullptr : &levels_.back(); }

  // depth 0 is the best level. Returns nullptr past the last level.
  const Level* level(std::size_t depth) const noexcept {
    return depth < levels_.size() ? &levels_[levels_.size() - 1 - depth] : nullptr;
  }

  std::size_t depth() const noexcept { return levels_.size(); }
  bool empty() const noexcept { return levels_.empty(); }
  Side side() const noexcept { return side_; }

 private:
  static constexpr std::size_t kNone = ~std::size_t{0};

  // True if a is a better price than b for this side.
  bool better(std::uint32_t a, std::uint32_t b) const noexcept {
    return side_ == Side::kBuy ? a > b : a < b;
  }

  std::size_t find(std::uint32_t price) const noexcept {
    for (std::size_t i = levels_.size(); i-- > 0;) {
      if (levels_[i].price == price) return i;
      if (better(price, levels_[i].price)) return kNone;  // walked past where it would be
    }
    return kNone;
  }

  std::size_t find_or_insert(std::uint32_t price) {
    std::size_t i = levels_.size();
    while (i > 0) {
      const std::uint32_t p = levels_[i - 1].price;
      if (p == price) return i - 1;
      if (better(price, p)) break;
      --i;
    }
    insert_at(i, Level{price, 0, 0});
    return i;
  }

  // Hand-rolled shifts: new and removed levels are almost always within a few
  // slots of the back, and this avoids a GCC 13 false-positive
  // -Wstringop-overflow on vector::insert/erase.
  void insert_at(std::size_t i, const Level& level) {
    levels_.push_back(level);
    for (std::size_t k = levels_.size() - 1; k > i; --k) levels_[k] = levels_[k - 1];
    levels_[i] = level;
  }

  void remove_at(std::size_t i) noexcept {
    for (std::size_t k = i + 1; k < levels_.size(); ++k) levels_[k - 1] = levels_[k];
    levels_.pop_back();
  }

  Side side_;
  std::vector<Level> levels_;  // worst ... best
};

class OrderBook {
 public:
  OrderBook() : bids_{Side::kBuy}, asks_{Side::kSell} {}

  BookSide& side(Side s) noexcept { return s == Side::kBuy ? bids_ : asks_; }
  const BookSide& side(Side s) const noexcept { return s == Side::kBuy ? bids_ : asks_; }
  const BookSide& bids() const noexcept { return bids_; }
  const BookSide& asks() const noexcept { return asks_; }

  // Best bid above best ask. Equal prices are locked(), not crossed.
  bool crossed() const noexcept {
    const Level* b = bids_.best();
    const Level* a = asks_.best();
    return b != nullptr && a != nullptr && b->price > a->price;
  }
  bool locked() const noexcept {
    const Level* b = bids_.best();
    const Level* a = asks_.best();
    return b != nullptr && a != nullptr && b->price == a->price;
  }

 private:
  BookSide bids_;
  BookSide asks_;
};

}  // namespace lab::book
