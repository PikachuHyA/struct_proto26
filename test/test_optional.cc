// Tests for proto3 explicit presence via std::optional<T>.
//
// Proto3 default-skip means a bare `int32 x = 1;` set to 0 is omitted on
// the wire — there's no way for the reader to distinguish "unset" from
// "explicitly zero". Wrapping the field in std::optional<T> opts into
// proto3 explicit presence: unset is omitted, set is always emitted (even
// at the language default value).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

struct OptInt {
    std::optional<std::int32_t> n;  // field 1
};

struct OptString {
    std::optional<std::string> s;  // field 1
};

struct Inner {
    std::int32_t code;  // field 1
    std::string note;   // field 2
};

struct OptMessage {
    std::optional<Inner> sub;  // field 1
};

struct Mixed {
    std::int32_t                id;       // field 1, plain (default-skip)
    std::optional<std::int32_t> page;     // field 2, explicit presence
    std::string                 query;    // field 3, plain
};

}  // namespace

TEST(Optional, UnsetOmits) {
    OptInt    a{};
    OptString b{};
    OptMessage c{};
    EXPECT_EQ(proto3::serialize(a).size(), 0u);
    EXPECT_EQ(proto3::serialize(b).size(), 0u);
    EXPECT_EQ(proto3::serialize(c).size(), 0u);
}

// Load-bearing: the whole reason std::optional exists in this library.
// A plain int32 = 0 vanishes; an optional<int32> SET to 0 must appear.
TEST(Optional, SetToDefaultIntStillEmits) {
    OptInt a{std::int32_t{0}};
    // field 1 VARINT tag 0x08, value 0x00.
    EXPECT_WIRE(proto3::serialize(a), "0800");

    auto back = proto3::deserialize<OptInt>(proto3::serialize(a));
    ASSERT_TRUE(back.n.has_value());
    EXPECT_EQ(*back.n, 0);
}

TEST(Optional, SetToEmptyStringStillEmits) {
    OptString a{std::string{}};
    // field 1 LEN tag 0x0a, length 0.
    EXPECT_WIRE(proto3::serialize(a), "0a00");

    auto back = proto3::deserialize<OptString>(proto3::serialize(a));
    ASSERT_TRUE(back.s.has_value());
    EXPECT_EQ(*back.s, "");
}

TEST(Optional, SetToDefaultMessageStillEmits) {
    // optional<Inner> set to a default-constructed Inner. All sub-fields
    // are at their proto3 defaults so the inner serializes to 0 bytes,
    // but the outer field MUST still appear as a 0-length LEN.
    OptMessage a{Inner{}};
    EXPECT_WIRE(proto3::serialize(a), "0a00");

    auto back = proto3::deserialize<OptMessage>(proto3::serialize(a));
    ASSERT_TRUE(back.sub.has_value());
    EXPECT_EQ(back.sub->code, 0);
    EXPECT_EQ(back.sub->note, "");
}

TEST(Optional, NonDefaultRoundTrip) {
    OptInt a{std::int32_t{42}};
    OptString b{std::string{"hi"}};
    OptMessage c{Inner{7, "hello"}};

    EXPECT_EQ(*proto3::deserialize<OptInt>(proto3::serialize(a)).n, 42);
    EXPECT_EQ(*proto3::deserialize<OptString>(proto3::serialize(b)).s, "hi");

    auto back_c = proto3::deserialize<OptMessage>(proto3::serialize(c));
    ASSERT_TRUE(back_c.sub.has_value());
    EXPECT_EQ(back_c.sub->code, 7);
    EXPECT_EQ(back_c.sub->note, "hello");
}

TEST(Optional, MixedWithPlainFields) {
    // id = 0 (default, omitted), page = 0 (optional set, emitted), query "go".
    Mixed m{0, std::int32_t{0}, "go"};
    // page: tag (2<<3)|VARINT = 0x10, value 0x00 -> "1000"
    // query: tag (3<<3)|LEN = 0x1a, len 2, "go" 6f7  -> "1a02676f"
    EXPECT_WIRE(proto3::serialize(m), "10001a02676f");

    auto back = proto3::deserialize<Mixed>(proto3::serialize(m));
    EXPECT_EQ(back.id, 0);
    ASSERT_TRUE(back.page.has_value());
    EXPECT_EQ(*back.page, 0);
    EXPECT_EQ(back.query, "go");
}

TEST(Optional, OmittedOnWireDecodesAsNullopt) {
    // A wire stream that carries only field 3 — page (field 2) is missing.
    // A non-optional decoder would default-init to 0, but the optional
    // decoder must leave it as nullopt to signal "the sender didn't set it".
    std::string bytes;
    bytes += '\x1a'; bytes += '\x02'; bytes += 'g'; bytes += 'o';

    auto back = proto3::deserialize<Mixed>(bytes);
    EXPECT_EQ(back.id, 0);
    EXPECT_FALSE(back.page.has_value());
    EXPECT_EQ(back.query, "go");
}

TEST(Optional, LaterSetWinsOnDecode) {
    // Two encodings of field 1 in sequence: should end up as the second.
    // first: optional<int32> = 5 -> "0805"
    // then : optional<int32> = 9 -> "0809"
    std::string bytes;
    bytes += '\x08'; bytes += '\x05';
    bytes += '\x08'; bytes += '\x09';

    auto back = proto3::deserialize<OptInt>(bytes);
    ASSERT_TRUE(back.n.has_value());
    EXPECT_EQ(*back.n, 9);
}

// ---- Optional combined with [[= proto3::zigzag]] ----

namespace {
struct OptZigzag {
    [[= proto3::zigzag]] std::optional<std::int32_t> delta;  // field 1, sint32
};
}  // namespace

TEST(Optional, ZigzagSetToZeroStillEmits) {
    // Without optional, sint32 = 0 default-skips and the wire is empty.
    // With optional, presence is explicit: zigzag(0) = 0, emit as "0800".
    OptZigzag a{std::int32_t{0}};
    EXPECT_WIRE(proto3::serialize(a), "0800");

    auto back = proto3::deserialize<OptZigzag>(proto3::serialize(a));
    ASSERT_TRUE(back.delta.has_value());
    EXPECT_EQ(*back.delta, 0);
}

TEST(Optional, ZigzagNegativeOneRoundTrip) {
    // zigzag(-1) = 1 -> wire "0801".
    OptZigzag a{std::int32_t{-1}};
    EXPECT_WIRE(proto3::serialize(a), "0801");

    auto back = proto3::deserialize<OptZigzag>(proto3::serialize(a));
    ASSERT_TRUE(back.delta.has_value());
    EXPECT_EQ(*back.delta, -1);
}

TEST(Optional, ZigzagUnsetOmits) {
    OptZigzag a{};
    EXPECT_EQ(proto3::serialize(a).size(), 0u);
}

// ---- Optional combined with [[= proto3::fixed]] ----

namespace {
struct OptFixed32 {
    [[= proto3::fixed]] std::optional<std::int32_t> v;  // field 1, sfixed32
};
struct OptFixed64 {
    [[= proto3::fixed]] std::optional<std::int64_t> v;  // field 1, sfixed64
};
}  // namespace

TEST(Optional, FixedSetToZeroStillEmits) {
    // sfixed32 explicitly set to 0 must emit 5 bytes (tag + 4 zero bytes).
    OptFixed32 a{std::int32_t{0}};
    EXPECT_WIRE(proto3::serialize(a), "0d00000000");

    auto back = proto3::deserialize<OptFixed32>(proto3::serialize(a));
    ASSERT_TRUE(back.v.has_value());
    EXPECT_EQ(*back.v, 0);
}

TEST(Optional, FixedNegativeOneRoundTrip) {
    // sfixed64 -1 = bit pattern 0xFFFFFFFFFFFFFFFF, 8 bytes LE.
    // tag (1<<3)|I64 = 0x09.
    OptFixed64 a{std::int64_t{-1}};
    EXPECT_WIRE(proto3::serialize(a), "09ffffffffffffffff");

    auto back = proto3::deserialize<OptFixed64>(proto3::serialize(a));
    ASSERT_TRUE(back.v.has_value());
    EXPECT_EQ(*back.v, -1);
}

TEST(Optional, FixedUnsetOmits) {
    OptFixed32 a{};
    OptFixed64 b{};
    EXPECT_EQ(proto3::serialize(a).size(), 0u);
    EXPECT_EQ(proto3::serialize(b).size(), 0u);
}
