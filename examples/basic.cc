// basic.cc — scalars, string, enum, round-trip.
//
// Run: bazelisk run //examples:basic

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

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

enum class Color : std::int32_t { RED = 0, GREEN = 1, BLUE = 2 };

struct Person {
    std::string  name;       // field 1
    std::int32_t age;        // field 2
    Color        favorite;   // field 3
};

const char* color_name(Color c) {
    switch (c) {
        case Color::RED:   return "RED";
        case Color::GREEN: return "GREEN";
        case Color::BLUE:  return "BLUE";
    }
    return "?";
}

}  // namespace

int main() {
    Person p{"alice", 30, Color::GREEN};

    std::string bytes = proto3::serialize(p);
    std::cout << "wire (" << bytes.size() << " bytes): " << hex(bytes) << "\n";

    Person back = proto3::deserialize<Person>(bytes);
    std::cout << "decoded: " << back.name
              << ", age=" << back.age
              << ", favorite=" << color_name(back.favorite) << "\n";
}
