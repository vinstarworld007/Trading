#include "lab/core/shm.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace lab {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::system_category(), what);
}

std::string shm_path(const std::string& name) {
  return name.empty() || name.front() != '/' ? "/" + name : name;
}

void* map_fd(int fd, std::size_t bytes, const ShmRegion::Options& opts, const std::string& name) {
  int flags = MAP_SHARED;
  if (opts.populate) flags |= MAP_POPULATE;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
  if (p == MAP_FAILED) throw_errno("mmap " + name);
  if (opts.huge_pages) madvise(p, bytes, MADV_HUGEPAGE);  // a hint; ignore failure
  if (opts.lock && mlock(p, bytes) != 0) {
    const int err = errno;
    munmap(p, bytes);
    errno = err;
    throw_errno("mlock " + name);
  }
  return p;
}

}  // namespace

ShmRegion ShmRegion::create(const std::string& name, std::size_t bytes, Options opts) {
  const std::string path = shm_path(name);
  const int fd = shm_open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0660);
  if (fd < 0) throw_errno("shm_open " + path);
  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    const int err = errno;
    close(fd);
    errno = err;
    throw_errno("ftruncate " + path);
  }
  void* p = nullptr;
  try {
    p = map_fd(fd, bytes, opts, path);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
  return ShmRegion{path, p, bytes};
}

ShmRegion ShmRegion::open(const std::string& name, Options opts) {
  const std::string path = shm_path(name);
  const int fd = shm_open(path.c_str(), O_RDWR, 0);
  if (fd < 0) throw_errno("shm_open " + path);
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    const int err = errno;
    close(fd);
    errno = err;
    throw_errno("fstat " + path);
  }
  const auto bytes = static_cast<std::size_t>(st.st_size);
  void* p = nullptr;
  try {
    p = map_fd(fd, bytes, opts, path);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
  return ShmRegion{path, p, bytes};
}

void ShmRegion::unlink(const std::string& name) noexcept { shm_unlink(shm_path(name).c_str()); }

ShmRegion::ShmRegion(ShmRegion&& other) noexcept
    : name_{std::move(other.name_)}, data_{other.data_}, size_{other.size_} {
  other.data_ = nullptr;
  other.size_ = 0;
}

ShmRegion& ShmRegion::operator=(ShmRegion&& other) noexcept {
  if (this != &other) {
    reset();
    name_ = std::move(other.name_);
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

ShmRegion::~ShmRegion() { reset(); }

void ShmRegion::reset() noexcept {
  if (data_ != nullptr) munmap(data_, size_);
  data_ = nullptr;
  size_ = 0;
}

}  // namespace lab
