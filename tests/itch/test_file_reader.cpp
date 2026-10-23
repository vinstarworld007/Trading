#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "lab/itch/codec.hpp"
#include "lab/itch/file_reader.hpp"

namespace {

using namespace lab::itch;

std::string temp_path(const char* suffix) {
  return "/tmp/lab-itch-test-" + std::to_string(getpid()) + suffix;
}

void write_day(const std::string& path, std::uint64_t count) {
  FileWriter w{path};
  std::array<std::byte, kMaxMessageSize> buf{};
  for (std::uint64_t i = 0; i < count; ++i) {
    AddOrder a;
    a.order_reference_number = i;
    a.shares = static_cast<std::uint32_t>(i % 1000);
    encode(a, buf.data());
    ASSERT_TRUE(w.write({buf.data(), AddOrder::kSize}));
    if (i % 7 == 0) {  // mix in a different length
      OrderDelete d;
      d.order_reference_number = i;
      encode(d, buf.data());
      ASSERT_TRUE(w.write({buf.data(), OrderDelete::kSize}));
    }
  }
  ASSERT_TRUE(w.close()) << w.error();
}

void read_back(const std::string& path, std::uint64_t count) {
  FileReader r{path, 1};  // smallest buffer, so messages straddle refills
  ASSERT_TRUE(r.ok()) << r.error();
  std::uint64_t adds = 0, deletes = 0;
  while (auto msg = r.next()) {
    if (static_cast<char>((*msg)[0]) == 'A') {
      ASSERT_EQ(msg->size(), AddOrder::kSize);
      AddOrder a;
      decode(msg->data(), a);
      ASSERT_EQ(a.order_reference_number, adds);
      ASSERT_EQ(a.shares, adds % 1000);
      ++adds;
    } else {
      ASSERT_EQ(msg->size(), OrderDelete::kSize);
      ++deletes;
    }
  }
  EXPECT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(adds, count);
  EXPECT_EQ(deletes, (count + 6) / 7);
  EXPECT_EQ(r.messages_read(), adds + deletes);
}

TEST(FileReader, ReadsGzipWithMessagesAcrossBufferRefills) {
  const auto path = temp_path(".gz");
  write_day(path, 20'000);  // ~800 KB decompressed, several buffer refills
  read_back(path, 20'000);
  std::remove(path.c_str());
}

TEST(FileReader, ReadsUncompressedFiles) {
  const auto path = temp_path(".bin");
  write_day(path, 5'000);
  read_back(path, 5'000);
  std::remove(path.c_str());
}

TEST(FileReader, ReportsTruncatedLastMessage) {
  const auto path = temp_path(".trunc");
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const unsigned char data[] = {0x00, 0x24, 'A', 0x00, 0x01};  // claims 36 bytes, has 3
  std::fwrite(data, 1, sizeof data, f);
  std::fclose(f);
  FileReader r{path};
  EXPECT_FALSE(r.next().has_value());
  EXPECT_FALSE(r.ok());
  EXPECT_NE(r.error().find("truncated"), std::string::npos);
  std::remove(path.c_str());
}

TEST(FileReader, MissingFileIsAnError) {
  FileReader r{"/nonexistent/day.gz"};
  EXPECT_FALSE(r.ok());
  EXPECT_FALSE(r.next().has_value());
}

}  // namespace
