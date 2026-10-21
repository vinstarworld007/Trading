// itch-replay: publishes a Nasdaq ITCH 5.0 day file as a live MoldUDP64 feed,
// the way the exchange's market data publisher would, with a re-request
// server for retransmissions and optional fault injection on the wire.
//
//   itch-replay ~/data/itch/01302019.NASDAQ_ITCH50.gz                 1M msgs/s to 239.1.1.1:30001
//   itch-replay FILE --speed 1                                         original timing
//   itch-replay FILE --rate 0                                          as fast as possible
//   itch-replay FILE --drop 0.001 --burst 0.0001 --dup 0.001 --reorder 0.001 --seed 7
//
// Packets go to --dest (multicast or unicast); the re-request server listens
// on --iface:--retrans-port and answers each request with one packet, unicast.
// After the last message it sends End of Session packets for --eos-seconds,
// still answering requests, then prints what it sent.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/endian.hpp"
#include "lab/itch/file_reader.hpp"
#include "lab/mold/fault_injector.hpp"
#include "lab/mold/mold.hpp"
#include "lab/mold/retransmit_store.hpp"
#include "lab/net/udp.hpp"

namespace {

namespace mold = lab::mold;
namespace net = lab::net;

constexpr std::size_t kMaxPayload = 8972;  // jumbo-frame UDP payload; --payload picks the limit
using Builder = mold::PacketBuilder<kMaxPayload>;

struct Options {
  std::string path;
  std::string dest = "239.1.1.1:30001";
  std::string iface = "127.0.0.1";
  int retrans_port = 30002;
  std::string session = "LAB0000001";
  double rate = 1'000'000;  // messages per second; 0 = unlimited
  double speed = 0;         // > 0 paces by ITCH timestamps instead of --rate
  std::size_t payload = mold::kDefaultMaxPayload;
  std::uint64_t max_delay_us = 20;  // longest a message waits for its packet to fill
  std::uint64_t limit = 0;
  std::uint64_t heartbeat_ms = 1000;
  double eos_seconds = 2;
  std::size_t store_mb = 256;
  int cpu = -1;
  mold::FaultConfig faults;
};

void usage() {
  std::fprintf(stderr,
               "usage: itch-replay FILE [--dest IP:PORT] [--iface IP] [--retrans-port N]\n"
               "                   [--session NAME] [--rate MSGS_PER_S | --speed X] [--payload N]\n"
               "                   [--max-delay-us N] [--limit N] [--heartbeat-ms N]\n"
               "                   [--eos-seconds S] [--store-mb N] [--cpu N]\n"
               "                   [--drop P] [--burst P] [--burst-len N] [--dup P] [--reorder P]\n"
               "                   [--seed N]\n");
}

bool parse(int argc, char** argv, Options& o) {
  if (argc < 2) return false;
  o.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return false;
    const char* v = argv[++i];
    if (a == "--dest")
      o.dest = v;
    else if (a == "--iface")
      o.iface = v;
    else if (a == "--retrans-port")
      o.retrans_port = std::atoi(v);
    else if (a == "--session")
      o.session = v;
    else if (a == "--rate")
      o.rate = std::strtod(v, nullptr);
    else if (a == "--speed")
      o.speed = std::strtod(v, nullptr);
    else if (a == "--payload")
      o.payload = std::strtoul(v, nullptr, 10);
    else if (a == "--max-delay-us")
      o.max_delay_us = std::strtoull(v, nullptr, 10);
    else if (a == "--limit")
      o.limit = std::strtoull(v, nullptr, 10);
    else if (a == "--heartbeat-ms")
      o.heartbeat_ms = std::strtoull(v, nullptr, 10);
    else if (a == "--eos-seconds")
      o.eos_seconds = std::strtod(v, nullptr);
    else if (a == "--store-mb")
      o.store_mb = std::strtoul(v, nullptr, 10);
    else if (a == "--cpu")
      o.cpu = std::atoi(v);
    else if (a == "--drop")
      o.faults.drop = std::strtod(v, nullptr);
    else if (a == "--burst")
      o.faults.burst = std::strtod(v, nullptr);
    else if (a == "--burst-len")
      o.faults.burst_len = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    else if (a == "--dup")
      o.faults.duplicate = std::strtod(v, nullptr);
    else if (a == "--reorder")
      o.faults.reorder = std::strtod(v, nullptr);
    else if (a == "--seed")
      o.faults.seed = std::strtoull(v, nullptr, 10);
    else
      return false;
  }
  return o.payload >= mold::kHeaderSize + 64 && o.payload <= kMaxPayload && o.store_mb > 0;
}

std::size_t pow2_floor(std::size_t v) {
  std::size_t p = 1;
  while (p * 2 <= v) p *= 2;
  return p;
}

class Publisher {
 public:
  Publisher(const Options& o)
      : opt_{o},
        session_{mold::make_session(o.session)},
        dest_{net::parse_endpoint(o.dest)},
        tx_{net::UdpSocket::sender(dest_, o.iface)},
        rq_{net::UdpSocket::bound(
            net::parse_endpoint(o.iface + ":" + std::to_string(o.retrans_port)))},
        // ~16 bytes of index per stored message; ITCH averages about 30 bytes.
        store_{o.store_mb << 20, pow2_floor((o.store_mb << 20) / 16)},
        faults_{o.faults},
        builder_{o.payload},
        rbuilder_{o.payload},
        held_(kMaxPayload) {
    builder_.begin(session_, store_.next_sequence());
  }

  // Adds one message, sending the current packet first if it is full or has
  // waited long enough.
  void add(std::span<const std::byte> msg, std::uint64_t now) {
    if (!builder_.empty() && now - opened_ns_ > opt_.max_delay_us * 1000) flush(now);
    if (!builder_.add(msg)) {
      flush(now);
      builder_.add(msg);
    }
    if (builder_.count() == 1) opened_ns_ = now;
    store_.append(msg);
    ++messages_;
  }

  // Sends the pending packet through the fault injector.
  void flush(std::uint64_t now) {
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

  // Answers waiting retransmission requests and sends a heartbeat when idle.
  void service(std::uint64_t now) {
    serve_requests();
    if (now - last_send_ns_ >= opt_.heartbeat_ms * 1'000'000) {
      mold::PacketBuilder<mold::kHeaderSize + 2> hb;
      hb.heartbeat(session_, sent_next_);
      send(hb.finish());
      ++heartbeats_;
      last_send_ns_ = now;
    }
  }

  void finish() {
    const std::uint64_t now = lab::mono_ns();
    flush(now);
    release_held();
    const std::uint64_t end = now + static_cast<std::uint64_t>(opt_.eos_seconds * 1e9);
    std::uint64_t next_eos = 0;
    for (std::uint64_t t = now; t < end; t = lab::mono_ns()) {
      if (t >= next_eos) {
        mold::PacketBuilder<mold::kHeaderSize + 2> eos;
        eos.end_of_session(session_, sent_next_);
        send(eos.finish());
        ++eos_sent_;
        next_eos = t + 100'000'000;  // every 100 ms, so receivers finish quickly
      }
      serve_requests();
    }
  }

  void report(double secs) const {
    const auto& f = faults_.stats();
    std::printf("Session      %s to %s, re-requests on %s\n", opt_.session.c_str(),
                dest_.str().c_str(), rq_.local().str().c_str());
    std::printf("Messages     %llu in %llu packets (%.2f s, %.2f M msgs/s)\n",
                static_cast<unsigned long long>(messages_),
                static_cast<unsigned long long>(f.packets), secs,
                secs > 0 ? static_cast<double>(messages_) / secs / 1e6 : 0.0);
    std::printf("Heartbeats   %llu, End of Session %llu, send errors %llu\n",
                static_cast<unsigned long long>(heartbeats_),
                static_cast<unsigned long long>(eos_sent_),
                static_cast<unsigned long long>(send_errors_));
    std::printf("Faults       dropped %llu (%llu bursts), duplicated %llu, reordered %llu\n",
                static_cast<unsigned long long>(f.dropped),
                static_cast<unsigned long long>(f.bursts),
                static_cast<unsigned long long>(f.duplicated),
                static_cast<unsigned long long>(f.reordered));
    std::printf("Re-requests  %llu served, %llu messages resent, %llu unanswerable\n",
                static_cast<unsigned long long>(requests_),
                static_cast<unsigned long long>(resent_),
                static_cast<unsigned long long>(unanswerable_));
  }

 private:
  void send(std::span<const std::byte> pkt) {
    if (tx_.send_to(pkt, dest_) < 0) ++send_errors_;
  }

  void release_held() {
    if (!holding_) return;
    send({held_.data(), held_len_});
    holding_ = false;
  }

  void serve_requests() {
    std::array<std::byte, 64> buf;
    net::Endpoint from;
    for (;;) {
      const ssize_t n = rq_.recv_from(buf, &from);
      if (n < 0) return;
      mold::Request r;
      if (!mold::decode_request(buf.data(), static_cast<std::size_t>(n), r) ||
          r.session != session_ || r.sequence >= sent_next_ || r.count == 0) {
        ++unanswerable_;
        continue;
      }
      const auto want =
          static_cast<std::uint16_t>(std::min<std::uint64_t>(r.count, sent_next_ - r.sequence));
      const std::uint16_t got = store_.fill(rbuilder_, session_, r.sequence, want);
      if (got == 0) {
        ++unanswerable_;  // evicted from the store
        continue;
      }
      if (rq_.send_to(rbuilder_.finish(), from) < 0) ++send_errors_;
      ++requests_;
      resent_ += got;
    }
  }

  const Options& opt_;
  mold::Session session_;
  net::Endpoint dest_;
  net::UdpSocket tx_;
  net::UdpSocket rq_;
  mold::RetransmitStore store_;
  mold::FaultInjector faults_;
  Builder builder_;
  Builder rbuilder_;
  std::vector<std::byte> held_;  // packet held back by a reorder fault
  std::size_t held_len_ = 0;
  bool holding_ = false;
  std::uint64_t sent_next_ = 1;  // next sequence number not yet sent
  std::uint64_t opened_ns_ = 0;
  std::uint64_t last_send_ns_ = 0;
  std::uint64_t messages_ = 0;
  std::uint64_t heartbeats_ = 0;
  std::uint64_t eos_sent_ = 0;
  std::uint64_t requests_ = 0;
  std::uint64_t resent_ = 0;
  std::uint64_t unanswerable_ = 0;
  std::uint64_t send_errors_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return 2;
  }
  lab::itch::FileReader reader{opt.path};
  if (!reader.ok()) {
    std::fprintf(stderr, "itch-replay: %s\n", reader.error().c_str());
    return 2;
  }
  std::unique_ptr<Publisher> pub;
  try {
    pub = std::make_unique<Publisher>(opt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "itch-replay: %s\n", e.what());
    return 2;
  }
  if (opt.cpu >= 0) {
    if (auto ec = lab::pin_this_thread(opt.cpu)) {
      std::fprintf(stderr, "itch-replay: cannot pin to cpu %d: %s\n", opt.cpu,
                   ec.message().c_str());
    }
  }
  lab::set_thread_name("itch-replay");

  const std::uint64_t start = lab::mono_ns();
  const double ns_per_msg = opt.rate > 0 ? 1e9 / opt.rate : 0;
  std::uint64_t n = 0;
  std::uint64_t first_ts = 0;
  while (auto msg = reader.next()) {
    // When this message is due, by message rate or by its ITCH timestamp.
    std::uint64_t due = start;
    if (opt.speed > 0 && msg->size() >= 11) {
      const std::uint64_t ts = lab::load_be48(msg->data() + 5);
      if (n == 0) first_ts = ts;
      due +=
          static_cast<std::uint64_t>(static_cast<double>(ts - std::min(ts, first_ts)) / opt.speed);
    } else if (ns_per_msg > 0) {
      due += static_cast<std::uint64_t>(static_cast<double>(n) * ns_per_msg);
    }
    std::uint64_t now = lab::mono_ns();
    if (now < due) {
      // Ahead of schedule: send what is waiting if it would wait too long, then idle.
      if (due - now > opt.max_delay_us * 1000) pub->flush(now);
      while ((now = lab::mono_ns()) < due) pub->service(now);
    }
    pub->add(*msg, now);
    if ((n & 255) == 0) pub->service(now);
    ++n;
    if (opt.limit != 0 && n >= opt.limit) break;
  }
  const double secs = static_cast<double>(lab::mono_ns() - start) / 1e9;
  if (!reader.ok()) {
    std::fprintf(stderr, "itch-replay: %s after %llu messages\n", reader.error().c_str(),
                 static_cast<unsigned long long>(n));
  }
  pub->finish();
  pub->report(secs);
  return reader.ok() ? 0 : 2;
}
