#pragma once

// Receiver-side MoldUDP64 sequencing: turns packets that may arrive lost,
// duplicated or out of order into messages delivered exactly once, in
// sequence order, and decides when to ask the re-request server for a gap.
//
//   Sequencer seq{cfg};
//   seq.on_packet(buf, len, now, deliver);     // deliver(seq, span) in order
//   if (Request r; seq.poll(now, r, deliver)) send r to the re-request server
//
// Messages that arrive ahead of a gap wait in a preallocated window and are
// delivered as soon as the gap fills. A gap that is still open after
// `max_retries` requests is given up: its messages are counted as lost and
// delivery resumes after it. Nothing here allocates, locks or makes a syscall
// after construction; `now` is whatever clock the caller uses (rdtsc ticks on
// the feed handler), and the timeouts are in the same units.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "lab/core/compiler.hpp"
#include "lab/mold/mold.hpp"

namespace lab::mold {

struct SequencerConfig {
  std::uint64_t first_sequence = 1;   // MoldUDP64 sessions start at 1
  std::size_t window = 1u << 16;      // messages held ahead of a gap; power of two
  std::size_t max_message = 64;       // bytes per held message (ITCH messages are at most 50)
  std::uint64_t gap_delay = 0;        // wait this long before the first request, for reordering
  std::uint64_t request_timeout = 0;  // re-request if a request gets no progress for this long
  std::uint32_t max_retries = 5;      // requests without progress before giving a gap up
  std::uint16_t max_request = 1024;   // messages asked for in one request
};

struct SequencerStats {
  std::uint64_t packets = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t delivered = 0;        // messages handed to the caller
  std::uint64_t duplicates = 0;       // messages already delivered or already held
  std::uint64_t held = 0;             // messages that waited for a gap to fill
  std::uint64_t gaps = 0;             // gaps opened
  std::uint64_t gaps_filled = 0;      // gaps closed without losing anything
  std::uint64_t gaps_given_up = 0;    // gaps closed by skipping lost messages
  std::uint64_t lost = 0;             // messages skipped when giving a gap up
  std::uint64_t requests = 0;         // retransmission requests asked for
  std::uint64_t window_overflow = 0;  // messages too far ahead of a gap to hold
  std::uint64_t oversize = 0;         // messages too big to hold
  std::uint64_t malformed = 0;        // truncated packets
  std::uint64_t wrong_session = 0;    // packets from another session
};

enum class PacketResult { kOk, kMalformed, kWrongSession };

class Sequencer {
 public:
  explicit Sequencer(const SequencerConfig& cfg = {})
      : cfg_{cfg},
        mask_{cfg.window - 1},
        expected_{cfg.first_sequence},
        high_{cfg.first_sequence},
        seqs_(cfg.window, 0),
        lens_(cfg.window, 0),
        arena_(cfg.window * cfg.max_message) {
    if (cfg.window < 2 || (cfg.window & (cfg.window - 1)) != 0) {
      throw std::invalid_argument("sequencer window must be a power of two");
    }
    if (cfg.first_sequence == 0) throw std::invalid_argument("sequence numbers start at 1");
  }

  // Processes one received datagram. `deliver(std::uint64_t seq,
  // std::span<const std::byte>)` is called for every message that becomes
  // deliverable, in order.
  template <typename Deliver>
  PacketResult on_packet(const std::byte* data, std::size_t len, std::uint64_t now,
                         Deliver&& deliver) {
    Packet p;
    if (LAB_UNLIKELY(!parse(data, len, p))) {
      ++stats_.malformed;
      return PacketResult::kMalformed;
    }
    if (LAB_UNLIKELY(!session_known_)) {
      session_ = p.header.session;
      session_known_ = true;
    } else if (LAB_UNLIKELY(p.header.session != session_)) {
      ++stats_.wrong_session;
      return PacketResult::kWrongSession;
    }
    ++stats_.packets;

    PacketResult result = PacketResult::kOk;
    if (p.header.messages() == 0) {
      // Heartbeat or End of Session: sequence is the next one the sender will use.
      ++stats_.heartbeats;
      if (p.header.end_of_session()) {
        eos_seen_ = true;
        eos_sequence_ = p.header.sequence;
      }
      if (p.header.sequence > high_) high_ = p.header.sequence;
    } else {
      const bool complete = p.for_each([&](std::uint64_t seq, std::span<const std::byte> msg) {
        if (seq + 1 > high_) high_ = seq + 1;
        accept(seq, msg, deliver);
      });
      if (LAB_UNLIKELY(!complete)) {
        ++stats_.malformed;
        result = PacketResult::kMalformed;
      }
    }
    update_gap(now);
    return result;
  }

  // Call regularly, including when no packets arrive. Returns true with `out`
  // filled when a retransmission request should be sent now. May deliver held
  // messages when it gives a gap up.
  template <typename Deliver>
  bool poll(std::uint64_t now, Request& out, Deliver&& deliver) {
    if (!in_gap_) return false;
    const bool progress = requested_ && expected_ > requested_from_;
    if (progress) retries_ = 0;
    const bool due = progress || (!requested_ && now - gap_opened_ >= cfg_.gap_delay) ||
                     (requested_ && now - last_request_ >= cfg_.request_timeout);
    if (!due) return false;

    if (retries_ >= cfg_.max_retries) {
      give_up(deliver);
      update_gap(now);
      return false;
    }

    // Ask for the first run of missing messages, up to the next one held.
    const std::uint64_t limit = std::min<std::uint64_t>(high_, expected_ + cfg_.max_request);
    std::uint64_t end = expected_ + 1;
    while (end < limit && !held(end)) ++end;
    out.session = session_;
    out.sequence = expected_;
    out.count = static_cast<std::uint16_t>(end - expected_);

    ++retries_;
    ++stats_.requests;
    requested_ = true;
    requested_from_ = expected_;
    last_request_ = now;
    return true;
  }

  // End of Session seen and every message before it delivered or given up.
  bool finished() const noexcept { return eos_seen_ && expected_ >= eos_sequence_; }
  bool end_of_session_seen() const noexcept { return eos_seen_; }
  bool in_gap() const noexcept { return in_gap_; }
  std::uint64_t expected() const noexcept { return expected_; }
  // One past the highest sequence number known to exist.
  std::uint64_t high_water() const noexcept { return high_; }
  const Session& session() const noexcept { return session_; }
  const SequencerStats& stats() const noexcept { return stats_; }

 private:
  bool held(std::uint64_t seq) const noexcept { return seqs_[seq & mask_] == seq; }

  template <typename Deliver>
  LAB_ALWAYS_INLINE void accept(std::uint64_t seq, std::span<const std::byte> msg,
                                Deliver& deliver) {
    if (LAB_LIKELY(seq == expected_)) {
      deliver(seq, msg);
      ++stats_.delivered;
      ++expected_;
      if (LAB_UNLIKELY(held_count_ != 0)) drain(deliver);
      return;
    }
    if (seq < expected_) {
      ++stats_.duplicates;
      return;
    }
    hold(seq, msg);
  }

  void hold(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
    if (seq - expected_ >= cfg_.window) {
      ++stats_.window_overflow;  // asked for again once the gap ahead of it fills
      return;
    }
    if (msg.size() > cfg_.max_message) {
      ++stats_.oversize;
      return;
    }
    const std::size_t i = seq & mask_;
    if (seqs_[i] == seq) {
      ++stats_.duplicates;
      return;
    }
    seqs_[i] = seq;
    lens_[i] = static_cast<std::uint16_t>(msg.size());
    std::memcpy(arena_.data() + i * cfg_.max_message, msg.data(), msg.size());
    ++held_count_;
    ++stats_.held;
  }

  template <typename Deliver>
  void drain(Deliver& deliver) {
    while (held_count_ != 0 && held(expected_)) {
      const std::size_t i = expected_ & mask_;
      deliver(expected_,
              std::span<const std::byte>{arena_.data() + i * cfg_.max_message, lens_[i]});
      seqs_[i] = 0;
      --held_count_;
      ++stats_.delivered;
      ++expected_;
    }
  }

  // Skips the missing run at expected_ up to the next held message, or to
  // the high-water mark if nothing is held.
  template <typename Deliver>
  void give_up(Deliver& deliver) {
    std::uint64_t next = expected_ + 1;
    const std::uint64_t limit = std::min<std::uint64_t>(high_, expected_ + cfg_.window);
    while (next < limit && !held(next)) ++next;
    stats_.lost += next - expected_;
    ++stats_.gaps_given_up;
    expected_ = next;
    in_gap_ = false;  // a remaining gap reopens fresh in update_gap()
    drain(deliver);
  }

  void update_gap(std::uint64_t now) noexcept {
    const bool gap = expected_ < high_;
    if (gap && !in_gap_) {
      in_gap_ = true;
      ++stats_.gaps;
      gap_opened_ = now;
      requested_ = false;
      retries_ = 0;
    } else if (!gap && in_gap_) {
      in_gap_ = false;
      ++stats_.gaps_filled;
    }
  }

  SequencerConfig cfg_;
  std::uint64_t mask_;
  std::uint64_t expected_;  // next sequence number to deliver
  std::uint64_t high_;      // one past the highest sequence number seen
  Session session_{};
  bool session_known_ = false;
  bool eos_seen_ = false;
  std::uint64_t eos_sequence_ = 0;

  bool in_gap_ = false;
  bool requested_ = false;
  std::uint64_t gap_opened_ = 0;
  std::uint64_t last_request_ = 0;
  std::uint64_t requested_from_ = 0;
  std::uint32_t retries_ = 0;

  std::size_t held_count_ = 0;
  std::vector<std::uint64_t> seqs_;  // sequence held in each slot, 0 when empty
  std::vector<std::uint16_t> lens_;
  std::vector<std::byte> arena_;  // window * max_message bytes
  SequencerStats stats_;
};

}  // namespace lab::mold
