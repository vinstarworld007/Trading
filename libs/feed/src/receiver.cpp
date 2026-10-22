#include "lab/feed/receiver.hpp"

#include <chrono>

#include "lab/core/affinity.hpp"

namespace lab::feed {

FeedReceiver::FeedReceiver(const ReceiverConfig& cfg)
    : cfg_{cfg},
      clock_{TscClock::calibrate(std::chrono::milliseconds{50})},
      shared_{std::make_unique<Shared>()} {
  mold::SequencerConfig sc;
  sc.gap_delay = clock_.to_ticks(static_cast<double>(cfg.gap_delay_us) * 1e3);
  sc.request_timeout = clock_.to_ticks(static_cast<double>(cfg.timeout_ms) * 1e6);
  sc.max_retries = cfg.retries;
  sc.window = cfg.window;
  seq_ = std::make_unique<mold::Sequencer>(sc);
  group_ = net::parse_endpoint(cfg.group);
  retrans_ = net::parse_endpoint(cfg.retrans);
  feed_ = net::UdpSocket::receiver(group_, cfg.iface);
  rq_ = net::UdpSocket::bound(net::parse_endpoint(cfg.iface + ":0"));
}

FeedReceiver::~FeedReceiver() { stop(); }

void FeedReceiver::start() {
  if (rx_.joinable()) return;
  shared_->stop.store(false, std::memory_order_relaxed);
  rx_ = std::thread{[this] { rx_loop(); }};
}

void FeedReceiver::stop() {
  shared_->stop.store(true, std::memory_order_relaxed);
  if (rx_.joinable()) rx_.join();
}

RxStats FeedReceiver::rx_stats() const noexcept {
  RxStats s = shared_->stats;
  s.request_ring_full = request_ring_full_;
  return s;
}

std::string FeedReceiver::describe() const {
  return group_.str() + ", re-requests to " + retrans_.str();
}

void FeedReceiver::rx_loop() {
  set_thread_name("feed-rx");
  if (cfg_.rx_cpu >= 0) {
    if (auto ec = pin_this_thread(cfg_.rx_cpu)) {
      std::fprintf(stderr, "feed-rx: cannot pin to cpu %d: %s\n", cfg_.rx_cpu,
                   ec.message().c_str());
    }
  }
  Shared& sh = *shared_;
  net::BatchReceiver batch{32, RxPacket::kMaxLen + 1};  // +1 spots oversize datagrams
  std::uint64_t received = 0;
  auto push = [&](std::span<const std::byte> d, std::uint64_t tsc) {
    if (d.size() > RxPacket::kMaxLen) {
      ++sh.stats.oversize;
      return;
    }
    RxPacket* slot = sh.packets.try_claim();
    if (slot == nullptr) {
      ++sh.stats.ring_full;
      return;
    }
    slot->rx_tsc = tsc;
    slot->len = static_cast<std::uint32_t>(d.size());
    std::memcpy(slot->data, d.data(), d.size());
    sh.packets.commit();
  };
  while (!sh.stop.load(std::memory_order_relaxed)) {
    int n = batch.receive(feed_);
    std::uint64_t tsc = rdtsc();
    for (int i = 0; i < n; ++i) push(batch.datagram(i), tsc);
    received += static_cast<std::uint64_t>(n);

    n = batch.receive(rq_);
    tsc = rdtsc();
    for (int i = 0; i < n; ++i) push(batch.datagram(i), tsc);
    sh.stats.retrans_packets += static_cast<std::uint64_t>(n);
    received += static_cast<std::uint64_t>(n);
    sh.rx_packets.store(received, std::memory_order_relaxed);

    mold::Request r;
    while (sh.requests.try_pop(r)) {
      std::array<std::byte, mold::kRequestSize> buf;
      mold::encode_request(buf.data(), r);
      if (rq_.send_to(buf, retrans_) < 0)
        ++sh.stats.request_errors;
      else
        ++sh.stats.requests_sent;
    }
  }
  sh.stats.packets = received;
}

void FeedReceiver::print_report(std::FILE* out) const {
  const auto& s = seq_->stats();
  const RxStats rx = rx_stats();
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  const auto session = mold::trimmed(seq_->session());
  std::fprintf(out, "Session      %.*s on %s, next sequence %llu\n",
               static_cast<int>(session.size()), session.data(), describe().c_str(),
               u(seq_->expected()));
  std::fprintf(out, "Packets      %llu (%llu retransmitted), %llu heartbeats or End of Session\n",
               u(s.packets), u(rx.retrans_packets), u(s.heartbeats));
  std::fprintf(out, "Messages     %llu delivered, %llu duplicates dropped, %llu held for a gap\n",
               u(s.delivered), u(s.duplicates), u(s.held));
  std::fprintf(out, "Gaps         %llu opened, %llu filled, %llu given up (%llu messages lost)\n",
               u(s.gaps), u(s.gaps_filled), u(s.gaps_given_up), u(s.lost));
  std::fprintf(out, "Requests     %llu sent, %llu send errors, %llu ring full\n",
               u(rx.requests_sent), u(rx.request_errors), u(rx.request_ring_full));
  std::fprintf(out, "Overruns     %llu packet ring full, %llu window overflow, %llu oversize\n",
               u(rx.ring_full), u(s.window_overflow), u(s.oversize + rx.oversize));
  std::fprintf(out, "Bad packets  %llu malformed, %llu wrong session\n", u(s.malformed),
               u(s.wrong_session));
}

}  // namespace lab::feed
