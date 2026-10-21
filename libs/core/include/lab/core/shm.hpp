#pragma once

// POSIX shared memory regions (/dev/shm) for passing data between processes.
//
// ShmRegion is the raw mapping. create_shared<T>() / attach_shared<T>()
// placement-construct a T (for example an SpscRing) inside a region behind a
// small header, so a consumer process can check it attached to the right type
// and wait until the producer has finished constructing it.
//
// Everything here is slow-path setup code and reports errors by throwing
// std::system_error or std::runtime_error.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "lab/core/compiler.hpp"

namespace lab {

class ShmRegion {
 public:
  struct Options {
    bool populate = true;     // fault every page in now, not on the hot path
    bool huge_pages = false;  // ask for transparent huge pages (best effort)
    bool lock = false;        // mlock the pages so they are never swapped
  };

  // Creates (or truncates) /dev/shm/<name> with `bytes` and maps it read-write.
  static ShmRegion create(const std::string& name, std::size_t bytes, Options opts);
  static ShmRegion create(const std::string& name, std::size_t bytes) {
    return create(name, bytes, Options{});
  }

  // Maps an existing region read-write.
  static ShmRegion open(const std::string& name, Options opts);
  static ShmRegion open(const std::string& name) { return open(name, Options{}); }

  // Removes the name; existing mappings stay valid until unmapped.
  static void unlink(const std::string& name) noexcept;

  ShmRegion(ShmRegion&& other) noexcept;
  ShmRegion& operator=(ShmRegion&& other) noexcept;
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;
  ~ShmRegion();

  void* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  const std::string& name() const noexcept { return name_; }

 private:
  ShmRegion(std::string name, void* data, std::size_t size) noexcept
      : name_{std::move(name)}, data_{data}, size_{size} {}
  void reset() noexcept;

  std::string name_;
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

namespace detail {

inline constexpr std::uint64_t kShmMagic = 0x4c41425f53484d31ULL;  // "LAB_SHM1"

struct alignas(kCacheLine) ShmHeader {
  std::uint64_t magic;
  std::uint64_t object_size;
  std::uint64_t object_align;
  std::atomic<std::uint32_t> ready;
};

template <typename T>
constexpr std::size_t object_offset() {
  constexpr std::size_t a = alignof(T) > kCacheLine ? alignof(T) : kCacheLine;
  return (sizeof(ShmHeader) + a - 1) / a * a;
}

}  // namespace detail

// Bytes a region needs to hold a T made by create_shared<T>().
template <typename T>
constexpr std::size_t shared_size() {
  return detail::object_offset<T>() + sizeof(T);
}

// Constructs a T in a fresh region. T must not hold pointers or own heap
// memory, since another process maps the region at a different address.
template <typename T>
T* create_shared(ShmRegion& region) {
  static_assert(std::is_standard_layout_v<T>, "shared objects must be standard layout");
  if (region.size() < shared_size<T>()) throw std::runtime_error("shm region too small");
  auto* base = static_cast<std::byte*>(region.data());
  auto* header = new (base) detail::ShmHeader{};
  header->magic = detail::kShmMagic;
  header->object_size = sizeof(T);
  header->object_align = alignof(T);
  T* obj = new (base + detail::object_offset<T>()) T{};
  header->ready.store(1, std::memory_order_release);
  return obj;
}

// Finds the T another process created, waiting up to `timeout` for it to be
// constructed. Throws if the region holds a different type.
template <typename T>
T* attach_shared(ShmRegion& region,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) {
  if (region.size() < shared_size<T>()) throw std::runtime_error("shm region too small");
  auto* base = static_cast<std::byte*>(region.data());
  auto* header = std::launder(reinterpret_cast<detail::ShmHeader*>(base));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (header->ready.load(std::memory_order_acquire) != 1) {
    if (std::chrono::steady_clock::now() > deadline) {
      throw std::runtime_error("timed out waiting for shm object in " + region.name());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  if (header->magic != detail::kShmMagic || header->object_size != sizeof(T) ||
      header->object_align != alignof(T)) {
    throw std::runtime_error("shm region " + region.name() + " holds a different type");
  }
  return std::launder(reinterpret_cast<T*>(base + detail::object_offset<T>()));
}

}  // namespace lab
