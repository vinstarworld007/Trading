// itch-fh: the firm's ITCH 5.0 feed handler. Joins a MoldUDP64 feed, recovers
// lost packets from the re-request server, and rebuilds every order book live.
//
//   itch-fh                                         239.1.1.1:30001, re-requests to 127.0.0.1:30002
//   itch-fh --rx-cpu 2 --book-cpu 3 --symbol AAPL
//   itch-fh --group 127.0.0.1:30001                 unicast feed (CI, or no loopback multicast)
//
// Two threads, joined by lock-free rings:
//
//   rx thread    recvmmsg on the feed and on the re-request socket, timestamps
//                each packet with rdtsc, pushes it to the packet ring; sends
//                retransmission requests from the request ring. All syscalls
//                happen here.
//   book thread  MoldUDP64 sequencing (gaps, duplicates, reordering), ITCH
//                decode and book updates. No syscalls, locks or allocation on
//                the message path.
//
// Runs until End of Session with every message delivered, then prints feed and
// book reports. Exits 0 when nothing was lost and every integrity check is
// zero, 1 otherwise, 2 on usage or setup errors, 3 if the feed went silent or
// was interrupted before End of Session.

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

#include "lab/book/book_builder.hpp"
#include "lab/book/report.hpp"
#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/core/spsc_ring.hpp"
#include "lab/mold/mold.hpp"
#include "lab/mold/sequencer.hpp"
#include "lab/net/udp.hpp"

namespace {

namespace mold = lab::mold;
namespace net = lab::net;

// One received datagram, as it crosses from the rx thread to the book thread.
struct alignas(64) RxPacket {
  static constexpr std::size_t kMaxLen = 1472;  // UDP payload in a 1500-byte MTU
  std::uint64_t rx_tsc;
  std::uint32_t len;
  std::uint32_t reserved;
  std::byte data[kMaxLen];
};
static_assert(sizeof(RxPacket) == 1536);
static_assert(std::is_trivially_copyable_v<RxPacket>);

using PacketRing = lab::SpscRing<RxPacket, 4096>;
using RequestRing = lab::SpscRing<mold::Request, 64>;

struct Options {
  std::string group = "239.1.1.1:30001";
  std::string iface = "127.0.0.1";
  std::string retrans = "127.0.0.1:30002";
  int rx_cpu = -1;
  int book_cpu = -1;
  std::uint64_t gap_delay_us = 100;  // allowance for reordering before the first request
  std::uint64_t timeout_ms = 20;     // re-request when a request makes no progress
  std::uint32_t retries = 10;
  double idle_seconds = 10;  // after the first packet, give up if the feed is silent this long
  std::string symbol;
  std::size_t depth = 10;
  std::size_t top = 10;
};

void usage() {
  std::fprintf(stderr,
               "usage: itch-fh [--group IP:PORT] [--iface IP] [--retrans IP:PORT]\n"
               "               [--rx-cpu N] [--book-cpu N] [--gap-delay-us N] [--timeout-ms N]\n"
               "               [--retries N] [--idle-seconds S] [--symbol SYM] [--depth N]\n"
               "               [--top N]\n");
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return false;
    const char* v = argv[++i];
    if (a == "--group")
      o.group = v;
    else if (a == "--iface")
      o.iface = v;
    else if (a == "--retrans")
      o.retrans = v;
    else if (a == "--rx-cpu")
      o.rx_cpu = std::atoi(v);
    else if (a == "--book-cpu")
      o.book_cpu = std::atoi(v);
    else if (a == "--gap-delay-us")
      o.gap_delay_us = std::strtoull(v, nullptr, 10);
    else if (a == "--timeout-ms")
      o.timeout_ms = std::strtoull(v, nullptr, 10);
    else if (a == "--retries")
      o.retries = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    else if (a == "--idle-seconds")
      o.idle_seconds = std::strtod(v, nullptr);
    else if (a == "--symbol")
      o.symbol = v;
    else if (a == "--depth")
      o.depth = std::strtoul(v, nullptr, 10);
    else if (a == "--top")
      o.top = std::strtoul(v, nullptr, 10);
    else
      return false;
  }
  return true;
}

std::atomic<bool> g_interrupted{false};

extern "C" void on_signal(int) { g_interrupted.store(true, std::memory_order_relaxed); }

void pin(int cpu, const char* name) {
  lab::set_thread_name(name);
  if (cpu < 0) return;
  if (auto ec = lab::pin_this_thread(cpu)) {
    std::fprintf(stderr, "itch-fh: cannot pin %s to cpu %d: %s\n", name, cpu, ec.message().c_str());
  }
}

struct Shared {
  PacketRing packets;
  RequestRing requests;
  std::atomic<bool> stop{false};
  std::atomic<bool> finished{false};
  std::atomic<std::uint64_t> rx_packets{0};  // written by rx, read by main for the idle check
  // rx thread counters, read after join.
  std::uint64_t ring_full = 0;
  std::uint64_t oversize = 0;
  std::uint64_t requests_sent = 0;
  std::uint64_t request_errors = 0;
  std::uint64_t retrans_packets = 0;
};

void rx_loop(const Options& opt, Shared& sh, const net::UdpSocket& feed, const net::UdpSocket& rq,
             const net::Endpoint& retrans) {
  pin(opt.rx_cpu, "fh-rx");
  net::BatchReceiver batch{32, RxPacket::kMaxLen + 1};  // +1 spots oversize datagrams
  std::uint64_t received = 0;
  auto push = [&](std::span<const std::byte> d, std::uint64_t tsc) {
    if (d.size() > RxPacket::kMaxLen) {
      ++sh.oversize;
      return;
    }
    RxPacket* slot = sh.packets.try_claim();
    if (slot == nullptr) {  // book thread behind: same as a lost packet, recovered later
      ++sh.ring_full;
      return;
    }
    slot->rx_tsc = tsc;
    slot->len = static_cast<std::uint32_t>(d.size());
    std::memcpy(slot->data, d.data(), d.size());
    sh.packets.commit();
  };
  while (!sh.stop.load(std::memory_order_relaxed)) {
    int n = batch.receive(feed);
    std::uint64_t tsc = lab::rdtsc();
    for (int i = 0; i < n; ++i) push(batch.datagram(i), tsc);
    received += static_cast<std::uint64_t>(n);

    n = batch.receive(rq);
    tsc = lab::rdtsc();
    for (int i = 0; i < n; ++i) push(batch.datagram(i), tsc);
    sh.retrans_packets += static_cast<std::uint64_t>(n);
    received += static_cast<std::uint64_t>(n);
    sh.rx_packets.store(received, std::memory_order_relaxed);

    mold::Request r;
    while (sh.requests.try_pop(r)) {
      std::array<std::byte, mold::kRequestSize> buf;
      mold::encode_request(buf.data(), r);
      if (rq.send_to(buf, retrans) < 0)
        ++sh.request_errors;
      else
        ++sh.requests_sent;
    }
  }
}

struct BookResult {
  lab::LatencyHistogram rx_to_book;  // ticks from rx timestamp to the packet's last book update
  std::uint64_t request_ring_full = 0;
};

void book_loop(const Options& opt, Shared& sh, mold::Sequencer& seq, lab::book::BookBuilder& books,
               BookResult& out) {
  pin(opt.book_cpu, "fh-book");
  auto deliver = [&books](std::uint64_t, std::span<const std::byte> msg) { books.apply(msg); };
  mold::Request req;
  while (!sh.stop.load(std::memory_order_relaxed)) {
    if (const RxPacket* p = sh.packets.front()) {
      seq.on_packet(p->data, p->len, lab::rdtsc(), deliver);
      out.rx_to_book.record(lab::rdtscp() - p->rx_tsc);
      sh.packets.pop();
    }
    if (seq.poll(lab::rdtsc(), req, deliver) && !sh.requests.try_push(req)) {
      ++out.request_ring_full;
    }
    if (seq.finished()) {
      sh.finished.store(true, std::memory_order_release);
      return;
    }
  }
}

void print_feed(const mold::Sequencer& seq, const Shared& sh, const BookResult& r,
                const lab::TscClock& clock) {
  const auto& s = seq.stats();
  auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("Session      %.*s, next sequence %llu\n",
              static_cast<int>(mold::trimmed(seq.session()).size()),
              mold::trimmed(seq.session()).data(), u(seq.expected()));
  std::printf("Packets      %llu (%llu retransmitted), %llu heartbeats or End of Session\n",
              u(s.packets), u(sh.retrans_packets), u(s.heartbeats));
  std::printf("Messages     %llu delivered, %llu duplicates dropped, %llu held for a gap\n",
              u(s.delivered), u(s.duplicates), u(s.held));
  std::printf("Gaps         %llu opened, %llu filled, %llu given up (%llu messages lost)\n",
              u(s.gaps), u(s.gaps_filled), u(s.gaps_given_up), u(s.lost));
  std::printf("Requests     %llu sent, %llu send errors, %llu ring full\n", u(sh.requests_sent),
              u(sh.request_errors), u(r.request_ring_full));
  std::printf("Overruns     %llu packet ring full, %llu window overflow, %llu oversize\n",
              u(sh.ring_full), u(s.window_overflow), u(s.oversize + sh.oversize));
  std::printf("Bad packets  %llu malformed, %llu wrong session\n", u(s.malformed),
              u(s.wrong_session));
  r.rx_to_book.print(stdout, "Rx to book (per packet)", 1.0 / clock.ticks_per_ns());
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return 2;
  }

  // Everything is allocated and faulted in here, before the threads start.
  auto shared = std::make_unique<Shared>();
  auto books = std::make_unique<lab::book::BookBuilder>();
  auto result = std::make_unique<BookResult>();
  const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{50});
  mold::SequencerConfig cfg;
  cfg.gap_delay = clock.to_ticks(static_cast<double>(opt.gap_delay_us) * 1e3);
  cfg.request_timeout = clock.to_ticks(static_cast<double>(opt.timeout_ms) * 1e6);
  cfg.max_retries = opt.retries;
  cfg.window = 1u << 18;
  auto seq = std::make_unique<mold::Sequencer>(cfg);

  net::UdpSocket feed;
  net::UdpSocket rq;
  net::Endpoint retrans;
  try {
    const auto group = net::parse_endpoint(opt.group);
    retrans = net::parse_endpoint(opt.retrans);
    feed = net::UdpSocket::receiver(group, opt.iface);
    rq = net::UdpSocket::bound(net::parse_endpoint(opt.iface + ":0"));
    std::fprintf(stderr, "itch-fh: listening on %s, re-requests to %s\n", group.str().c_str(),
                 retrans.str().c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "itch-fh: %s\n", e.what());
    return 2;
  }

  const auto wall_start = std::chrono::steady_clock::now();
  std::thread rx{rx_loop,         std::cref(opt), std::ref(*shared),
                 std::cref(feed), std::cref(rq),  std::cref(retrans)};
  std::thread bk{book_loop,      std::cref(opt),   std::ref(*shared),
                 std::ref(*seq), std::ref(*books), std::ref(*result)};

  // Main thread: wait for End of Session, Ctrl-C, or a feed gone silent.
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  bool timed_out = false;
  std::uint64_t last_rx = 0;
  auto last_change = std::chrono::steady_clock::now();
  while (!shared->finished.load(std::memory_order_acquire) &&
         !g_interrupted.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t rx_now = shared->rx_packets.load(std::memory_order_relaxed);
    if (rx_now != last_rx) {
      last_rx = rx_now;
      last_change = now;
    } else if (last_rx != 0 &&
               std::chrono::duration<double>(now - last_change).count() > opt.idle_seconds) {
      timed_out = true;
      break;
    }
  }
  shared->stop.store(true, std::memory_order_relaxed);
  rx.join();
  bk.join();
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

  const auto& b = *books;
  print_feed(*seq, *shared, *result, clock);
  std::printf("Elapsed      %.2f s, last feed time %s\n", secs,
              lab::book::format_feed_time(b.last_timestamp()).c_str());
  std::printf("Live orders  %zu\n", b.orders().size());
  std::printf("Book digest  %016llx\n", static_cast<unsigned long long>(b.digest()));
  lab::book::print_message_types(stdout, b);
  lab::book::print_integrity(stdout, b.checks());
  lab::book::print_top(stdout, b, opt.top);
  if (!opt.symbol.empty()) {
    if (const auto l = b.find_locate(opt.symbol))
      lab::book::print_book(stdout, b, l, opt.depth);
    else
      std::printf("\nSymbol %s not found in the stock directory\n", opt.symbol.c_str());
  }

  if (g_interrupted.load()) {
    std::fprintf(stderr, "itch-fh: interrupted before End of Session\n");
    return 3;
  }
  if (timed_out) {
    std::fprintf(stderr, "itch-fh: feed silent for %.0f s before End of Session\n",
                 opt.idle_seconds);
    return 3;
  }
  const bool clean = seq->stats().lost == 0 && b.checks().error_count() == 0;
  return clean ? 0 : 1;
}
