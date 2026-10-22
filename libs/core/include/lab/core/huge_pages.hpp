#pragma once

// An allocator for large preallocated tables (an order pool, a hash index)
// that asks the kernel for transparent huge pages. A table of hundreds of MB
// touched at random costs a TLB miss on most accesses with 4 KB pages; with
// 2 MB pages the same table needs 512 times fewer TLB entries.
//
// Allocations of 2 MB or more are mmap'ed on a 2 MB boundary and madvise'd
// MADV_HUGEPAGE, which works when /sys/kernel/mm/transparent_hugepage/enabled
// is "always" or "madvise" (lab-check reports it). Anything smaller, or a
// kernel that refuses, falls back to normal pages: the allocator never fails
// because huge pages are unavailable. LAB_NO_HUGE_PAGES=1 in the environment
// turns the advice off, for before/after measurements. Allocation is a startup cost; a vector
// that grows on a hot path should not use this.
//
//   std::vector<Node, lab::HugePageAllocator<Node>> nodes(n);  // value-init faults pages in

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace lab {

inline constexpr std::size_t kHugePageSize = 2u << 20;

// Maps `bytes` (rounded up to whole huge pages when large), 2 MB aligned.
inline void* huge_page_map(std::size_t bytes) {
  if (bytes < kHugePageSize) {
    void* p = ::operator new(bytes);
    return p;
  }
  const std::size_t size = (bytes + kHugePageSize - 1) & ~(kHugePageSize - 1);
  const std::size_t span = size + kHugePageSize;
  void* raw = ::mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) throw std::bad_alloc{};
  const auto base = reinterpret_cast<std::uintptr_t>(raw);
  const std::uintptr_t aligned = (base + kHugePageSize - 1) & ~(kHugePageSize - 1);
  if (aligned > base) ::munmap(raw, aligned - base);
  const std::uintptr_t end = base + span;
  if (end > aligned + size) ::munmap(reinterpret_cast<void*>(aligned + size), end - aligned - size);
  void* p = reinterpret_cast<void*>(aligned);
  if (std::getenv("LAB_NO_HUGE_PAGES") == nullptr) {  // set it to compare with 4 KB pages
    (void)::madvise(p, size, MADV_HUGEPAGE);          // advisory: normal pages if refused
  }
  return p;
}

inline void huge_page_unmap(void* p, std::size_t bytes) noexcept {
  if (bytes < kHugePageSize) {
    ::operator delete(p);
    return;
  }
  ::munmap(p, (bytes + kHugePageSize - 1) & ~(kHugePageSize - 1));
}

template <class T>
struct HugePageAllocator {
  using value_type = T;

  HugePageAllocator() noexcept = default;
  template <class U>
  HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) { return static_cast<T*>(huge_page_map(n * sizeof(T))); }
  void deallocate(T* p, std::size_t n) noexcept { huge_page_unmap(p, n * sizeof(T)); }

  template <class U>
  bool operator==(const HugePageAllocator<U>&) const noexcept {
    return true;
  }
};

}  // namespace lab
