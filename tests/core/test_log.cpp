#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <thread>

#include "lab/core/log.hpp"

namespace {

std::string read_all(std::FILE* f) {
  std::fflush(f);
  std::rewind(f);
  std::string out;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  return out;
}

const auto kClock = lab::TscClock::calibrate(std::chrono::milliseconds{5});

TEST(AsyncLogger, FormatsPlaceholdersOnFlush) {
  std::FILE* f = std::tmpfile();
  ASSERT_NE(f, nullptr);
  {
    lab::AsyncLogger logger{f, kClock};
    auto& ch = logger.channel("itch-fh");
    EXPECT_TRUE(
        ch.warn("gap on session {} from {} to {} ({} msgs, {})", "A", 1001, 1042, -42, 1.5));
    EXPECT_EQ(logger.flush(), 1u);
  }
  const std::string out = read_all(f);
  std::fclose(f);
  EXPECT_NE(out.find("WARN  [itch-fh] gap on session A from 1001 to 1042 (-42 msgs, 1.5)"),
            std::string::npos)
      << out;
}

TEST(AsyncLogger, ExtraPlaceholdersStayLiteral) {
  std::FILE* f = std::tmpfile();
  ASSERT_NE(f, nullptr);
  {
    lab::AsyncLogger logger{f, kClock};
    logger.channel("t").info("a={} b={}", 1);
    logger.flush();
  }
  const std::string out = read_all(f);
  std::fclose(f);
  EXPECT_NE(out.find("a=1 b={}"), std::string::npos) << out;
}

TEST(AsyncLogger, DropsAndCountsWhenChannelIsFull) {
  std::FILE* f = std::tmpfile();
  ASSERT_NE(f, nullptr);
  lab::AsyncLogger logger{f, kClock};
  auto& ch = logger.channel("busy");
  for (std::size_t i = 0; i < lab::LogChannel::kCapacity; ++i) ASSERT_TRUE(ch.debug("{}", i));
  EXPECT_FALSE(ch.debug("overflow"));
  EXPECT_EQ(ch.dropped(), 1u);
  EXPECT_EQ(logger.flush(), lab::LogChannel::kCapacity);
  std::fclose(f);
}

TEST(AsyncLogger, BackgroundThreadDrainsManyProducers) {
  std::FILE* f = std::tmpfile();
  ASSERT_NE(f, nullptr);
  constexpr int kThreads = 3;
  constexpr int kPerThread = 20'000;
  std::uint64_t dropped = 0;
  {
    lab::AsyncLogger logger{f, kClock};
    lab::LogChannel* channels[kThreads];
    for (int t = 0; t < kThreads; ++t) channels[t] = &logger.channel("worker");
    logger.start();
    std::thread workers[kThreads];
    for (int t = 0; t < kThreads; ++t) {
      workers[t] = std::thread([ch = channels[t]] {
        for (int i = 0; i < kPerThread; ++i) {
          while (!ch->info("msg {}", i)) std::this_thread::yield();
        }
      });
    }
    for (auto& w : workers) w.join();
    logger.stop();
    for (auto* ch : channels) dropped += ch->dropped();
  }
  const std::string out = read_all(f);
  std::fclose(f);
  std::size_t lines = 0;
  for (char c : out) lines += (c == '\n');
  // Each failed attempt counts as a drop and is retried, so every message lands once.
  EXPECT_EQ(lines, static_cast<std::size_t>(kThreads * kPerThread));
  (void)dropped;
}

}  // namespace
