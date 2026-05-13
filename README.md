# serde protobuf (proto3) with C++26 reflection

A header-only proto3 wire-format serializer/deserializer where **the user's
C++ struct *is* the schema**. No `.proto` files, no codegen, no descriptor
runtime — C++26 `<meta>` reflection walks your struct's data members at
compile time and the encoder/decoder pick up every field automatically.

```cpp
#include "proto3.hpp"

struct SearchRequest {
    std::string  query;             // field 1
    std::int32_t page_number;       // field 2
    std::int32_t results_per_page;  // field 3
};

SearchRequest req{"hello", 1, 10};
std::string   bytes = proto3::serialize(req);          // -> proto3 wire bytes
SearchRequest back  = proto3::deserialize<SearchRequest>(bytes);
```

Field numbers default to declaration order (1, 2, 3, …). Override with
`[[= proto3::field(N)]]`.

## What's covered on the wire

Every proto3 wire-format type:

- **Scalars**: `bool`, all signed/unsigned integer widths, `float`, `double`,
  `enum` (via underlying type), `std::string` (carries both `string` and
  `bytes`).
- **Composite**: nested message (any class type), `repeated` via `std::vector`
  (packed for scalars, repeated LEN entries for strings/messages), `map<K,V>`
  via `std::map` / `std::unordered_map`, `oneof` via
  `std::variant<std::monostate, …>`, explicit-presence `optional` via
  `std::optional<T>`.
- **Per-field annotations** (C++26 attribute syntax `[[= ann]]`):
  - `proto3::field(N)` — override the wire field number.
  - `proto3::skip` — exclude a member from serialization.
  - `proto3::zigzag` — proto3 `sint32` / `sint64` (zigzag varint).
  - `proto3::fixed` — proto3 `fixed32` / `sfixed32` / `fixed64` / `sfixed64`.
  - `proto3::bytes` — documentation marker on a `std::string` carrying
    `bytes` rather than `string`.
  - `proto3::oneof<N1, N2, …>` — mark a `std::variant` as a oneof with the
    given per-alternative field numbers.
  - `proto3::as_timestamp` / `proto3::as_duration` — let
    `std::chrono::system_clock::time_point` / `std::chrono::nanoseconds`
    fields serialize as `google.protobuf.Timestamp` / `Duration`.
  - `proto3::unknown_fields` — opt into proto3 unknown-field preservation
    by marking a `std::string` member as the sink.

## Well-known types (`well_known.hpp`)

`proto3::Timestamp`, `proto3::Duration`, `proto3::Empty`, `proto3::FieldMask`,
the nine `*Value` wrapper types, and `proto3::Any` (with
`pack_any<T>` / `unpack_any<T>`) ship as plain structs in a companion header.
`Timestamp` and `Duration` have implicit conversions to/from
`std::chrono::system_clock::time_point` and `std::chrono::nanoseconds`.

## Compile-time safety

`serialize_into` / `deserialize_from` `static_assert` that field numbers are
in `[1, 2^29 − 1]`, outside the proto3-reserved range `[19000, 19999]`, and
unique within the message (oneof alternatives count). A bad schema breaks
the build with a specific message instead of silently emitting non-conformant
bytes.

## Building

The library is header-only but ships with a Bazel build for the test suite.
The toolchain is pinned to a patched bazel + gcc-16 with `-freflection`:

```bash
bazelisk test //...                       # build everything, run all tests
bazelisk test //:test_search_request      # one target
bazelisk build //examples:basic           # build an example
```

`.bazeliskrc` pins the bazel version (fetched from an internal mirror).
`.bazelrc` sets `CC=gcc-16` and `BAZEL_CXXOPTS=-std=c++26:-freflection`.
You'll need a gcc that supports `-freflection` (the experimental C++26
static-reflection patches) — mainline gcc-16+ is on the way.

## Examples

`examples/` contains short self-contained programs that compile and run:

- `basic.cc` — scalars, string, enum, round-trip.
- `composite.cc` — nested message, vector, map, optional.
- `oneof.cc` — `std::variant` + `proto3::oneof<…>` annotation.
- `annotations.cc` — zigzag, fixed, field, skip, unknown_fields.
- `well_known.cc` — Timestamp/Duration with chrono interop, Any pack/unpack.

Each example prints its result, so `bazelisk run //examples:basic` shows
both the wire-byte hex and the round-trip output.

## Layout

```
include/
  proto3.hpp        core wire format + reflection-driven dispatch
  well_known.hpp    Timestamp, Duration, Empty, wrappers, Any, chrono interop
test/
  test_*.cc         unit tests, hex-asserted wire-byte tests, fuzz, etc.
examples/
  *.cc              short self-contained programs
```

Unit tests live in `test/`; the patterns there (hex `EXPECT_WIRE` asserts
plus round-trip checks) are the canonical reference for any new feature
added to the library.
