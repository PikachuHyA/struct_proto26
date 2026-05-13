// composite.cc — nested message + std::vector + std::map + std::optional.
//
// Run: bazelisk run //examples:composite

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "proto3.hpp"

namespace {

struct Address {
    std::string street;
    std::string city;
};

struct Person {
    std::string                          name;        // 1
    std::int32_t                         age;         // 2
    std::vector<Address>                 addresses;   // 3 (repeated)
    std::map<std::string, std::int32_t>  labels;      // 4 (map)
    std::optional<std::string>           nickname;    // 5 (explicit presence)
};

}  // namespace

int main() {
    Person p{};
    p.name = "alice";
    p.age  = 30;
    p.addresses.push_back({"1 Infinite Loop", "Cupertino"});
    p.addresses.push_back({"1600 Amphitheatre Pkwy", "Mountain View"});
    p.labels["loyalty"] = 5;
    p.labels["tier"]    = 3;
    p.nickname = "ali";

    std::string bytes = proto3::serialize(p);
    std::cout << "wire size: " << bytes.size() << " bytes\n";

    Person back = proto3::deserialize<Person>(bytes);
    std::cout << "name: " << back.name << "\n";
    std::cout << "addresses (" << back.addresses.size() << "):\n";
    for (const auto& a : back.addresses)
        std::cout << "  - " << a.street << ", " << a.city << "\n";
    std::cout << "labels (" << back.labels.size() << "):\n";
    for (const auto& [k, v] : back.labels)
        std::cout << "  " << k << " = " << v << "\n";
    std::cout << "nickname: "
              << (back.nickname ? *back.nickname : "(unset)") << "\n";
}
