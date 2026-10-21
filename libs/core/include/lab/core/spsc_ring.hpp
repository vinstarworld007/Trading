#pragma once

// Bounded single-producer single-consumer ring buffer.
//
// The ring owns its storage inline and holds no pointers, so it can be
// placement-constructed in shared memory and used across processes (see
// shm.hpp). Producer and consumer indices live on separate cache lines, and
// each side keeps a private cached copy of the other side's index so the
// common case touches no shared cache line except the slot itself.
//
// Two APIs:
//   try_push / try_pop        copy a T in or out
//   try_claim+commit, front+pop  zero-copy: write or read the slot in place

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lab/core/compiler.hpp"

namespace lab {

template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "slots are copied with plain stores");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "lock-free 64-bit atomics are required for cross-process use");

 public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;

  SpscRing() noexcept = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer side.

  LAB_ALWAYS_INLINE bool try_push(const T& value) noexcept {
    T* slot = try_claim();
    if (LAB_UNLIKELY(slot == nullptr)) return false;
    *slot = value;
    commit();
    return true;
  }

  // Returns the next free slot, or nullptr when full. The slot is not visible
  // to the consumer until commit().
  LAB_ALWAYS_INLINE T* try_claim() noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (LAB_UNLIKELY(head - cached_tail_ == Capacity)) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - cached_tail_ == Capacity) return nullptr;
    }
    return &slots_[head & kMask];
  }

  LAB_ALWAYS_INLINE void commit() noexcept {
    head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // Consumer side.

  LAB_ALWAYS_INLINE bool try_pop(T& out) noexcept {
    const T* slot = front();
    if (LAB_UNLIKELY(slot == nullptr)) return false;
    out = *slot;
    pop();
    return true;
  }

  // Returns the oldest unread slot, or nullptr when empty. The slot stays valid
  // until pop().
  LAB_ALWAYS_INLINE const T* front() noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (LAB_UNLIKELY(tail == cached_head_)) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return nullptr;
    }
    return &slots_[tail & kMask];
  }

  LAB_ALWAYS_INLINE void pop() noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // Either side. Exact only when the other side is idle.
  std::size_t size_approx() const noexcept {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(head - tail);
  }

  bool empty_approx() const noexcept { return size_approx() == 0; }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::uint64_t kMask = Capacity - 1;

  // Producer-owned line.
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_{0};

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_{0};

  // Zeroed on construction, which also faults the pages in before the hot path runs.
  alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace lab
