#include "lab/feed/publisher.hpp"

#include <algorithm>
#include <array>

#include "lab/core/clock.hpp"

namespace lab::feed {
namespace {

std::size_t pow2_floor(std::size_t v) {
  std::size_t p = 1;
  while (p * 2 <= v) p *= 2;
  return p;
}

}  // namespace

MoldPublisher::MoldPublisher(const PublisherConfig& cfg)
    : cfg_{cfg},
      session_{mold::make_session(cfg.session)},
      dest_{net::parse_endpoint(cfg.dest)},
      tx_{net::UdpSocket::sender(dest_, cfg.iface)},
      rq_{net::UdpSocket::bound(
          net::parse_endpoint(cfg.iface + ":" + std::to_string(cfg.retrans_port)))},
      // ~16 bytes of index per stored message; ITCH averages about 30 bytes.
      store_{std::max<std::size_t>(cfg.store_mb, 1) << 20,
             pow2_floor((std::max<std::size_t>(cfg.store_mb, 1) << 20) / 16)},
      faults_{cfg.faults},
      builder_{cfg.payload},
      rbuilder_{cfg.payload},
      held_(kMaxPayload) {
  builder_.begin(session_, store_.next_sequence());
}

void MoldPublisher::add(std::span<const std::byte> msg, std::uint64_t now) {
  if (!builder_.empty() && now - opened_ns_ > cfg_.max_delay_us * 1000) flush(now);
  if (!builder_.add(msg)) {
    flush(now);
    builder_.add(msg);
  }
  if (builder_.count() == 1) opened_ns_ = now;
  store_.append(msg);
  ++stats_.messages;
}

void MoldPublisher::flush(std::uint64_t now) {
  if (builder_.empty()) return;
  const auto pkt = builder_.finish();
  sent_next_ = builder_.next_sequence();
  switch (faults_.next()) {
    case mold::Fault::kDrop:
      break;
    case mold::Fault::kReorder:
      if (!holding_) {
        std::copy(pkt.begin(), pkt.end(), held_.begin());
        held_len_ = pkt.size();
        holding_ = true;
        break;
      }
      [[fallthrough]];
    case mold::Fault::kNone:
      send(pkt);
      release_held();
      break;
    case mold::Fault::kDuplicate:
      send(pkt);
      send(pkt);
      release_held();
      break;
  }
  last_send_ns_ = now;
  builder_.begin(session_, store_.next_sequence());
}

void MoldPublisher::service(std::uint64_t now) {
  serve_requests();
  if (now - opened_ns_ > cfg_.max_delay_us * 1000) flush(now);
  if (now - last_send_ns_ >= cfg_.heartbeat_ms * 1'000'000) {
    mold::PacketBuilder<mold::kHeaderSize + 2> hb;
    hb.heartbeat(session_, sent_next_);
    send(hb.finish());
    ++stats_.heartbeats;
    last_send_ns_ = now;
  }
}

void MoldPublisher::finish(double eos_seconds) {
  const std::uint64_t now = mono_ns();
  flush(now);
  release_held();
  const std::uint64_t end = now + static_cast<std::uint64_t>(eos_seconds * 1e9);
  std::uint64_t next_eos = 0;
  for (std::uint64_t t = now; t < end; t = mono_ns()) {
    if (t >= next_eos) {
      mold::PacketBuilder<mold::kHeaderSize + 2> eos;
      eos.end_of_session(session_, sent_next_);
      send(eos.finish());
      ++stats_.end_of_session;
      next_eos = t + 100'000'000;  // every 100 ms, so receivers finish quickly
    }
    serve_requests();
  }
}

std::string MoldPublisher::describe() const {
  return std::string{mold::trimmed(session_)} + " to " + dest_.str() + ", re-requests on " +
         rq_.local().str();
}

void MoldPublisher::print_report(std::FILE* out, double secs) const {
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  const auto& f = faults_.stats();
  std::fprintf(out, "Session      %s\n", describe().c_str());
  std::fprintf(out, "Messages     %llu in %llu packets (%.2f s, %.2f M msgs/s)\n",
               u(stats_.messages), u(f.packets), secs,
               secs > 0 ? static_cast<double>(stats_.messages) / secs / 1e6 : 0.0);
  std::fprintf(out, "Heartbeats   %llu, End of Session %llu, send errors %llu\n",
               u(stats_.heartbeats), u(stats_.end_of_session), u(stats_.send_errors));
  std::fprintf(out, "Faults       dropped %llu (%llu bursts), duplicated %llu, reordered %llu\n",
               u(f.dropped), u(f.bursts), u(f.duplicated), u(f.reordered));
  std::fprintf(out, "Re-requests  %llu served, %llu messages resent, %llu unanswerable\n",
               u(stats_.requests), u(stats_.resent), u(stats_.unanswerable));
}

void MoldPublisher::send(std::span<const std::byte> pkt) {
  if (tx_.send_to(pkt, dest_) < 0) ++stats_.send_errors;
}

void MoldPublisher::release_held() {
  if (!holding_) return;
  send({held_.data(), held_len_});
  holding_ = false;
}

void MoldPublisher::serve_requests() {
  std::array<std::byte, 64> buf;
  net::Endpoint from;
  for (;;) {
    const ssize_t n = rq_.recv_from(buf, &from);
    if (n < 0) return;
    mold::Request r;
    if (!mold::decode_request(buf.data(), static_cast<std::size_t>(n), r) ||
        r.session != session_ || r.sequence >= sent_next_ || r.count == 0) {
      ++stats_.unanswerable;
      continue;
    }
    const auto want =
        static_cast<std::uint16_t>(std::min<std::uint64_t>(r.count, sent_next_ - r.sequence));
    const std::uint16_t got = store_.fill(rbuilder_, session_, r.sequence, want);
    if (got == 0) {
      ++stats_.unanswerable;  // evicted from the store
      continue;
    }
    if (rq_.send_to(rbuilder_.finish(), from) < 0) ++stats_.send_errors;
    ++stats_.requests;
    stats_.resent += got;
  }
}

}  // namespace lab::feed
