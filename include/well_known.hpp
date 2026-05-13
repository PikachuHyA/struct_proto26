#pragma once

// proto3 "well-known types" expressed as plain C++ structs that the
// reflection-driven serializer in <proto3.hpp> picks up automatically.
//
// Wire-format identical to the canonical google/protobuf/*.proto definitions.
// JSON encoding (RFC 3339 for Timestamp, "1.5s" for Duration, etc.) is NOT
// implemented — these structs are wire-compatible only.
//
// Excluded by design (would change the library's scope): Api, SourceContext,
// Type (runtime descriptor reflection), and Struct/Value/ListValue (the
// JSON-shaped recursive value tree — possible but requires unique_ptr
// indirection and is the most work for the least-common use case).

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "proto3.hpp"

namespace proto3 {

// ===== google.protobuf.Empty =====
struct Empty {
    bool operator==(const Empty&) const = default;
};

// ===== google.protobuf.Timestamp =====
// RFC 3339 instant. seconds = whole seconds since 1970-01-01T00:00:00Z;
// nanos = sub-second nanoseconds, ALWAYS in [0, 1e9). For pre-epoch
// instants, seconds is negative and nanos is the floor-adjusted positive
// fractional part — e.g. 0.5 s before epoch is {seconds=-1, nanos=500000000}.
//
// Converts implicitly to/from std::chrono::system_clock::time_point. C++20
// standardized system_clock to use Unix epoch, so the conversion is exact.
struct Timestamp {
    std::int64_t seconds;
    std::int32_t nanos;

    Timestamp() = default;
    constexpr Timestamp(std::int64_t s, std::int32_t n) : seconds(s), nanos(n) {}

    Timestamp(std::chrono::system_clock::time_point tp) {
        auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         tp.time_since_epoch()).count();
        // C++ integer division truncates toward zero. Adjust to floor so
        // nanos lands in [0, 1e9) even for pre-epoch instants.
        std::int64_t s = total / 1'000'000'000;
        std::int64_t n = total % 1'000'000'000;
        if (n < 0) { s -= 1; n += 1'000'000'000; }
        seconds = s;
        nanos   = static_cast<std::int32_t>(n);
    }

    operator std::chrono::system_clock::time_point() const {
        using namespace std::chrono;
        return system_clock::time_point(
            nanoseconds(seconds * 1'000'000'000LL + nanos));
    }

    bool operator==(const Timestamp&) const = default;
};

// ===== google.protobuf.Duration =====
// Signed, fixed-length span of seconds + nanos. seconds and nanos must have
// the SAME sign (or one of them zero). nanos is in (-1e9, 1e9). Differs
// from Timestamp: -1.5 s as a Duration is {seconds=-1, nanos=-500000000},
// not the floor-adjusted form Timestamp uses.
//
// Converts implicitly to/from std::chrono::nanoseconds.
struct Duration {
    std::int64_t seconds;
    std::int32_t nanos;

    Duration() = default;
    constexpr Duration(std::int64_t s, std::int32_t n) : seconds(s), nanos(n) {}

    Duration(std::chrono::nanoseconds d) {
        auto total = d.count();
        // Truncation toward zero is what we want here — the components
        // share a sign, no floor adjustment needed.
        seconds = total / 1'000'000'000;
        nanos   = static_cast<std::int32_t>(total % 1'000'000'000);
    }

    operator std::chrono::nanoseconds() const {
        return std::chrono::nanoseconds(seconds * 1'000'000'000LL + nanos);
    }

    bool operator==(const Duration&) const = default;
};

// ===== google.protobuf.FieldMask =====
struct FieldMask {
    std::vector<std::string> paths;
    bool operator==(const FieldMask&) const = default;
};

// ===== Wrapper types (google.protobuf.*Value) =====
// These give scalars message-like presence semantics. Largely obsolete now
// that proto3 has explicit `optional` (i.e. std::optional<T> in this
// library), but kept for wire-compatibility with messages that use them.
struct DoubleValue { double         value; bool operator==(const DoubleValue&) const = default; };
struct FloatValue  { float          value; bool operator==(const FloatValue&)  const = default; };
struct Int64Value  { std::int64_t   value; bool operator==(const Int64Value&)  const = default; };
struct UInt64Value { std::uint64_t  value; bool operator==(const UInt64Value&) const = default; };
struct Int32Value  { std::int32_t   value; bool operator==(const Int32Value&)  const = default; };
struct UInt32Value { std::uint32_t  value; bool operator==(const UInt32Value&) const = default; };
struct BoolValue   { bool           value; bool operator==(const BoolValue&)   const = default; };
struct StringValue { std::string    value; bool operator==(const StringValue&) const = default; };
struct BytesValue {
    [[= proto3::bytes]] std::string value;
    bool operator==(const BytesValue&) const = default;
};

// ===== google.protobuf.Any =====
// Carries a serialized message of an arbitrary type, identified by `type_url`.
// The convention for `type_url` is "type.googleapis.com/<fully.qualified.Name>",
// but this library does not enforce that — pick whatever string lets your
// readers identify the payload.
struct Any {
    std::string                     type_url;
    [[= proto3::bytes]] std::string value;
    bool operator==(const Any&) const = default;
};

// Pack `msg` into an Any, tagging it with `type_url`.
template <class T>
Any pack_any(const T& msg, std::string type_url) {
    Any a;
    a.type_url = std::move(type_url);
    serialize_into(a.value, msg);
    return a;
}

// Typed unpack — caller asserts the contained message is a T. No type_url
// check; use the overload below if you need that.
template <class T>
T unpack_any(const Any& a) {
    return deserialize<T>(a.value);
}

// Verified unpack — throws std::runtime_error if `a.type_url` doesn't match
// `expected_type_url`. The string compare is exact (no leading-host
// canonicalization).
template <class T>
T unpack_any(const Any& a, std::string_view expected_type_url) {
    if (std::string_view{a.type_url} != expected_type_url) {
        throw std::runtime_error(
            "proto3: Any type_url mismatch — expected '" +
            std::string(expected_type_url) + "', got '" + a.type_url + "'");
    }
    return deserialize<T>(a.value);
}

}  // namespace proto3
