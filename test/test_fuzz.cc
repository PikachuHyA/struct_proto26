// In-process property-based fuzz tests for proto3::deserialize.
//
// Two halves per representative shape:
//   1. Round-trip: random valid instance -> serialize -> deserialize ->
//      compare. Catches encoder/decoder asymmetries.
//   2. Crash-resistance: random / truncated / bit-flipped byte streams ->
//      deserialize must either succeed cleanly or throw std::runtime_error.
//      Any other outcome (different exception type, crash, abort, UB) is a
//      bug.
//
// PRNG is std::mt19937 with a fixed seed so failures reproduce
// deterministically. Each TEST() uses a slightly different sub-seed so
// adding/removing a test doesn't shift the random stream of unrelated tests.

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "proto3.hpp"

namespace {

// ---------- PRNG-driven primitive generators ----------

std::string random_string(std::mt19937& rng, std::size_t max_len = 16) {
    std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
    std::uniform_int_distribution<int>         byte_dist(0, 255);
    std::size_t len = len_dist(rng);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        s.push_back(static_cast<char>(byte_dist(rng)));
    return s;
}

std::int32_t random_int32(std::mt19937& rng) {
    std::uniform_int_distribution<std::int32_t> d(
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max());
    return d(rng);
}

std::int64_t random_int64(std::mt19937& rng) {
    std::uniform_int_distribution<std::int64_t> d(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max());
    return d(rng);
}

std::uint32_t random_uint32(std::mt19937& rng) {
    std::uniform_int_distribution<std::uint32_t> d(
        0, std::numeric_limits<std::uint32_t>::max());
    return d(rng);
}

std::string random_bytes(std::mt19937& rng, std::size_t max_len = 256) {
    return random_string(rng, max_len);
}

// Take a valid encoding and corrupt it: truncate, flip a bit, or both.
std::string corrupt(std::mt19937& rng, std::string bytes) {
    if (bytes.empty()) return bytes;
    std::uniform_int_distribution<int> mode(0, 2);
    int m = mode(rng);
    if (m == 0 || m == 2) {
        std::uniform_int_distribution<std::size_t> trunc(0, bytes.size());
        bytes.resize(trunc(rng));
    }
    if (!bytes.empty() && (m == 1 || m == 2)) {
        std::uniform_int_distribution<std::size_t> idx(0, bytes.size() - 1);
        std::uniform_int_distribution<int>         bit(0, 7);
        bytes[idx(rng)] ^= static_cast<char>(1 << bit(rng));
    }
    return bytes;
}

// Test policy: deserialize must succeed cleanly or throw std::runtime_error.
// Any other std::exception subtype is a bug. Non-std exceptions (or aborts)
// would terminate the test process — that's also the signal we want.
template <class T>
void expect_deserialize_safe(std::string_view bytes) {
    try {
        T msg = proto3::deserialize<T>(bytes);
        (void)msg;
    } catch (const std::runtime_error&) {
        // expected on malformed input
    } catch (const std::exception& e) {
        FAIL() << "unexpected std::exception type from deserialize<"
               << typeid(T).name() << ">: " << e.what();
    }
}

// ---------- Shape 1: scalar mix (string + int32 + int32) ----------

struct ScalarMsg {
    std::string  query;
    std::int32_t page;
    std::int32_t per_page;
    bool operator==(const ScalarMsg&) const = default;
};

ScalarMsg gen_scalar(std::mt19937& rng) {
    return ScalarMsg{random_string(rng), random_int32(rng), random_int32(rng)};
}

TEST(Fuzz, ScalarRoundTrip) {
    std::mt19937 rng(101);
    for (int i = 0; i < 1000; ++i) {
        ScalarMsg m    = gen_scalar(rng);
        ScalarMsg back = proto3::deserialize<ScalarMsg>(proto3::serialize(m));
        ASSERT_EQ(m, back) << "iteration " << i;
    }
}

TEST(Fuzz, ScalarCrashResistance) {
    std::mt19937 rng(102);
    for (int i = 0; i < 5000; ++i)
        expect_deserialize_safe<ScalarMsg>(random_bytes(rng));
    for (int i = 0; i < 5000; ++i) {
        auto bytes = proto3::serialize(gen_scalar(rng));
        expect_deserialize_safe<ScalarMsg>(corrupt(rng, bytes));
    }
}

// ---------- Shape 2: zigzag scalars + packed sint vector ----------

struct ZigzagMsg {
    [[= proto3::zigzag]] std::int32_t              a;
    [[= proto3::zigzag]] std::int64_t              b;
    [[= proto3::zigzag]] std::vector<std::int32_t> xs;
    bool operator==(const ZigzagMsg&) const = default;
};

ZigzagMsg gen_zigzag(std::mt19937& rng) {
    ZigzagMsg m{};
    m.a = random_int32(rng);
    m.b = random_int64(rng);
    std::uniform_int_distribution<std::size_t> n(0, 20);
    std::size_t k = n(rng);
    m.xs.reserve(k);
    for (std::size_t i = 0; i < k; ++i) m.xs.push_back(random_int32(rng));
    return m;
}

TEST(Fuzz, ZigzagRoundTrip) {
    std::mt19937 rng(201);
    for (int i = 0; i < 1000; ++i) {
        ZigzagMsg m    = gen_zigzag(rng);
        ZigzagMsg back = proto3::deserialize<ZigzagMsg>(proto3::serialize(m));
        ASSERT_EQ(m, back) << "iteration " << i;
    }
}

TEST(Fuzz, ZigzagCrashResistance) {
    std::mt19937 rng(202);
    for (int i = 0; i < 5000; ++i)
        expect_deserialize_safe<ZigzagMsg>(random_bytes(rng));
    for (int i = 0; i < 5000; ++i) {
        auto bytes = proto3::serialize(gen_zigzag(rng));
        expect_deserialize_safe<ZigzagMsg>(corrupt(rng, bytes));
    }
}

// ---------- Shape 3: fixed scalars + packed fixed vector ----------

struct FixedMsg {
    [[= proto3::fixed]] std::uint32_t                hash;
    [[= proto3::fixed]] std::int64_t                 ts;
    [[= proto3::fixed]] std::vector<std::uint32_t>   keys;
    bool operator==(const FixedMsg&) const = default;
};

FixedMsg gen_fixed(std::mt19937& rng) {
    FixedMsg m{};
    m.hash = random_uint32(rng);
    m.ts   = random_int64(rng);
    std::uniform_int_distribution<std::size_t> n(0, 20);
    std::size_t k = n(rng);
    m.keys.reserve(k);
    for (std::size_t i = 0; i < k; ++i) m.keys.push_back(random_uint32(rng));
    return m;
}

TEST(Fuzz, FixedRoundTrip) {
    std::mt19937 rng(301);
    for (int i = 0; i < 1000; ++i) {
        FixedMsg m    = gen_fixed(rng);
        FixedMsg back = proto3::deserialize<FixedMsg>(proto3::serialize(m));
        ASSERT_EQ(m, back) << "iteration " << i;
    }
}

TEST(Fuzz, FixedCrashResistance) {
    std::mt19937 rng(302);
    for (int i = 0; i < 5000; ++i)
        expect_deserialize_safe<FixedMsg>(random_bytes(rng));
    for (int i = 0; i < 5000; ++i) {
        auto bytes = proto3::serialize(gen_fixed(rng));
        expect_deserialize_safe<FixedMsg>(corrupt(rng, bytes));
    }
}

// ---------- Shape 4: map ----------

struct MapMsg {
    std::map<std::string, std::int32_t> attrs;
    bool operator==(const MapMsg&) const = default;
};

MapMsg gen_map(std::mt19937& rng) {
    MapMsg m{};
    std::uniform_int_distribution<std::size_t> n(0, 8);
    std::size_t k = n(rng);
    for (std::size_t i = 0; i < k; ++i)
        m.attrs[random_string(rng, 8)] = random_int32(rng);
    return m;
}

TEST(Fuzz, MapRoundTrip) {
    std::mt19937 rng(401);
    for (int i = 0; i < 1000; ++i) {
        MapMsg m    = gen_map(rng);
        MapMsg back = proto3::deserialize<MapMsg>(proto3::serialize(m));
        ASSERT_EQ(m, back) << "iteration " << i;
    }
}

TEST(Fuzz, MapCrashResistance) {
    std::mt19937 rng(402);
    for (int i = 0; i < 5000; ++i)
        expect_deserialize_safe<MapMsg>(random_bytes(rng));
    for (int i = 0; i < 5000; ++i) {
        auto bytes = proto3::serialize(gen_map(rng));
        expect_deserialize_safe<MapMsg>(corrupt(rng, bytes));
    }
}

// ---------- Shape 5: oneof ----------

struct OneofSubMsg {
    std::int32_t code;
    std::string  note;
    bool operator==(const OneofSubMsg&) const = default;
};

struct OneofMsg {
    std::int32_t id;
    [[= proto3::oneof<5, 6, 7>]]
    std::variant<std::monostate, std::int32_t, std::string, OneofSubMsg> result;
    bool operator==(const OneofMsg&) const = default;
};

OneofMsg gen_oneof(std::mt19937& rng) {
    OneofMsg m{};
    m.id = random_int32(rng);
    std::uniform_int_distribution<int> alt(0, 3);
    switch (alt(rng)) {
        case 0: /* leave monostate */ break;
        case 1: m.result = random_int32(rng); break;
        case 2: m.result = random_string(rng); break;
        case 3: m.result = OneofSubMsg{random_int32(rng), random_string(rng)};
                break;
    }
    return m;
}

TEST(Fuzz, OneofRoundTrip) {
    std::mt19937 rng(501);
    for (int i = 0; i < 1000; ++i) {
        OneofMsg m    = gen_oneof(rng);
        OneofMsg back = proto3::deserialize<OneofMsg>(proto3::serialize(m));
        ASSERT_EQ(m, back) << "iteration " << i;
    }
}

TEST(Fuzz, OneofCrashResistance) {
    std::mt19937 rng(502);
    for (int i = 0; i < 5000; ++i)
        expect_deserialize_safe<OneofMsg>(random_bytes(rng));
    for (int i = 0; i < 5000; ++i) {
        auto bytes = proto3::serialize(gen_oneof(rng));
        expect_deserialize_safe<OneofMsg>(corrupt(rng, bytes));
    }
}

// ---------- Shape 6: optional (mix of plain + presence + msg) ----------

struct OptionalMsg {
    std::int32_t                id;
    std::optional<std::int32_t> page;
    std::string                 query;
    std::optional<OneofSubMsg>  sub;
    bool operator==(const OptionalMsg&) const = default;
};

OptionalMsg gen_optional(std::mt19937& rng) {
    OptionalMsg m{};
    m.id = random_int32(rng);
    std::bernoulli_distribution coin(0.5);
    if (coin(rng)) m.page = random_int32(rng);
    m.query = random_string(rng);
    if (coin(rng))
        m.sub = OneofSubMsg{random_int32(rng), random_string(rng)};
    return m;
}

TEST(Fuzz, OptionalRoundTrip) {
    std::mt19937 rng(601);
    for (int i = 0; i < 1000; ++i) {
        OptionalMsg m    = gen_optional(rng);
        OptionalMsg back =
            proto3::deserialize<OptionalMsg>(proto3::serialize(m));
        ASSERT_EQ(m, back) << "iteration " << i;
    }
}

TEST(Fuzz, OptionalCrashResistance) {
    std::mt19937 rng(602);
    for (int i = 0; i < 5000; ++i)
        expect_deserialize_safe<OptionalMsg>(random_bytes(rng));
    for (int i = 0; i < 5000; ++i) {
        auto bytes = proto3::serialize(gen_optional(rng));
        expect_deserialize_safe<OptionalMsg>(corrupt(rng, bytes));
    }
}

// ---------- All-shapes sweep ----------
// Throw the same arbitrary byte stream at every shape's deserializer. This
// finds bugs where a wire-format quirk crashes one shape's decode path even
// though it would never be produced by that shape's own serializer.

TEST(Fuzz, AllShapesAcceptArbitraryBytes) {
    std::mt19937 rng(701);
    for (int i = 0; i < 2000; ++i) {
        std::string bytes = random_bytes(rng, 512);
        expect_deserialize_safe<ScalarMsg>(bytes);
        expect_deserialize_safe<ZigzagMsg>(bytes);
        expect_deserialize_safe<FixedMsg>(bytes);
        expect_deserialize_safe<MapMsg>(bytes);
        expect_deserialize_safe<OneofMsg>(bytes);
        expect_deserialize_safe<OptionalMsg>(bytes);
    }
}

}  // namespace
