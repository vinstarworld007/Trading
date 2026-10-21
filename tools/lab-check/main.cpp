// lab-check: is this machine (usually a WSL2 distro) ready to run the lab?
//
// Prints one line per check with OK, WARN or INFO, then measures a
// core-to-core round trip through two SPSC rings as a quick latency baseline.
//
//   lab-check                 checks + ping-pong on CPUs 2 and 3
//   lab-check --cpus 4,5      ping-pong on CPUs 4 and 5
//   lab-check --no-pingpong   checks only

#include <net/if.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "lab/core/affinity.hpp"
#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/core/spsc_ring.hpp"

namespace {

int g_warnings = 0;

void ok(const std::string& what) { std::printf("  OK    %s\n", what.c_str()); }
void info(const std::string& what) { std::printf("  INFO  %s\n", what.c_str()); }
void warn(const std::string& what, const std::string& fix) {
  ++g_warnings;
  std::printf("  WARN  %s\n        fix: %s\n", what.c_str(), fix.c_str());
}

std::string read_file(const char* path) {
  std::ifstream in{path};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

long meminfo_value(const std::string& meminfo, const char* key) {
  const auto pos = meminfo.find(key);
  if (pos == std::string::npos) return -1;
  return std::strtol(meminfo.c_str() + pos + std::strlen(key), nullptr, 10);
}

void check_platform() {
  utsname u{};
  uname(&u);
  const std::string version = read_file("/proc/version");
  const bool wsl = version.find("microsoft") != std::string::npos ||
                   version.find("Microsoft") != std::string::npos;
  info(std::string{"kernel "} + u.release + (wsl ? " (WSL2)" : " (not WSL)"));
}

void check_cpus() {
  const int cpus = lab::available_cpus();
  const std::string msg = std::to_string(cpus) + " CPUs available";
  if (cpus >= 8)
    ok(msg);
  else
    warn(msg + "; the default core plan wants 8",
         "raise processors= in %UserProfile%\\.wslconfig, then wsl --shutdown");

  const std::string cpuinfo = read_file("/proc/cpuinfo");
  const bool constant = cpuinfo.find(" constant_tsc") != std::string::npos;
  const bool nonstop = cpuinfo.find(" nonstop_tsc") != std::string::npos;
  if (constant && nonstop)
    ok("invariant TSC (constant_tsc, nonstop_tsc)");
  else
    warn("TSC is not reported as invariant; rdtsc timings may drift",
         "timings still work; trust histograms over absolute numbers");

  const auto clock = lab::TscClock::calibrate();
  char buf[64];
  std::snprintf(buf, sizeof buf, "TSC calibrated at %.3f GHz", clock.ghz());
  info(buf);
}

void check_memory() {
  const std::string meminfo = read_file("/proc/meminfo");
  const long total_gb = meminfo_value(meminfo, "MemTotal:") / 1024 / 1024;
  const std::string msg = std::to_string(total_gb) + " GB RAM visible to Linux";
  if (total_gb >= 24)
    ok(msg);
  else
    warn(msg, "set memory= in .wslconfig (about 75% of your RAM), then wsl --shutdown");

  const long huge = meminfo_value(meminfo, "HugePages_Total:");
  if (huge >= 512)
    ok(std::to_string(huge) + " huge pages reserved");
  else
    warn(std::to_string(huge < 0 ? 0 : huge) + " huge pages reserved",
         "sudo sysctl -w vm.nr_hugepages=1024 (scripts/wsl-setup.sh persists it)");

  const std::string thp = trim(read_file("/sys/kernel/mm/transparent_hugepage/enabled"));
  if (!thp.empty()) info("transparent huge pages: " + thp);

  struct statvfs shm {};
  if (statvfs("/dev/shm", &shm) == 0) {
    const auto gb = shm.f_bavail * shm.f_frsize / (1ULL << 30);
    const std::string m = "/dev/shm has " + std::to_string(gb) + " GB free";
    if (gb >= 2)
      ok(m);
    else
      warn(m, "sudo mount -o remount,size=8G /dev/shm");
  }
}

void check_network() {
  const std::string flags_hex = trim(read_file("/sys/class/net/lo/flags"));
  const unsigned long flags = std::strtoul(flags_hex.c_str(), nullptr, 16);
  if (flags & IFF_MULTICAST)
    ok("loopback has multicast enabled");
  else
    warn("loopback multicast is off", "sudo ip link set lo multicast on");

  // /proc/net/route stores addresses as little-endian hex: 239.0.0.0/8 is 000000EF/000000FF.
  std::ifstream routes{"/proc/net/route"};
  std::string line;
  bool found = false;
  while (std::getline(routes, line)) {
    std::istringstream ls{line};
    std::string iface, dest, gateway, flags_s, refcnt, use, metric, mask;
    ls >> iface >> dest >> gateway >> flags_s >> refcnt >> use >> metric >> mask;
    if (iface == "lo" && dest == "000000EF" && mask == "000000FF") found = true;
  }
  if (found)
    ok("multicast route 239.0.0.0/8 via lo");
  else
    warn("no 239.0.0.0/8 route on lo; lab multicast would leave the VM",
         "sudo ip route replace 239.0.0.0/8 dev lo");
}

void check_data_dir() {
  const char* home = std::getenv("HOME");
  const std::string dir = std::string{home ? home : "."} + "/data/itch";
  struct statvfs st {};
  if (statvfs(dir.c_str(), &st) != 0) {
    warn(dir + " does not exist", "mkdir -p " + dir + " (keep data on the WSL disk, not /mnt/c)");
    return;
  }
  const auto gb = st.f_bavail * st.f_frsize / (1ULL << 30);
  const std::string m = dir + " has " + std::to_string(gb) + " GB free";
  if (gb >= 100)
    ok(m);
  else
    warn(m, "one ITCH sample day is 3.5 to 18 GB compressed; free up space");
}

struct Ping {
  std::uint64_t tsc;
};

void pingpong(int cpu_a, int cpu_b) {
  static lab::SpscRing<Ping, 64> to_b, to_a;
  constexpr int kWarmup = 10'000, kSamples = 200'000;
  std::atomic<bool> ready{false};

  std::thread echo([&] {
    lab::set_thread_name("lab-echo");
    lab::pin_this_thread(cpu_b);
    ready.store(true);
    for (int i = 0; i < kWarmup + kSamples; ++i) {
      Ping p{};
      while (!to_b.try_pop(p)) lab::cpu_relax();
      while (!to_a.try_push(p)) lab::cpu_relax();
    }
  });

  if (auto ec = lab::pin_this_thread(cpu_a)) {
    std::printf("  INFO  could not pin to CPU %d (%s); results include migrations\n", cpu_a,
                ec.message().c_str());
  }
  while (!ready.load()) lab::cpu_relax();

  lab::LatencyHistogram rtt;
  for (int i = 0; i < kWarmup + kSamples; ++i) {
    const Ping out{lab::rdtsc()};
    while (!to_b.try_push(out)) lab::cpu_relax();
    Ping back{};
    while (!to_a.try_pop(back)) lab::cpu_relax();
    if (i >= kWarmup) rtt.record(lab::rdtscp() - back.tsc);
  }
  echo.join();

  const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{20});
  std::printf("\nCore-to-core round trip through two SPSC rings, CPU %d <-> %d:\n", cpu_a, cpu_b);
  rtt.print(stdout, "  round trip", 1.0 / clock.ticks_per_ns());
  std::printf("  One-way is about half. Under WSL2, expect a long tail from host scheduling.\n");
}

}  // namespace

int main(int argc, char** argv) {
  int cpu_a = 2, cpu_b = 3;
  bool run_pingpong = true;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--no-pingpong") {
      run_pingpong = false;
    } else if (arg == "--cpus" && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%d,%d", &cpu_a, &cpu_b) != 2) {
        std::fprintf(stderr, "--cpus expects two CPU numbers, e.g. --cpus 4,5\n");
        return 2;
      }
    } else {
      std::fprintf(stderr, "usage: lab-check [--cpus A,B] [--no-pingpong]\n");
      return 2;
    }
  }

  const int cpus = lab::available_cpus();
  if (cpu_a >= cpus || cpu_b >= cpus) {
    cpu_a = cpus > 1 ? cpus - 2 : 0;
    cpu_b = cpus > 1 ? cpus - 1 : 0;
  }

  std::printf("Trading lab machine check\n\n");
  check_platform();
  check_cpus();
  check_memory();
  check_network();
  check_data_dir();
  std::printf("\n%d warning(s)\n", g_warnings);

  if (run_pingpong && cpus >= 2) pingpong(cpu_a, cpu_b);
  return 0;
}
