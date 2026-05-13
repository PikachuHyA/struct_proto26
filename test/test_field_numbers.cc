// Compile-time tests for proto3 field-number validation.
//
// Validation runs as static_asserts inside serialize_into / deserialize_from,
// so a struct that violates the rules cannot be serialized or deserialized at
// all — the build itself fails. Most assertions in this file are therefore
// `static_assert(predicate<T>())` evaluated at compile time on the predicate
// directly, with one runtime TEST() at the end confirming a valid struct
// still round-trips through the (now assert-guarded) entry points.

#include <cstdint>
#include <string>
#include <variant>

#include "gtest/gtest.h"
#include "proto3.hpp"

namespace {

// ---------- Positive cases ----------

struct DeclOrder {
    std::int32_t a;  // 1
    std::int32_t b;  // 2
    std::int32_t c;  // 3
};
static_assert(proto3::field_numbers_in_range<DeclOrder>());
static_assert(proto3::field_numbers_not_reserved<DeclOrder>());
static_assert(proto3::field_numbers_unique<DeclOrder>());

struct ExplicitNumbers {
    [[= proto3::field(10)]] std::int32_t a;
    [[= proto3::field(20)]] std::int32_t b;
    [[= proto3::field(30)]] std::int32_t c;
};
static_assert(proto3::field_numbers_unique<ExplicitNumbers>());

// Skipped fields don't claim a number, so they can sit between two valid
// ones without forcing the explicit numbers to dodge their declaration index.
struct WithSkip {
    [[= proto3::field(1)]] std::int32_t a;
    [[= proto3::skip]]     std::int32_t scratch;
    [[= proto3::field(2)]] std::int32_t b;
};
static_assert(proto3::field_numbers_unique<WithSkip>());

// Boundary: 18999 (just below reserved) and 20000 (just above) must pass.
struct BoundaryLow  { [[= proto3::field(18999)]] std::int32_t a; };
struct BoundaryHigh { [[= proto3::field(20000)]] std::int32_t a; };
static_assert(proto3::field_numbers_not_reserved<BoundaryLow>());
static_assert(proto3::field_numbers_not_reserved<BoundaryHigh>());

// Max legal: 2^29 - 1.
struct MaxField { [[= proto3::field(536870911)]] std::int32_t a; };
static_assert(proto3::field_numbers_in_range<MaxField>());

// Oneof: the struct-level position of the variant doesn't claim a number,
// but each per-alternative number from the [[= oneof<...>]] does.
struct OneofGood {
    std::int32_t id;  // 1
    [[= proto3::oneof<5, 6, 7>]]
    std::variant<std::monostate, std::int32_t, std::string, std::int32_t> result;
    std::int32_t trailing;  // declaration-index 2 -> wire field 3
};
static_assert(proto3::field_numbers_unique<OneofGood>());

// ---------- Negative cases ----------

struct ZeroNumber { [[= proto3::field(0)]] std::int32_t a; };
static_assert(!proto3::field_numbers_in_range<ZeroNumber>());

struct NegativeNumber { [[= proto3::field(-1)]] std::int32_t a; };
static_assert(!proto3::field_numbers_in_range<NegativeNumber>());

struct TooBigNumber { [[= proto3::field(536870912)]] std::int32_t a; };
static_assert(!proto3::field_numbers_in_range<TooBigNumber>());

// Reserved range is closed on both ends.
struct ReservedLow  { [[= proto3::field(19000)]] std::int32_t a; };
struct ReservedMid  { [[= proto3::field(19500)]] std::int32_t a; };
struct ReservedHigh { [[= proto3::field(19999)]] std::int32_t a; };
static_assert(!proto3::field_numbers_not_reserved<ReservedLow>());
static_assert(!proto3::field_numbers_not_reserved<ReservedMid>());
static_assert(!proto3::field_numbers_not_reserved<ReservedHigh>());

struct DuplicateExplicit {
    [[= proto3::field(7)]] std::int32_t a;
    [[= proto3::field(7)]] std::int32_t b;
};
static_assert(!proto3::field_numbers_unique<DuplicateExplicit>());

// Implicit declaration-order numbers can collide with an explicit number.
struct DuplicateImplicit {
    std::int32_t a;                       // 1
    [[= proto3::field(1)]] std::int32_t b;
};
static_assert(!proto3::field_numbers_unique<DuplicateImplicit>());

// Two oneof alternatives with the same number.
struct OneofDupAlt {
    [[= proto3::oneof<5, 5>]]
    std::variant<std::monostate, std::int32_t, std::string> result;
};
static_assert(!proto3::field_numbers_unique<OneofDupAlt>());

// Oneof alternative collides with a non-oneof field's number.
struct OneofVsField {
    [[= proto3::field(7)]] std::int32_t explicit_seven;
    [[= proto3::oneof<5, 7>]]
    std::variant<std::monostate, std::int32_t, std::string> result;
};
static_assert(!proto3::field_numbers_unique<OneofVsField>());

}  // namespace

// Runtime sanity: a valid struct still serializes and round-trips. This
// exercises the static_asserts now living inside serialize_into and
// deserialize_from — a typo in either would fail to compile this target.
TEST(FieldNumbers, ValidStructStillRoundTrips) {
    DeclOrder g{1, 2, 3};
    auto bytes = proto3::serialize(g);
    auto back  = proto3::deserialize<DeclOrder>(bytes);
    EXPECT_EQ(back.a, 1);
    EXPECT_EQ(back.b, 2);
    EXPECT_EQ(back.c, 3);
}
