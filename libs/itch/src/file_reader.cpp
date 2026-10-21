#include "lab/itch/file_reader.hpp"

#include <zlib.h>

#include <climits>
#include <cstring>

#include "lab/core/endian.hpp"

namespace lab::itch {
namespace {

gzFile as_gz(void* p) { return static_cast<gzFile>(p); }

std::string gz_error(void* gz) {
  int errnum = 0;
  const char* msg = gzerror(as_gz(gz), &errnum);
  return msg != nullptr ? msg : "unknown zlib error";
}

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

FileReader::FileReader(const std::string& path, std::size_t buffer_bytes)
    : buf_(buffer_bytes < (1u << 17) ? (1u << 17) : buffer_bytes) {
  // gzopen reads plain files transparently.
  gz_ = gzopen(path.c_str(), "rb");
  if (gz_ == nullptr) {
    error_ = "cannot open " + path;
    return;
  }
  gzbuffer(as_gz(gz_), 1u << 20);
}

FileReader::~FileReader() {
  if (gz_ != nullptr) gzclose(as_gz(gz_));
}

bool FileReader::fill(std::size_t need) {
  // Move the unread tail to the front, then read until `need` bytes are buffered.
  if (pos_ > 0) {
    std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
    end_ -= pos_;
    pos_ = 0;
  }
  while (end_ < need && !eof_) {
    const std::size_t room = buf_.size() - end_;
    const unsigned ask = room > UINT_MAX ? UINT_MAX : static_cast<unsigned>(room);
    const int n = gzread(as_gz(gz_), buf_.data() + end_, ask);
    if (n < 0) {
      error_ = "read failed: " + gz_error(gz_);
      return false;
    }
    if (n == 0) eof_ = true;
    end_ += static_cast<std::size_t>(n);
  }
  return end_ >= need;
}

std::optional<std::span<const std::byte>> FileReader::next() {
  if (gz_ == nullptr || !error_.empty()) return std::nullopt;
  if (end_ - pos_ < 2 && !fill(2)) {
    if (error_.empty() && end_ != pos_) error_ = "truncated length prefix at end of file";
    return std::nullopt;
  }
  const std::size_t len = load_be16(buf_.data() + pos_);
  if (end_ - pos_ < 2 + len && !fill(2 + len)) {
    if (error_.empty()) error_ = "truncated message at end of file";
    return std::nullopt;
  }
  const std::byte* msg = buf_.data() + pos_ + 2;
  pos_ += 2 + len;
  ++messages_;
  bytes_ += 2 + len;
  return std::span<const std::byte>{msg, len};
}

FileWriter::FileWriter(const std::string& path) {
  gz_ = gzopen(path.c_str(), ends_with(path, ".gz") ? "wb6" : "wbT");
  if (gz_ == nullptr) error_ = "cannot open " + path + " for writing";
}

FileWriter::~FileWriter() { close(); }

bool FileWriter::write(std::span<const std::byte> message) {
  if (gz_ == nullptr || !error_.empty()) return false;
  if (message.size() > 0xffff) {
    error_ = "message longer than 65535 bytes";
    return false;
  }
  std::byte prefix[2];
  store_be16(prefix, static_cast<std::uint16_t>(message.size()));
  if (gzwrite(as_gz(gz_), prefix, 2) != 2 ||
      gzwrite(as_gz(gz_), message.data(), static_cast<unsigned>(message.size())) !=
          static_cast<int>(message.size())) {
    error_ = "write failed: " + gz_error(gz_);
    return false;
  }
  return true;
}

bool FileWriter::close() {
  if (gz_ == nullptr) return error_.empty();
  const int rc = gzclose(as_gz(gz_));
  gz_ = nullptr;
  if (rc != Z_OK && error_.empty()) error_ = "close failed";
  return error_.empty();
}

}  // namespace lab::itch
