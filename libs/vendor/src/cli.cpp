#include "lab/vendor/cli.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

namespace lab::vendor {

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int run_vendor(int argc, char** argv, ServerConfig cfg, std::string_view usage_extra,
               const std::function<bool(std::string_view, const std::string&)>& extra,
               const std::function<Server::Factory()>& factory) {
  double duration = 0;
  auto usage = [&] {
    std::fprintf(stderr,
                 "usage: %s [--listen IP:PORT] [--md GROUP:PORT] [--iface IP] [--retrans IP:PORT]\n"
                 "       %*s [--idle-us N] [--luld-band-pct N] [--duration S]%.*s\n",
                 cfg.name.c_str(), static_cast<int>(cfg.name.size()), "",
                 static_cast<int>(usage_extra.size()), usage_extra.data());
    std::exit(2);
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) usage();
    const std::string v = argv[++i];
    if (a == "--listen") {
      cfg.listen = v;
    } else if (a == "--md") {
      cfg.feed.group = v;
    } else if (a == "--iface") {
      cfg.feed.iface = v;
    } else if (a == "--retrans") {
      cfg.feed.retrans = v;
    } else if (a == "--idle-us") {
      cfg.feed.idle_sleep_us = std::stoull(v);
    } else if (a == "--luld-band-pct") {
      cfg.luld_band_pct = static_cast<std::uint32_t>(std::stoul(v));
    } else if (a == "--duration") {
      duration = std::stod(v);
    } else if (!extra(a, v)) {
      usage();
    }
  }
  try {
    Server server{cfg, factory()};
    std::fprintf(stderr, "%s: serving on %s, feed %s (retransmission %s)\n", cfg.name.c_str(),
                 server.address().c_str(), cfg.feed.group.c_str(), cfg.feed.retrans.c_str());
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::thread timer;
    if (duration > 0) {
      timer = std::thread{[duration] {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::duration<double>{duration};
        while (!g_stop.load() && std::chrono::steady_clock::now() < until) {
          std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        g_stop.store(true);
      }};
    }
    server.run(g_stop);
    g_stop.store(true);
    if (timer.joinable()) timer.join();
    return server.report(stdout) ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", cfg.name.c_str(), e.what());
    return 1;
  }
}

}  // namespace lab::vendor
