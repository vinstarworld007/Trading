#include <benchmark/benchmark.h>

#include "lab/core/histogram.hpp"

static void BM_HistogramRecord(benchmark::State& state) {
  lab::LatencyHistogram h;
  std::uint64_t v = 12345;
  for (auto _ : state) {
    h.record(v);
    benchmark::DoNotOptimize(h);
    v = v * 6364136223846793005ULL + 1442695040888963407ULL;  // vary the bucket
    v >>= 44;
  }
  benchmark::DoNotOptimize(h.count());
}
BENCHMARK(BM_HistogramRecord);
