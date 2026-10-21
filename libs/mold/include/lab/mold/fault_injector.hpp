#pragma once

// Decides, packet by packet, what goes wrong on the wire: a lost packet, a
// burst of lost packets, a duplicate, or a packet held back and sent after the
// next one. Seeded and deterministic, so a failing run can be replayed exactly.

#include <cstdint>

namespace lab::mold {

struct FaultConfig {
  double drop = 0;               // probability a packet is lost
  double burst = 0;              // probability a burst of losses starts at a packet
  std::uint32_t burst_len = 20;  // packets lost in a burst
  double duplicate = 0;          // probability a packet is sent twice
  double reorder = 0;            // probability a packet is held and sent after the next one
  std::uint64_t seed = 1;

  bool any() const noexcept { return drop > 0 || burst > 0 || duplicate > 0 || reorder > 0; }
};

enum class Fault : std::uint8_t { kNone, kDrop, kDuplicate, kReorder };

struct FaultStats {
  std::uint64_t packets = 0;
  std::uint64_t dropped = 0;
  std::uint64_t bursts = 0;
  std::uint64_t duplicated = 0;
  std::uint64_t reordered = 0;
};

class FaultInjector {
 public:
  explicit FaultInjector(const FaultConfig& cfg = {}) noexcept
      : cfg_{cfg}, state_{cfg.seed == 0 ? 0x9E3779B97F4A7C15ULL : cfg.seed} {}

  // What to do with the next packet.
  Fault next() noexcept {
    ++stats_.packets;
    if (!cfg_.any()) return Fault::kNone;
    if (burst_left_ > 0) {
      --burst_left_;
      ++stats_.dropped;
      return Fault::kDrop;
    }
    if (chance(cfg_.burst) && cfg_.burst_len > 0) {
      burst_left_ = cfg_.burst_len - 1;
      ++stats_.bursts;
      ++stats_.dropped;
      return Fault::kDrop;
    }
    if (chance(cfg_.drop)) {
      ++stats_.dropped;
      return Fault::kDrop;
    }
    if (chance(cfg_.duplicate)) {
      ++stats_.duplicated;
      return Fault::kDuplicate;
    }
    if (chance(cfg_.reorder)) {
      ++stats_.reordered;
      return Fault::kReorder;
    }
    return Fault::kNone;
  }

  const FaultStats& stats() const noexcept { return stats_; }

 private:
  // xorshift64*: fast, good enough for fault decisions.
  std::uint64_t rand() noexcept {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }
  bool chance(double p) noexcept {
    if (p <= 0) return false;
    return static_cast<double>(rand() >> 11) * 0x1.0p-53 < p;
  }

  FaultConfig cfg_;
  std::uint64_t state_;
  std::uint32_t burst_left_ = 0;
  FaultStats stats_;
};

}  // namespace lab::mold
