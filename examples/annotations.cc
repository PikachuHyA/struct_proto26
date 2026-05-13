// annotations.cc — every per-field annotation in one place.
//   field(N), skip, zigzag, fixed, bytes, unknown_fields.
//
// Run: bazelisk run //examples:annotations

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "proto3.hpp"

namespace {

std::string hex(std::string_view s) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

struct Sender {
    [[= proto3::field(1)]] std::int32_t                a;
    [[= proto3::field(2)]] std::string                 b;
    [[= proto3::field(3)]] std::int32_t                c;
    [[= proto3::field(4)]] std::string                 d;
};

// Receiver doesn't know fields 2 or 4 — they end up in `unknown_`.
struct Receiver {
    [[= proto3::field(1)]] std::int32_t a;
    [[= proto3::skip]]     std::int32_t scratch;   // local-only, never on the wire
    [[= proto3::field(3)]] std::int32_t c;

    [[= proto3::unknown_fields]] std::string unknown_;
};

// Wire-format demos for zigzag and fixed.
struct WireDemo {
    [[= proto3::zigzag]] std::int32_t delta;            // sint32 — 1 byte for -1
    [[= proto3::fixed]]  std::uint32_t hash;            // fixed32 — always 4 bytes
    [[= proto3::bytes]]  std::string blob;              // string with 'bytes' intent
};

}  // namespace

int main() {
    // 1. Wire layout for zigzag + fixed + bytes in one struct.
    {
        // 5 bytes including a mid-string NUL — demonstrates that the bytes
        // marker is binary-clean (a plain `string` field would handle this
        // too; the marker is documentation only).
        std::string blob("raw\x00\xff", 5);
        WireDemo a{-1, 0xdeadbeef, blob};
        std::cout << "WireDemo bytes: " << hex(proto3::serialize(a)) << "\n";
        // delta=-1 zigzags to 1 -> 1-byte varint, plus tag = 2 bytes for f1.
        // hash is fixed32 = tag + 4 bytes = 5 bytes for f2.
        // blob is LEN + length(5) + 5 raw bytes = 7 bytes for f3.
    }

    // 2. unknown_fields preservation
    {
        Sender s{1, "two", 3, "four"};
        std::string wire = proto3::serialize(s);

        Receiver r = proto3::deserialize<Receiver>(wire);
        std::cout << "Receiver decoded a=" << r.a << " c=" << r.c
                  << " unknown_(" << r.unknown_.size() << " bytes)\n";

        // Re-encode preserves the unknowns at the tail.
        std::string re = proto3::serialize(r);

        // Round-trip back to Sender — all fields restored.
        Sender back = proto3::deserialize<Sender>(re);
        std::cout << "Round-trip back: a=" << back.a
                  << " b=\"" << back.b << "\""
                  << " c=" << back.c
                  << " d=\"" << back.d << "\"\n";
    }
}
