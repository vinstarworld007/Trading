#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lab/book/order_map.hpp"

namespace {

using lab::book::Order;
using lab::book::OrderMap;
using lab::book::Side;

// Copy of an order, or an all-zero order when it is missing.
Order at(const Order* o) { return o != nullptr ? *o : Order{}; }

TEST(OrderMap, InsertFindErase) {
  OrderMap m{16};
  EXPECT_TRUE(m.insert(5, Order{100, 10, 1, Side::kBuy}));
  EXPECT_FALSE(m.insert(5, Order{200, 20, 1, Side::kSell})) << "duplicate refused";
  ASSERT_NE(m.find(5), nullptr);
  EXPECT_EQ(at(m.find(5)).price, 100u);
  EXPECT_EQ(m.find(6), nullptr);
  EXPECT_TRUE(m.erase(5));
  EXPECT_FALSE(m.erase(5));
  EXPECT_EQ(m.size(), 0u);
}

TEST(OrderMap, EraseFoundRemovesThatEntry) {
  OrderMap m{16};
  for (std::uint64_t r = 1; r <= 10; ++r)
    m.insert(r, Order{static_cast<std::uint32_t>(r), 1, 1, Side::kBuy});
  Order* o = m.find(7);
  ASSERT_NE(o, nullptr);
  m.erase_found(o);
  EXPECT_EQ(m.find(7), nullptr);
  for (std::uint64_t r = 1; r <= 10; ++r) {
    if (r != 7) {
      ASSERT_NE(m.find(r), nullptr) << r;
      EXPECT_EQ(at(m.find(r)).price, r);
    }
  }
}

TEST(OrderMap, MatchesReferenceMapUnderRandomChurnAndGrowth) {
  OrderMap m{16};  // forces many rehashes
  std::unordered_map<std::uint64_t, Order> ref;
  std::mt19937_64 rng{7};
  std::uint64_t next = 1;
  for (int i = 0; i < 200'000; ++i) {
    const auto op = rng() % 10;
    if (op < 5 || ref.empty()) {
      const std::uint64_t key = (rng() % 4 == 0) ? (rng() % next + 1) : next++;
      const Order o{static_cast<std::uint32_t>(rng()), static_cast<std::uint32_t>(i), 1,
                    Side::kSell};
      EXPECT_EQ(m.insert(key, o), ref.emplace(key, o).second);
    } else {
      const std::uint64_t key = rng() % next + 1;
      EXPECT_EQ(m.erase(key), ref.erase(key) == 1);
    }
  }
  EXPECT_EQ(m.size(), ref.size());
  for (const auto& [k, o] : ref) {
    const Order* found = m.find(k);
    ASSERT_NE(found, nullptr) << k;
    EXPECT_EQ(at(found).price, o.price);
    EXPECT_EQ(at(found).shares, o.shares);
  }
  std::size_t seen = 0;
  m.for_each([&](std::uint64_t, const Order&) { ++seen; });
  EXPECT_EQ(seen, ref.size());
}

}  // namespace
