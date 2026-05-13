// Tests for proto3 well-known types and Any pack/unpack helpers.

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "proto3.hpp"
#include "well_known.hpp"

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

// ---------- Empty ----------

TEST(WellKnown, EmptySerializesToZeroBytes) {
    EXPECT_EQ(proto3::serialize(proto3::Empty{}).size(), 0u);
}

TEST(WellKnown, EmptyDeserializesFromZeroBytes) {
    auto e = proto3::deserialize<proto3::Empty>("");
    EXPECT_EQ(e, proto3::Empty{});
}

// ---------- Timestamp ----------

TEST(WellKnown, TimestampWireFormat) {
    // seconds = 1, nanos = 2
    //   field 1 VARINT: tag 0x08, value 0x01
    //   field 2 VARINT: tag 0x10, value 0x02
    proto3::Timestamp ts{1, 2};
    EXPECT_WIRE(proto3::serialize(ts), "08011002");
}

TEST(WellKnown, TimestampRoundTrip) {
    proto3::Timestamp ts{1700000000, 123456789};
    auto back = proto3::deserialize<proto3::Timestamp>(proto3::serialize(ts));
    EXPECT_EQ(back, ts);
}

TEST(WellKnown, TimestampDefaultsAreSkipped) {
    // proto3 default-skip: seconds=0 and nanos=0 produce empty wire.
    proto3::Timestamp ts{};
    EXPECT_EQ(proto3::serialize(ts).size(), 0u);
}

TEST(WellKnown, TimestampFromTimePointPostEpoch) {
    using namespace std::chrono;
    auto tp = system_clock::time_point(seconds(1700000000) + nanoseconds(42));
    proto3::Timestamp ts(tp);
    EXPECT_EQ(ts.seconds, 1700000000);
    EXPECT_EQ(ts.nanos, 42);

    // Round-trip back through the conversion operator.
    system_clock::time_point back = ts;
    EXPECT_EQ(back, tp);
}

TEST(WellKnown, TimestampFromTimePointPreEpoch) {
    // 0.5 seconds before the epoch — the load-bearing case for the floor
    // adjustment. nanos must remain non-negative; seconds must be -1.
    using namespace std::chrono;
    auto tp = system_clock::time_point(nanoseconds(-500'000'000));
    proto3::Timestamp ts(tp);
    EXPECT_EQ(ts.seconds, -1);
    EXPECT_EQ(ts.nanos, 500'000'000);

    system_clock::time_point back = ts;
    EXPECT_EQ(back, tp);
}

TEST(WellKnown, TimestampDefaultIsEpoch) {
    using namespace std::chrono;
    proto3::Timestamp ts(system_clock::time_point{});
    EXPECT_EQ(ts.seconds, 0);
    EXPECT_EQ(ts.nanos, 0);
}

// ---------- Duration ----------

TEST(WellKnown, DurationRoundTrip) {
    proto3::Duration d{-30, -500'000'000};
    auto back = proto3::deserialize<proto3::Duration>(proto3::serialize(d));
    EXPECT_EQ(back, d);
}

TEST(WellKnown, DurationFromNanosecondsPositive) {
    using namespace std::chrono;
    proto3::Duration d(seconds(60) + nanoseconds(123));
    EXPECT_EQ(d.seconds, 60);
    EXPECT_EQ(d.nanos, 123);

    nanoseconds back = d;
    EXPECT_EQ(back, seconds(60) + nanoseconds(123));
}

TEST(WellKnown, DurationFromNanosecondsNegative) {
    // -1.5 s as a Duration: components share a sign, NOT floor-adjusted
    // like Timestamp would be.
    using namespace std::chrono;
    proto3::Duration d(nanoseconds(-1'500'000'000));
    EXPECT_EQ(d.seconds, -1);
    EXPECT_EQ(d.nanos, -500'000'000);

    nanoseconds back = d;
    EXPECT_EQ(back, nanoseconds(-1'500'000'000));
}

// ---------- FieldMask ----------

TEST(WellKnown, FieldMaskRoundTrip) {
    proto3::FieldMask m{{"user.name", "user.email", "settings.theme"}};
    auto back = proto3::deserialize<proto3::FieldMask>(proto3::serialize(m));
    EXPECT_EQ(back.paths.size(), 3u);
    EXPECT_EQ(back.paths[0], "user.name");
    EXPECT_EQ(back.paths[1], "user.email");
    EXPECT_EQ(back.paths[2], "settings.theme");
}

TEST(WellKnown, FieldMaskEmptyOmits) {
    EXPECT_EQ(proto3::serialize(proto3::FieldMask{}).size(), 0u);
}

// ---------- Wrappers ----------

TEST(WellKnown, Int32ValueRoundTrip) {
    proto3::Int32Value v{42};
    auto back = proto3::deserialize<proto3::Int32Value>(proto3::serialize(v));
    EXPECT_EQ(back.value, 42);
}

TEST(WellKnown, StringValueRoundTrip) {
    proto3::StringValue v{"hello"};
    auto back = proto3::deserialize<proto3::StringValue>(proto3::serialize(v));
    EXPECT_EQ(back.value, "hello");
}

TEST(WellKnown, BoolValueRoundTrip) {
    proto3::BoolValue t{true};
    proto3::BoolValue f{false};
    EXPECT_TRUE(
        proto3::deserialize<proto3::BoolValue>(proto3::serialize(t)).value);
    // BoolValue{false} default-skips, but deserialize from "" yields false.
    EXPECT_EQ(proto3::serialize(f).size(), 0u);
    EXPECT_FALSE(
        proto3::deserialize<proto3::BoolValue>(proto3::serialize(f)).value);
}

TEST(WellKnown, BytesValueHandlesArbitraryBytes) {
    // Concatenation breaks the \xff escape so it doesn't greedily eat 'c'.
    std::string raw("a\x00" "b\xff" "c", 5);
    proto3::BytesValue v{raw};
    auto back = proto3::deserialize<proto3::BytesValue>(proto3::serialize(v));
    EXPECT_EQ(back.value, raw);
}

// ---------- Any ----------

TEST(WellKnown, AnyPackUnpackRoundTrip) {
    proto3::Timestamp ts{1700000000, 42};
    auto any = proto3::pack_any(ts, "type.googleapis.com/google.protobuf.Timestamp");

    EXPECT_EQ(any.type_url,
              "type.googleapis.com/google.protobuf.Timestamp");
    // Any.value is exactly the serialized inner message.
    EXPECT_EQ(any.value, proto3::serialize(ts));

    auto out = proto3::unpack_any<proto3::Timestamp>(any);
    EXPECT_EQ(out, ts);
}

TEST(WellKnown, AnyVerifiedUnpackAcceptsMatchingUrl) {
    proto3::StringValue inner{"payload"};
    auto any = proto3::pack_any(inner, "type.googleapis.com/google.protobuf.StringValue");

    auto out = proto3::unpack_any<proto3::StringValue>(
        any, "type.googleapis.com/google.protobuf.StringValue");
    EXPECT_EQ(out, inner);
}

TEST(WellKnown, AnyVerifiedUnpackRejectsMismatchedUrl) {
    proto3::Int32Value inner{7};
    auto any = proto3::pack_any(inner, "type.googleapis.com/google.protobuf.Int32Value");

    EXPECT_THROW(
        (proto3::unpack_any<proto3::Int32Value>(any, "type.googleapis.com/Wrong")),
        std::runtime_error);
}

TEST(WellKnown, AnyRoundTripsThroughSerialize) {
    // The Any itself is also a serializable struct.
    proto3::Duration inner{60, 0};
    auto any = proto3::pack_any(inner, "type.googleapis.com/google.protobuf.Duration");

    auto bytes = proto3::serialize(any);
    auto back  = proto3::deserialize<proto3::Any>(bytes);
    EXPECT_EQ(back.type_url, any.type_url);
    EXPECT_EQ(back.value, any.value);

    auto inner_back = proto3::unpack_any<proto3::Duration>(back);
    EXPECT_EQ(inner_back, inner);
}

TEST(WellKnown, AnyCanCarryEmpty) {
    // proto3::Empty serializes to "" — Any.value default-skips on the wire,
    // but pack/unpack is still meaningful (the type_url survives).
    auto any = proto3::pack_any(proto3::Empty{}, "type.googleapis.com/google.protobuf.Empty");
    EXPECT_EQ(any.value, "");

    auto back = proto3::deserialize<proto3::Any>(proto3::serialize(any));
    EXPECT_EQ(back.type_url, any.type_url);
    EXPECT_EQ(back.value, "");

    auto inner = proto3::unpack_any<proto3::Empty>(back);
    EXPECT_EQ(inner, proto3::Empty{});
}

// ---------- as_timestamp / as_duration annotations ----------

namespace {

struct WireBaseline {
    proto3::Timestamp t;            // field 1
    proto3::Duration  d;            // field 2
};

struct ChronoMsg {
    [[= proto3::as_timestamp]] std::chrono::system_clock::time_point t;  // field 1
    [[= proto3::as_duration]]  std::chrono::nanoseconds              d;  // field 2
};

struct ChronoVec {
    [[= proto3::as_timestamp]] std::vector<std::chrono::system_clock::time_point> ts;
};

struct ChronoOpt {
    [[= proto3::as_timestamp]] std::optional<std::chrono::system_clock::time_point> t;
};

}  // namespace

TEST(WellKnown, AsTimestampWireIdenticalToBaselineStruct) {
    // The whole point: a chrono field with [[= as_timestamp]] must produce
    // the EXACT bytes a hand-written proto3::Timestamp field would.
    using namespace std::chrono;

    auto tp = system_clock::time_point(seconds(1700000000) + nanoseconds(42));
    auto dn = seconds(60) + nanoseconds(500);

    ChronoMsg    a{tp, dn};
    WireBaseline b{proto3::Timestamp(tp), proto3::Duration(dn)};

    EXPECT_EQ(proto3::serialize(a), proto3::serialize(b));
}

TEST(WellKnown, AsTimestampScalarRoundTrip) {
    using namespace std::chrono;
    auto tp = system_clock::time_point(seconds(1700000000) + nanoseconds(42));
    auto dn = seconds(60) + nanoseconds(500);

    ChronoMsg orig{tp, dn};
    auto back = proto3::deserialize<ChronoMsg>(proto3::serialize(orig));

    EXPECT_EQ(back.t, tp);
    EXPECT_EQ(back.d, dn);
}

TEST(WellKnown, AsTimestampHandlesPreEpoch) {
    // Tests the floor-adjustment is preserved through a wire round-trip.
    using namespace std::chrono;
    ChronoMsg orig{};
    orig.t = system_clock::time_point(nanoseconds(-500'000'000));
    orig.d = nanoseconds{0};

    auto back = proto3::deserialize<ChronoMsg>(proto3::serialize(orig));
    EXPECT_EQ(back.t, orig.t);
}

TEST(WellKnown, AsTimestampVector) {
    using namespace std::chrono;
    ChronoVec v{};
    v.ts.push_back(system_clock::time_point(seconds(1)));
    v.ts.push_back(system_clock::time_point(seconds(2)));
    v.ts.push_back(system_clock::time_point(seconds(3)));

    auto back = proto3::deserialize<ChronoVec>(proto3::serialize(v));
    ASSERT_EQ(back.ts.size(), 3u);
    EXPECT_EQ(back.ts[0], v.ts[0]);
    EXPECT_EQ(back.ts[1], v.ts[1]);
    EXPECT_EQ(back.ts[2], v.ts[2]);
}

TEST(WellKnown, AsTimestampOptionalUnset) {
    ChronoOpt orig{};
    EXPECT_EQ(proto3::serialize(orig).size(), 0u);

    auto back = proto3::deserialize<ChronoOpt>(proto3::serialize(orig));
    EXPECT_FALSE(back.t.has_value());
}

TEST(WellKnown, AsTimestampOptionalSetToEpochStillEmits) {
    // The presence test for optional applies through the as_timestamp path.
    using namespace std::chrono;
    ChronoOpt orig{system_clock::time_point{}};

    auto bytes = proto3::serialize(orig);
    EXPECT_GT(bytes.size(), 0u) << "optional set to default time_point must still emit";

    auto back = proto3::deserialize<ChronoOpt>(bytes);
    ASSERT_TRUE(back.t.has_value());
    EXPECT_EQ(*back.t, system_clock::time_point{});
}
