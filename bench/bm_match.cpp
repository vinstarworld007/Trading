#include <benchmark/benchmark.h>

#include <chrono>
#include <memory>
#include <random>
#include <vector>

#include "lab/core/clock.hpp"
#include "lab/core/histogram.hpp"
#include "lab/match/engine.hpp"

namespace {

using namespace lab::match;

struct Null {
  void on_accepted(const OrderView&) {}
  void on_rejected(OrderKey, Reject) {}
  void on_execution(const OrderView&, const OrderView&, std::uint32_t, std::uint32_t,
                    std::uint64_t) {}
  void on_rested(const OrderView&) {}
  void on_canceled(const OrderView&, std::uint32_t, CancelReason) {}
  void on_replaced(const OrderView&, const OrderView&) {}
};

std::unique_ptr<Engine> make_engine(std::size_t max_orders = 1u << 20) {
  EngineConfig cfg;
  cfg.max_orders = max_orders;
  auto e = std::make_unique<Engine>(cfg);
  e->add_instrument(1);
  return e;
}

// Add a passive order and cancel it again: the resting-order round trip.
void BM_AddCancelPassive(benchmark::State& state) {
  auto e = make_engine();
  Null l;
  std::mt19937 rng{1};
  std::uint64_t ref = 1;
  // A standing book a few levels deep on both sides.
  for (std::uint32_t i = 0; i < 1000; ++i) {
    e->add({make_key(1, ref++), 1, Side::kBuy, Tif::kDay, 100, 9'900 - 100 * (i % 20)}, l);
    e->add({make_key(1, ref++), 1, Side::kSell, Tif::kDay, 100, 10'100 + 100 * (i % 20)}, l);
  }
  for (auto _ : state) {
    const OrderKey k = make_key(2, ref++);
    const bool buy = (rng() & 1) != 0;
    const std::uint32_t px = buy ? 9'900 - 100 * (rng() % 20) : 10'100 + 100 * (rng() % 20);
    e->add({k, 1, buy ? Side::kBuy : Side::kSell, Tif::kDay, 100, px}, l);
    e->cancel(k, 0, l);
  }
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddCancelPassive);

// An aggressive order that takes one resting order, which is then replenished.
void BM_CrossOneLevel(benchmark::State& state) {
  auto e = make_engine();
  Null l;
  std::uint64_t ref = 1;
  for (int i = 0; i < 1000; ++i) {
    e->add({make_key(1, ref++), 1, Side::kSell, Tif::kDay, 100, 10'100}, l);
  }
  for (auto _ : state) {
    e->add({make_key(2, ref++), 1, Side::kBuy, Tif::kIoc, 100, 10'100}, l);
    e->add({make_key(1, ref++), 1, Side::kSell, Tif::kDay, 100, 10'100}, l);
  }
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_CrossOneLevel);

// A mixed flow like simex sees: adds near the touch, cancels, replaces and
// some crossing orders, over a book of about 10k live orders. The argument is
// the engine's capacity: simex runs with 4M (1 << 22), which spreads the key
// index over 128 MB. Besides the mean, reports p50/p99/p99.9 of single
// commands (rdtsc ticks converted with TscClock).
void BM_MixedFlow(benchmark::State& state) {
  auto e = make_engine(static_cast<std::size_t>(state.range(0)));
  lab::LatencyHistogram h;
  Null l;
  std::mt19937_64 rng{7};
  std::vector<OrderKey> live;
  live.reserve(1u << 20);
  std::uint64_t ref = 1;
  auto add = [&](bool cross) {
    const bool buy = (rng() & 1) != 0;
    const std::uint32_t off = 100 * static_cast<std::uint32_t>(rng() % 30);
    std::uint32_t px = buy ? 9'990 - off : 10'010 + off;
    if (cross) px = buy ? 10'300 : 9'700;
    const OrderKey k = make_key(1, ref++);
    e->add({k, 1, buy ? Side::kBuy : Side::kSell, cross ? Tif::kIoc : Tif::kDay,
            100 * static_cast<std::uint32_t>(1 + rng() % 5), px},
           l);
    if (!cross) live.push_back(k);
  };
  for (int i = 0; i < 10'000; ++i) add(false);
  for (auto _ : state) {
    const std::uint64_t t0 = lab::rdtsc();
    const auto r = rng() % 100;
    if (r < 50 || live.size() < 1000) {
      add(r < 5);
    } else {
      const std::size_t i = rng() % live.size();
      const OrderKey k = live[i];
      live[i] = live.back();
      live.pop_back();
      if (r < 85) {
        e->cancel(k, 0, l);
      } else {
        const OrderKey n = make_key(1, ref++);
        const OrderView* o = e->find(k);
        if (o == nullptr) continue;
        const bool buy = o->side == Side::kBuy;
        const std::uint32_t px = buy ? 9'990 - 100 * static_cast<std::uint32_t>(rng() % 30)
                                     : 10'010 + 100 * static_cast<std::uint32_t>(rng() % 30);
        e->replace(k, {n, 0, o->side, Tif::kDay, o->executed + 200, px}, l);
        live.push_back(n);
      }
    }
    h.record(lab::rdtsc() - t0);
  }
  state.SetItemsProcessed(state.iterations());
  static const auto clock = lab::TscClock::calibrate(std::chrono::milliseconds{50});
  const double ns = 1.0 / clock.ticks_per_ns();
  state.counters["p50_ns"] = static_cast<double>(h.percentile(50)) * ns;
  state.counters["p99_ns"] = static_cast<double>(h.percentile(99)) * ns;
  state.counters["p99.9_ns"] = static_cast<double>(h.percentile(99.9)) * ns;
}
BENCHMARK(BM_MixedFlow)->Arg(1 << 20)->Arg(1 << 22);

}  // namespace
