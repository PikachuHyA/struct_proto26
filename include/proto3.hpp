#pragma once

// proto3 wire-format serialize / deserialize built on C++26 reflection.
//
// Mapping rules (proto3 conformant on the wire):
//   bool / [u]int{8,16,32,64} / enum  -> VARINT
//   float                              -> I32  (little-endian bit_cast)
//   double                             -> I64
//   std::string                        -> LEN  (carries proto3 `string` AND
//                                                `bytes` — wire-identical;
//                                                use [[= proto3::bytes]] to
//                                                document `bytes` intent)
//   nested struct (class type)         -> LEN  (recursively encoded)
//   std::vector<scalar>                -> packed LEN
//   std::vector<std::string|message>   -> repeated LEN entries
//   std::map<K,V>, std::unordered_map  -> repeated LEN entry messages
//                                         (entry: key=field 1, value=field 2)
//   std::optional<T>                   -> explicit presence: omitted on the
//                                         wire when unset; emitted on the wire
//                                         when set, even if the contained
//                                         value equals T{} (this is what
//                                         distinguishes it from a bare scalar,
//                                         which is omitted at default).
//                                         Inner T must be a scalar, string,
//                                         or message — not vector/map/variant.
//   std::variant<std::monostate, ...>  -> proto3 oneof (see annotation below)
//
// Per-field annotations (applied with C++26 attribute syntax `[[= ann]]`):
//   [[= proto3::field(N)]]    Override the field number (default: declaration
//                             order, starting at 1).
//   [[= proto3::skip]]        Omit a member from serialization entirely.
//   [[= proto3::zigzag]]      Encode a signed-int field — or std::vector /
//                             std::optional of same — as proto3 sint32 /
//                             sint64 (zigzag varint).
//   [[= proto3::fixed]]       Encode a 4- or 8-byte integer field — or
//                             std::vector / std::optional of same — on the
//                             I32 / I64 fixed-width wire (proto3 fixed32 /
//                             sfixed32 / fixed64 / sfixed64). Mutually
//                             exclusive with [[= proto3::zigzag]].
//   [[= proto3::bytes]]       Documentation marker on a std::string (or
//                             std::vector<std::string>) field that carries
//                             proto3 `bytes` rather than `string`. No wire-
//                             format effect — the two are byte-identical.
//   [[= proto3::oneof<N1,...>]]  Mark a std::variant<std::monostate, T1, ...>
//                             field as a proto3 oneof. The numbers are the
//                             per-alternative field numbers (one per non-
//                             monostate alt). The struct-level field number
//                             on the variant member is ignored.
//
// Standard proto3 semantics: scalar fields equal to their default (0 / false /
// "") are not emitted on the wire. Embedded messages, non-empty repeated /
// map fields, and active oneof alternatives are always emitted.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace proto3 {

namespace meta = std::meta;

// ===== Annotations =====

struct field_number_t {
    int number;
};

consteval field_number_t field(int n) {
    return field_number_t{n};
}

struct skip_t {};

constexpr inline skip_t skip{};

// Mark a signed-integer field (or vector<signed-integer>) as zigzag-encoded
// on the wire. Equivalent to proto3 `sint32` / `sint64`.
//   [[= proto3::zigzag]] std::int32_t delta;
struct zigzag_t {};

constexpr inline zigzag_t zigzag{};

// Mark an integer field (or vector<integer>) as fixed-width on the wire —
// proto3 `fixed32` / `sfixed32` / `fixed64` / `sfixed64`. Width follows
// `sizeof(T)` (must be 4 or 8); signedness follows `T`.
//   [[= proto3::fixed]] std::uint32_t hash;
//   [[= proto3::fixed]] std::int64_t  ts;
//   [[= proto3::fixed]] std::vector<std::uint32_t> keys;  // packed
struct fixed_t {};

constexpr inline fixed_t fixed{};

// Documentation marker for std::string (or vector<std::string>) fields that
// carry proto3 `bytes` rather than `string`. Has no wire-format effect — the
// two types are byte-identical on the wire — but it lets readers tell the
// intent apart at the C++ level. The static_assert in encode_one rejects the
// annotation on any other field type so it can't drift from documentation.
//   [[= proto3::bytes]] std::string blob;
struct bytes_t {};

constexpr inline bytes_t bytes{};

// Mark a field as serializing through proto3::Timestamp / proto3::Duration
// on the wire even though the C++ field type is something else (typically
// std::chrono::system_clock::time_point or std::chrono::nanoseconds). The
// field type must be convertible both ways with the wire-side struct.
//   [[= proto3::as_timestamp]] std::chrono::system_clock::time_point t;
//   [[= proto3::as_duration]]  std::chrono::nanoseconds              d;
//
// Composes with std::vector and std::optional. Mutually exclusive with
// every other annotation in this header (Timestamp/Duration are nested
// messages on the wire — zigzag/fixed/bytes don't apply to messages).
//
// Forward declarations only; the full struct definitions and the chrono
// converting constructors live in <well_known.hpp>, which users of these
// annotations must include.
struct Timestamp;
struct Duration;

struct as_timestamp_t {};
constexpr inline as_timestamp_t as_timestamp{};
struct as_duration_t {};
constexpr inline as_duration_t as_duration{};

// Mark a single std::string member as the sink for proto3 unknown-field
// preservation. On decode, any incoming field whose tag isn't matched by
// any known field of the struct is appended to this member as raw wire
// bytes (tag varint + value bytes). On encode, the contents are written
// out at the END of the message, after all known fields. The annotated
// member itself does NOT claim a wire field number — it is metadata, not
// a wire field — and is excluded from field-number validation.
//
//   struct MyMsg {
//       std::int32_t known;
//       [[= proto3::unknown_fields]] std::string unknown_;
//   };
//
// At most one such field per struct; static_assert fires otherwise. Field
// order is NOT preserved across a round-trip: knowns come out first in
// declaration order, unknowns at the tail. proto3 decoders accept any
// field order, so this is conformant — but byte-for-byte stability under
// `serialize(deserialize(X)) == X` only holds when X carried no unknowns.
struct unknown_fields_t {};
constexpr inline unknown_fields_t unknown_fields{};

// ===== Wire types =====

enum class wire_type : std::uint8_t {
    VARINT = 0,
    I64    = 1,
    LEN    = 2,
    I32    = 5,
};

// ===== Type traits =====

template <class T> struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};
template <class T> inline constexpr bool is_vector_v = is_vector<T>::value;

template <class T> struct is_map : std::false_type {};
template <class K, class V, class C, class A>
struct is_map<std::map<K, V, C, A>> : std::true_type {
    using key_type = K;
    using mapped_type = V;
};
template <class K, class V, class H, class E, class A>
struct is_map<std::unordered_map<K, V, H, E, A>> : std::true_type {
    using key_type = K;
    using mapped_type = V;
};
template <class T> inline constexpr bool is_map_v = is_map<T>::value;

consteval bool is_instantiation_of(meta::info type, meta::info templ) {
    return has_template_arguments(type) &&
           template_of(type) == templ;
}

consteval bool is_variant(meta::info type) {
    return is_instantiation_of(type, ^^std::variant);
}

template <class T> inline constexpr bool is_variant_v = is_variant(^^T);

consteval bool is_optional(meta::info type) {
    return is_instantiation_of(type, ^^std::optional);
}

template <class T> inline constexpr bool is_optional_v = is_optional(^^T);

template <class T>
inline constexpr bool is_message_v =
    std::is_class_v<T> &&
    !std::is_same_v<T, std::string> &&
    !is_vector_v<T> &&
    !is_map_v<T> &&
    !is_variant_v<T> &&
    !is_optional_v<T>;

// ===== oneof annotation =====
//
// Apply to a field of type
//   std::variant<std::monostate, T1, T2, ..., Tn>
// to mark it as a proto3 oneof. The annotation carries one field number per
// alternative (excluding the leading std::monostate which represents "unset").
//
//   [[= proto3::oneof<5, 6, 7>]]
//   std::variant<std::monostate, std::int32_t, std::string, Sub> result;

template <int... Ns>
struct oneof_t {
    static constexpr std::array<int, sizeof...(Ns)> field_numbers{Ns...};
};

template <int... Ns>
constexpr inline oneof_t<Ns...> oneof{};

// ===== Reflection helpers =====

consteval auto fields_of_type(meta::info t) {
    return std::define_static_array(
        meta::nonstatic_data_members_of(t, meta::access_context::unchecked()));
}

consteval bool has_annotation(meta::info entity, meta::info ann_type) {
    auto anns = meta::annotations_of_with_type(entity, ann_type);
    return !anns.empty();
}

consteval bool field_skipped_at(meta::info field) {
    return has_annotation(field, ^^skip_t);
}

consteval int field_number_at(meta::info field, std::size_t I) {
    auto anns = meta::annotations_of_with_type(field, ^^field_number_t);
    if (!anns.empty()) {
        return meta::extract<field_number_t>(anns.front()).number;
    } else {
        return static_cast<int>(I) + 1;
    }
}

consteval bool field_zigzag_at(meta::info field) {
    return has_annotation(field, ^^zigzag_t);
}

consteval bool field_fixed_at(meta::info field) {
    return has_annotation(field, ^^fixed_t);
}

consteval bool field_bytes_at(meta::info field) {
    return has_annotation(field, ^^bytes_t);
}

consteval bool field_as_timestamp_at(meta::info field) {
    return has_annotation(field, ^^as_timestamp_t);
}

consteval bool field_as_duration_at(meta::info field) {
    return has_annotation(field, ^^as_duration_t);
}

consteval bool field_unknown_fields_at(meta::info field) {
    return has_annotation(field, ^^unknown_fields_t);
}

// How many [[= proto3::unknown_fields]] members the struct has. Used by
// serialize_into / deserialize_from to static_assert the at-most-one rule.
template <class T>
consteval std::size_t unknown_fields_count() {
    constexpr static auto fields = fields_of_type(^^T);
    size_t count = 0;
    for (auto field : fields) {
        if (field_unknown_fields_at(field))
            ++count;
    }
    return count;
}

// Index of the (single) unknown-fields member, or nullopt if the struct
// has none. Combined with the at-most-one static_assert, "first" is "only".
template <class T>
consteval std::optional<std::size_t> unknown_fields_index() {
    constexpr static auto fields = fields_of_type(^^T);
    for (std::size_t i = 0; i != fields.size(); ++i) {
        if (field_unknown_fields_at(fields[i]))
            return i;
    }
    return std::nullopt;
}

// True iff the unknown-fields member (if any) has type std::string.
// Reported via static_assert so the user gets a specific message rather
// than a "no member 'append' on int" error from inside deserialize_from.
template <class T>
consteval bool unknown_fields_member_is_string() {
    constexpr auto idx = unknown_fields_index<T>();
    if constexpr (!idx.has_value()) {
        return true;
    } else {
        constexpr static auto fields = fields_of_type(^^T);
        auto ft = meta::type_of(fields[*idx]);
        return is_same_type(remove_cv(ft), ^^std::string);
    }
}

// True iff type is `std::string` or `std::vector<std::string>` — the only
// types that may legally carry the [[= proto3::bytes]] documentation marker.
consteval bool is_string_or_vector_string(meta::info type) {
    if (is_same_type(type, ^^std::string)) {
        return true;
    }
    if (has_template_arguments(type) && template_of(type) == ^^std::vector) {
        auto template_arguments = template_arguments_of(type);
        return template_arguments.size() == 2 &&
               is_same_type(template_arguments[0], ^^std::string);
    }
    return false;
}

// Find any oneof_t<...> annotation on field 'm'.
consteval std::optional<meta::info> find_oneof_ann_at(meta::info m) {
    auto anns = meta::annotations_of(m);
    for (auto ann : anns) {
        if (is_instantiation_of(remove_cv(type_of(ann)), ^^oneof_t))
            return ann;
    }
    return std::nullopt;
}

consteval bool is_oneof_at(meta::info m) {
    return find_oneof_ann_at(m).has_value();
}

template <meta::info m>
consteval auto oneof_field_numbers_at() {
    constexpr auto opt_ann = find_oneof_ann_at(m);
    static_assert(opt_ann.has_value(),
                  "expected a [[= proto3::oneof<...>]] annotation");
    using AnnRaw = [: meta::type_of(*opt_ann) :];
    using AnnT = std::remove_cv_t<AnnRaw>;
    return AnnT::field_numbers;
}

// ===== Field-number validation =====
//
// Compile-time check that every active field number in a message satisfies
// the proto3 wire-format rules:
//   - in the legal range [1, 2^29 - 1]
//   - not in the reserved range [19000, 19999] (used internally by the
//     reference protobuf implementation)
//   - unique within the message
//
// "Active" excludes fields tagged [[= proto3::skip]]. For oneof variant
// members, each per-alternative number from the [[= proto3::oneof<...>]]
// annotation is treated as a separate active number — the struct-level
// number on the variant is unused on the wire.

inline constexpr int kMinFieldNumber = 1;
inline constexpr int kMaxFieldNumber = 536870911;          // 2^29 - 1
inline constexpr int kReservedFirst  = 19000;
inline constexpr int kReservedLast   = 19999;

// How many wire field numbers field contributes (0 for skipped or
// for an [[= proto3::unknown_fields]] sink, the oneof alternative count
// for oneof variants, 1 otherwise).
template <meta::info field>
consteval std::size_t numbers_at_field() {
    if constexpr (field_skipped_at(field) || field_unknown_fields_at(field)) {
        return 0;
    } else {
        constexpr static auto ft = meta::type_of(field);
        if constexpr (is_variant(ft)) {
            if constexpr (is_oneof_at(field)) {
                return oneof_field_numbers_at<field>().size();
            } else {
                return 0;  // rejected at encode_one with a clearer message
            }
        } else {
            return 1;
        }
    }
}

template <class T>
consteval std::size_t count_active_field_numbers() {
    constexpr static auto fields = fields_of_type(^^T);
    std::size_t count = 0;
    template for (constexpr auto field : fields)
        count += numbers_at_field<field>();
    return count;
}

// Append field s contribution to nums, advancing pos. Skipped and
// unknown-fields-sink members contribute nothing.
template <meta::info field, std::size_t N>
consteval void emit_field_numbers_at(std::size_t field_index,
                                     std::array<int, N>& nums,
                                     std::size_t& pos) {
    if constexpr (!field_skipped_at(field) && !field_unknown_fields_at(field)) {
        constexpr static auto ft = meta::type_of(field);
        if constexpr (is_variant(ft)) {
            if constexpr (is_oneof_at(field)) {
                constexpr auto fns = oneof_field_numbers_at<field>();
                for (int n : fns) nums[pos++] = n;
            }
        } else {
            nums[pos++] = field_number_at(field, field_index);
        }
    }
}

template <class T>
consteval auto collect_field_numbers() {
    constexpr std::size_t N = count_active_field_numbers<T>();
    constexpr static auto fields = fields_of_type(^^T);
    std::array<int, N> nums{};
    std::size_t pos = 0;
    std::size_t field_index = 0;
    template for (constexpr auto field : fields)
        emit_field_numbers_at<field>(field_index++, nums, pos);
    return nums;
}

template <class T>
consteval bool field_numbers_in_range() {
    constexpr auto nums = collect_field_numbers<T>();
    for (int n : nums) {
        if (n < kMinFieldNumber || n > kMaxFieldNumber) return false;
    }
    return true;
}

template <class T>
consteval bool field_numbers_not_reserved() {
    constexpr auto nums = collect_field_numbers<T>();
    for (int n : nums) {
        if (n >= kReservedFirst && n <= kReservedLast) return false;
    }
    return true;
}

template <class T>
consteval bool field_numbers_unique() {
    constexpr auto nums = collect_field_numbers<T>();
    auto sorted = nums;
    std::sort(sorted.begin(), sorted.end());
    return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}

// ===== Varint =====

inline void write_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

inline std::uint64_t read_varint(std::string_view& in) {
    std::uint64_t result = 0;
    int shift = 0;
    while (!in.empty()) {
        std::uint8_t b = static_cast<std::uint8_t>(in.front());
        in.remove_prefix(1);
        result |= std::uint64_t(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return result;
        shift += 7;
        if (shift >= 70) throw std::runtime_error("proto3: varint too long");
    }
    throw std::runtime_error("proto3: truncated varint");
}

inline void write_tag(std::string& out, int field_num, wire_type wt) {
    write_varint(out, (std::uint64_t(field_num) << 3) |
                          static_cast<std::uint64_t>(wt));
}

// ===== Zigzag varint helpers =====

template <class T>
void write_zigzag_varint(std::string& out, T v) {
    static_assert(std::is_integral_v<T> && std::is_signed_v<T>,
                  "proto3::zigzag requires a signed integer field");
    using U = std::make_unsigned_t<T>;
    constexpr int bits = static_cast<int>(sizeof(T) * 8);
    U z = static_cast<U>(static_cast<U>(v) << 1) ^
          static_cast<U>(v >> (bits - 1));
    write_varint(out, std::uint64_t(z));
}

template <class T>
void read_zigzag_varint(std::string_view& in, T& v) {
    static_assert(std::is_integral_v<T> && std::is_signed_v<T>,
                  "proto3::zigzag requires a signed integer field");
    using U = std::make_unsigned_t<T>;
    std::uint64_t z = read_varint(in);
    U zu = static_cast<U>(z);
    v = static_cast<T>((zu >> 1) ^ -static_cast<U>(zu & 1));
}

inline void write_fixed32(std::string& out, std::uint32_t bits) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
}

inline void write_fixed64(std::string& out, std::uint64_t bits) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
}

inline std::uint32_t read_fixed32(std::string_view& in) {
    if (in.size() < 4) throw std::runtime_error("proto3: truncated fixed32");
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i)
        bits |= std::uint32_t(static_cast<std::uint8_t>(in[i])) << (i * 8);
    in.remove_prefix(4);
    return bits;
}

inline std::uint64_t read_fixed64(std::string_view& in) {
    if (in.size() < 8) throw std::runtime_error("proto3: truncated fixed64");
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
        bits |= std::uint64_t(static_cast<std::uint8_t>(in[i])) << (i * 8);
    in.remove_prefix(8);
    return bits;
}

// ===== Wire-type for a scalar T =====

template <class T>
consteval wire_type scalar_wire_type() {
    if constexpr (std::is_same_v<T, float>)  return wire_type::I32;
    else if constexpr (std::is_same_v<T, double>) return wire_type::I64;
    else if constexpr (std::is_same_v<T, std::string>) return wire_type::LEN;
    else return wire_type::VARINT;  // bool, ints, enums, sint
}

// ===== Scalar read/write =====

template <class T>
void write_scalar(std::string& out, const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        write_varint(out, v ? 1u : 0u);
    } else if constexpr (std::is_same_v<T, float>) {
        write_fixed32(out, std::bit_cast<std::uint32_t>(v));
    } else if constexpr (std::is_same_v<T, double>) {
        write_fixed64(out, std::bit_cast<std::uint64_t>(v));
    } else if constexpr (std::is_same_v<T, std::string>) {
        write_varint(out, v.size());
        out.append(v);
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        write_varint(out, std::uint64_t(std::int64_t(static_cast<U>(v))));
    } else if constexpr (std::is_signed_v<T>) {
        // proto3 int32/int64: sign-extended into a 64-bit varint.
        write_varint(out, std::uint64_t(std::int64_t(v)));
    } else {
        write_varint(out, std::uint64_t(v));
    }
}

template <class T>
void read_scalar(std::string_view& in, T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        v = read_varint(in) != 0;
    } else if constexpr (std::is_same_v<T, float>) {
        v = std::bit_cast<float>(read_fixed32(in));
    } else if constexpr (std::is_same_v<T, double>) {
        v = std::bit_cast<double>(read_fixed64(in));
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::uint64_t len = read_varint(in);
        if (in.size() < len) throw std::runtime_error("proto3: truncated string");
        v.assign(in.data(), len);
        in.remove_prefix(len);
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        v = static_cast<T>(static_cast<U>(read_varint(in)));
    } else {
        v = static_cast<T>(read_varint(in));
    }
}

// proto3: a scalar field equal to its language default is omitted on the wire.
template <class T>
bool is_default_scalar(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) return v.empty();
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) return v == T{};
    else return false;
}

// ===== Forward declarations =====

template <class T> void serialize_into(std::string& out, const T& msg);
template <class T> void deserialize_from(std::string_view in, T& msg);

// Always-emit (presence-carrying) field encoder. Bypasses the proto3
// default-skip rule that `encode_field` applies to scalars. Used by:
//   - map entries (proto3 map entries always serialize key and value, even
//     when defaulted), and
//   - std::optional<T> fields, where presence is carried by has_value().
template <class T>
void encode_field_present(std::string& out, int fn, const T& v) {
    if constexpr (is_message_v<T>) {
        write_tag(out, fn, wire_type::LEN);
        std::string buf;
        serialize_into(buf, v);
        write_varint(out, buf.size());
        out.append(buf);
    } else {
        write_tag(out, fn, scalar_wire_type<T>());
        write_scalar(out, v);
    }
}

// ===== Encode a single field with its tag =====

template <class T>
void encode_field(std::string& out, int fn, const T& v) {
    using U = std::remove_cvref_t<T>;

    if constexpr (is_optional_v<U>) {
        using Inner = typename U::value_type;
        static_assert(!is_vector_v<Inner> && !is_map_v<Inner> &&
                      !is_variant_v<Inner> && !is_optional_v<Inner>,
            "std::optional<T> requires T to be a scalar, std::string, or "
            "message type — not vector/map/variant/optional");
        if (!v.has_value()) return;
        encode_field_present<Inner>(out, fn, *v);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        if (v.empty()) return;

        if constexpr (is_message_v<E>) {
            for (const auto& e : v) {
                write_tag(out, fn, wire_type::LEN);
                std::string buf;
                serialize_into(buf, e);
                write_varint(out, buf.size());
                out.append(buf);
            }
        } else if constexpr (std::is_same_v<E, std::string>) {
            for (const auto& e : v) {
                write_tag(out, fn, wire_type::LEN);
                write_scalar(out, e);
            }
        } else {
            // Packed encoding for repeated scalars (proto3 default).
            std::string buf;
            for (const auto& e : v) write_scalar(buf, e);
            write_tag(out, fn, wire_type::LEN);
            write_varint(out, buf.size());
            out.append(buf);
        }
    } else if constexpr (is_map_v<U>) {
        using K = typename is_map<U>::key_type;
        using V = typename is_map<U>::mapped_type;
        for (const auto& kv : v) {
            std::string entry;
            encode_field_present<K>(entry, 1, kv.first);
            encode_field_present<V>(entry, 2, kv.second);
            write_tag(out, fn, wire_type::LEN);
            write_varint(out, entry.size());
            out.append(entry);
        }
    } else if constexpr (is_message_v<U>) {
        write_tag(out, fn, wire_type::LEN);
        std::string buf;
        serialize_into(buf, v);
        write_varint(out, buf.size());
        out.append(buf);
    } else {
        if (is_default_scalar(v)) return;
        write_tag(out, fn, scalar_wire_type<U>());
        write_scalar(out, v);
    }
}

// ===== Zigzag-encoded variant of encode_field / decode_field =====
// Used for fields tagged with [[= proto3::zigzag]]. Only legal on signed
// integers and vector<signed integer>.

template <class T>
void encode_field_zz(std::string& out, int fn, const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        // Presence-carrying — emit even when *v equals U::value_type{}.
        // Inner type's signed-integer constraint is enforced by
        // write_zigzag_varint's own static_assert.
        if (!v.has_value()) return;
        write_tag(out, fn, wire_type::VARINT);
        write_zigzag_varint(out, *v);
    } else if constexpr (is_vector_v<U>) {
        if (v.empty()) return;
        std::string buf;
        for (const auto& e : v) write_zigzag_varint(buf, e);
        write_tag(out, fn, wire_type::LEN);
        write_varint(out, buf.size());
        out.append(buf);
    } else {
        if (v == U{}) return;  // proto3 default skip
        write_tag(out, fn, wire_type::VARINT);
        write_zigzag_varint(out, v);
    }
}

template <class T>
void decode_field_zz(std::string_view& in, T& v, wire_type wt) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        if (!v.has_value()) v.emplace();
        read_zigzag_varint(in, *v);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        if (wt == wire_type::LEN) {
            std::uint64_t len = read_varint(in);
            if (in.size() < len) throw std::runtime_error("proto3: truncated packed");
            std::string_view sub(in.data(), len);
            in.remove_prefix(len);
            while (!sub.empty()) {
                E e;
                read_zigzag_varint(sub, e);
                v.push_back(e);
            }
        } else {
            E e;
            read_zigzag_varint(in, e);
            v.push_back(e);
        }
    } else {
        read_zigzag_varint(in, v);
    }
}

// ===== Fixed-width varint replacement =====
// Used for fields tagged with [[= proto3::fixed]]. Only legal on 4- or 8-byte
// integers and vector<same>; the C++ type's signedness selects fixed vs
// sfixed and its width selects I32 vs I64.

template <class T>
consteval void check_fixed_element_type() {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "proto3::fixed requires a non-bool integer field");
    static_assert(sizeof(T) == 4 || sizeof(T) == 8,
                  "proto3::fixed requires a 4- or 8-byte integer "
                  "(int32, uint32, int64, uint64)");
}

template <class T>
consteval wire_type fixed_wire_type() {
    if constexpr (sizeof(T) == 4) return wire_type::I32;
    else                          return wire_type::I64;
}

template <class T>
void write_fixed_value(std::string& out, T v) {
    if constexpr (sizeof(T) == 4) {
        write_fixed32(out, std::bit_cast<std::uint32_t>(v));
    } else {
        write_fixed64(out, std::bit_cast<std::uint64_t>(v));
    }
}

template <class T>
void read_fixed_value(std::string_view& in, T& v) {
    if constexpr (sizeof(T) == 4) {
        v = std::bit_cast<T>(read_fixed32(in));
    } else {
        v = std::bit_cast<T>(read_fixed64(in));
    }
}

template <class T>
void encode_field_fx(std::string& out, int fn, const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        using Inner = typename U::value_type;
        check_fixed_element_type<Inner>();
        if (!v.has_value()) return;
        write_tag(out, fn, fixed_wire_type<Inner>());
        write_fixed_value(out, *v);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        check_fixed_element_type<E>();
        if (v.empty()) return;
        std::string buf;
        for (const auto& e : v) write_fixed_value(buf, e);
        write_tag(out, fn, wire_type::LEN);
        write_varint(out, buf.size());
        out.append(buf);
    } else {
        check_fixed_element_type<U>();
        if (v == U{}) return;  // proto3 default skip
        write_tag(out, fn, fixed_wire_type<U>());
        write_fixed_value(out, v);
    }
}

template <class T>
void decode_field_fx(std::string_view& in, T& v, wire_type wt) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        using Inner = typename U::value_type;
        check_fixed_element_type<Inner>();
        if (!v.has_value()) v.emplace();
        read_fixed_value(in, *v);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        check_fixed_element_type<E>();
        if (wt == wire_type::LEN) {
            std::uint64_t len = read_varint(in);
            if (in.size() < len) throw std::runtime_error("proto3: truncated packed");
            std::string_view sub(in.data(), len);
            in.remove_prefix(len);
            while (!sub.empty()) {
                E e;
                read_fixed_value(sub, e);
                v.push_back(e);
            }
        } else {
            E e;
            read_fixed_value(in, e);
            v.push_back(e);
        }
    } else {
        check_fixed_element_type<U>();
        read_fixed_value(in, v);
    }
}

// ===== Wire-via-conversion =====
// Used for fields tagged [[= proto3::as_timestamp]] / [[= proto3::as_duration]]
// (and any future as_<wire-type> annotations). The C++ field type is
// converted to the wire-side struct on encode and back on decode. The
// wire-side type itself must be a normal proto3 message — these helpers
// route through the existing message encode/decode paths.
//
// Composes with std::vector (repeated wire messages) and std::optional
// (presence-tracked wire message). The field type, vector element type,
// or optional inner type must be:
//   - constructible from a const Wire& (for decode), and
//   - convertible to Wire (for encode), typically via a converting ctor
//     or operator on Wire — see proto3::Timestamp / proto3::Duration in
//     <well_known.hpp> for the canonical examples.

template <class Wire, class T>
void encode_field_as(std::string& out, int fn, const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        using Inner = typename U::value_type;
        static_assert(std::is_constructible_v<Wire, const Inner&>,
            "field type must be convertible to the wire-side struct");
        if (!v.has_value()) return;
        Wire w(*v);
        encode_field_present<Wire>(out, fn, w);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        static_assert(std::is_constructible_v<Wire, const E&>,
            "vector element type must be convertible to the wire-side struct");
        for (const auto& e : v) {
            Wire w(e);
            encode_field_present<Wire>(out, fn, w);
        }
    } else {
        static_assert(std::is_constructible_v<Wire, const U&>,
            "field type must be convertible to the wire-side struct");
        Wire w(v);
        encode_field_present<Wire>(out, fn, w);
    }
}

template <class Wire, class T>
void decode_field_as(std::string_view& in, T& v, wire_type wt) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        using Inner = typename U::value_type;
        static_assert(std::is_convertible_v<Wire, Inner>,
            "wire-side struct must be convertible to the field type");
        Wire w;
        decode_field(in, w, wt);
        v = static_cast<Inner>(w);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        static_assert(std::is_convertible_v<Wire, E>,
            "wire-side struct must be convertible to the vector element type");
        Wire w;
        decode_field(in, w, wt);
        v.push_back(static_cast<E>(w));
    } else {
        static_assert(std::is_convertible_v<Wire, U>,
            "wire-side struct must be convertible to the field type");
        Wire w;
        decode_field(in, w, wt);
        v = static_cast<U>(w);
    }
}

// ===== Skip an unknown field in the input =====

inline void skip_field(std::string_view& in, wire_type wt) {
    switch (wt) {
        case wire_type::VARINT:
            read_varint(in);
            return;
        case wire_type::I64:
            if (in.size() < 8) throw std::runtime_error("proto3: truncated I64");
            in.remove_prefix(8);
            return;
        case wire_type::LEN: {
            std::uint64_t len = read_varint(in);
            if (in.size() < len) throw std::runtime_error("proto3: truncated LEN");
            in.remove_prefix(len);
            return;
        }
        case wire_type::I32:
            if (in.size() < 4) throw std::runtime_error("proto3: truncated I32");
            in.remove_prefix(4);
            return;
    }
    throw std::runtime_error("proto3: unknown wire type");
}

// ===== Decode a field's value into v =====

template <class T>
void decode_field(std::string_view& in, T& v, wire_type wt) {
    using U = std::remove_cvref_t<T>;

    if constexpr (is_optional_v<U>) {
        // Constraints already asserted on the encode side; mirror here in
        // case decode_field is reached without going through encode_field
        // first (e.g., a unit test that only round-trips wire bytes).
        using Inner = typename U::value_type;
        static_assert(!is_vector_v<Inner> && !is_map_v<Inner> &&
                      !is_variant_v<Inner> && !is_optional_v<Inner>);
        if (!v.has_value()) v.emplace();
        decode_field(in, *v, wt);
    } else if constexpr (is_vector_v<U>) {
        using E = typename U::value_type;
        if constexpr (is_message_v<E>) {
            std::uint64_t len = read_varint(in);
            if (in.size() < len) throw std::runtime_error("proto3: truncated message");
            E e{};
            deserialize_from(std::string_view(in.data(), len), e);
            in.remove_prefix(len);
            v.push_back(std::move(e));
        } else if constexpr (std::is_same_v<E, std::string>) {
            E e;
            read_scalar(in, e);
            v.push_back(std::move(e));
        } else {
            // Repeated scalar may arrive packed (LEN) or one tag per value.
            if (wt == wire_type::LEN) {
                std::uint64_t len = read_varint(in);
                if (in.size() < len) throw std::runtime_error("proto3: truncated packed");
                std::string_view sub(in.data(), len);
                in.remove_prefix(len);
                while (!sub.empty()) {
                    E e;
                    read_scalar(sub, e);
                    v.push_back(e);
                }
            } else {
                E e;
                read_scalar(in, e);
                v.push_back(e);
            }
        }
    } else if constexpr (is_map_v<U>) {
        using K = typename is_map<U>::key_type;
        using V = typename is_map<U>::mapped_type;
        std::uint64_t len = read_varint(in);
        if (in.size() < len) throw std::runtime_error("proto3: truncated map entry");
        std::string_view sub(in.data(), len);
        in.remove_prefix(len);
        K key{};
        V val{};
        while (!sub.empty()) {
            std::uint64_t etag = read_varint(sub);
            int efn = static_cast<int>(etag >> 3);
            wire_type ewt = static_cast<wire_type>(etag & 0x7);
            if (efn == 1)      decode_field(sub, key, ewt);
            else if (efn == 2) decode_field(sub, val, ewt);
            else               skip_field(sub, ewt);
        }
        v.insert_or_assign(std::move(key), std::move(val));
    } else if constexpr (is_message_v<U>) {
        std::uint64_t len = read_varint(in);
        if (in.size() < len) throw std::runtime_error("proto3: truncated message");
        deserialize_from(std::string_view(in.data(), len), v);
        in.remove_prefix(len);
    } else {
        read_scalar(in, v);
    }
}

// ===== oneof encode/decode (variant + annotation) =====

template <auto Nums, class V>
void encode_oneof_variant(std::string& out, const V& v) {
    constexpr std::size_t N = Nums.size();
    static_assert(std::variant_size_v<V> == N + 1,
        "proto3::oneof field-number count must equal variant alternative "
        "count minus 1 (for the leading std::monostate)");
    static_assert(std::is_same_v<std::variant_alternative_t<0, V>, std::monostate>,
        "proto3 oneof variant's first alternative must be std::monostate");

    if (v.index() == 0) return;
    auto try_one = [&]<std::size_t I>() {
        if (v.index() == I + 1) {
            // Bypass is_default_scalar so an explicitly-set oneof alternative
            // always reaches the wire (proto3 oneof carries presence).
            const auto& val = std::get<I + 1>(v);
            using ValT = std::remove_cvref_t<decltype(val)>;
            constexpr int fn = Nums[I];
            if constexpr (is_message_v<ValT>) {
                encode_field(out, fn, val);
            } else {
                write_tag(out, fn, scalar_wire_type<ValT>());
                write_scalar(out, val);
            }
        }
    };
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (try_one.template operator()<Is>(), ...);
    }(std::make_index_sequence<N>{});
}

template <auto Nums, class V>
bool try_decode_oneof_variant(std::string_view& in, V& v, int fn, wire_type wt) {
    constexpr std::size_t N = Nums.size();
    static_assert(std::variant_size_v<V> == N + 1);
    static_assert(std::is_same_v<std::variant_alternative_t<0, V>, std::monostate>);

    bool matched = false;
    auto try_one = [&]<std::size_t I>() {
        if (matched) return;
        if (fn != Nums[I]) return;
        v.template emplace<I + 1>();
        decode_field(in, std::get<I + 1>(v), wt);
        matched = true;
    };
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (try_one.template operator()<Is>(), ...);
    }(std::make_index_sequence<N>{});
    return matched;
}

// ===== Per-field dispatch =====

template <std::size_t I, class T>
void encode_one(std::string& out, const T& msg) {
    constexpr static auto fields = fields_of_type(^^T);
    constexpr auto m = fields[I];
    // The unknown-fields sink is metadata, not a wire field. serialize_into
    // appends its bytes after the known-field fold; encoding it here would
    // double-emit it as a plain bytes field with whatever default field
    // number declaration order assigned.
    if constexpr (!field_skipped_at(m) && !field_unknown_fields_at(m)) {
        constexpr auto field_type = remove_cvref(type_of(m));
        if constexpr (is_variant(field_type)) {
            static_assert(is_oneof_at(m),
                "std::variant field requires a [[= proto3::oneof<...>]] annotation");
            encode_oneof_variant<oneof_field_numbers_at<m>()>(out, msg.[:m:]);
        } else {
            static_assert(!(field_zigzag_at(m) && field_fixed_at(m)),
                "[[= proto3::zigzag]] and [[= proto3::fixed]] are mutually "
                "exclusive on the same field");
            static_assert(!field_bytes_at(m) ||
                          is_string_or_vector_string(field_type),
                "[[= proto3::bytes]] is only valid on std::string or "
                "std::vector<std::string> fields");
            static_assert(!(field_as_timestamp_at(m) &&
                            field_as_duration_at(m)),
                "[[= proto3::as_timestamp]] and [[= proto3::as_duration]] "
                "are mutually exclusive on the same field");
            constexpr int fn = field_number_at(m, I);
            if constexpr (field_as_timestamp_at(m)) {
                encode_field_as<Timestamp>(out, fn, msg.[:m:]);
            } else if constexpr (field_as_duration_at(m)) {
                encode_field_as<Duration>(out, fn, msg.[:m:]);
            } else if constexpr (field_zigzag_at(m)) {
                encode_field_zz(out, fn, msg.[:m:]);
            } else if constexpr (field_fixed_at(m)) {
                encode_field_fx(out, fn, msg.[:m:]);
            } else {
                encode_field(out, fn, msg.[:m:]);
            }
        }
    }
}

template <std::size_t I, class T>
bool try_decode_one(std::string_view& in, T& msg, int fn, wire_type wt) {
    constexpr static auto fields = fields_of_type(^^T);
    constexpr auto m = fields[I];
    // Unknown-fields sink is matched by no wire field number — fall through
    // so deserialize_from's "unhandled" path captures the bytes for it.
    if constexpr (field_skipped_at(m) || field_unknown_fields_at(m)) {
        return false;
    } else {
        constexpr auto field_type = remove_cvref(type_of(m));
        if constexpr (is_variant(field_type)) {
            static_assert(is_oneof_at(m),
                "std::variant field requires a [[= proto3::oneof<...>]] annotation");
            return try_decode_oneof_variant<oneof_field_numbers_at<m>()>(
                in, msg.[:m:], fn, wt);
        } else {
            constexpr int my_fn = field_number_at(m, I);
            if (fn == my_fn) {
                if constexpr (field_as_timestamp_at(m)) {
                    decode_field_as<Timestamp>(in, msg.[:m:], wt);
                } else if constexpr (field_as_duration_at(m)) {
                    decode_field_as<Duration>(in, msg.[:m:], wt);
                } else if constexpr (field_zigzag_at(m)) {
                    decode_field_zz(in, msg.[:m:], wt);
                } else if constexpr (field_fixed_at(m)) {
                    decode_field_fx(in, msg.[:m:], wt);
                } else {
                    decode_field(in, msg.[:m:], wt);
                }
                return true;
            }
            return false;
        }
    }
}

// ===== Top-level entry points =====

template <class T>
void serialize_into(std::string& out, const T& msg) {
    static_assert(field_numbers_in_range<T>(),
        "proto3: field number must be in [1, 2^29 - 1]");
    static_assert(field_numbers_not_reserved<T>(),
        "proto3: field number 19000-19999 is reserved by the protobuf "
        "implementation");
    static_assert(field_numbers_unique<T>(),
        "proto3: duplicate field number — every field (including each "
        "oneof alternative) must have a distinct number");
    static_assert(unknown_fields_count<T>() <= 1,
        "proto3: at most one [[= proto3::unknown_fields]] member per struct");
    static_assert(unknown_fields_member_is_string<T>(),
        "proto3: [[= proto3::unknown_fields]] is only valid on a "
        "std::string member");
    constexpr static auto fields = fields_of_type(^^T);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (encode_one<Is, T>(out, msg), ...);
    }(std::make_index_sequence<fields.size()>{});
    // Preserved unknown fields (if the struct opted into the sink) get
    // appended at the tail. Their bytes are already complete (tag +
    // value) and properly framed, so we just splice them onto the output.
    constexpr auto uf_idx = unknown_fields_index<T>();
    if constexpr (uf_idx.has_value()) {
        constexpr auto m = fields[*uf_idx];
        out.append(msg.[:m:]);
    }
}

template <class T>
std::string serialize(const T& msg) {
    std::string out;
    serialize_into(out, msg);
    return out;
}

template <class T>
void deserialize_from(std::string_view in, T& msg) {
    static_assert(field_numbers_in_range<T>(),
        "proto3: field number must be in [1, 2^29 - 1]");
    static_assert(field_numbers_not_reserved<T>(),
        "proto3: field number 19000-19999 is reserved by the protobuf "
        "implementation");
    static_assert(field_numbers_unique<T>(),
        "proto3: duplicate field number — every field (including each "
        "oneof alternative) must have a distinct number");
    static_assert(unknown_fields_count<T>() <= 1,
        "proto3: at most one [[= proto3::unknown_fields]] member per struct");
    static_assert(unknown_fields_member_is_string<T>(),
        "proto3: [[= proto3::unknown_fields]] is only valid on a "
        "std::string member");
    constexpr static auto fields = fields_of_type(^^T);
    while (!in.empty()) {
        // Remember where this field's tag begins, so we can capture the
        // full (tag + value) byte range if it's unrecognized.
        const char* field_start = in.data();
        std::uint64_t tag = read_varint(in);
        int fn = static_cast<int>(tag >> 3);
        wire_type wt = static_cast<wire_type>(tag & 0x7);

        bool handled = false;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((handled = handled || try_decode_one<Is, T>(in, msg, fn, wt)), ...);
        }(std::make_index_sequence<fields.size()>{});

        if (!handled) {
            skip_field(in, wt);
            constexpr auto uf_idx = unknown_fields_index<T>();
            if constexpr (uf_idx.has_value()) {
                constexpr auto m = fields[*uf_idx];
                msg.[:m:].append(
                    field_start,
                    static_cast<std::size_t>(in.data() - field_start));
            }
        }
    }
}

template <class T>
T deserialize(std::string_view data) {
    T msg{};
    deserialize_from(data, msg);
    return msg;
}

} // namespace proto3
