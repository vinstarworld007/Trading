#include <benchmark/benchmark.h>

#include "lab/core/clock.hpp"

static void BM_Rdtsc(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(lab::rdtsc());
}
BENCHMARK(BM_Rdtsc);

static void BM_Rdtscp(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(lab::rdtscp());
}
BENCHMARK(BM_Rdtscp);

static void BM_MonoNs(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(lab::mono_ns());
}
BENCHMARK(BM_MonoNs);
