#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "lab/core/spsc_ring.hpp"

namespace {

struct Msg {
  std::uint64_t seq;
  std::uint64_t payload;
};

TEST(SpscRing, StartsEmpty) {
  lab::SpscRing<Msg, 8> ring;
  Msg m{};
  EXPECT_FALSE(ring.try_pop(m));
  EXPECT_EQ(ring.front(), nullptr);
  EXPECT_EQ(ring.size_approx(), 0u);
}

TEST(SpscRing, FillsToCapacityThenRejects) {
  lab::SpscRing<Msg, 8> ring;
  for (std::uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(ring.try_push({i, i * 10}));
  EXPECT_FALSE(ring.try_push({99, 0}));
  EXPECT_EQ(ring.try_claim(), nullptr);
  EXPECT_EQ(ring.size_approx(), 8u);
}

TEST(SpscRing, PreservesOrderAcrossWraparound) {
  lab::SpscRing<Msg, 4> ring;
  std::uint64_t next_in = 0, next_out = 0;
  for (int round = 0; round < 100; ++round) {
    while (ring.try_push({next_in, 0})) ++next_in;
    Msg m{};
    for (int k = 0; k < 3 && ring.try_pop(m); ++k) EXPECT_EQ(m.seq, next_out++);
  }
  Msg m{};
  while (ring.try_pop(m)) EXPECT_EQ(m.seq, next_out++);
  EXPECT_EQ(next_in, next_out);
}

TEST(SpscRing, ZeroCopyClaimAndFront) {
  lab::SpscRing<Msg, 4> ring;
  Msg* slot = ring.try_claim();
  ASSERT_NE(slot, nullptr);
  slot->seq = 7;
  slot->payload = 70;
  EXPECT_EQ(ring.front(), nullptr) << "not visible before commit";
  ring.commit();
  const Msg* read = ring.front();
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->seq, 7u);
  EXPECT_EQ(read->payload, 70u);
  ring.pop();
  EXPECT_EQ(ring.front(), nullptr);
}

TEST(SpscRing, TwoThreadsTransferEveryMessageInOrder) {
#if defined(LAB_SANITIZED)
  constexpr std::uint64_t kCount = 200'000;
#else
  constexpr std::uint64_t kCount = 5'000'000;
#endif
  static lab::SpscRing<Msg, 1024> ring;

  std::thread producer([] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!ring.try_push({i, i ^ 0x5a5aULL})) lab::cpu_relax();
    }
  });

  std::uint64_t expected = 0;
  bool ok = true;
  while (expected < kCount) {
    if (const Msg* m = ring.front()) {
      ok &= (m->seq == expected) && (m->payload == (expected ^ 0x5a5aULL));
      ring.pop();
      ++expected;
    } else {
      lab::cpu_relax();
    }
  }
  producer.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(ring.size_approx(), 0u);
}

}  // namespace
