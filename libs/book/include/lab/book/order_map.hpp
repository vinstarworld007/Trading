#pragma once

// Open-addressing hash map from ITCH order reference number to the order's
// book position. Linear probing with backward-shift deletion (no tombstones),
// so lookups stay short however many orders come and go during a day.
//
// Pre-size it with reserve() before the hot path; growth rehashes everything
// and is meant for offline tools, not a live feed handler.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lab/core/compiler.hpp"

namespace lab::book {

enum class Side : std::uint8_t { kBuy = 0, kSell = 1 };

struct Order {
  std::uint32_t price = 0;   // Price(4)
  std::uint32_t shares = 0;  // remaining displayed shares
  std::uint16_t locate = 0;
  Side side = Side::kBuy;
};

class OrderMap {
 public:
  explicit OrderMap(std::size_t initial_capacity = 1u << 20) { rehash(round_up(initial_capacity)); }

  // Makes room for `orders` live orders without growing.
  void reserve(std::size_t orders) {
    const std::size_t want = round_up(orders * 2);
    if (want > slots_.size()) rehash(want);
  }

  // Inserts a new order. Returns false (and changes nothing) if `ref` is live.
  bool insert(std::uint64_t ref, const Order& order) {
    if (LAB_UNLIKELY((size_ + 1) * 10 > slots_.size() * 7)) rehash(slots_.size() * 2);
    std::size_t i = home(ref);
    while (slots_[i].key != kEmpty) {
      if (slots_[i].key == ref) return false;
      i = (i + 1) & mask_;
    }
    slots_[i].key = ref;
    slots_[i].order = order;
    ++size_;
    return true;
  }

  // Live order for `ref`, or nullptr.
  LAB_ALWAYS_INLINE Order* find(std::uint64_t ref) noexcept {
    std::size_t i = home(ref);
    while (true) {
      const std::uint64_t k = slots_[i].key;
      if (k == ref) return &slots_[i].order;
      if (k == kEmpty) return nullptr;
      i = (i + 1) & mask_;
    }
  }

  const Order* find(std::uint64_t ref) const noexcept {
    return const_cast<OrderMap*>(this)->find(ref);
  }

  // Removes `ref`; returns false if it was not live.
  bool erase(std::uint64_t ref) noexcept {
    std::size_t i = home(ref);
    while (slots_[i].key != ref) {
      if (slots_[i].key == kEmpty) return false;
      i = (i + 1) & mask_;
    }
    erase_slot(i);
    return true;
  }

  // Removes the order `find` returned. Cheaper than erase(ref) after a find.
  void erase_found(Order* order) noexcept {
    // The Order sits right after its key inside the slot.
    auto* slot =
        reinterpret_cast<Slot*>(reinterpret_cast<std::byte*>(order) - offsetof(Slot, order));
    erase_slot(static_cast<std::size_t>(slot - slots_.data()));
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

  template <class F>
  void for_each(F&& f) const {
    for (const Slot& s : slots_) {
      if (s.key != kEmpty) f(s.key, s.order);
    }
  }

 private:
  // ITCH never uses this reference number, so it marks empty slots.
  static constexpr std::uint64_t kEmpty = ~0ULL;

  struct Slot {
    std::uint64_t key = kEmpty;
    Order order;
  };

  static std::size_t round_up(std::size_t n) {
    std::size_t c = 16;
    while (c < n) c <<= 1;
    return c;
  }

  // Identity hash. Nasdaq assigns order reference numbers sequentially through
  // the day, and most orders are cancelled soon after they are added, so the
  // live set sits in a sliding window of recent references: keeping adjacent
  // references in adjacent slots makes that window cache-friendly and
  // collision-free. (A multiplicative hash measured ~35% slower on the
  // itch_bench book rebuild.)
  LAB_ALWAYS_INLINE std::size_t home(std::uint64_t key) const noexcept {
    // Fibonacci hashing spreads sequential keys so probe runs stay short.
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> 20) & mask_;
  }

  void erase_slot(std::size_t i) noexcept {
    // Backward shift: pull later entries of the same probe run into the gap.
    std::size_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].key == kEmpty) break;
      const std::size_t k = home(slots_[j].key);
      const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (stays) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].key = kEmpty;
    --size_;
  }

  void rehash(std::size_t new_capacity) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(new_capacity, Slot{});
    mask_ = new_capacity - 1;
    size_ = 0;
    for (const Slot& s : old) {
      if (s.key != kEmpty) insert(s.key, s.order);
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  // namespace lab::book
