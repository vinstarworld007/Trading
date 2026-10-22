#pragma once

// The receiving half of a MoldUDP64 feed, shared by itch-fh and the firm's
// trading engine. An rx thread owned by FeedReceiver makes every syscall: it
// receives packets from the feed and from the re-request socket, timestamps
// them with rdtsc and pushes them to a lock-free ring, and sends the
// retransmission requests the consumer asks for. The consumer thread calls
// poll(), which sequences packets (gaps, duplicates, reordering) and delivers
// messages in order with no syscalls, locks or allocation.
//
//   FeedReceiver rx{cfg};                    // opens sockets, allocates rings
//   rx.start();
//   while (!rx.finished()) rx.poll([&](std::uint64_t seq, auto msg, std::uint64_t rx_tsc) {...});
//   rx.stop();

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <type_traits>

#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/core/spsc_ring.hpp"
#include "lab/mold/mold.hpp"
#include "lab/mold/sequencer.hpp"
#include "lab/net/udp.hpp"

namespace lab::feed {

// One received datagram on its way from the rx thread to the consumer.
struct alignas(64) RxPacket {
  static constexpr std::size_t kMaxLen = 1472;  // UDP payload in a 1500-byte MTU
  std::uint64_t rx_tsc;
  std::uint32_t len;
  std::uint32_t reserved;
  std::byte data[kMaxLen];
};
static_assert(sizeof(RxPacket) == 1536);
static_assert(std::is_trivially_copyable_v<RxPacket>);

struct ReceiverConfig {
  std::string group = "239.1.1.1:30001";
  std::string iface = "127.0.0.1";
  std::string retrans = "127.0.0.1:30002";
  int rx_cpu = -1;
  std::uint64_t gap_delay_us = 100;  // allowance for reordering before the first request
  std::uint64_t timeout_ms = 20;     // re-request when a request makes no progress
  std::uint32_t retries = 10;        // requests without progress before a gap is given up
  std::size_t window = 1u << 18;     // messages held ahead of a gap
  // 0: the receive thread spins (the hot path). Otherwise it sleeps this long
  // when nothing arrived, for processes off the hot path sharing few cores.
  std::uint64_t idle_sleep_us = 0;
};

struct RxStats {
  std::uint64_t packets = 0;  // feed and retransmission
  std::uint64_t retrans_packets = 0;
  std::uint64_t ring_full = 0;  // consumer behind: dropped, recovered like any lost packet
  std::uint64_t oversize = 0;
  std::uint64_t requests_sent = 0;
  std::uint64_t request_errors = 0;
  std::uint64_t request_ring_full = 0;
};

class FeedReceiver {
 public:
  // Opens sockets and calibrates the TSC. Throws on socket errors.
  explicit FeedReceiver(const ReceiverConfig& cfg);
  ~FeedReceiver();
  FeedReceiver(const FeedReceiver&) = delete;
  FeedReceiver& operator=(const FeedReceiver&) = delete;

  void start();
  void stop();

  // Consumer thread. Handles at most one packet and any due retransmission
  // request; calls deliver(std::uint64_t seq, std::span<const std::byte> msg,
  // std::uint64_t rx_tsc) for each message that becomes deliverable. Returns
  // true if a packet was processed.
  template <class Deliver>
  LAB_ALWAYS_INLINE bool poll(Deliver&& deliver) {
    bool got = false;
    if (const RxPacket* p = shared_->packets.front()) {
      const std::uint64_t tsc = p->rx_tsc;
      seq_->on_packet(p->data, p->len, rdtsc(),
                      [&](std::uint64_t s, std::span<const std::byte> m) { deliver(s, m, tsc); });
      shared_->packets.pop();
      got = true;
    }
    mold::Request req;
    if (LAB_UNLIKELY(seq_->in_gap()) &&
        seq_->poll(
            rdtsc(), req,
            [&](std::uint64_t s, std::span<const std::byte> m) { deliver(s, m, rdtsc()); }) &&
        !shared_->requests.try_push(req)) {
      ++request_ring_full_;
    }
    return got;
  }

  // End of Session seen and every message before it delivered or given up.
  bool finished() const noexcept { return seq_->finished(); }
  // Packets received so far; any thread (for idle detection).
  std::uint64_t packets_received() const noexcept {
    return shared_->rx_packets.load(std::memory_order_relaxed);
  }
  const mold::Sequencer& sequencer() const noexcept { return *seq_; }
  const TscClock& clock() const noexcept { return clock_; }
  // Read after stop().
  RxStats rx_stats() const noexcept;
  std::string describe() const;
  // Session, packets, gaps, requests and overrun lines. After stop().
  void print_report(std::FILE* out) const;

 private:
  using PacketRing = SpscRing<RxPacket, 4096>;
  using RequestRing = SpscRing<mold::Request, 64>;
  struct Shared {
    PacketRing packets;
    RequestRing requests;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> rx_packets{0};
    RxStats stats;  // written by the rx thread, read after join
  };

  void rx_loop();

  ReceiverConfig cfg_;
  TscClock clock_;
  std::unique_ptr<Shared> shared_;
  std::unique_ptr<mold::Sequencer> seq_;
  net::UdpSocket feed_;
  net::UdpSocket rq_;
  net::Endpoint group_;
  net::Endpoint retrans_;
  std::thread rx_;
  std::uint64_t request_ring_full_ = 0;
};

}  // namespace lab::feed
