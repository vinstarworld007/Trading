#pragma once

// Reads Nasdaq ITCH "BinaryFILE" captures, the format of the sample days at
// emi.nasdaq.com: each message is preceded by a 2-byte big-endian length, with
// no packet or session layer. Reads .gz directly (zlib), or plain files.
//
//   lab::itch::FileReader reader{"01302019.NASDAQ_ITCH50.gz"};
//   while (auto msg = reader.next()) dispatch(msg->data(), msg->size(), handler);
//   if (!reader.ok()) ... reader.error() ...
//
// The span returned by next() is valid until the following call.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lab::itch {

class FileReader {
 public:
  // `buffer_bytes` is the decompressed read-ahead; the default suits full days.
  explicit FileReader(const std::string& path, std::size_t buffer_bytes = 8u << 20);
  ~FileReader();
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  // Next message, or nullopt at end of file or on error (check ok()).
  std::optional<std::span<const std::byte>> next();

  bool ok() const noexcept { return error_.empty(); }
  const std::string& error() const noexcept { return error_; }

  std::uint64_t messages_read() const noexcept { return messages_; }
  // Decompressed bytes consumed so far, including length prefixes.
  std::uint64_t bytes_read() const noexcept { return bytes_; }

 private:
  bool fill(std::size_t need);

  void* gz_ = nullptr;  // gzFile; kept opaque so callers don't need zlib.h
  std::vector<std::byte> buf_;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  bool eof_ = false;
  std::string error_;
  std::uint64_t messages_ = 0;
  std::uint64_t bytes_ = 0;
};

// Writes the same format, for tests and the synthetic day generator. A path
// ending in .gz is gzip-compressed.
class FileWriter {
 public:
  explicit FileWriter(const std::string& path);
  ~FileWriter();
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;

  bool write(std::span<const std::byte> message);
  bool close();
  bool ok() const noexcept { return error_.empty(); }
  const std::string& error() const noexcept { return error_; }

 private:
  void* gz_ = nullptr;
  std::string error_;
};

}  // namespace lab::itch
