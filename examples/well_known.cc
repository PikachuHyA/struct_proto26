// well_known.cc — Timestamp/Duration with chrono interop, Any pack/unpack.
//
// Run: bazelisk run //examples:well_known

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "proto3.hpp"
#include "well_known.hpp"

namespace {

// User struct uses native chrono types directly; the annotations route them
// through proto3::Timestamp / proto3::Duration on the wire.
struct Event {
    std::string                                            name;
    [[= proto3::as_timestamp]] std::chrono::system_clock::time_point at;
    [[= proto3::as_duration]]  std::chrono::nanoseconds              ttl;
};

}  // namespace

int main() {
    using namespace std::chrono;

    // 1. Chrono fields ride directly in the struct.
    Event ev{"login", system_clock::time_point(seconds(1700000000)),
             minutes(5)};

    std::string bytes = proto3::serialize(ev);
    Event       back  = proto3::deserialize<Event>(bytes);

    std::cout << "Event name: " << back.name << "\n";
    std::cout << "  at  = " << duration_cast<seconds>(
                                 back.at.time_since_epoch()).count()
              << "s past epoch\n";
    std::cout << "  ttl = " << duration_cast<seconds>(back.ttl).count()
              << "s\n";

    // 2. Any: pack a typed message, send, unpack on the other side.
    proto3::StringValue inner{"hello from Any"};
    proto3::Any any = proto3::pack_any(
        inner, "type.googleapis.com/google.protobuf.StringValue");

    std::cout << "\nAny:\n";
    std::cout << "  type_url = " << any.type_url << "\n";
    std::cout << "  value    = " << any.value.size() << " bytes\n";

    // Verified unpack — throws if the type_url doesn't match.
    proto3::StringValue out = proto3::unpack_any<proto3::StringValue>(
        any, "type.googleapis.com/google.protobuf.StringValue");
    std::cout << "  unpacked.value = \"" << out.value << "\"\n";
}
