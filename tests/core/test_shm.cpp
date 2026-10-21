#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "lab/core/shm.hpp"
#include "lab/core/spsc_ring.hpp"

namespace {

struct Tick {
  std::uint64_t seq;
  std::int64_t price;
};
using TickRing = lab::SpscRing<Tick, 1024>;

std::string unique_name(const char* tag) {
  return std::string{"lab-test-"} + tag + "-" + std::to_string(getpid());
}

TEST(Shm, CreateAndReopenSeeSameBytes) {
  const auto name = unique_name("bytes");
  auto a = lab::ShmRegion::create(name, 4096);
  auto b = lab::ShmRegion::open(name);
  lab::ShmRegion::unlink(name);
  ASSERT_EQ(b.size(), 4096u);
  static_cast<char*>(a.data())[100] = 'x';
  EXPECT_EQ(static_cast<char*>(b.data())[100], 'x');
}

TEST(Shm, OpenMissingRegionThrows) {
  EXPECT_THROW(lab::ShmRegion::open(unique_name("missing")), std::system_error);
}

TEST(Shm, AttachRejectsWrongType) {
  const auto name = unique_name("type");
  auto region = lab::ShmRegion::create(name, lab::shared_size<TickRing>());
  lab::create_shared<TickRing>(region);
  lab::ShmRegion::unlink(name);
  using SmallerRing = lab::SpscRing<Tick, 512>;
  EXPECT_THROW(lab::attach_shared<SmallerRing>(region), std::runtime_error);
}

TEST(Shm, RingCarriesTicksBetweenProcesses) {
  const auto name = unique_name("ring");
  auto region = lab::ShmRegion::create(name, lab::shared_size<TickRing>());
  TickRing* ring = lab::create_shared<TickRing>(region);
  constexpr std::uint64_t kCount = 100'000;

  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: map the region by name, as a separate process would.
    int rc = 0;
    try {
      auto child_region = lab::ShmRegion::open(name);
      TickRing* r = lab::attach_shared<TickRing>(child_region);
      for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!r->try_push({i, static_cast<std::int64_t>(i) * 100})) lab::cpu_relax();
      }
    } catch (...) {
      rc = 1;
    }
    _exit(rc);
  }

  std::uint64_t expected = 0;
  bool ok = true;
  while (expected < kCount) {
    Tick t{};
    if (ring->try_pop(t)) {
      ok &= t.seq == expected && t.price == static_cast<std::int64_t>(expected) * 100;
      ++expected;
    } else {
      lab::cpu_relax();
    }
  }
  int status = 0;
  waitpid(pid, &status, 0);
  lab::ShmRegion::unlink(name);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace
