#pragma once

// Fixed-memory latency histogram with log-linear buckets (the HdrHistogram
// layout): values below 64 are exact, and above that each power of two is
// split into 32 buckets, so any recorded value is reported within ~3%.
// record() is a handful of instructions and never allocates, so it is safe on
// a hot thread; percentiles and printing are for the slow path.
//
// Units are whatever you record (TSC ticks or ns). Record ticks on the hot
// path and convert when reporting.

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "lab/core/compiler.hpp"

namespace lab {

class LatencyHistogram {
 public:
  static constexpr int kSubBucketBits = 5;
  static constexpr std::uint64_t kSubBuckets = 1ULL << kSubBucketBits;  // 32
  static constexpr std::size_t kBuckets = 64 * kSubBuckets;             // covers 2^64

  LAB_ALWAYS_INLINE void record(std::uint64_t value) noexcept {
    ++counts_[index_of(value)];
    ++count_;
    sum_ += value;
    if (value < min_) min_ = value;
    if (value > max_) max_ = value;
  }

  void merge(const LatencyHistogram& other) noexcept;
  void reset() noexcept;

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept {
    return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
  }

  // Value at percentile p in [0, 100]: the highest value of the bucket holding
  // that rank, clamped to the recorded max. Returns 0 when empty.
  std::uint64_t percentile(double p) const noexcept;

  // One line: count, min, p50, p90, p99, p99.9, p99.99, max, each multiplied
  // by `scale` (for example 1/ticks_per_ns to print ns from ticks).
  void print(std::FILE* out, const char* label, double scale = 1.0, const char* unit = "ns") const;

  static constexpr std::size_t index_of(std::uint64_t value) noexcept {
    if (value < 2 * kSubBuckets) return static_cast<std::size_t>(value);
    const int msb = 63 - __builtin_clzll(value);
    const int shift = msb - kSubBucketBits;
    const std::uint64_t top = value >> shift;  // in [32, 63]
    return static_cast<std::size_t>(static_cast<std::uint64_t>(shift) * kSubBuckets + top);
  }

  // Highest value that maps to bucket `index`.
  static constexpr std::uint64_t highest_value_at(std::size_t index) noexcept {
    if (index < 2 * kSubBuckets) return index;
    const std::uint64_t shift = index / kSubBuckets - 1;
    const std::uint64_t top = index - shift * kSubBuckets;
    const std::uint64_t low = top << shift;
    return low + ((1ULL << shift) - 1);
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace lab
