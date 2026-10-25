#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/fix/session.hpp"

namespace {

namespace fix = lab::fix;
namespace net = lab::net;

struct Recorder {
  std::vector<std::string> app;  // ClOrdIDs of application messages, in delivery order
  int logons = 0;
  int disconnects = 0;
  void on_app(const fix::Message& m) { app.emplace_back(m.get(fix::tag::kClOrdID).value_or("")); }
  void on_logon() { ++logons; }
  void on_disconnect() { ++disconnects; }
};

fix::SessionConfig config(std::string sender, std::string target) {
  fix::SessionConfig c;
  c.sender = std::move(sender);
  c.target = std::move(target);
  c.heartbeat_s = 30;
  c.store_bytes = 1u << 20;
  c.store_messages = 1u << 12;
  return c;
}

// An initiator ("FIRM") and an acceptor ("VENUE") over loopback TCP. The
// acceptor side reads the first message itself, as a gateway would.
struct Pair {
  net::TcpListener listener = net::TcpListener::listen(net::parse_endpoint("127.0.0.1:0"));
  fix::Session firm{config("FIRM", "VENUE")};
  fix::Session venue{config("VENUE", "FIRM")};
  Recorder f, v;
  std::uint64_t clock_offset = 0;  // lets a test move time forward

  std::uint64_t now() const { return lab::mono_ns() + clock_offset; }

  void pump(int rounds = 50) {
    for (int i = 0; i < rounds; ++i) {
      firm.poll(now(), f);
      venue.poll(now(), v);
    }
  }

  // Connects the firm and lets the venue accept its Logon. Returns whether
  // the venue went active.
  bool connect(bool reset) { return connect(firm, reset); }
  bool connect(fix::Session& initiator, bool reset) {
    initiator.connect(net::TcpStream::connect(listener.local()), now(), reset);
    std::optional<net::TcpStream> s;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!s && std::chrono::steady_clock::now() < until) s = listener.accept();
    if (!s) throw std::runtime_error("no connection");
    auto in = std::make_unique<fix::StreamParser>();
    while (std::chrono::steady_clock::now() < until) {
      initiator.flush();
      const auto area = in->write_area();
      const auto n = s->recv(std::as_writable_bytes(area));
      if (n > 0) in->commit(static_cast<std::size_t>(n));
      if (auto wire = in->next()) {
        fix::Message logon;
        if (!logon.parse(*wire) || logon.type() != fix::msg_type::kLogon) return false;
        const bool ok = venue.accept(std::move(*s), std::move(in), logon, now(), v);
        pump();
        return ok;
      }
    }
    return false;
  }

  void send_order(fix::Session& s, const std::string& id) {
    fix::Writer& w = s.begin(fix::msg_type::kNewOrderSingle);
    w.add(fix::tag::kClOrdID, id);
    w.add(fix::tag::kSymbol, "AAPL");
    s.send();
  }
};

TEST(FixSession, LogsOnAndDeliversInOrder) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  EXPECT_TRUE(p.firm.active());
  EXPECT_TRUE(p.venue.active());
  EXPECT_EQ(p.f.logons, 1);
  EXPECT_EQ(p.v.logons, 1);
  for (int i = 0; i < 5; ++i) p.send_order(p.firm, "o" + std::to_string(i));
  p.send_order(p.venue, "r0");
  p.pump();
  EXPECT_EQ(p.v.app, (std::vector<std::string>{"o0", "o1", "o2", "o3", "o4"}));
  EXPECT_EQ(p.f.app, (std::vector<std::string>{"r0"}));
  EXPECT_EQ(p.firm.next_out_seq(), 7u);  // Logon + 5
  EXPECT_EQ(p.venue.next_in_seq(), 7u);
}

TEST(FixSession, FillsAGapWithResendAndGapFill) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  p.firm.skip_outbound(3);  // three messages the venue never sees
  p.send_order(p.firm, "after-gap");
  p.pump();
  // The venue asks for 2.. and gets a gap fill for 2-4, then the order again.
  EXPECT_EQ(p.venue.stats().resend_requests_sent, 1u);
  EXPECT_EQ(p.firm.stats().gap_fills_sent, 1u);
  EXPECT_EQ(p.firm.stats().resent, 1u);
  EXPECT_EQ(p.v.app, (std::vector<std::string>{"after-gap"}));
  EXPECT_EQ(p.venue.next_in_seq(), p.firm.next_out_seq());
  p.send_order(p.firm, "next");
  p.pump();
  EXPECT_EQ(p.v.app, (std::vector<std::string>{"after-gap", "next"}));
}

TEST(FixSession, ResendsWhatWasMissedWhileDisconnected) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  p.firm.close(p.f);
  p.pump();
  EXPECT_FALSE(p.venue.active());
  p.send_order(p.venue, "missed-1");  // sequenced and stored, but nobody is connected
  p.send_order(p.venue, "missed-2");
  ASSERT_TRUE(p.connect(false));  // same day: sequence numbers carry on
  p.pump();
  EXPECT_EQ(p.f.app, (std::vector<std::string>{"missed-1", "missed-2"}));
  EXPECT_EQ(p.venue.stats().resent, 2u);
  EXPECT_EQ(p.firm.next_in_seq(), p.venue.next_out_seq());
  p.send_order(p.venue, "live");
  p.pump();
  EXPECT_EQ(p.f.app.back(), "live");
}

TEST(FixSession, LogsOutALogonWithASequenceNumberTooLow) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  p.send_order(p.firm, "a");
  p.pump();
  p.firm.close(p.f);
  p.pump();
  // A firm that lost its state reconnects at 1 without asking for a reset.
  fix::Session fresh{config("FIRM", "VENUE")};
  EXPECT_FALSE(p.connect(fresh, false));
  EXPECT_FALSE(p.venue.active());
}

TEST(FixSession, AnswersTestRequestsAndHeartbeats) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  // 40 s of silence from the firm's point of view: it heartbeats, then
  // tests the venue, which answers; nobody disconnects.
  p.clock_offset = 37'000'000'000ULL;
  p.pump();
  EXPECT_EQ(p.firm.stats().test_requests_sent, 1u);
  EXPECT_TRUE(p.firm.active());
  EXPECT_TRUE(p.venue.active());
  EXPECT_EQ(p.f.disconnects, 0);
}

TEST(FixSession, LogoutIsAnswered) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  p.firm.logout("done", p.now());
  p.pump();
  EXPECT_EQ(p.firm.state(), fix::Session::State::kDisconnected);
  EXPECT_EQ(p.venue.state(), fix::Session::State::kDisconnected);
  EXPECT_EQ(p.f.disconnects, 1);
  EXPECT_EQ(p.v.disconnects, 1);
}

TEST(FixSession, HoldsMessagesBehindAGapUntilTheResendFillsIt) {
  Pair p;
  ASSERT_TRUE(p.connect(true));
  p.firm.skip_outbound(2);
  p.send_order(p.firm, "after-gap");
  p.pump();
  // The order is processed once, in sequence, when it is resent: nothing is
  // taken out of order and then thrown away as a duplicate.
  EXPECT_EQ(p.venue.stats().duplicates_ignored, 0u);
  EXPECT_EQ(p.v.app, (std::vector<std::string>{"after-gap"}));
  EXPECT_EQ(p.venue.next_in_seq(), p.firm.next_out_seq());
}

}  // namespace
