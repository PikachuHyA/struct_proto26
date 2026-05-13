// Tests for proto3 oneof.

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

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

struct OneofSub {
    std::int32_t code;  // field 1
    std::string note;   // field 2
};

// proto-equivalent:
//   message Resp {
//     int32 id = 1;
//     oneof result {
//       int32      ok    = 5;
//       string     error = 6;
//       OneofSub   detail = 7;
//     }
//   }
struct Resp {
    std::int32_t id;  // field 1

    [[= proto3::oneof<5, 6, 7>]]
    std::variant<std::monostate, std::int32_t, std::string, OneofSub> result;
};

}  // namespace

TEST(Oneof, UnsetEmitsNothingForOneof) {
    Resp r{};
    r.id = 100;
    auto bytes = proto3::serialize(r);
    // Only id is on the wire: tag 0x08, value 0x64.
    EXPECT_WIRE(bytes, "0864");
}

TEST(Oneof, IntAlternativeUsesItsOwnFieldNumber) {
    Resp r{};
    r.id = 100;
    r.result = std::int32_t{42};
    // id "0864" + field 5 VARINT 42 -> tag (5<<3)=0x28, value 0x2a.
    EXPECT_WIRE(proto3::serialize(r), "0864282a");
}

TEST(Oneof, StringAlternative) {
    Resp r{};
    r.result = std::string("hi");
    // field 6 LEN tag 0x32, len 2, "hi" = 68 69.
    EXPECT_WIRE(proto3::serialize(r), "32026869");
}

TEST(Oneof, MessageAlternative) {
    Resp r{};
    r.result = OneofSub{7, "x"};
    // OneofSub bytes: field 1 VARINT 7 (08 07), field 2 LEN "x" (12 01 78)
    //   -> 5 bytes "0807120178".
    // Outer: field 7 LEN tag (7<<3)|2 = 0x3a, length 5, then payload.
    EXPECT_WIRE(proto3::serialize(r), "3a050807120178");
}

TEST(Oneof, RoundTripIntAlt) {
    Resp r{};
    r.id = 9;
    r.result = std::int32_t{-3};
    auto back = proto3::deserialize<Resp>(proto3::serialize(r));
    EXPECT_EQ(back.id, 9);
    ASSERT_NE(back.result.index(), 0u);
    EXPECT_TRUE(std::holds_alternative<std::int32_t>(back.result));
    EXPECT_EQ(std::get<std::int32_t>(back.result), -3);
}

TEST(Oneof, RoundTripStringAlt) {
    Resp r{};
    r.result = std::string("error!");
    auto back = proto3::deserialize<Resp>(proto3::serialize(r));
    EXPECT_EQ(back.id, 0);
    ASSERT_TRUE(std::holds_alternative<std::string>(back.result));
    EXPECT_EQ(std::get<std::string>(back.result), "error!");
}

TEST(Oneof, RoundTripMessageAlt) {
    Resp r{};
    r.id = 1;
    r.result = OneofSub{42, "ok"};
    auto back = proto3::deserialize<Resp>(proto3::serialize(r));
    EXPECT_EQ(back.id, 1);
    ASSERT_TRUE(std::holds_alternative<OneofSub>(back.result));
    EXPECT_EQ(std::get<OneofSub>(back.result).code, 42);
    EXPECT_EQ(std::get<OneofSub>(back.result).note, "ok");
}

TEST(Oneof, IntAltWithDefaultValueStillCarriesPresence) {
    // proto3 oneof preserves "set to default" — int32 = 0 inside a oneof
    // must still appear on the wire.
    Resp r{};
    r.result = std::int32_t{0};
    auto bytes = proto3::serialize(r);
    EXPECT_WIRE(bytes, "2800");  // field 5 VARINT, value 0

    auto back = proto3::deserialize<Resp>(bytes);
    ASSERT_TRUE(std::holds_alternative<std::int32_t>(back.result));
    EXPECT_EQ(std::get<std::int32_t>(back.result), 0);
}

TEST(Oneof, LaterTagWinsOnDecode) {
    // Hand-craft a wire that carries two oneof members; the last one wins.
    // field 5 VARINT 1 -> 28 01
    // field 6 LEN "later" -> 32 05 "later"
    std::string bytes;
    bytes += '\x28'; bytes += '\x01';
    bytes += '\x32'; bytes += '\x05';
    bytes += 'l'; bytes += 'a'; bytes += 't'; bytes += 'e'; bytes += 'r';

    auto back = proto3::deserialize<Resp>(bytes);
    ASSERT_TRUE(std::holds_alternative<std::string>(back.result));
    EXPECT_EQ(std::get<std::string>(back.result), "later");
}

TEST(Oneof, ChangingAltClearsPrevious) {
    Resp r{};
    r.result = std::int32_t{42};
    EXPECT_TRUE(std::holds_alternative<std::int32_t>(r.result));
    r.result = std::string("now string");
    EXPECT_FALSE(std::holds_alternative<std::int32_t>(r.result));
    ASSERT_TRUE(std::holds_alternative<std::string>(r.result));
    EXPECT_EQ(std::get<std::string>(r.result), "now string");
}

TEST(Oneof, VisitWorksOnDeserializedVariant) {
    // Sanity check that the deserialized field is a plain std::variant the
    // user can drive with std::visit — no wrapper API in the way.
    Resp r{};
    r.result = OneofSub{55, "v"};
    auto back = proto3::deserialize<Resp>(proto3::serialize(r));

    int code = std::visit(
        [](const auto& x) -> int {
            using X = std::remove_cvref_t<decltype(x)>;
            if constexpr (std::is_same_v<X, OneofSub>) return x.code;
            else return -1;
        },
        back.result);
    EXPECT_EQ(code, 55);
}
