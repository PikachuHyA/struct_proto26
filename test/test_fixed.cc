// Tests for proto3 fixed32 / sfixed32 / fixed64 / sfixed64 (fixed-width wire).

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

struct Fixed32Msg {
    [[= proto3::fixed]] std::uint32_t hash;  // field 1, fixed32
};

struct Fixed64Msg {
    [[= proto3::fixed]] std::uint64_t big;   // field 1, fixed64
};

struct SFixed32Msg {
    [[= proto3::fixed]] std::int32_t v;      // field 1, sfixed32
};

struct SFixed64Msg {
    [[= proto3::fixed]] std::int64_t v;      // field 1, sfixed64
};

struct PackedFixed32 {
    [[= proto3::fixed]] std::vector<std::uint32_t> xs;  // field 1, packed
};

struct PackedSFixed64 {
    [[= proto3::fixed]] std::vector<std::int64_t> xs;   // field 1, packed
};

}  // namespace

TEST(Fixed, Fixed32SingleValue) {
    // tag (1<<3)|I32 = 0x0d; 0x12345678 little-endian = 78 56 34 12.
    Fixed32Msg m{0x12345678u};
    EXPECT_WIRE(proto3::serialize(m), "0d78563412");
}

TEST(Fixed, Fixed64SingleValue) {
    // tag (1<<3)|I64 = 0x09; 0x1122334455667788 LE = 88 77 66 55 44 33 22 11.
    Fixed64Msg m{0x1122334455667788ull};
    EXPECT_WIRE(proto3::serialize(m), "098877665544332211");
}

TEST(Fixed, SFixed32NegativeOneIsAllFFs) {
    // sfixed32 -1 = bit pattern 0xFFFFFFFF — verifies signed bit_cast path.
    // Crucially, this is 4 bytes — a plain int32 -1 would be a 10-byte varint.
    SFixed32Msg m{-1};
    EXPECT_WIRE(proto3::serialize(m), "0dffffffff");
}

TEST(Fixed, SFixed64NegativeOneIsEightBytes) {
    SFixed64Msg m{-1};
    EXPECT_WIRE(proto3::serialize(m), "09ffffffffffffffff");
}

TEST(Fixed, ZeroIsOmittedDefaultSkip) {
    // proto3 default-skip applies to fixed-width fields too.
    Fixed32Msg a{0u};
    SFixed64Msg b{0};
    EXPECT_EQ(proto3::serialize(a).size(), 0u);
    EXPECT_EQ(proto3::serialize(b).size(), 0u);
}

TEST(Fixed, RoundTripBoundary) {
    // Round-trip the extremes of every supported width / signedness.
    Fixed32Msg a{std::numeric_limits<std::uint32_t>::max()};
    EXPECT_EQ(proto3::deserialize<Fixed32Msg>(proto3::serialize(a)).hash,
              std::numeric_limits<std::uint32_t>::max());

    Fixed64Msg b{std::numeric_limits<std::uint64_t>::max()};
    EXPECT_EQ(proto3::deserialize<Fixed64Msg>(proto3::serialize(b)).big,
              std::numeric_limits<std::uint64_t>::max());

    SFixed32Msg c{std::numeric_limits<std::int32_t>::min()};
    EXPECT_EQ(proto3::deserialize<SFixed32Msg>(proto3::serialize(c)).v,
              std::numeric_limits<std::int32_t>::min());

    SFixed64Msg d{std::numeric_limits<std::int64_t>::min()};
    EXPECT_EQ(proto3::deserialize<SFixed64Msg>(proto3::serialize(d)).v,
              std::numeric_limits<std::int64_t>::min());
}

TEST(Fixed, PackedFixed32) {
    // field 1 LEN tag 0x0a; 3 elements * 4 bytes = 12 -> varint 0x0c.
    // payload: 01 00 00 00, 02 00 00 00, 03 00 00 00.
    PackedFixed32 p{{1u, 2u, 3u}};
    EXPECT_WIRE(proto3::serialize(p), "0a0c010000000200000003000000");

    auto back = proto3::deserialize<PackedFixed32>(proto3::serialize(p));
    ASSERT_EQ(back.xs.size(), 3u);
    EXPECT_EQ(back.xs[0], 1u);
    EXPECT_EQ(back.xs[1], 2u);
    EXPECT_EQ(back.xs[2], 3u);
}

TEST(Fixed, PackedSFixed64) {
    // {-1, 7}: 2 * 8 = 16 bytes -> length varint 0x10.
    // -1 LE = ff ff ff ff ff ff ff ff; 7 LE = 07 00 00 00 00 00 00 00.
    PackedSFixed64 p{{-1, 7}};
    EXPECT_WIRE(proto3::serialize(p),
                "0a10ffffffffffffffff0700000000000000");

    auto back = proto3::deserialize<PackedSFixed64>(proto3::serialize(p));
    ASSERT_EQ(back.xs.size(), 2u);
    EXPECT_EQ(back.xs[0], -1);
    EXPECT_EQ(back.xs[1], 7);
}

TEST(Fixed, EmptyVectorOmits) {
    PackedFixed32 p{};
    EXPECT_EQ(proto3::serialize(p).size(), 0u);
}

// Sanity: an int32 field WITHOUT [[= proto3::fixed]] still uses varint
// (sign-extended, as covered in test_sint.cc). This test confirms the same
// type encodes very differently with vs without the annotation.
TEST(Fixed, WithoutAnnotationIsPlainVarint) {
    struct Plain { std::int32_t v; };
    Plain p{1};
    // varint tag 0x08 + varint 0x01 = 2 bytes total; fixed would be 5 bytes.
    EXPECT_WIRE(proto3::serialize(p), "0801");
    EXPECT_EQ(proto3::serialize(p).size(), 2u);
    EXPECT_EQ(proto3::serialize(SFixed32Msg{1}).size(), 5u);
}
