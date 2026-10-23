#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "lab/core/huge_pages.hpp"

namespace {

TEST(HugePageAllocator, LargeTablesAreHugePageAligned) {
  std::vector<std::uint64_t, lab::HugePageAllocator<std::uint64_t>> v(1u << 20);  // 8 MB
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(v.data()) % lab::kHugePageSize, 0u);
  std::iota(v.begin(), v.end(), 0);
  EXPECT_EQ(v[12345], 12345u);
  EXPECT_EQ(v.back(), (1u << 20) - 1);
}

TEST(HugePageAllocator, SmallAllocationsUseTheHeap) {
  std::vector<int, lab::HugePageAllocator<int>> v(10, 7);
  v.push_back(8);  // grows: allocate and deallocate with matching sizes
  EXPECT_EQ(v.size(), 11u);
  EXPECT_EQ(v[3], 7);
  EXPECT_EQ(v.back(), 8);
}

}  // namespace
