#pragma once

// A FIX 4.4 session over one TCP connection at a time, for either side
// (docs/specs/fix44-subset.md, "Session rules"): logon, sequence numbers that
// persist across reconnects, gap detection with ResendRequest, resending
// stored application messages with PossDupFlag, gap fills over session
// messages, heartbeats and test requests, logout.
//
// Single-threaded and poll driven, like the rest of the lab's session code:
// call poll() from the I/O thread; it delivers application messages to a
// handler with these members:
//
//   void on_app(const fix::Message& m);   // in sequence, exactly once
//   void on_logon();                      // the session became active
//   void on_disconnect();                 // the connection is gone
//
// Sending an application message:
//
//   fix::Writer& w = session.begin(fix::msg_type::kExecutionReport);
//   w.add(fix::tag::kOrderID, ...);
//   session.send();
//
// The resend store and buffers are allocated once, up front.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/fix/codec.hpp"
#include "lab/net/tcp.hpp"

namespace lab::fix {

struct SessionConfig {
  std::string sender;              // our CompID (49 on what we send)
  std::string target;              // theirs (56 on what we send)
  std::uint32_t heartbeat_s = 30;  // initiator proposes; the acceptor echoes it
  std::string username;            // initiator: 553/554 on Logon, if set
  std::string password;
  std::size_t store_bytes = 32u << 20;    // application messages kept for resend
  std::size_t store_messages = 1u << 20;  // beyond either limit, gap fills instead
  std::uint64_t logon_timeout_ns = 10'000'000'000;
  std::uint64_t logout_timeout_ns = 2'000'000'000;
};

struct SessionStats {
  std::uint64_t received = 0;  // messages in sequence
  std::uint64_t sent = 0;      // messages with a new sequence number
  std::uint64_t resent = 0;    // application messages resent with PossDupFlag
  std::uint64_t gap_fills_sent = 0;
  std::uint64_t resend_requests_sent = 0;
  std::uint64_t resend_requests_received = 0;
  std::uint64_t test_requests_sent = 0;
  std::uint64_t rejects_sent = 0;
  std::uint64_t rejects_received = 0;
  std::uint64_t duplicates_ignored = 0;  // PossDupFlag below the expected number
  std::uint64_t garbled = 0;             // bad CheckSum or BodyLength, or unparsable
  std::uint64_t logons = 0;
  std::uint64_t not_stored = 0;  // application messages the store had no room for
};

class Session {
 public:
  enum class State { kDisconnected, kLogonSent, kActive, kLogoutSent };

  explicit Session(SessionConfig cfg) : cfg_{std::move(cfg)}, hb_s_{cfg_.heartbeat_s} {
    store_.reserve(cfg_.store_messages);
    arena_.reserve(cfg_.store_bytes);
  }

  // ---- Connecting --------------------------------------------------------

  // Initiator: takes a connected socket and sends Logon. With `reset`, both
  // sides restart at sequence number 1 (ResetSeqNumFlag).
  void connect(net::TcpStream sock, std::uint64_t now_ns, bool reset) {
    attach(std::move(sock), std::make_unique<StreamParser>(), now_ns);
    if (reset) reset_sequences();
    Writer& w = begin(msg_type::kLogon);
    w.add(tag::kEncryptMethod, '0');
    w.add_uint(tag::kHeartBtInt, hb_s_);
    if (reset) w.add(tag::kResetSeqNumFlag, 'Y');
    if (!cfg_.username.empty()) w.add(tag::kUsername, cfg_.username);
    if (!cfg_.password.empty()) w.add(tag::kPassword, cfg_.password);
    send();
    state_ = State::kLogonSent;
  }

  // Acceptor: `logon` is the first message read from `sock` (by `in`, which
  // keeps whatever arrived after it) and names this session. The caller has
  // already checked CompIDs and credentials. Answers with a Logon, or with a
  // Logout if the Logon's sequence number is too low. Returns true if the
  // session is active.
  template <class H>
  bool accept(net::TcpStream sock, std::unique_ptr<StreamParser> in, const Message& logon,
              std::uint64_t now_ns, H& h) {
    attach(std::move(sock), std::move(in), now_ns);
    const auto seq = logon.get_uint(tag::kMsgSeqNum);
    const auto hb = logon.get_uint(tag::kHeartBtInt);
    if (!seq || !hb || *hb == 0 || *hb > 3600 || logon.get(tag::kEncryptMethod) != "0") {
      logout_and_close("Logon needs MsgSeqNum, EncryptMethod 0 and HeartBtInt 1..3600", h);
      return false;
    }
    const bool reset = logon.flag(tag::kResetSeqNumFlag);
    if (reset) {
      reset_sequences();
    } else if (*seq < next_in_) {
      logout_and_close(too_low(*seq), h);
      return false;
    }
    hb_s_ = static_cast<std::uint32_t>(*hb);
    Writer& w = begin(msg_type::kLogon);
    w.add(tag::kEncryptMethod, '0');
    w.add_uint(tag::kHeartBtInt, hb_s_);
    if (reset) w.add(tag::kResetSeqNumFlag, 'Y');
    send();
    state_ = State::kActive;
    ++stats_.logons;
    after_logon_sequence(*seq);
    h.on_logon();
    return state_ == State::kActive;
  }

  // ---- Running -----------------------------------------------------------

  // Reads what has arrived, handles it, and runs the timers. Cheap when
  // there is nothing to do.
  template <class H>
  void poll(std::uint64_t now_ns, H& h) {
    if (state_ == State::kDisconnected) return;
    now_ns_ = now_ns;
    for (;;) {
      const auto area = in_->write_area();
      const auto n = sock_.recv(std::as_writable_bytes(area));
      if (n < 0) {
        close(h);
        return;
      }
      if (n == 0) break;
      in_->commit(static_cast<std::size_t>(n));
      last_rx_ns_ = now_ns;
      while (auto wire = in_->next()) {
        Message m;
        if (!m.parse(*wire)) {
          ++garbled_parse_;
          continue;
        }
        handle(m, h);
        if (state_ == State::kDisconnected) return;
      }
      stats_.garbled = in_->garbled() + garbled_parse_;
      if (in_->stuck()) {
        close(h);
        return;
      }
    }
    timers(now_ns, h);
    if (state_ != State::kDisconnected && !sock_.flush()) close(h);
  }

  // Starts an application (or session) message with the standard header.
  Writer& begin(char type) {
    w_.begin(kBeginString, type);
    w_.add(tag::kSenderCompID, cfg_.sender);
    w_.add(tag::kTargetCompID, cfg_.target);
    w_.add_uint(tag::kMsgSeqNum, next_out_);
    format_utc_timestamp(sending_time_.data(), wall_ns());
    w_.add(tag::kSendingTime, std::string_view{sending_time_.data(), kTimestampSize});
    body_mark_ = w_.mark();
    type_ = type;
    return w_;
  }

  // Finishes the message begun with begin(), stores it for resend if it is
  // an application message, and sends it. False if it did not fit.
  bool send() {
    const std::string_view body = w_.written_since(body_mark_);
    const std::string_view wire = w_.finish();
    if (wire.empty()) return false;
    Stored s{};
    s.type = type_;
    if (!msg_type::is_session(type_)) {
      if (store_.size() < cfg_.store_messages && arena_.size() + body.size() <= cfg_.store_bytes) {
        s.offset = arena_.size();
        s.len = static_cast<std::uint32_t>(body.size());
        s.kept = true;
        std::copy(sending_time_.begin(), sending_time_.end(), s.time.begin());
        arena_.insert(arena_.end(), body.begin(), body.end());
      } else {
        ++stats_.not_stored;
      }
    }
    if (store_.size() < cfg_.store_messages) store_.push_back(s);
    ++next_out_;
    ++stats_.sent;
    write(wire);
    return true;
  }

  // Sends Logout and waits (in poll) for the answer, then disconnects.
  void logout(std::string_view text, std::uint64_t now_ns) {
    if (state_ != State::kActive && state_ != State::kLogonSent) return;
    Writer& w = begin(msg_type::kLogout);
    if (!text.empty()) w.add(tag::kText, text);
    send();
    state_ = State::kLogoutSent;
    logout_sent_ns_ = now_ns;
  }

  // Drops the connection without a Logout. Sequence numbers are kept.
  template <class H>
  void close(H& h) {
    if (state_ == State::kDisconnected) return;
    for (int i = 0; i < 100 && sock_.queued() > 0 && sock_.flush(); ++i) {
    }
    sock_.close();
    state_ = State::kDisconnected;
    h.on_disconnect();
  }

  State state() const noexcept { return state_; }
  bool active() const noexcept { return state_ == State::kActive; }
  std::uint64_t next_out_seq() const noexcept { return next_out_; }
  std::uint64_t next_in_seq() const noexcept { return next_in_; }
  std::uint32_t heartbeat_s() const noexcept { return hb_s_; }
  const SessionConfig& config() const noexcept { return cfg_; }
  const SessionStats& stats() const noexcept { return stats_; }
  bool flush() { return sock_.flush(); }

  // For tests: skip sequence numbers, as a peer that lost messages would.
  void skip_outbound(std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i) store_.push_back(Stored{});
    next_out_ += n;
  }

 private:
  struct Stored {
    std::size_t offset = 0;
    std::uint32_t len = 0;
    char type = 0;
    bool kept = false;  // false: a session message, or no room: gap fill it
    std::array<char, kTimestampSize> time{};
  };

  void attach(net::TcpStream sock, std::unique_ptr<StreamParser> in, std::uint64_t now_ns) {
    sock_ = std::move(sock);
    in_ = std::move(in);
    garbled_parse_ = 0;
    now_ns_ = connected_ns_ = last_rx_ns_ = last_tx_ns_ = now_ns;
    test_pending_ = false;
    resend_pending_ = false;
  }

  void reset_sequences() {
    next_in_ = 1;
    next_out_ = 1;
    store_.clear();
    arena_.clear();
  }

  void write(std::string_view wire) {
    sock_.send(std::as_bytes(std::span{wire.data(), wire.size()}));
    last_tx_ns_ = now_ns_;
  }

  std::string_view too_low(std::uint64_t seq) {
    const int n = std::snprintf(
        text_.data(), text_.size(), "MsgSeqNum too low, expecting %llu but received %llu",
        static_cast<unsigned long long>(next_in_), static_cast<unsigned long long>(seq));
    return {text_.data(), static_cast<std::size_t>(n)};
  }

  template <class H>
  void logout_and_close(std::string_view text, H& h) {
    Writer& w = begin(msg_type::kLogout);
    if (!text.empty()) w.add(tag::kText, text);
    send();
    close(h);
  }

  // After a Logon with sequence number `seq` is accepted.
  void after_logon_sequence(std::uint64_t seq) {
    if (seq > next_in_) {
      request_resend(seq);
    } else {
      next_in_ = seq + 1;
    }
  }

  void request_resend(std::uint64_t seen) {
    if (resend_pending_) return;
    resend_pending_ = true;
    resend_until_ = seen;
    Writer& w = begin(msg_type::kResendRequest);
    w.add_uint(tag::kBeginSeqNo, next_in_);
    w.add_uint(tag::kEndSeqNo, 0);
    send();
    ++stats_.resend_requests_sent;
  }

  void reject(std::uint64_t ref_seq, int ref_tag, char ref_type, int reason,
              std::string_view text) {
    Writer& w = begin(msg_type::kReject);
    w.add_uint(tag::kRefSeqNum, ref_seq);
    if (ref_tag != 0) w.add_uint(tag::kRefTagID, static_cast<std::uint64_t>(ref_tag));
    if (ref_type != 0) w.add(tag::kRefMsgType, ref_type);
    w.add_uint(tag::kSessionRejectReason, static_cast<std::uint64_t>(reason));
    if (!text.empty()) w.add(tag::kText, text);
    send();
    ++stats_.rejects_sent;
  }

  template <class H>
  void handle(const Message& m, H& h) {
    if (m.get(tag::kBeginString) != kBeginString) {
      logout_and_close("BeginString must be FIX.4.4", h);
      return;
    }
    const auto seq = m.get_uint(tag::kMsgSeqNum);
    if (!seq) {
      logout_and_close("MsgSeqNum missing", h);
      return;
    }
    if (m.get(tag::kSenderCompID) != cfg_.target || m.get(tag::kTargetCompID) != cfg_.sender) {
      reject(*seq, tag::kSenderCompID, m.type(), 9, "CompID problem");
      logout_and_close("CompID problem", h);
      return;
    }
    if (state_ == State::kLogonSent && m.type() != msg_type::kLogon) {
      logout_and_close("expected Logon", h);
      return;
    }
    // SequenceReset in reset mode moves the expected number regardless of
    // its own MsgSeqNum.
    if (m.type() == msg_type::kSequenceReset && !m.flag(tag::kGapFillFlag)) {
      const auto to = m.get_uint(tag::kNewSeqNo);
      if (!to || *to < next_in_) {
        reject(*seq, tag::kNewSeqNo, m.type(), 5, "NewSeqNo may not go backwards");
        return;
      }
      next_in_ = *to;
      resend_pending_ = false;
      return;
    }
    if (*seq > next_in_) {
      if (m.type() == msg_type::kLogon && state_ == State::kLogonSent) {
        on_logon_answer(*seq, false, h);
        return;
      }
      if (m.type() == msg_type::kLogout) {
        on_logout(h);
        return;
      }
      request_resend(*seq);
      return;
    }
    if (*seq < next_in_) {
      if (m.flag(tag::kPossDupFlag)) {
        ++stats_.duplicates_ignored;
        return;
      }
      logout_and_close(too_low(*seq), h);
      return;
    }
    ++next_in_;
    ++stats_.received;
    if (resend_pending_ && next_in_ > resend_until_) resend_pending_ = false;
    switch (m.type()) {
      case msg_type::kLogon:
        if (state_ == State::kLogonSent) {
          on_logon_answer(*seq, true, h);
        } else {
          logout_and_close("unexpected Logon", h);
        }
        break;
      case msg_type::kHeartbeat:
        if (test_pending_ && m.get(tag::kTestReqID) == test_id()) test_pending_ = false;
        break;
      case msg_type::kTestRequest: {
        Writer& w = begin(msg_type::kHeartbeat);
        if (const auto id = m.get(tag::kTestReqID)) w.add(tag::kTestReqID, *id);
        send();
        break;
      }
      case msg_type::kResendRequest: {
        const auto from = m.get_uint(tag::kBeginSeqNo);
        const auto to = m.get_uint(tag::kEndSeqNo);
        if (!from || !to) {
          reject(*seq, from ? tag::kEndSeqNo : tag::kBeginSeqNo, m.type(), 1, "");
          break;
        }
        resend(*from, *to);
        break;
      }
      case msg_type::kReject:
        ++stats_.rejects_received;
        break;
      case msg_type::kSequenceReset: {  // gap fill
        const auto to = m.get_uint(tag::kNewSeqNo);
        if (!to) {
          reject(*seq, tag::kNewSeqNo, m.type(), 1, "");
        } else if (*to > next_in_) {
          next_in_ = *to;
          if (resend_pending_ && next_in_ > resend_until_) resend_pending_ = false;
        }
        break;
      }
      case msg_type::kLogout:
        on_logout(h);
        break;
      default:
        if (state_ == State::kActive || state_ == State::kLogoutSent) h.on_app(m);
        break;
    }
  }

  // The acceptor's Logon. `in_sequence`: handle() has already counted it.
  template <class H>
  void on_logon_answer(std::uint64_t seq, bool in_sequence, H& h) {
    state_ = State::kActive;
    ++stats_.logons;
    if (!in_sequence) request_resend(seq);
    h.on_logon();
  }

  template <class H>
  void on_logout(H& h) {
    if (state_ != State::kLogoutSent) {
      begin(msg_type::kLogout);
      send();
    }
    close(h);
  }

  // Answers a ResendRequest: stored application messages again with
  // PossDupFlag, gap fills over everything else.
  void resend(std::uint64_t from, std::uint64_t to) {
    ++stats_.resend_requests_received;
    const std::uint64_t last = next_out_ - 1;
    if (to == 0 || to > last) to = last;
    if (from == 0) from = 1;
    std::uint64_t gap_start = 0;
    auto fill_gap = [&](std::uint64_t upto) {  // gap_start..upto-1
      if (gap_start == 0) return;
      std::array<char, 256> buf;
      Writer w{buf.data(), buf.size()};
      w.begin(kBeginString, msg_type::kSequenceReset);
      w.add(tag::kSenderCompID, cfg_.sender);
      w.add(tag::kTargetCompID, cfg_.target);
      w.add_uint(tag::kMsgSeqNum, gap_start);
      w.add(tag::kPossDupFlag, 'Y');
      w.add_time(tag::kSendingTime, wall_ns());
      w.add(tag::kGapFillFlag, 'Y');
      w.add_uint(tag::kNewSeqNo, upto);
      write(w.finish());
      ++stats_.gap_fills_sent;
      gap_start = 0;
    };
    for (std::uint64_t s = from; s <= to; ++s) {
      const Stored* st = s - 1 < store_.size() ? &store_[s - 1] : nullptr;
      if (st == nullptr || !st->kept) {
        if (gap_start == 0) gap_start = s;
        continue;
      }
      fill_gap(s);
      Writer w{resend_buf_.data(), resend_buf_.size()};
      w.begin(kBeginString, st->type);
      w.add(tag::kSenderCompID, cfg_.sender);
      w.add(tag::kTargetCompID, cfg_.target);
      w.add_uint(tag::kMsgSeqNum, s);
      w.add(tag::kPossDupFlag, 'Y');
      w.add_time(tag::kSendingTime, wall_ns());
      w.add(tag::kOrigSendingTime, std::string_view{st->time.data(), st->time.size()});
      w.add_raw(std::string_view{arena_.data() + st->offset, st->len});
      const auto wire = w.finish();
      if (!wire.empty()) {
        write(wire);
        ++stats_.resent;
      }
    }
    fill_gap(to + 1);
  }

  std::string_view test_id() const noexcept { return {test_id_.data(), test_id_len_}; }

  static std::uint64_t since(std::uint64_t now, std::uint64_t then) noexcept {
    return now > then ? now - then : 0;
  }

  template <class H>
  void timers(std::uint64_t now_ns, H& h) {
    const std::uint64_t hb_ns = static_cast<std::uint64_t>(hb_s_) * 1'000'000'000ULL;
    switch (state_) {
      case State::kLogonSent:
        if (since(now_ns, connected_ns_) > cfg_.logon_timeout_ns) close(h);
        return;
      case State::kLogoutSent:
        if (since(now_ns, logout_sent_ns_) > cfg_.logout_timeout_ns) close(h);
        return;
      case State::kActive:
        break;
      case State::kDisconnected:
        return;
    }
    if (since(now_ns, last_tx_ns_) >= hb_ns) {
      begin(msg_type::kHeartbeat);
      send();
    }
    if (test_pending_) {
      if (since(now_ns, test_sent_ns_) > hb_ns) close(h);  // no answer: the peer is gone
    } else if (since(now_ns, last_rx_ns_) > hb_ns + hb_ns / 5) {
      ++test_seq_;
      const auto r =
          std::to_chars(test_id_.data() + 4, test_id_.data() + test_id_.size(), test_seq_);
      std::memcpy(test_id_.data(), "TEST", 4);
      test_id_len_ = static_cast<std::size_t>(r.ptr - test_id_.data());
      Writer& w = begin(msg_type::kTestRequest);
      w.add(tag::kTestReqID, test_id());
      send();
      test_pending_ = true;
      test_sent_ns_ = now_ns;
      ++stats_.test_requests_sent;
    }
  }

  SessionConfig cfg_;
  std::uint32_t hb_s_;
  State state_ = State::kDisconnected;
  net::TcpStream sock_;
  std::unique_ptr<StreamParser> in_;
  std::uint64_t next_in_ = 1;
  std::uint64_t next_out_ = 1;
  std::vector<Stored> store_;  // index: MsgSeqNum - 1
  std::vector<char> arena_;
  std::array<char, 4096> out_buf_{};
  std::array<char, 4096> resend_buf_{};
  Writer w_{out_buf_.data(), out_buf_.size()};
  std::size_t body_mark_ = 0;
  char type_ = 0;
  std::array<char, kTimestampSize> sending_time_{};
  std::array<char, 128> text_{};
  std::uint64_t now_ns_ = 0;  // the time given to the last poll(), connect() or accept()
  std::uint64_t connected_ns_ = 0;
  std::uint64_t last_rx_ns_ = 0;
  std::uint64_t last_tx_ns_ = 0;
  std::uint64_t logout_sent_ns_ = 0;
  bool resend_pending_ = false;
  std::uint64_t resend_until_ = 0;
  bool test_pending_ = false;
  std::uint64_t test_sent_ns_ = 0;
  std::uint64_t test_seq_ = 0;
  std::array<char, 24> test_id_{};
  std::size_t test_id_len_ = 0;
  std::uint64_t garbled_parse_ = 0;
  SessionStats stats_;
};

}  // namespace lab::fix
