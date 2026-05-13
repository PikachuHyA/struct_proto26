// oneof.cc — std::variant<std::monostate, ...> with [[= proto3::oneof<...>]].
//
// Run: bazelisk run //examples:oneof

#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

#include "proto3.hpp"

namespace {

struct ErrorDetail {
    std::int32_t code;
    std::string  message;
};

// proto-equivalent:
//   message Resp {
//     int32 id = 1;
//     oneof result {
//       int32       ok     = 5;
//       string      error  = 6;
//       ErrorDetail detail = 7;
//     }
//   }
struct Resp {
    std::int32_t id;

    [[= proto3::oneof<5, 6, 7>]]
    std::variant<std::monostate, std::int32_t, std::string, ErrorDetail> result;
};

void show(const Resp& r) {
    std::cout << "id=" << r.id << "  result=";
    std::visit([](const auto& x) {
        using X = std::remove_cvref_t<decltype(x)>;
        if constexpr (std::is_same_v<X, std::monostate>)
            std::cout << "(unset)";
        else if constexpr (std::is_same_v<X, std::int32_t>)
            std::cout << "ok(" << x << ")";
        else if constexpr (std::is_same_v<X, std::string>)
            std::cout << "error(\"" << x << "\")";
        else
            std::cout << "detail(code=" << x.code
                      << ", msg=\"" << x.message << "\")";
    }, r.result);
    std::cout << "\n";
}

}  // namespace

int main() {
    for (Resp r : {
            Resp{100, std::int32_t{42}},
            Resp{101, std::string{"timeout"}},
            Resp{102, ErrorDetail{500, "internal"}},
            Resp{103, std::monostate{}},
         }) {
        auto bytes = proto3::serialize(r);
        Resp back  = proto3::deserialize<Resp>(bytes);
        show(back);
    }
}
