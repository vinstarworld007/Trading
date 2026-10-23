// End to end without sockets: a synthetic ITCH day is packed into MoldUDP64
// packets, mangled by the fault injector, sequenced with retransmissions served
// from the store, and applied to books. The books must match a straight
// replay of the same day exactly.

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/itch/synthetic.hpp"
#include "lab/mold/fault_injector.hpp"
#include "lab/mold/retransmit_store.hpp"
#include "lab/mold/sequencer.hpp"

namespace {

using namespace lab::mold;
using lab::book::BookBuilder;

struct Day {
  std::vector<std::vector<std::byte>> messages;
  std::uint64_t digest = 0;
};

const Day& day() {
  static const Day d = [] {
    lab::itch::SyntheticConfig cfg;
    cfg.symbols = 20;
#if defined(LAB_SANITIZED)
    cfg.book_messages = 30'000;
#else
    cfg.book_messages = 200'000;
#endif
    Day out;
    auto books = std::make_unique<BookBuilder>(1u << 16);
    lab::itch::SyntheticDay{cfg}.generate([&](std::span<const std::byte> m) {
      out.messages.emplace_back(m.begin(), m.end());
      books->apply(m);
    });
    EXPECT_EQ(books->checks().error_count(), 0u);
    out.digest = books->digest();
    return out;
  }();
  return d;
}

struct Result {
  std::uint64_t digest;
  SequencerStats stats;
  bool finished;
};

// Sends the day through a lossy "network" with a one-tick delay and a
// re-request server that answers within the same tick.
Result run(const FaultConfig& faults) {
  const Session session = make_session("PIPE");
  RetransmitStore store{8u << 20, 1u << 20};
  FaultInjector inject{faults};
  SequencerConfig cfg;
  cfg.gap_delay = 2;
  cfg.request_timeout = 5;
  cfg.max_retries = 20;
  Sequencer seq{cfg};
  auto books = std::make_unique<BookBuilder>(1u << 16);
  auto deliver = [&](std::uint64_t, std::span<const std::byte> m) { books->apply(m); };

  std::deque<std::vector<std::byte>> wire;
  std::vector<std::byte> held;
  std::uint64_t now = 0;
  auto tick = [&] {
    ++now;
    while (!wire.empty()) {
      seq.on_packet(wire.front().data(), wire.front().size(), now, deliver);
      wire.pop_front();
    }
    Request r;
    if (seq.poll(now, r, deliver)) {
      PacketBuilder<> rb;
      if (store.fill(rb, session, r.sequence, r.count) > 0) {
        const auto p = rb.finish();
        wire.emplace_back(p.begin(), p.end());
      }
    }
  };
  auto send = [&](std::span<const std::byte> p) {
    switch (inject.next()) {
      case Fault::kDrop:
        return;
      case Fault::kReorder:
        if (held.empty()) {
          held.assign(p.begin(), p.end());
          return;
        }
        break;
      case Fault::kDuplicate:
        wire.emplace_back(p.begin(), p.end());
        break;
      case Fault::kNone:
        break;
    }
    wire.emplace_back(p.begin(), p.end());
    if (!held.empty()) {
      wire.push_back(std::move(held));
      held.clear();
    }
  };

  PacketBuilder<> b;
  b.begin(session, 1);
  for (const auto& m : day().messages) {
    if (!b.add(m)) {
      send(b.finish());
      tick();
      b.begin(session, store.next_sequence());
      b.add(m);
    }
    store.append(m);
  }
  send(b.finish());
  if (!held.empty()) wire.push_back(std::move(held));
  for (int i = 0; i < 50 && !seq.finished(); ++i) {
    PacketBuilder<> eos;
    eos.end_of_session(session, store.next_sequence());
    send(eos.finish());
    for (int t = 0; t < 10; ++t) tick();
  }
  return {books->digest(), seq.stats(), seq.finished()};
}

TEST(Pipeline, CleanFeedRebuildsTheSameBooks) {
  const Result r = run({});
  EXPECT_TRUE(r.finished);
  EXPECT_EQ(r.digest, day().digest);
  EXPECT_EQ(r.stats.gaps, 0u);
  EXPECT_EQ(r.stats.requests, 0u);
  EXPECT_EQ(r.stats.delivered, day().messages.size());
}

TEST(Pipeline, LossyFeedRecoversEveryMessage) {
  FaultConfig f;
  f.drop = 0.02;
  f.burst = 0.002;
  f.burst_len = 30;
  f.duplicate = 0.02;
  f.reorder = 0.02;
  f.seed = 11;
  const Result r = run(f);
  EXPECT_TRUE(r.finished);
  EXPECT_EQ(r.stats.lost, 0u);
  EXPECT_GT(r.stats.gaps, 10u);
  EXPECT_GT(r.stats.duplicates, 0u);
  EXPECT_EQ(r.stats.delivered, day().messages.size());
  EXPECT_EQ(r.digest, day().digest);
}

}  // namespace
