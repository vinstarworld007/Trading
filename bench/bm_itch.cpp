#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/itch/synthetic.hpp"

namespace {

// One synthetic day held in memory as length-prefixed messages, so the
// benchmarks measure decode and book work without file or zlib cost.
struct Day {
  std::vector<std::byte> bytes;
  std::vector<std::uint32_t> offsets;
  std::vector<std::uint16_t> lengths;

  Day() {
    lab::itch::SyntheticConfig config;
    config.symbols = 500;
    config.book_messages = 2'000'000;
    lab::itch::SyntheticDay day{config};
    day.generate([this](std::span<const std::byte> m) {
      offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
      lengths.push_back(static_cast<std::uint16_t>(m.size()));
      bytes.insert(bytes.end(), m.begin(), m.end());
    });
  }
};

const Day& day() {
  static const Day d;
  return d;
}

struct CountingHandler : lab::itch::HandlerBase {
  using HandlerBase::on;
  std::uint64_t shares = 0;
  void on(const lab::itch::AddOrder& m) { shares += m.shares; }
  void on(const lab::itch::OrderExecuted& m) { shares += m.executed_shares; }
};

// Decode only: dispatch every message to a near-empty handler.
void BM_ItchDecode(benchmark::State& state) {
  const Day& d = day();
  CountingHandler h;
  for (auto _ : state) {
    for (std::size_t i = 0; i < d.offsets.size(); ++i) {
      lab::itch::dispatch(d.bytes.data() + d.offsets[i], d.lengths[i], h);
    }
  }
  benchmark::DoNotOptimize(h.shares);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * d.offsets.size()));
}
BENCHMARK(BM_ItchDecode)->Unit(benchmark::kMillisecond);

// Decode plus full book rebuild with integrity checks: the itch-dump hot loop.
void BM_ItchBookRebuild(benchmark::State& state) {
  const Day& d = day();
  for (auto _ : state) {
    state.PauseTiming();
    auto builder = std::make_unique<lab::book::BookBuilder>(1u << 20);
    state.ResumeTiming();
    for (std::size_t i = 0; i < d.offsets.size(); ++i) {
      builder->apply(d.bytes.data() + d.offsets[i], d.lengths[i]);
    }
    benchmark::DoNotOptimize(builder->orders().size());
    state.PauseTiming();
    builder.reset();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * d.offsets.size()));
}
BENCHMARK(BM_ItchBookRebuild)->Unit(benchmark::kMillisecond);

}  // namespace
