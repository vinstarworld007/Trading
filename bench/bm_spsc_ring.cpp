#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>

#include "lab/core/affinity.hpp"
#include "lab/core/spsc_ring.hpp"

namespace {

struct Msg {
  std::uint64_t seq;
  std::uint64_t data[7];  // one cache line, like a normalized market data event
};

// Single thread push then pop: the cost of the ring's own bookkeeping.
void BM_RingPushPopSameThread(benchmark::State& state) {
  static lab::SpscRing<Msg, 1024> ring;
  Msg m{};
  for (auto _ : state) {
    ring.try_push(m);
    ring.try_pop(m);
    benchmark::DoNotOptimize(m);
  }
}
BENCHMARK(BM_RingPushPopSameThread);

// Producer on another core, consumer timed here: throughput across cores.
void BM_RingCrossThreadThroughput(benchmark::State& state) {
  static lab::SpscRing<Msg, 4096> ring;
  std::atomic<bool> stop{false};
  const int cpus = lab::available_cpus();
  std::thread producer([&] {
    if (cpus > 2) lab::pin_this_thread(2);
    Msg m{};
    while (!stop.load(std::memory_order_relaxed)) {
      if (ring.try_push(m))
        ++m.seq;
      else
        lab::cpu_relax();
    }
  });
  if (cpus > 1) lab::pin_this_thread(1);
  Msg m{};
  for (auto _ : state) {
    while (!ring.try_pop(m)) lab::cpu_relax();
    benchmark::DoNotOptimize(m);
  }
  stop.store(true);
  producer.join();
  while (ring.try_pop(m)) {
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingCrossThreadThroughput)->UseRealTime();

}  // namespace
