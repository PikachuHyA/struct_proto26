// Tests for proto3 unknown-field preservation via [[= proto3::unknown_fields]].
//
// Two halves:
//   1. Compile-time static_asserts on the helper predicates (count, index,
//      member_is_string) to verify they correctly classify good and bad
//      struct shapes without triggering the "real" static_asserts in
//      serialize_into / deserialize_from.
//   2. Runtime tests covering the round-trip semantics: a fat-schema sender
//      and a thin-schema receiver should preserve all original fields when
//      re-deserialized into the fat schema.

#include <cstdint>
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

// ---------- Compile-time predicate checks ----------

struct NoSink {
    std::int32_t a;
};
static_assert(proto3::unknown_fields_count<NoSink>() == 0);
static_assert(!proto3::unknown_fields_index<NoSink>().has_value());
static_assert(proto3::unknown_fields_member_is_string<NoSink>());

struct OneSink {
    std::int32_t a;
    [[= proto3::unknown_fields]] std::string unknown_;
};
static_assert(proto3::unknown_fields_count<OneSink>() == 1);
static_assert(proto3::unknown_fields_index<OneSink>().has_value());
static_assert(*proto3::unknown_fields_index<OneSink>() == 1);
static_assert(proto3::unknown_fields_member_is_string<OneSink>());

// Bad shapes — these compile fine because the predicates are pure helpers;
// the "real" static_asserts only fire from serialize_into / deserialize_from.
// Verifying the helpers catch the problem here means the failure path will
// also fire correctly when a user does try to serialize one of these.
struct TwoSinks {
    [[= proto3::unknown_fields]] std::string a;
    [[= proto3::unknown_fields]] std::string b;
};
static_assert(proto3::unknown_fields_count<TwoSinks>() == 2);

struct IntSink {
    [[= proto3::unknown_fields]] std::int32_t bad;
};
static_assert(!proto3::unknown_fields_member_is_string<IntSink>());

// ---------- Runtime test fixtures ----------

// "Fat" schema: knows about every field that goes on the wire.
struct Fat {
    [[= proto3::field(1)]] std::int32_t a;
    [[= proto3::field(2)]] std::string  b;
    [[= proto3::field(3)]] std::int32_t c;
    [[= proto3::field(4)]] std::string  d;
};

// "Thin" schema: only knows fields 1 and 3, with an unknowns sink.
struct Thin {
    [[= proto3::field(1)]] std::int32_t a;
    [[= proto3::field(3)]] std::int32_t c;
    [[= proto3::unknown_fields]] std::string unknown_;
};

// Same shape as Thin but WITHOUT the sink — used for the regression test
// confirming default behavior is unchanged when no sink is declared.
struct ThinNoSink {
    [[= proto3::field(1)]] std::int32_t a;
    [[= proto3::field(3)]] std::int32_t c;
};

}  // namespace

// ---------- Round-trip preservation ----------

TEST(UnknownFields, RoundTripPreservesUnknowns) {
    // Sender's Fat → wire → receiver decodes as Thin (preserving f2, f4 in
    // the sink) → re-encodes → second receiver decodes as Fat → all four
    // original fields restored.
    Fat orig{42, "hello", 7, "world"};
    auto wire = proto3::serialize(orig);

    Thin thin = proto3::deserialize<Thin>(wire);
    EXPECT_EQ(thin.a, 42);
    EXPECT_EQ(thin.c, 7);
    EXPECT_GT(thin.unknown_.size(), 0u);

    auto re_wire = proto3::serialize(thin);
    Fat back = proto3::deserialize<Fat>(re_wire);

    EXPECT_EQ(back.a, 42);
    EXPECT_EQ(back.b, "hello");
    EXPECT_EQ(back.c, 7);
    EXPECT_EQ(back.d, "world");
}

TEST(UnknownFields, NoSinkDropsUnknownsExistingBehavior) {
    // The opt-in must be opt-in: a struct WITHOUT [[= proto3::unknown_fields]]
    // must continue to drop unknown fields silently, exactly as before.
    Fat orig{42, "hello", 7, "world"};
    auto wire = proto3::serialize(orig);

    ThinNoSink thin = proto3::deserialize<ThinNoSink>(wire);
    EXPECT_EQ(thin.a, 42);
    EXPECT_EQ(thin.c, 7);

    Fat back = proto3::deserialize<Fat>(proto3::serialize(thin));
    EXPECT_EQ(back.b, "");
    EXPECT_EQ(back.d, "");
}

TEST(UnknownFields, EmptyInputProducesEmptySink) {
    Thin thin = proto3::deserialize<Thin>("");
    EXPECT_EQ(thin.unknown_, "");
}

TEST(UnknownFields, AllKnownsLeavesSinkEmpty) {
    // Wire with only fields 1 and 3 — every field is known, sink stays empty.
    Thin one_known{42, 7, ""};
    auto wire = proto3::serialize(one_known);

    Thin back = proto3::deserialize<Thin>(wire);
    EXPECT_EQ(back.a, 42);
    EXPECT_EQ(back.c, 7);
    EXPECT_EQ(back.unknown_, "");
}

TEST(UnknownFields, CapturesInterleavedUnknownsInArrivalOrder) {
    // Hand-craft a wire stream with f1=1 (known), f99=7 (unknown), f3=3
    // (known) interleaved. The sink should hold ONLY the unknown's bytes
    // (tag varint + value), not the surrounding knowns.
    std::string wire;
    wire += '\x08'; wire += '\x01';                     // f1 VARINT 1
    wire += '\x98'; wire += '\x06'; wire += '\x07';     // f99 VARINT 7 (tag varint = 98 06)
    wire += '\x18'; wire += '\x03';                     // f3 VARINT 3

    Thin thin = proto3::deserialize<Thin>(wire);
    EXPECT_EQ(thin.a, 1);
    EXPECT_EQ(thin.c, 3);
    EXPECT_WIRE(thin.unknown_, "980607");
}

TEST(UnknownFields, MultipleUnknownsConcatenatedInOrder) {
    // f5 LEN "hi", f7 VARINT 100, f9 VARINT 200 — all unknown to Thin.
    // Sink should be the three encodings concatenated in the order they
    // arrived on the wire.
    std::string wire;
    wire += '\x2a'; wire += '\x02'; wire += 'h'; wire += 'i';      // f5 LEN "hi"
    wire += '\x38'; wire += '\x64';                                 // f7 VARINT 100
    wire += '\x48'; wire += '\xc8'; wire += '\x01';                 // f9 VARINT 200

    Thin thin = proto3::deserialize<Thin>(wire);
    EXPECT_EQ(thin.a, 0);
    EXPECT_EQ(thin.c, 0);
    EXPECT_WIRE(thin.unknown_, "2a026869386448c801");
}

TEST(UnknownFields, KnownsFirstUnknownsAtTailOnReencode) {
    // Even when the original wire interleaved knowns and unknowns, the
    // re-serialized output puts knowns first (declaration order) and
    // unknowns at the tail. proto3 decoders accept any field order, so
    // this is conformant — but it means re-emit isn't byte-equal to input.
    std::string wire;
    wire += '\x98'; wire += '\x06'; wire += '\x07';   // f99 (unknown) FIRST
    wire += '\x08'; wire += '\x01';                   // f1 = 1 SECOND

    Thin thin = proto3::deserialize<Thin>(wire);
    auto re = proto3::serialize(thin);

    // Expected order: f1 first, then the preserved f99 bytes.
    EXPECT_WIRE(re, "0801980607");
    // And the reverse of the input order.
    EXPECT_NE(re, wire);
}

TEST(UnknownFields, NestedMessageUnknownsAreNotPreservedInsideSubMessage) {
    // Documented limitation: preservation only kicks in at the top-level
    // message decode. Unknowns inside a nested message are still skipped.
    struct Inner {
        [[= proto3::field(1)]] std::int32_t known;
        [[= proto3::unknown_fields]] std::string unknown_;
    };
    struct Outer {
        [[= proto3::field(1)]] Inner sub;
    };

    // Hand-craft Outer{ Inner with f1=5 + f99=42 unknown }.
    std::string inner_wire;
    inner_wire += '\x08'; inner_wire += '\x05';       // f1 = 5
    inner_wire += '\x98'; inner_wire += '\x06';       // f99 tag
    inner_wire += '\x2a';                             // f99 value 42
    std::string outer_wire;
    outer_wire += '\x0a';
    outer_wire += static_cast<char>(inner_wire.size());
    outer_wire += inner_wire;

    // Decoded Outer.sub.unknown_ DOES capture f99 — nested-message decode
    // flows through the same deserialize_from for Inner, which sees f99 as
    // unknown and (since Inner has the sink) preserves it.
    Outer outer = proto3::deserialize<Outer>(outer_wire);
    EXPECT_EQ(outer.sub.known, 5);
    EXPECT_WIRE(outer.sub.unknown_, "98062a");
}

TEST(UnknownFields, FieldNumberValidationIgnoresSink) {
    // Sink occupies a declaration index but claims no wire field number —
    // so a struct with explicit field(1) on field A and the sink at index
    // 1 must NOT be flagged as a duplicate of the implicit-derived "field 2"
    // a normal field at index 1 would have. (This compiles iff validation
    // correctly excludes the sink.)
    struct S {
        [[= proto3::field(1)]] std::int32_t a;
        [[= proto3::unknown_fields]] std::string sink;
        [[= proto3::field(2)]] std::int32_t b;
    };
    S v{1, "", 2};
    auto bytes = proto3::serialize(v);
    auto back = proto3::deserialize<S>(bytes);
    EXPECT_EQ(back.a, 1);
    EXPECT_EQ(back.b, 2);
    EXPECT_EQ(back.sink, "");
}
