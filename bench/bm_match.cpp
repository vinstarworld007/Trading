#include <benchmark/benchmark.h>

#include <memory>
#include <random>
#include <vector>

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

std::unique_ptr<Engine> make_engine() {
  EngineConfig cfg;
  cfg.max_orders = 1u << 20;
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
// some crossing orders, over a book of about 10k live orders.
void BM_MixedFlow(benchmark::State& state) {
  auto e = make_engine();
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
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MixedFlow);

}  // namespace
