// Tests for proto3 map<K, V> via std::map / std::unordered_map.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

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

struct Sub {
    std::int32_t code;  // field 1
    std::string note;   // field 2
};

struct WithStringIntMap {
    std::map<std::string, std::int32_t> attrs;  // field 1
};

struct WithIntStringMap {
    std::unordered_map<std::int32_t, std::string> labels;  // field 1
};

struct WithMessageMap {
    std::map<std::int32_t, Sub> subs;  // field 1
};

}  // namespace

TEST(Map, EmptyMapEmitsNothing) {
    WithStringIntMap m{};
    EXPECT_EQ(proto3::serialize(m).size(), 0u);
}

TEST(Map, StringIntStableOrderHexCheck) {
    // std::map iterates in key order, so we can hex-check.
    // Entry "a"->7  : key field 1 LEN 'a' = 0a 01 61, val field 2 VARINT 7 = 10 07
    //   contents 5 bytes; outer tag 0a (field 1 LEN), len 05.
    //   -> 0a 05 0a 01 61 10 07
    // Entry "b"->8  : analogous -> 0a 05 0a 01 62 10 08
    WithStringIntMap m{{{"a", 7}, {"b", 8}}};
    EXPECT_WIRE(proto3::serialize(m),
                "0a050a016110070a050a01621008");
}

TEST(Map, RoundTripStringInt) {
    WithStringIntMap m{{{"alpha", 1}, {"beta", 2}, {"gamma", 3}}};
    auto bytes = proto3::serialize(m);
    auto back = proto3::deserialize<WithStringIntMap>(bytes);
    EXPECT_EQ(back.attrs.size(), 3u);
    EXPECT_EQ(back.attrs["alpha"], 1);
    EXPECT_EQ(back.attrs["beta"],  2);
    EXPECT_EQ(back.attrs["gamma"], 3);
}

TEST(Map, RoundTripUnorderedIntString) {
    // Iteration order isn't guaranteed for unordered_map, so just round-trip.
    WithIntStringMap m{};
    m.labels[1]  = "one";
    m.labels[42] = "answer";
    m.labels[7]  = "lucky";
    auto bytes = proto3::serialize(m);
    auto back = proto3::deserialize<WithIntStringMap>(bytes);
    EXPECT_EQ(back.labels.size(), 3u);
    EXPECT_EQ(back.labels[1],  "one");
    EXPECT_EQ(back.labels[42], "answer");
    EXPECT_EQ(back.labels[7],  "lucky");
}

TEST(Map, RoundTripMessageValue) {
    WithMessageMap m{};
    m.subs[10] = Sub{100, "hello"};
    m.subs[20] = Sub{200, "world"};
    auto bytes = proto3::serialize(m);
    auto back = proto3::deserialize<WithMessageMap>(bytes);
    ASSERT_EQ(back.subs.size(), 2u);
    EXPECT_EQ(back.subs[10].code, 100);
    EXPECT_EQ(back.subs[10].note, "hello");
    EXPECT_EQ(back.subs[20].code, 200);
    EXPECT_EQ(back.subs[20].note, "world");
}

TEST(Map, LaterEntryOverwritesEarlier) {
    // Hand-build a wire stream with two entries for the same key. The proto3
    // map contract is "last write wins".
    // entry "k"->1  contents = 0a 01 6b 10 01 (5 bytes)  outer = 0a 05 ...
    // entry "k"->99 contents = 0a 01 6b 10 63 (5 bytes)
    std::string bytes;
    bytes += '\x0a'; bytes += '\x05';
    bytes += '\x0a'; bytes += '\x01'; bytes += 'k';
    bytes += '\x10'; bytes += '\x01';
    bytes += '\x0a'; bytes += '\x05';
    bytes += '\x0a'; bytes += '\x01'; bytes += 'k';
    bytes += '\x10'; bytes += '\x63';

    auto back = proto3::deserialize<WithStringIntMap>(bytes);
    ASSERT_EQ(back.attrs.size(), 1u);
    EXPECT_EQ(back.attrs["k"], 99);
}

TEST(Map, EntryWithDefaultedValueDecodesAsZero) {
    // Hand-build an entry that omits the value field. proto3 map decoders
    // must default-initialize missing key/value fields.
    // contents = 0a 01 7a (just key "z")  -> outer = 0a 03 0a 01 7a
    std::string bytes;
    bytes += '\x0a'; bytes += '\x03';
    bytes += '\x0a'; bytes += '\x01'; bytes += 'z';

    auto back = proto3::deserialize<WithStringIntMap>(bytes);
    ASSERT_EQ(back.attrs.size(), 1u);
    EXPECT_EQ(back.attrs["z"], 0);
}
