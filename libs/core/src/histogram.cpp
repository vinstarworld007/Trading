#include "lab/core/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace lab {

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
  for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
  count_ += other.count_;
  sum_ += other.sum_;
  min_ = std::min(min_, other.min_);
  max_ = std::max(max_, other.max_);
}

void LatencyHistogram::reset() noexcept { *this = LatencyHistogram{}; }

std::uint64_t LatencyHistogram::percentile(double p) const noexcept {
  if (count_ == 0) return 0;
  p = std::clamp(p, 0.0, 100.0);
  // Rank of the requested value, 1-based: p50 of 4 values is the 2nd.
  auto rank = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(count_)));
  rank = std::clamp<std::uint64_t>(rank, 1, count_);
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    seen += counts_[i];
    if (seen >= rank) return std::min(highest_value_at(i), max_);
  }
  return max_;
}

void LatencyHistogram::print(std::FILE* out, const char* label, double scale,
                             const char* unit) const {
  auto v = [&](std::uint64_t x) { return static_cast<double>(x) * scale; };
  std::fprintf(out,
               "%-24s n=%-10llu min=%.0f p50=%.0f p90=%.0f p99=%.0f p99.9=%.0f p99.99=%.0f "
               "max=%.0f %s\n",
               label, static_cast<unsigned long long>(count_), v(min()), v(percentile(50)),
               v(percentile(90)), v(percentile(99)), v(percentile(99.9)), v(percentile(99.99)),
               v(max()), unit);
}

}  // namespace lab
