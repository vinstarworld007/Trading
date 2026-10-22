#pragma once

// Fixed-capacity open-addressing map from a 64-bit key to a 32-bit slot
// index. Linear probing with backward-shift deletion, so it never needs
// tombstones or rehashing: size it for the most live keys at construction.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lab/core/compiler.hpp"

namespace lab::match {

class KeyIndex {
 public:
  static constexpr std::uint32_t kNone = ~std::uint32_t{0};

  // Holds up to `max_keys` keys at a load factor of at most one half.
  explicit KeyIndex(std::size_t max_keys) {
    std::size_t cap = 16;
    while (cap < max_keys * 2) cap *= 2;
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
    max_ = max_keys;
  }

  LAB_ALWAYS_INLINE std::uint32_t find(std::uint64_t key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.value == kNone) return kNone;
      if (s.key == key) return s.value;
    }
  }

  // False if the key is present or the index is full.
  bool insert(std::uint64_t key, std::uint32_t value) noexcept {
    if (LAB_UNLIKELY(size_ >= max_)) return false;
    std::size_t i = home(key);
    for (; slots_[i].value != kNone; i = (i + 1) & mask_) {
      if (slots_[i].key == key) return false;
    }
    slots_[i] = Slot{key, value};
    ++size_;
    return true;
  }

  bool erase(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].value == kNone) return false;
      if (slots_[i].key == key) break;
    }
    // Backward-shift: pull later entries of the probe run into the hole.
    std::size_t hole = i;
    for (std::size_t j = (i + 1) & mask_; slots_[j].value != kNone; j = (j + 1) & mask_) {
      const std::size_t h = home(slots_[j].key);
      // Move j into the hole unless its home lies cyclically in (hole, j].
      const bool stays = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
      if (!stays) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return max_; }

 private:
  struct Slot {
    std::uint64_t key = 0;
    std::uint32_t value = kNone;
  };

  std::size_t home(std::uint64_t key) const noexcept {
    // Keys are often sequential, so mix them before masking.
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDULL;
    key ^= key >> 33;
    return static_cast<std::size_t>(key) & mask_;
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  std::size_t max_ = 0;
};

}  // namespace lab::match
