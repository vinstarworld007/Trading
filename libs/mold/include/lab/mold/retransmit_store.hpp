#pragma once

// Sender-side store of recently sent messages for the MoldUDP64 re-request
// server. A byte ring holds message bodies and an index ring maps sequence
// numbers to them; both are fixed at construction, so the oldest messages
// fall out as new ones arrive. Messages never straddle the end of the byte
// ring, which keeps every stored message contiguous.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "lab/mold/mold.hpp"

namespace lab::mold {

class RetransmitStore {
 public:
  // `index_slots` must be a power of two.
  RetransmitStore(std::size_t arena_bytes, std::size_t index_slots,
                  std::uint64_t first_sequence = 1)
      : arena_(arena_bytes),
        index_(index_slots),
        mask_{index_slots - 1},
        next_{first_sequence},
        first_{first_sequence} {
    if (index_slots < 2 || (index_slots & (index_slots - 1)) != 0) {
      throw std::invalid_argument("index_slots must be a power of two");
    }
    if (arena_bytes < 2 * 0xFFFF) throw std::invalid_argument("arena too small");
  }

  // Stores the message with the next sequence number and returns that number.
  std::uint64_t append(std::span<const std::byte> msg) noexcept {
    const std::size_t size = arena_.size();
    std::size_t at = static_cast<std::size_t>(written_ % size);
    if (at + msg.size() > size) {  // pad to the start rather than split
      written_ += size - at;
      at = 0;
    }
    std::memcpy(arena_.data() + at, msg.data(), msg.size());
    index_[next_ & mask_] = Entry{written_, static_cast<std::uint32_t>(msg.size())};
    written_ += msg.size();
    return next_++;
  }

  // The stored message, or nullopt if it was never sent or has been evicted.
  std::optional<std::span<const std::byte>> get(std::uint64_t seq) const noexcept {
    if (seq < first_ || seq >= next_ || next_ - seq > index_.size()) return std::nullopt;
    const Entry& e = index_[seq & mask_];
    const std::size_t at = static_cast<std::size_t>(e.start % arena_.size());
    return std::span<const std::byte>{arena_.data() + at, e.len};
  }

  // Fills `b` with as many messages from `first` on as fit, up to `count`.
  // Returns how many were added; 0 if `first` is no longer stored.
  template <std::size_t Capacity>
  std::uint16_t fill(PacketBuilder<Capacity>& b, const Session& session, std::uint64_t first,
                     std::uint16_t count) const noexcept {
    b.begin(session, first);
    std::uint16_t n = 0;
    for (; n < count; ++n) {
      const auto msg = get(first + n);
      if (!msg || !b.add(*msg)) break;
    }
    return n;
  }

  // Next sequence number append() will assign.
  std::uint64_t next_sequence() const noexcept { return next_; }

 private:
  struct Entry {
    std::uint64_t start = 0;  // absolute byte position (written_ when stored)
    std::uint32_t len = 0;
  };

  std::vector<std::byte> arena_;
  std::vector<Entry> index_;
  std::uint64_t mask_;
  std::uint64_t next_;
  std::uint64_t first_;
  std::uint64_t written_ = 0;  // bytes ever written, including padding
};

}  // namespace lab::mold
