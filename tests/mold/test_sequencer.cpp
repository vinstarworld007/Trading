#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "lab/core/endian.hpp"
#include "lab/mold/mold.hpp"
#include "lab/mold/sequencer.hpp"

namespace {

using namespace lab::mold;

const Session kSession = make_session("TEST");

// A packet carrying messages first..first+n-1; each message is 8 bytes holding
// its own sequence number, so delivery order can be checked.
std::vector<std::byte> packet(std::uint64_t first, std::uint16_t n,
                              const Session& session = kSession) {
  PacketBuilder<> b;
  b.begin(session, first);
  for (std::uint64_t s = first; s < first + n; ++s) {
    std::array<std::byte, 8> m;
    lab::store_be64(m.data(), s);
    EXPECT_TRUE(b.add(m));
  }
  const auto p = b.finish();
  return {p.begin(), p.end()};
}

std::vector<std::byte> heartbeat(std::uint64_t next, bool eos = false) {
  PacketBuilder<> b;
  if (eos)
    b.end_of_session(kSession, next);
  else
    b.heartbeat(kSession, next);
  const auto p = b.finish();
  return {p.begin(), p.end()};
}

struct Harness {
  explicit Harness(SequencerConfig cfg = {}) : seq{cfg} {}

  auto sink() {
    return [this](std::uint64_t s, std::span<const std::byte> m) {
      ASSERT_EQ(m.size(), 8u);
      EXPECT_EQ(lab::load_be64(m.data()), s) << "message body does not match its sequence";
      got.push_back(s);
    };
  }

  PacketResult feed(const std::vector<std::byte>& p, std::uint64_t now = 0) {
    return seq.on_packet(p.data(), p.size(), now, sink());
  }
  bool poll(std::uint64_t now, Request& r) { return seq.poll(now, r, sink()); }

  std::vector<std::uint64_t> range(std::uint64_t first, std::uint64_t last) const {
    std::vector<std::uint64_t> v;
    for (std::uint64_t s = first; s <= last; ++s) v.push_back(s);
    return v;
  }

  Sequencer seq;
  std::vector<std::uint64_t> got;
};

TEST(Sequencer, DeliversInOrderTrafficStraightThrough) {
  Harness h;
  h.feed(packet(1, 3));
  h.feed(packet(4, 2));
  EXPECT_EQ(h.got, h.range(1, 5));
  EXPECT_EQ(h.seq.expected(), 6u);
  EXPECT_FALSE(h.seq.in_gap());
  Request r;
  EXPECT_FALSE(h.poll(0, r));
  EXPECT_EQ(h.seq.stats().held, 0u);
  EXPECT_EQ(trimmed(h.seq.session()), "TEST");
}

TEST(Sequencer, DropsDuplicates) {
  Harness h;
  h.feed(packet(1, 3));
  h.feed(packet(1, 3));
  h.feed(packet(2, 3));  // 2 and 3 old, 4 new
  EXPECT_EQ(h.got, h.range(1, 4));
  EXPECT_EQ(h.seq.stats().duplicates, 5u);
}

TEST(Sequencer, ReorderedPacketFillsTheGapWithoutARequest) {
  SequencerConfig cfg;
  cfg.gap_delay = 100;
  Harness h{cfg};
  h.feed(packet(1, 2), 0);
  h.feed(packet(5, 2), 10);  // 3 and 4 late
  EXPECT_TRUE(h.seq.in_gap());
  EXPECT_EQ(h.got, h.range(1, 2));
  Request r;
  EXPECT_FALSE(h.poll(50, r));  // still inside the reorder allowance
  h.feed(packet(3, 2), 60);
  EXPECT_EQ(h.got, h.range(1, 6));
  EXPECT_FALSE(h.seq.in_gap());
  EXPECT_EQ(h.seq.stats().gaps, 1u);
  EXPECT_EQ(h.seq.stats().gaps_filled, 1u);
  EXPECT_EQ(h.seq.stats().requests, 0u);
  EXPECT_EQ(h.seq.stats().held, 2u);
}

TEST(Sequencer, RequestsTheMissingRunAndDeliversHeldMessagesAfterIt) {
  SequencerConfig cfg;
  cfg.gap_delay = 100;
  cfg.request_timeout = 1000;
  Harness h{cfg};
  h.feed(packet(1, 2), 0);
  h.feed(packet(10, 3), 0);  // 3..9 lost
  Request r;
  ASSERT_TRUE(h.poll(100, r));
  EXPECT_EQ(r.session, kSession);
  EXPECT_EQ(r.sequence, 3u);
  EXPECT_EQ(r.count, 7u);        // up to the first held message
  EXPECT_FALSE(h.poll(150, r));  // waiting for the answer

  h.feed(packet(3, 7), 200);  // the retransmission
  EXPECT_EQ(h.got, h.range(1, 12));
  EXPECT_FALSE(h.seq.in_gap());
  EXPECT_EQ(h.seq.stats().gaps_filled, 1u);
  EXPECT_EQ(h.seq.stats().lost, 0u);
}

TEST(Sequencer, AsksForTheRestRightAwayAfterAPartialAnswer) {
  SequencerConfig cfg;
  cfg.request_timeout = 1000;
  Harness h{cfg};
  h.feed(packet(1, 1));
  h.feed(packet(100, 1));
  Request r;
  ASSERT_TRUE(h.poll(0, r));
  EXPECT_EQ(r.sequence, 2u);
  EXPECT_EQ(r.count, 98u);
  h.feed(packet(2, 40), 10);   // the server sent only what fit in one packet
  ASSERT_TRUE(h.poll(11, r));  // progress: no need to wait for the timeout
  EXPECT_EQ(r.sequence, 42u);
  EXPECT_EQ(r.count, 58u);
  h.feed(packet(42, 58), 20);
  EXPECT_EQ(h.got, h.range(1, 100));
}

TEST(Sequencer, RetriesAfterTimeoutThenGivesUp) {
  SequencerConfig cfg;
  cfg.request_timeout = 10;
  cfg.max_retries = 3;
  Harness h{cfg};
  h.feed(packet(1, 1));
  h.feed(packet(5, 2));  // 2..4 lost for good
  Request r;
  std::uint64_t now = 0;
  int requests = 0;
  for (; now < 1000; ++now) {
    if (h.poll(now, r)) {
      ++requests;
      EXPECT_EQ(r.sequence, 2u);
      EXPECT_EQ(r.count, 3u);
    }
    if (!h.seq.in_gap()) break;
  }
  EXPECT_EQ(requests, 3);
  EXPECT_EQ(now, 30u);  // requests at 0, 10, 20; given up at 30
  EXPECT_EQ(h.got, (std::vector<std::uint64_t>{1, 5, 6}));
  EXPECT_EQ(h.seq.stats().lost, 3u);
  EXPECT_EQ(h.seq.stats().gaps_given_up, 1u);
  EXPECT_EQ(h.seq.expected(), 7u);
}

TEST(Sequencer, GivingUpOneGapLeavesTheNextOneOpen) {
  SequencerConfig cfg;
  cfg.request_timeout = 10;
  cfg.max_retries = 1;
  Harness h{cfg};
  h.feed(packet(1, 1));
  h.feed(packet(3, 1));  // 2 lost
  h.feed(packet(6, 1));  // 4, 5 lost
  Request r;
  ASSERT_TRUE(h.poll(0, r));
  EXPECT_EQ(r.sequence, 2u);
  EXPECT_EQ(r.count, 1u);
  EXPECT_FALSE(h.poll(10, r));  // gives 2 up, delivers 3, opens the gap at 4
  EXPECT_EQ(h.got, (std::vector<std::uint64_t>{1, 3}));
  EXPECT_TRUE(h.seq.in_gap());
  ASSERT_TRUE(h.poll(11, r));
  EXPECT_EQ(r.sequence, 4u);
  EXPECT_EQ(r.count, 2u);
  h.feed(packet(4, 2), 12);
  EXPECT_EQ(h.got, (std::vector<std::uint64_t>{1, 3, 4, 5, 6}));
  EXPECT_EQ(h.seq.stats().lost, 1u);
}

TEST(Sequencer, HeartbeatRevealsALostTail) {
  Harness h;
  h.feed(packet(1, 3));
  h.feed(heartbeat(4));
  EXPECT_FALSE(h.seq.in_gap());
  h.feed(heartbeat(6));  // 4 and 5 were sent and lost
  EXPECT_TRUE(h.seq.in_gap());
  Request r;
  ASSERT_TRUE(h.poll(0, r));
  EXPECT_EQ(r.sequence, 4u);
  EXPECT_EQ(r.count, 2u);
  EXPECT_EQ(h.seq.stats().heartbeats, 2u);
}

TEST(Sequencer, FinishesOnlyWhenEverythingBeforeEndOfSessionArrived) {
  Harness h;
  h.feed(packet(1, 3));
  h.feed(heartbeat(6, /*eos=*/true));
  EXPECT_TRUE(h.seq.end_of_session_seen());
  EXPECT_FALSE(h.seq.finished());
  Request r;
  ASSERT_TRUE(h.poll(0, r));  // re-requests are still served during End of Session
  EXPECT_EQ(r.sequence, 4u);
  h.feed(packet(4, 2));
  EXPECT_TRUE(h.seq.finished());
}

TEST(Sequencer, LateJoinerRecoversFromTheStartOfTheSession) {
  Harness h;
  h.feed(packet(500, 10));
  EXPECT_TRUE(h.got.empty());
  Request r;
  ASSERT_TRUE(h.poll(0, r));
  EXPECT_EQ(r.sequence, 1u);
  EXPECT_EQ(r.count, 499u);
}

TEST(Sequencer, IgnoresOtherSessionsAndMalformedPackets) {
  Harness h;
  h.feed(packet(1, 1));
  EXPECT_EQ(h.feed(packet(2, 1, make_session("OTHER"))), PacketResult::kWrongSession);
  auto p = packet(2, 2);
  p.pop_back();
  EXPECT_EQ(h.feed(p), PacketResult::kMalformed);  // the first message is still used
  EXPECT_EQ(h.got, (std::vector<std::uint64_t>{1, 2}));
  const std::vector<std::byte> runt(5);
  EXPECT_EQ(h.feed(runt), PacketResult::kMalformed);
  EXPECT_EQ(h.seq.stats().wrong_session, 1u);
  EXPECT_EQ(h.seq.stats().malformed, 2u);
}

TEST(Sequencer, MessagesBeyondTheWindowAreRequestedAgainLater) {
  SequencerConfig cfg;
  cfg.window = 8;
  Harness h{cfg};
  h.feed(packet(1, 1));
  h.feed(packet(3, 10));  // 3..9 fit in the window from 2; 10..12 do not
  EXPECT_EQ(h.seq.stats().window_overflow, 3u);
  Request r;
  ASSERT_TRUE(h.poll(0, r));
  EXPECT_EQ(r.sequence, 2u);
  EXPECT_EQ(r.count, 1u);
  h.feed(packet(2, 1));
  EXPECT_EQ(h.got, h.range(1, 9));
  ASSERT_TRUE(h.poll(1, r));  // progress, so ask for 10..12 at once
  EXPECT_EQ(r.sequence, 10u);
  EXPECT_EQ(r.count, 3u);
  h.feed(packet(10, 3));
  EXPECT_EQ(h.got, h.range(1, 12));
}

TEST(Sequencer, RejectsBadConfig) {
  SequencerConfig cfg;
  cfg.window = 1000;
  EXPECT_THROW(Sequencer{cfg}, std::invalid_argument);
  cfg.window = 1024;
  cfg.first_sequence = 0;
  EXPECT_THROW(Sequencer{cfg}, std::invalid_argument);
}

}  // namespace
