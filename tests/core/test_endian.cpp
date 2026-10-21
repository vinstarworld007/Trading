#include <gtest/gtest.h>

#include "lab/core/endian.hpp"

namespace {

TEST(Endian, LoadsBigEndian) {
  const unsigned char b[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  EXPECT_EQ(lab::load_be16(b), 0x0102u);
  EXPECT_EQ(lab::load_be32(b), 0x01020304u);
  EXPECT_EQ(lab::load_be48(b), 0x010203040506ULL);
  EXPECT_EQ(lab::load_be64(b), 0x0102030405060708ULL);
}

TEST(Endian, LoadsFromUnalignedAddress) {
  const unsigned char b[] = {0xff, 0x00, 0x00, 0x00, 0x2a};
  EXPECT_EQ(lab::load_be32(b + 1), 42u);
}

TEST(Endian, StoreRoundTrips) {
  unsigned char b[8] = {};
  lab::store_be16(b, 0xbeef);
  EXPECT_EQ(lab::load_be16(b), 0xbeefu);
  lab::store_be32(b, 0xdeadbeef);
  EXPECT_EQ(lab::load_be32(b), 0xdeadbeefu);
  // ITCH timestamp: nanoseconds since midnight, e.g. 15:59:59.999999999.
  const std::uint64_t ts = 57'599'999'999'999ULL;
  lab::store_be48(b, ts);
  EXPECT_EQ(lab::load_be48(b), ts);
  lab::store_be64(b, 0x0102030405060708ULL);
  EXPECT_EQ(b[0], 0x01);
  EXPECT_EQ(b[7], 0x08);
}

}  // namespace
