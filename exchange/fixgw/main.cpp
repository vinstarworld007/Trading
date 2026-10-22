// fixgw: a FIX 4.4 order entry venue in front of simex. Firms log in with
// FIX; each FIX session trades through its own simex account over OUCH 5.0.
// The FIX subset and its mapping onto OUCH are in docs/specs/fix44-subset.md.
//
//   simex --symbols AAPL,MSFT --accounts OUCHA:pa,OUCHB:pb
//   fixgw --simex 127.0.0.1:15000 --session FIXA:OUCHA:pa --session FIXB:OUCHB:pb
//
// Runs until Ctrl-C, --duration seconds, or simex ends the session; then
// logs every FIX session out and prints a report. Exit 0 if no check failed.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include "gateway.hpp"
#include "lab/core/affinity.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: fixgw [--listen IP:PORT] [--simex IP:PORT] [--comp-id ID]\n"
               "             --session COMPID:OUCH_USER:OUCH_PASS [--session ...]\n"
               "             [--max-orders N] [--no-cancel-on-disconnect] [--duration S]\n"
               "             [--cpu N]\n");
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  fixgw::GatewayConfig cfg;
  double duration = 0;
  int cpu = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--listen") {
      cfg.listen = value();
    } else if (a == "--simex") {
      cfg.simex = value();
    } else if (a == "--comp-id") {
      cfg.comp_id = value();
    } else if (a == "--session") {
      const std::string s = value();
      const auto c1 = s.find(':');
      const auto c2 = c1 == std::string::npos ? c1 : s.find(':', c1 + 1);
      if (c2 == std::string::npos) usage();
      cfg.links.push_back({s.substr(0, c1), s.substr(c1 + 1, c2 - c1 - 1), s.substr(c2 + 1)});
    } else if (a == "--max-orders") {
      cfg.max_orders = std::stoull(value());
    } else if (a == "--no-cancel-on-disconnect") {
      cfg.cancel_on_disconnect = false;
    } else if (a == "--duration") {
      duration = std::stod(value());
    } else if (a == "--cpu") {
      cpu = std::stoi(value());
    } else {
      usage();
    }
  }
  if (cfg.links.empty()) usage();
  try {
    fixgw::Gateway gw{cfg};
    gw.connect_simex();
    std::fprintf(stderr, "fixgw: FIX 4.4 on %s as %s, %zu sessions, simex at %s\n",
                 gw.address().c_str(), cfg.comp_id.c_str(), cfg.links.size(), cfg.simex.c_str());
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
    if (cpu >= 0) (void)lab::pin_this_thread(cpu);
    gw.run(g_stop);
    g_stop.store(true);
    if (timer.joinable()) timer.join();
    return gw.report(stdout) ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fixgw: %s\n", e.what());
    return 1;
  }
}
