// Test for:
//   syntax = "proto3";
//   message SearchRequest {
//     string query = 1;
//     int32  page_number = 2;
//     int32  results_per_page = 3;
//   }
//
// Build & run:
//   bazelisk test //:test_search_request

#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "proto3.hpp"

struct SearchRequest {
    std::string query;             // field 1
    std::int32_t page_number;      // field 2
    std::int32_t results_per_page; // field 3
};

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

}  // namespace

TEST(SearchRequest, RoundTripTypical) {
    SearchRequest req{"hello", 1, 10};
    std::string bytes = proto3::serialize(req);

    // Hand-computed proto3 wire:
    //   field 1, LEN, len=5, "hello" -> 0a 05 68 65 6c 6c 6f
    //   field 2, VARINT, 1           -> 10 01
    //   field 3, VARINT, 10          -> 18 0a
    EXPECT_WIRE(bytes, "0a0568656c6c6f1001180a");

    SearchRequest back = proto3::deserialize<SearchRequest>(bytes);
    EXPECT_EQ(back.query, "hello");
    EXPECT_EQ(back.page_number, 1);
    EXPECT_EQ(back.results_per_page, 10);
}

TEST(SearchRequest, AllDefaultsEmitNothing) {
    SearchRequest req{"", 0, 0};
    std::string bytes = proto3::serialize(req);

    // proto3: scalar fields equal to their default are omitted.
    EXPECT_EQ(bytes.size(), 0u);

    SearchRequest back = proto3::deserialize<SearchRequest>(bytes);
    EXPECT_EQ(back.query, "");
    EXPECT_EQ(back.page_number, 0);
    EXPECT_EQ(back.results_per_page, 0);
}

TEST(SearchRequest, PartialFields) {
    // Only query and results_per_page set; page_number = 0 stays default.
    SearchRequest req{"x", 0, 42};
    std::string bytes = proto3::serialize(req);

    //   field 1, LEN, len=1, "x"  -> 0a 01 78
    //   (field 2 omitted, default 0)
    //   field 3, VARINT, 42       -> 18 2a
    EXPECT_WIRE(bytes, "0a0178182a");

    SearchRequest back = proto3::deserialize<SearchRequest>(bytes);
    EXPECT_EQ(back.query, "x");
    EXPECT_EQ(back.page_number, 0);
    EXPECT_EQ(back.results_per_page, 42);
}

TEST(SearchRequest, NegativeInt32SignExtends) {
    // proto3 spec: int32 with a negative value is sign-extended to 64 bits
    // and emitted as a 10-byte varint. (sint32 would zigzag — we model int32.)
    SearchRequest req{"q", -1, 5};
    std::string bytes = proto3::serialize(req);

    //   field 1, LEN, len=1, "q"    -> 0a 01 71
    //   field 2, VARINT, -1 as u64  -> 10 ffffffffffffffffff01
    //   field 3, VARINT, 5          -> 18 05
    EXPECT_WIRE(bytes, "0a017110ffffffffffffffffff011805");

    SearchRequest back = proto3::deserialize<SearchRequest>(bytes);
    EXPECT_EQ(back.query, "q");
    EXPECT_EQ(back.page_number, -1);
    EXPECT_EQ(back.results_per_page, 5);
}

TEST(SearchRequest, FieldOrderIndependentOnDecode) {
    // Hand-crafted wire: emit field 3 first, then field 2, then field 1.
    // Real proto decoders MUST accept any field order — we should too.
    std::string bytes;
    bytes += '\x18'; bytes += '\x07';                                  // f3 = 7
    bytes += '\x10'; bytes += '\x02';                                  // f2 = 2
    bytes += '\x0a'; bytes += '\x03'; bytes += 'a'; bytes += 'b'; bytes += 'c'; // f1 = "abc"

    SearchRequest back = proto3::deserialize<SearchRequest>(bytes);
    EXPECT_EQ(back.query, "abc");
    EXPECT_EQ(back.page_number, 2);
    EXPECT_EQ(back.results_per_page, 7);
}

// The [[= proto3::bytes]] marker is documentation-only — wire-identical to
// an unmarked std::string. This test confirms the annotation has no effect
// on the bytes produced and that round-trip works (including embedded NULs,
// which would be a problem for any text-only encoding).
TEST(SearchRequest, BytesMarkerIsWireIdenticalToString) {
    struct Plain {
        std::string blob;
    };
    struct Marked {
        [[= proto3::bytes]] std::string blob;
    };

    // Explicit length — NUL is mid-string. String concatenation breaks the
    // \xff escape so it doesn't greedily consume the trailing 'c'.
    std::string raw("a\x00" "b\xff" "c", 5);
    Plain  p{raw};
    Marked m{raw};

    EXPECT_EQ(proto3::serialize(p), proto3::serialize(m));

    auto back = proto3::deserialize<Marked>(proto3::serialize(m));
    EXPECT_EQ(back.blob, raw);
}

TEST(SearchRequest, UnknownFieldSkipped) {
    // A real SearchRequest only has fields 1..3; inject an unknown field 99
    // (VARINT, value 12345) and a known field 1 ("ok") together. The unknown
    // field must be skipped silently and parsing must continue.
    std::string bytes;
    // field 99 VARINT: tag = (99 << 3) | 0 = 792 = 0x318 -> varint 98 06
    bytes += '\x98'; bytes += '\x06';
    // 12345 as varint = b9 60
    bytes += '\xb9'; bytes += '\x60';
    // field 1 LEN, len=2, "ok"
    bytes += '\x0a'; bytes += '\x02'; bytes += 'o'; bytes += 'k';

    SearchRequest back = proto3::deserialize<SearchRequest>(bytes);
    EXPECT_EQ(back.query, "ok");
    EXPECT_EQ(back.page_number, 0);
    EXPECT_EQ(back.results_per_page, 0);
}
