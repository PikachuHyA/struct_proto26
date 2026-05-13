// Tests for proto3 sint32 / sint64 (zigzag-encoded varint).

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "proto3.hpp"

namespace {

std::string hex(std::string_view s) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

#define EXPECT_WIRE(actual, expected_hex) \
    EXPECT_EQ(hex(actual), std::string(expected_hex))

struct Signed {
    [[= proto3::zigzag]] std::int32_t a;  // field 1, sint32 on the wire
    [[= proto3::zigzag]] std::int64_t b;  // field 2, sint64 on the wire
};

struct PackedSints {
    [[= proto3::zigzag]] std::vector<std::int32_t> xs;  // field 1, packed sint32
};

}  // namespace

TEST(Sint, AllZeroOmitsBothFields) {
    // proto3 default-skip applies to sint as well.
    Signed s{0, 0};
    EXPECT_EQ(proto3::serialize(s).size(), 0u);
}

TEST(Sint, NegativeOneIsSingleByte) {
    // zigzag(-1) = 1.  field 1 VARINT tag = 0x08, varint 0x01.
    Signed s{-1, 0};
    EXPECT_WIRE(proto3::serialize(s), "0801");
}

TEST(Sint, PositiveOneIsTwoOnTheWire) {
    // zigzag(1) = 2.
    Signed s{1, 0};
    EXPECT_WIRE(proto3::serialize(s), "0802");
}

TEST(Sint, NegativeTwoIsThreeOnTheWire) {
    // zigzag(-2) = 3.
    Signed s{-2, 0};
    EXPECT_WIRE(proto3::serialize(s), "0803");
}

TEST(Sint, Int32MaxFiveByteVarint) {
    // zigzag(INT32_MAX) = 0xFFFFFFFE -> varint fe ff ff ff 0f
    Signed s{std::numeric_limits<std::int32_t>::max(), 0};
    EXPECT_WIRE(proto3::serialize(s), "08feffffff0f");
}

TEST(Sint, Int32MinFiveByteVarint) {
    // zigzag(INT32_MIN) = 0xFFFFFFFF -> varint ff ff ff ff 0f
    Signed s{std::numeric_limits<std::int32_t>::min(), 0};
    EXPECT_WIRE(proto3::serialize(s), "08ffffffff0f");
}

TEST(Sint, Sint64MinIsTenByteVarint) {
    // zigzag(INT64_MIN) = 0xFFFFFFFFFFFFFFFF -> 10-byte varint.
    Signed s{0, std::numeric_limits<std::int64_t>::min()};
    // tag for field 2 VARINT = 0x10, then 10 bytes of 0xFF... 0x01.
    EXPECT_WIRE(proto3::serialize(s), "10ffffffffffffffffff01");
}

TEST(Sint, RoundTripBoundary) {
    Signed s{
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int64_t>::max(),
    };
    auto bytes = proto3::serialize(s);
    auto back = proto3::deserialize<Signed>(bytes);
    EXPECT_EQ(back.a, std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(back.b, std::numeric_limits<std::int64_t>::max());
}

TEST(Sint, PackedRepeatedSint32) {
    // {-1, 1, -2, 2}  zigzag-> {1, 2, 3, 4}.  All single-byte varints.
    // field 1 LEN tag 0x0a, length 4, bytes 01 02 03 04.
    PackedSints p{{-1, 1, -2, 2}};
    EXPECT_WIRE(proto3::serialize(p), "0a0401020304");

    auto back = proto3::deserialize<PackedSints>(proto3::serialize(p));
    ASSERT_EQ(back.xs.size(), 4u);
    EXPECT_EQ(back.xs[0], -1);
    EXPECT_EQ(back.xs[1], 1);
    EXPECT_EQ(back.xs[2], -2);
    EXPECT_EQ(back.xs[3], 2);
}

// Sanity check: an int field WITHOUT [[= proto3::zigzag]] still uses plain
// (sign-extended) varint, not zigzag.
TEST(Sint, WithoutAnnotationIsPlainVarint) {
    struct Plain { std::int32_t a; };
    Plain p{-1};
    // -1 sign-extended to 64-bit varint = 10 bytes of 0xFF + 0x01.
    EXPECT_WIRE(proto3::serialize(p), "08ffffffffffffffffff01");
}
