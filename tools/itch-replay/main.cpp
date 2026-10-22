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

#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/endian.hpp"
#include "lab/feed/publisher.hpp"
#include "lab/itch/file_reader.hpp"
#include "lab/mold/fault_injector.hpp"
#include "lab/mold/mold.hpp"

namespace {

namespace mold = lab::mold;

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
  return o.payload >= mold::kHeaderSize + 64 &&
         o.payload <= lab::feed::MoldPublisher::kMaxPayload && o.store_mb > 0;
}

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
  lab::feed::PublisherConfig pc;
  pc.dest = opt.dest;
  pc.iface = opt.iface;
  pc.retrans_port = opt.retrans_port;
  pc.session = opt.session;
  pc.payload = opt.payload;
  pc.max_delay_us = opt.max_delay_us;
  pc.heartbeat_ms = opt.heartbeat_ms;
  pc.store_mb = opt.store_mb;
  pc.faults = opt.faults;
  std::unique_ptr<lab::feed::MoldPublisher> pub;
  try {
    pub = std::make_unique<lab::feed::MoldPublisher>(pc);
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
  pub->finish(opt.eos_seconds);
  pub->print_report(stdout, secs);
  return reader.ok() ? 0 : 2;
}
