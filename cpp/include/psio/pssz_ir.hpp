#pragma once

// Checked PSSZ runtime codec for the actual schema_ir.hpp IR.
// This does not reinterpret the older lowercase psio::schema artifact.
//
// Two width profiles:
//   encode<W> / decode_exact<W> / decode_compatible<W>, W = 1, 2 or 4: the
//     explicit profile (psio::pssz_<W>), one width inherited by every type.
//   encode_auto / decode_auto / validate_auto (W = 0 internally): the
//     automatic profile (psio::pssz). Every record, array and list uses its
//     own type-derived width, computed from the IR exactly as
//     auto_pssz_width<T>() computes it from the C++ type: an explicit
//     "maxDynamicData" attribute rules, otherwise the bound inferred from
//     bounded lists / strings and member "lengthBound" attributes
//     (max_size.hpp bookkeeping), otherwise 4. Caps and length bounds are
//     enforced on encode and decode.
//
// Attributes read by the codec (schema_builder.hpp writes them):
//   Object  definitionWillNotChange    fixed layout (no header)
//           maxDynamicData = "N"       explicit cap (automatic profile: every
//                                      record; explicit: the top-level type)
//   Member  lengthBound = "max=N" | "exact=N" | "min=N" (comma-separated)
//           jsonInt / ijsonInt = "number" | "string"   (pssz_ir_json.hpp)
//   Variant member  ijsonName = "..."                   (pssz_ir_json.hpp)
// The JSON / I-JSON text path over the same IR is pssz_ir_json.hpp.
//
// Custom types (pssz_ir_custom.hpp). Every entry point takes an optional
// CustomHandlers registry. With one, each value of a custom node whose
// handler has a `check` is checked — on decode / validate after its
// structure is accepted, on encode before any byte is returned, nested
// values first — and, when the registry says `require_all`, a custom node
// with no handler is an error. Without a registry a custom node is its wire
// type: structure only.
#include <psio/detail/pssz_record_layout.hpp>
#include <psio/detail/validate_depth.hpp>
#include <psio/pssz_ir_custom.hpp>
#include <psio/schema_ir.hpp>
#include <psio/time.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio::pssz_ir
{
struct Value;
struct Signed
{
   __int128 value = 0;
   friend bool operator==(const Signed &, const Signed &) = default;
};
struct Unsigned
{
   unsigned __int128 value = 0;
   friend bool operator==(const Unsigned &, const Unsigned &) = default;
};
using Bytes = std::vector<unsigned char>;
struct Bits
{
   Bytes bytes;
   std::uint64_t bit_count = 0;
   friend bool operator==(const Bits &, const Bits &) = default;
};
using Sequence = std::vector<Value>;
using Record = std::vector<std::pair<std::string, Value>>;
struct Optional
{
   std::shared_ptr<const Value> some;
   friend bool operator==(const Optional &, const Optional &);
};
struct Variant
{
   std::size_t selector = 0;
   std::shared_ptr<const Value> payload;
   friend bool operator==(const Variant &, const Variant &);
};
struct Value
{
   using Storage = std::variant<bool, Signed, Unsigned, float, double, std::string, Bytes, Bits,
                                Sequence, Record, Optional, Variant>;
   Storage value = Optional{};
   Value() = default;
   template <typename T>
      requires(!std::is_same_v<std::remove_cvref_t<T>, Value>)
   Value(T &&v) : value(std::forward<T>(v))
   {
   }
   static Value none()
   {
      return Optional{};
   }
   static Value some(Value value)
   {
      return Optional{std::make_shared<const Value>(std::move(value))};
   }
   static Value variant(std::size_t selector, Value value)
   {
      return Variant{selector, std::make_shared<const Value>(std::move(value))};
   }
   friend bool operator==(const Value &, const Value &) = default;
};
inline bool operator==(const Optional &a, const Optional &b)
{
   return !a.some ? !b.some : b.some && *a.some == *b.some;
}
inline bool operator==(const Variant &a, const Variant &b)
{
   return a.selector == b.selector &&
          (!a.payload ? !b.payload : b.payload && *a.payload == *b.payload);
}

namespace detail
{
inline codec_status fail(const char *message)
{
   return codec_fail(message, 0, "pssz-ir");
}
inline codec_status depth(std::size_t n)
{
   return n <= kMaxValidationDepth ? codec_ok() : fail("pssz IR: node depth limit exceeded");
}
inline bool dwnc(const std::vector<schema_types::Attribute> &attributes)
{
   return std::any_of(attributes.begin(), attributes.end(),
                      [](const auto &a) { return a.name == "definitionWillNotChange"; });
}
inline bool none(const Value &value)
{
   auto p = std::get_if<Optional>(&value.value);
   return p && !p->some;
}
// A member's length_bound annotation: bytes of a string, elements of a list.
struct LengthBound
{
   std::optional<std::uint64_t> exact, max, min;
   // The count that bounds the encoded size: exact, else max.
   std::optional<std::uint64_t> count() const { return exact ? exact : max; }
   bool admits(std::uint64_t n) const
   {
      return !((max && n > *max) || (exact && n != *exact) || (min && n < *min));
   }
};
inline bool parse_u64(std::string_view text, std::uint64_t &out)
{
   if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0'))
      return false;
   unsigned __int128 value = 0;
   for (char c : text)
   {
      if (c < '0' || c > '9')
         return false;
      value = value * 10 + static_cast<unsigned>(c - '0');
   }
   if (value > std::numeric_limits<std::uint64_t>::max())
      return false;
   out = static_cast<std::uint64_t>(value);
   return true;
}
inline const schema_types::Attribute *find_attribute(
    const std::vector<schema_types::Attribute> &values, std::string_view name)
{
   for (const auto &value : values)
      if (value.name == name)
         return &value;
   return nullptr;
}
// "max=N", "exact=N", "min=N", comma-separated, each at most once.
inline codec_status parse_length_bound(const std::vector<schema_types::Attribute> &values,
                                       std::optional<LengthBound> &out)
{
   out.reset();
   const auto *attribute = find_attribute(values, "lengthBound");
   if (!attribute)
      return codec_ok();
   if (!attribute->value || attribute->value->empty())
      return fail("pssz IR: malformed lengthBound attribute");
   LengthBound bound;
   std::string_view rest = *attribute->value;
   while (true)
   {
      const auto comma = rest.find(',');
      const auto item = rest.substr(0, comma);
      const auto equals = item.find('=');
      std::uint64_t n;
      if (equals == std::string_view::npos || !parse_u64(item.substr(equals + 1), n))
         return fail("pssz IR: malformed lengthBound attribute");
      const auto key = item.substr(0, equals);
      auto *slot = key == "max" ? &bound.max : key == "exact" ? &bound.exact
                                           : key == "min"     ? &bound.min
                                                              : nullptr;
      if (!slot || *slot)
         return fail("pssz IR: malformed lengthBound attribute");
      *slot = n;
      if (comma == std::string_view::npos)
         break;
      rest = rest.substr(comma + 1);
   }
   out = bound;
   return codec_ok();
}
inline codec_status parse_cap(const std::vector<schema_types::Attribute> &values,
                              std::optional<std::uint64_t> &out)
{
   out.reset();
   const auto *attribute = find_attribute(values, "maxDynamicData");
   if (!attribute)
      return codec_ok();
   std::uint64_t n;
   if (!attribute->value || !parse_u64(*attribute->value, n))
      return fail("pssz IR: malformed maxDynamicData attribute");
   out = n;
   return codec_ok();
}
enum class attribute_site
{
   record,          // Object / Struct
   record_member,
   variant,
   variant_member,
};
// maxDynamicData is implemented by both profiles, as the typed codecs do:
// the automatic profile (psio::pssz) derives widths from it and enforces it
// on every record; an explicit profile (psio::pssz_<W>) keeps its one
// inherited width and enforces the cap of the top-level type only.
// `caps`: the automatic profile, which also takes maxFields; an explicit
// profile rejects that constraint, as it rejects any it does not implement.
inline codec_status attributes(const std::vector<schema_types::Attribute> &values,
                               attribute_site site, bool caps)
{

   for (const auto &value : values)
   {
      const auto &name = value.name;
      if (name == "doc" || name == "description" || name == "visible")
         continue;
      if (site == attribute_site::record && name == "definitionWillNotChange")
         continue;
      if (site == attribute_site::record &&
          (name == "maxDynamicData" || (caps && name == "maxFields")))
         continue;
      if (site == attribute_site::record_member &&
          (name == "lengthBound" || name == "jsonInt" || name == "ijsonInt"))
      {
         if (name != "lengthBound" && (!value.value || (*value.value != "number" &&
                                                        *value.value != "string")))
            return fail("pssz IR: malformed integer form attribute");
         continue;
      }
      if (site == attribute_site::variant_member && name == "ijsonName" && value.value)
         continue;
      return fail("pssz IR: unsupported schema constraint or annotation");
   }
   if (site == attribute_site::record_member)
   {
      std::optional<LengthBound> bound;
      return parse_length_bound(values, bound);
   }
   if (site == attribute_site::record)
   {
      std::optional<std::uint64_t> cap;
      return parse_cap(values, cap);
   }
   return codec_ok();
}
inline codec_status append(std::vector<char> &out, std::span<const char> bytes)
{
   if (bytes.size() > out.max_size() - out.size())
      return fail("pssz IR: output size overflow");
   out.insert(out.end(), bytes.begin(), bytes.end());
   return codec_ok();
}
inline codec_status add(std::size_t &total, std::size_t n)
{
   if (n > std::numeric_limits<std::size_t>::max() - total)
      return fail("pssz IR: size overflow");
   total += n;
   return codec_ok();
}
struct Layout
{
   bool fixed = false;
   std::size_t size = 0;
   bool optional = false;
};
struct Field
{
   Layout layout;
   std::vector<char> bytes;
   bool omit = false;
};
inline std::size_t packed_byte_count(std::size_t count)
{
   return count / 8 + (count % 8 != 0);
}
inline codec_status validate_bit_data(std::span<const unsigned char> data, std::size_t count)
{
   if (data.size() != packed_byte_count(count))
      return fail("pssz IR: packed bit byte count mismatch");
   if (count % 8 && (data.back() & ~((1u << (count % 8)) - 1)))
      return fail("pssz IR: packed bits above logical count");
   return codec_ok();
}
inline codec_status bitlist_count(std::span<const char> wire, std::size_t bound, std::size_t &count)
{
   if (wire.empty() || wire.back() == 0)
      return fail("pssz IR: bitlist delimiter missing or noncanonical");
   const auto last = static_cast<unsigned char>(wire.back());
   unsigned highest = 0;
   for (unsigned byte = last; byte > 1; byte >>= 1)
      ++highest;
   if (wire.size() - 1 > (std::numeric_limits<std::size_t>::max() - highest) / 8)
      return fail("pssz IR: bitlist count overflow");
   count = (wire.size() - 1) * 8 + highest;
   if (count > bound)
      return fail("pssz IR: bitlist count exceeds bound");
   return codec_ok();
}

#define PSIO_PSSZ_IR_TRY(expression)                                                               \
   do                                                                                              \
   {                                                                                               \
      auto psio_ir_status_ = (expression);                                                         \
      if (!psio_ir_status_.ok())                                                                   \
         return psio_ir_status_;                                                                   \
   } while (false)

// ── Custom ids ─────────────────────────────────────────────────────────────
//
// A Custom node is `type` (what is on the wire) plus an `id`. These ids are
// part of the binary form:
//
//   bool, char             a one-byte primitive over Int{1} / Int{8}
//   string                 a RAW string: any bytes (C++ std::string, Rust
//                          RawString) over a byte container
//   utf8                   a UTF-8 string (C++ utf8_string, Rust String)
//                          over a byte container: validated on every read
//                          and write
//   hex                    bytes over a byte container
//   bitvector, bitlist     packed bits
//   box                    the inner type on the wire, with no inferred
//                          size bound (C++ / Rust Box<T>): the typed codecs
//                          give a boxed value no bound, so a type holding
//                          one gets the same offset width from the schema
//
// "unknown" marks a C++ type the schema builder could not describe and has
// no codec. Every other id adds nothing to the bytes: on the wire the node
// is exactly its inner type. The time ids ("time-point-<unit>",
// "duration-<unit>", "monotonic-<unit>", time.hpp) are built-in
// presentations over a tick integer; any other id names a custom type — a
// text presentation, an invariant, or both — whose handler is registered
// with the codecs (pssz_ir_custom.hpp CustomHandlers).
inline bool byte_adapter_id(std::string_view id) noexcept
{
   return id == "string" || id == "utf8" || id == "hex";
}
inline bool text_adapter_id(std::string_view id) noexcept
{
   return id == "string" || id == "utf8";
}
inline bool wire_custom_id(std::string_view id) noexcept
{
   return id == "bool" || id == "char" || byte_adapter_id(id) || id == "bitvector" ||
          id == "bitlist" || id == "unknown";
}

// W = 1, 2, 4: the explicit profile. W = 0: the automatic profile.
template <std::size_t W> class Codec
{
   static_assert(W == 0 || W == 1 || W == 2 || W == 4);
   static constexpr bool automatic = W == 0;
   const schema_types::Schema &schema;
   using Bound = std::optional<std::size_t>;
   // Widths already derived for a container type (automatic profile).
   mutable std::vector<std::pair<const schema_types::AnyType *, std::size_t>> widths;

   static Bound bound_add(Bound a, Bound b)
   {
      if (!a || !b || *b > std::numeric_limits<std::size_t>::max() - *a)
         return std::nullopt;
      return *a + *b;
   }
   static Bound bound_multiply(std::uint64_t count, Bound value)
   {
      if (count == 0)
         return 0;
      if (!value || count > std::numeric_limits<std::size_t>::max() ||
          *value > std::numeric_limits<std::size_t>::max() / count)
         return std::nullopt;
      return static_cast<std::size_t>(count) * *value;
   }
   static std::size_t width_class(Bound bound)
   {
      return bound && *bound <= 0xff ? 1 : bound && *bound <= 0xffff ? 2 : 4;
   }
   static const std::vector<schema_types::Attribute> *
   record_attributes(const schema_types::AnyType &type)
   {
      if (auto object = std::get_if<schema_types::Object>(&type.value))
         return &object->attributes;
      if (auto record = std::get_if<schema_types::Struct>(&type.value))
         return &record->attributes;
      return nullptr;
   }
   // Explicit cap of a resolved record type (automatic profile only).
   static codec_status cap_of(const schema_types::AnyType &type, std::optional<std::uint64_t> &out)
   {
      out.reset();
      if constexpr (automatic)
         if (auto values = record_attributes(type))
            return parse_cap(*values, out);
      return codec_ok();
   }
   // max_size.hpp fixed_payload<T>(): inline bytes, no offset slot in the
   // bound bookkeeping.
   codec_status fixed_payload(const schema_types::AnyType &input, bool &out,
                              std::vector<const schema_types::AnyType *> &active) const
   {
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(resolve(input, type));
      out = false;
      if (std::find(active.begin(), active.end(), type) != active.end())
         return codec_ok();
      active.push_back(type);
      auto status = std::visit(
          [&](const auto &t) -> codec_status
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, schema_types::Int> ||
                           std::is_same_v<T, schema_types::Float>)
                out = true;
             else if constexpr (std::is_same_v<T, schema_types::Array>)
             {
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                return fixed_payload(*child, out, active);
             }
             else if constexpr (std::is_same_v<T, schema_types::Custom>)
             {
                if (t.id == "bool" || t.id == "char")
                   out = true;
                else if (byte_adapter_id(t.id))
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(inner(t.type, child));
                   if (std::holds_alternative<schema_types::Array>(child->value))
                      return fixed_payload(*child, out, active);
                }
             }
             else if constexpr (std::is_same_v<T, schema_types::Struct> ||
                                std::is_same_v<T, schema_types::Object>)
             {
                if constexpr (std::is_same_v<T, schema_types::Object>)
                   if (!dwnc(t.attributes))
                      return codec_ok();
                out = true;
                for (const auto &member : t.members)
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(inner(member.type, child));
                   bool fixed;
                   PSIO_PSSZ_IR_TRY(fixed_payload(*child, fixed, active));
                   out = out && fixed;
                }
             }
             return codec_ok();
          },
          type->value);
      active.pop_back();
      return status;
   }
   // The bound max_size.hpp infers: max_field_with_annots<T>(annots) when
   // `reserve_cap` (a child reserves its declared cap in full), max_bare<T>()
   // for the type's own width. `annotation` is the member's lengthBound; it
   // reaches a string or list through Option. A recursive type is unbounded.
   codec_status bound_of(const schema_types::AnyType &input, const LengthBound *annotation,
                         bool reserve_cap, Bound &out,
                         std::vector<const schema_types::AnyType *> &active) const
   {
      const schema_types::AnyType *type;
      bool boxed = false;
      PSIO_PSSZ_IR_TRY(resolve(input, type, &boxed));
      out.reset();
      // A boxed value has no inferred bound (max_size.hpp has none for Box).
      if (boxed)
         return codec_ok();
      if (std::find(active.begin(), active.end(), type) != active.end())
         return codec_ok();
      if (reserve_cap)
      {
         std::optional<std::uint64_t> cap;
         PSIO_PSSZ_IR_TRY(cap_of(*type, cap));
         if (cap)
         {
            if (*cap <= std::numeric_limits<std::size_t>::max())
               out = static_cast<std::size_t>(*cap);
            return codec_ok();
         }
      }
      active.push_back(type);
      // N elements, each its reserved payload plus an offset slot when it
      // is not an inline fixed payload.
      auto elements = [&](const schema_types::Box<schema_types::AnyType> &box,
                          std::uint64_t count) -> codec_status
      {
         if (count == 0)
         {
            out = 0;
            return codec_ok();
         }
         const schema_types::AnyType *child;
         PSIO_PSSZ_IR_TRY(inner(box, child));
         Bound element;
         // (the unresolved type: a box on the way must be seen)
         PSIO_PSSZ_IR_TRY(bound_of(*box.get(), nullptr, true, element, active));
         bool fixed;
         std::vector<const schema_types::AnyType *> fresh;
         PSIO_PSSZ_IR_TRY(fixed_payload(*child, fixed, fresh));
         if (!fixed)
            element = bound_add(element, 4);
         out = bound_multiply(count, element);
         return codec_ok();
      };
      auto sequence = [&](const schema_types::AnyType &container) -> codec_status
      {
         if (auto list = std::get_if<schema_types::List>(&container.value))
         {
            if (annotation && annotation->count())
               return elements(list->type, *annotation->count());
            return codec_ok();
         }
         if (auto list = std::get_if<schema_types::BoundedList>(&container.value))
            return elements(list->type, list->maxCount);
         if (auto array = std::get_if<schema_types::Array>(&container.value))
            return elements(array->type, array->len);
         return codec_ok();
      };
      auto status = std::visit(
          [&](const auto &t) -> codec_status
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, schema_types::Int>)
             {
                std::size_t size;
                PSIO_PSSZ_IR_TRY(int_size(t, size));
                out = size;
             }
             else if constexpr (std::is_same_v<T, schema_types::Float>)
             {
                std::size_t size;
                PSIO_PSSZ_IR_TRY(float_size(t, size));
                out = size;
             }
             else if constexpr (std::is_same_v<T, schema_types::List> ||
                                std::is_same_v<T, schema_types::BoundedList> ||
                                std::is_same_v<T, schema_types::Array>)
                return sequence(*type);
             else if constexpr (std::is_same_v<T, schema_types::Option>)
             {
                // One selector byte is reserved for every optional.
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                PSIO_PSSZ_IR_TRY(bound_of(*t.type.get(), annotation, true, out, active));
                out = bound_add(out, 1);
             }
             else if constexpr (std::is_same_v<T, schema_types::Custom>)
             {
                if (t.id == "bool" || t.id == "char")
                   out = 1;
                else if (byte_adapter_id(t.id))
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(inner(t.type, child));
                   return sequence(*child);
                }
                // Bit types have no inferred bound.
             }
             else if constexpr (std::is_same_v<T, schema_types::Struct> ||
                                std::is_same_v<T, schema_types::Object>)
             {
                bool stable = true;
                if constexpr (std::is_same_v<T, schema_types::Object>)
                   stable = dwnc(t.attributes);
                Bound total = stable ? 0 : 4;
                for (const auto &member : t.members)
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(inner(member.type, child));
                   std::optional<LengthBound> bound;
                   PSIO_PSSZ_IR_TRY(parse_length_bound(member.attributes, bound));
                   Bound payload;
                   PSIO_PSSZ_IR_TRY(
                       bound_of(*member.type.get(), bound ? &*bound : nullptr, true, payload,
                                active));
                   bool fixed;
                   std::vector<const schema_types::AnyType *> fresh;
                   PSIO_PSSZ_IR_TRY(fixed_payload(*child, fixed, fresh));
                   if (!fixed)
                      payload = bound_add(payload, 4);
                   total = bound_add(total, payload);
                }
                out = total;
             }
             // Tuple, Variant: no inferred bound.
             return codec_ok();
          },
          type->value);
      active.pop_back();
      return status;
   }
   // The width of a record's header and slots, or of a list / array offset
   // table. Explicit profile: W. Automatic: the type's own (see the header).
   codec_status width_of(const schema_types::AnyType &type, std::size_t &out) const
   {
      if constexpr (!automatic)
      {
         (void)type;
         out = W;
         return codec_ok();
      }
      else
      {
         for (const auto &[known, width] : widths)
            if (known == &type)
            {
               out = width;
               return codec_ok();
            }
         out = 4;
         // A list's offset table is always 4 wide (a std::vector has no
         // bound of its own; a bounded list keeps the vector's width).
         if (!std::holds_alternative<schema_types::List>(type.value) &&
             !std::holds_alternative<schema_types::BoundedList>(type.value))
         {
            std::optional<std::uint64_t> cap;
            PSIO_PSSZ_IR_TRY(cap_of(type, cap));
            if (cap)
               out = *cap <= 0xff ? 1 : *cap <= 0xffff ? 2 : 4;
            else
            {
               Bound bound;
               std::vector<const schema_types::AnyType *> active;
               PSIO_PSSZ_IR_TRY(bound_of(type, nullptr, false, bound, active));
               out = width_class(bound);
            }
         }
         // Tuples and variants have no inferred bound: 4.
         widths.emplace_back(&type, out);
         return codec_ok();
      }
   }
 public:
   // Explicit profile: the cap of the top-level type bounds the whole
   // encoding (psio::pssz_<W>'s encode_pssz_checked / validate). Nested caps
   // are not looked up. No-op for the automatic profile, which checks every
   // record where it is read or written.
   codec_status check_top_cap(const schema_types::AnyType &root, std::size_t size) const
   {
      if constexpr (!automatic)
      {
         const schema_types::AnyType *type;
         PSIO_PSSZ_IR_TRY(resolve(root, type));
         if (auto values = record_attributes(*type))
         {
            std::optional<std::uint64_t> cap;
            PSIO_PSSZ_IR_TRY(parse_cap(*values, cap));
            if (cap && size > *cap)
               return fail("pssz IR: encoded size exceeds maxDynamicData cap");
         }
      }
      else
      {
         (void)root;
         (void)size;
      }
      return codec_ok();
   }
 private:
   // A record over its explicit cap is not a value of its type.
   static codec_status check_cap(const schema_types::AnyType &type, std::size_t size)
   {
      std::optional<std::uint64_t> cap;
      PSIO_PSSZ_IR_TRY(cap_of(type, cap));
      if (cap && size > *cap)
         return fail("pssz IR: encoded size exceeds maxDynamicData cap");
      return codec_ok();
   }
   // The length a member lengthBound constrains, looking through Option.
   static bool bound_admits(const LengthBound &bound, const Value &value)
   {
      if (auto optional = std::get_if<Optional>(&value.value))
         return !optional->some || bound_admits(bound, *optional->some);
      if (auto text = std::get_if<std::string>(&value.value))
         return bound.admits(text->size());
      if (auto bytes = std::get_if<Bytes>(&value.value))
         return bound.admits(bytes->size());
      if (auto items = std::get_if<Sequence>(&value.value))
         return bound.admits(items->size());
      return true;
   }
   static codec_status spans_for(std::size_t w, std::span<const char> bytes, bool stable,
                                 std::span<const psio::detail::pssz_field_layout> fields,
                                 std::span<psio::detail::pssz_field_span> spans, bool &unknown)
   {
      switch (w)
      {
      case 1:
         return psio::detail::pssz_record_spans<1>(bytes, 0, bytes.size(), stable, fields, spans,
                                                   unknown);
      case 2:
         return psio::detail::pssz_record_spans<2>(bytes, 0, bytes.size(), stable, fields, spans,
                                                   unknown);
      default:
         return psio::detail::pssz_record_spans<4>(bytes, 0, bytes.size(), stable, fields, spans,
                                                   unknown);
      }
   }

   // The type `input` is on the wire: named aliases followed, and the nodes
   // that add nothing to the bytes (Enum, Custom ids that are not part of
   // the binary form) looked through. `boxed` reports a "box" on the way.
   // The schema says which version of the IR it is written in, and it is
   // the one this codec reads (schema_ir.hpp, schema_ir_version).
   codec_status ir_version() const
   {
      if (!schema.ir_version)
         return fail("pssz IR: schema has no ir_version marker (it predates the box / utf8 "
                     "markers and would derive different widths); regenerate it");
      if (*schema.ir_version != schema_types::schema_ir_version)
         return fail("pssz IR: schema ir_version is newer than this library");
      return codec_ok();
   }
   // `input` with named aliases followed, and nothing else.
   codec_status alias(const schema_types::AnyType &input, const schema_types::AnyType *&out) const
   {
      out = &input;
      for (std::size_t hops = 0; hops <= schema.types.size(); ++hops)
      {
         auto ref = std::get_if<schema_types::Type>(&out->value);
         if (!ref)
            return codec_ok();
         const schema_types::AnyType *found = nullptr;
         for (const auto &member : schema.types)
            if (member.name == ref->type)
            {
               if (found)
                  return fail("pssz IR: duplicate named type");
               found = member.type.get();
            }
         if (!found)
            return fail("pssz IR: unresolved named type");
         out = found;
      }
      return fail("pssz IR: cyclic named aliases");
   }
   // The type an adapter (a Custom id with a binary meaning, a time id) or
   // an Enum sits on: found through named aliases only. A wrapper in that
   // place (an Enum, a "box", a text presentation) is a malformed schema in
   // every language, not something to look through.
   codec_status direct(const schema_types::AnyType &input, const schema_types::AnyType *&out) const
   {
      PSIO_PSSZ_IR_TRY(alias(input, out));
      auto custom = std::get_if<schema_types::Custom>(&out->value);
      if (std::holds_alternative<schema_types::Enum>(out->value) ||
          (custom && !wire_custom_id(custom->id)))
         return fail("pssz IR: an adapter or enum must sit directly on its wire type");
      return codec_ok();
   }
   codec_status direct(const schema_types::Box<schema_types::AnyType> &box,
                       const schema_types::AnyType *&out) const
   {
      if (!box.get())
         return fail("pssz IR: null schema box");
      return direct(*box, out);
   }
   codec_status enum_underlying(const schema_types::Enum &named) const
   {
      const schema_types::AnyType *under;
      PSIO_PSSZ_IR_TRY(direct(named.type, under));
      // Any integer the codec has; text needs 8 to 64 bits (pssz_ir_json.hpp).
      if (!std::holds_alternative<schema_types::Int>(under->value))
         return fail("pssz IR: an enum needs an underlying integer");
      return codec_ok();
   }
   codec_status resolve(const schema_types::AnyType &input, const schema_types::AnyType *&out,
                        bool *boxed = nullptr) const
   {
      out = &input;
      // Wrappers (Enum, Custom) between two aliases are finite in a valid
      // schema; the limit only stops a malformed one.
      for (std::size_t hops = 0; hops <= 4 * schema.types.size() + 64; ++hops)
      {
         // An Enum is its underlying integer on the wire.
         if (auto named = std::get_if<schema_types::Enum>(&out->value))
         {
            PSIO_PSSZ_IR_TRY(enum_underlying(*named));
            out = named->type.get();
            continue;
         }
         if (auto custom = std::get_if<schema_types::Custom>(&out->value);
             custom && !wire_custom_id(custom->id))
         {
            if (!custom->type.get())
               return fail("pssz IR: null schema box");
            if (boxed && custom->id == "box")
               *boxed = true;
            if (::psio::parse_time_schema_id(custom->id))
               PSIO_PSSZ_IR_TRY(time_ticks(*custom->type));
            else if (::psio::reserved_time_schema_id(custom->id))
               return fail("pssz IR: unsupported custom adapter");
            out = custom->type.get();
            continue;
         }
         auto ref = std::get_if<schema_types::Type>(&out->value);
         if (!ref)
            return codec_ok();
         const schema_types::AnyType *found = nullptr;
         for (const auto &member : schema.types)
            if (member.name == ref->type)
            {
               if (found)
                  return fail("pssz IR: duplicate named type");
               found = member.type.get();
            }
         if (!found)
            return fail("pssz IR: unresolved named type");
         out = found;
      }
      return fail("pssz IR: cyclic named aliases");
   }
   // A time id (time.hpp: "time-point-..." / "duration-...") is a built-in
   // presentation over the tick integer: anything else under it is a
   // malformed schema.
   codec_status time_ticks(const schema_types::AnyType &inner_type) const
   {
      const schema_types::AnyType *ticks;
      PSIO_PSSZ_IR_TRY(direct(inner_type, ticks));
      auto integer = std::get_if<schema_types::Int>(&ticks->value);
      if (!integer || (integer->bits != 8 && integer->bits != 16 && integer->bits != 32 &&
                       integer->bits != 64))
         return fail("pssz IR: time value requires an 8/16/32/64-bit tick integer");
      return codec_ok();
   }
   codec_status inner(const schema_types::Box<schema_types::AnyType> &box,
                      const schema_types::AnyType *&out) const
   {
      if (!box.get())
         return fail("pssz IR: null schema box");
      return resolve(*box, out);
   }
   // The type in `box`, as written: the value codecs resolve it themselves,
   // and see the custom nodes on the way.
   static codec_status child_of(const schema_types::Box<schema_types::AnyType> &box,
                                const schema_types::AnyType *&out)
   {
      if (!box.get())
         return fail("pssz IR: null schema box");
      out = box.get();
      return codec_ok();
   }
   // The `check` handlers of the custom nodes `input` passes through on the
   // way to its wire type, outermost first. A strict registry makes a custom
   // node with no handler an error. The schema was validated by metadata().
   codec_status custom_checks(const schema_types::AnyType &input,
                              std::vector<const CustomHandlers::Handler *> &out) const
   {
      const schema_types::AnyType *at = &input;
      for (std::size_t hops = 0; hops <= 4 * schema.types.size() + 64; ++hops)
      {
         if (auto custom = std::get_if<schema_types::Custom>(&at->value))
         {
            if (wire_custom_id(custom->id) || !custom->type.get())
               return codec_ok();
            if (custom->id != "box" && !::psio::parse_time_schema_id(custom->id) &&
                !::psio::reserved_time_schema_id(custom->id))
            {
               const auto *handler = customs->find(custom->id);
               if (!handler && customs->require_all)
                  return fail("pssz IR: no handler registered for a custom type");
               if (handler && handler->check)
                  out.push_back(handler);
            }
            at = custom->type.get();
            continue;
         }
         auto ref = std::get_if<schema_types::Type>(&at->value);
         if (!ref)
            return codec_ok();
         const schema_types::AnyType *found = nullptr;
         for (const auto &member : schema.types)
            if (member.name == ref->type)
               found = member.type.get();
         if (!found)
            return codec_ok();
         at = found;
      }
      return codec_ok();
   }
   // Runs `checks` innermost first on the automatic-profile bytes of the
   // value; `bytes` are those bytes when this codec is the automatic one.
   codec_status run_checks(const std::vector<const CustomHandlers::Handler *> &checks,
                           const schema_types::AnyType &input, const Value &value,
                           std::span<const char> bytes) const
   {
      if (checks.empty())
         return codec_ok();
      std::vector<char> canonical;
      if constexpr (!automatic)
      {
         Codec<0> codec(schema);
         PSIO_PSSZ_IR_TRY(codec.encode_at(input, value, canonical));
         bytes = std::span<const char>{canonical};
      }
      else
         (void)value, (void)input;
      for (auto at = checks.rbegin(); at != checks.rend(); ++at)
         if (auto status = (*at)->check(bytes); !status.ok())
         {
            check_failed = true;
            return status;
         }
      return codec_ok();
   }

   static codec_status int_size(const schema_types::Int &type, std::size_t &size)
   {
      switch (type.bits)
      {
      case 1:
         size = 1;
         return codec_ok();
      case 8:
      case 16:
      case 32:
      case 64:
      case 128:
         size = type.bits / 8;
         return codec_ok();
      case 256:
         // An unsigned 256-bit integer is carried as its 32 little-endian
         // bytes (Value Bytes).
         if (type.isSigned)
            return fail("pssz IR: unsupported integer width");
         size = 32;
         return codec_ok();
      default:
         return fail("pssz IR: unsupported integer width");
      }
   }
   static codec_status float_size(const schema_types::Float &type, std::size_t &size)
   {
      if (type.exp == 8 && (type.mantissa == 23 || type.mantissa == 24))
         size = 4;
      else if (type.exp == 11 && (type.mantissa == 52 || type.mantissa == 53))
         size = 8;
      else
         return fail("pssz IR: unsupported IEEE float width");
      return codec_ok();
   }
   codec_status record_layout(const std::vector<schema_types::Member> &members, Layout &out,
                              std::size_t n) const
   {
      out = {true, 0, false};
      for (const auto &member : members)
      {
         const schema_types::AnyType *type;
         PSIO_PSSZ_IR_TRY(inner(member.type, type));
         Layout child;
         PSIO_PSSZ_IR_TRY(layout(*type, child, n + 1));
         out.fixed = out.fixed && child.fixed;
         PSIO_PSSZ_IR_TRY(add(out.size, child.size));
      }
      if (!out.fixed)
         out.size = 0;
      return codec_ok();
   }
   codec_status primitive_adapter(const schema_types::Custom &custom, std::uint32_t bits) const
   {
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(direct(custom.type, type));
      auto integer = std::get_if<schema_types::Int>(&type->value);
      return integer && integer->bits == bits && !integer->isSigned
                 ? codec_ok()
                 : fail("pssz IR: primitive adapter has incompatible inner schema");
   }
   codec_status bit_shape(const schema_types::Custom &custom, std::size_t &count, bool &list) const
   {
      const schema_types::AnyType *container;
      PSIO_PSSZ_IR_TRY(direct(custom.type, container));
      const schema_types::AnyType *element;
      std::uint64_t bound;
      if (custom.id == "bitvector")
      {
         auto array = std::get_if<schema_types::Array>(&container->value);
         if (!array)
            return fail("pssz IR: bitvector requires array bit metadata");
         PSIO_PSSZ_IR_TRY(direct(array->type, element));
         bound = array->len;
         list = false;
      }
      else
      {
         auto bounded = std::get_if<schema_types::BoundedList>(&container->value);
         if (custom.id != "bitlist" || !bounded)
            return fail("pssz IR: bitlist requires bounded bit metadata");
         PSIO_PSSZ_IR_TRY(direct(bounded->type, element));
         bound = bounded->maxCount;
         list = true;
      }
      auto integer = std::get_if<schema_types::Int>(&element->value);
      if (!integer || integer->bits != 1 || integer->isSigned)
         return fail("pssz IR: packed bit element must be unsigned one-bit integer");
      if (bound > std::numeric_limits<std::size_t>::max())
         return fail("pssz IR: bit count exceeds platform capacity");
      count = static_cast<std::size_t>(bound);
      return codec_ok();
   }
   codec_status layout(const schema_types::AnyType &input, Layout &out, std::size_t n = 0) const
   {
      PSIO_PSSZ_IR_TRY(depth(n));
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(resolve(input, type));
      out = {};
      return std::visit(
          [&](const auto &t) -> codec_status
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, schema_types::Int>)
             {
                out.fixed = true;
                return int_size(t, out.size);
             }
             else if constexpr (std::is_same_v<T, schema_types::Float>)
             {
                out.fixed = true;
                return float_size(t, out.size);
             }
             else if constexpr (std::is_same_v<T, schema_types::Option>)
             {
                out.optional = true;
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Array>)
             {
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                PSIO_PSSZ_IR_TRY(layout(*child, out, n + 1));
                out.optional = false;
                if (out.fixed)
                {
                   if (t.len > std::numeric_limits<std::size_t>::max() ||
                       (out.size && t.len > std::numeric_limits<std::size_t>::max() / out.size))
                      return fail("pssz IR: array size overflow");
                   out.size *= static_cast<std::size_t>(t.len);
                }
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Struct>)
             {
                return record_layout(t.members, out, n);
             }
             else if constexpr (std::is_same_v<T, schema_types::Object>)
             {
                return dwnc(t.attributes) ? record_layout(t.members, out, n) : codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Tuple> ||
                                std::is_same_v<T, schema_types::List> ||
                                std::is_same_v<T, schema_types::BoundedList> ||
                                std::is_same_v<T, schema_types::Variant>)
             {
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Custom>)
             {
                if (t.id == "bool" || t.id == "char")
                {
                   PSIO_PSSZ_IR_TRY(primitive_adapter(t, t.id == "bool" ? 1 : 8));
                   out = {true, 1, false};
                   return codec_ok();
                }
                if (t.id == "bitvector" || t.id == "bitlist")
                {
                   std::size_t count;
                   bool list;
                   PSIO_PSSZ_IR_TRY(bit_shape(t, count, list));
                   out = {!list, list ? 0 : packed_byte_count(count), false};
                   return codec_ok();
                }
                if (byte_adapter_id(t.id))
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(inner(t.type, child));
                   return layout(*child, out, n + 1);
                }
                return fail("pssz IR: unsupported custom adapter");
             }
             else if constexpr (std::is_same_v<T, schema_types::FracPack>)
             {
                return fail("pssz IR: legacy FracPack marker is unsupported");
             }
             else
                return fail("pssz IR: unsupported resource or schema type");
          },
          type->value);
   }
   codec_status can_be_empty(const schema_types::AnyType &input, bool &out, std::size_t n = 0) const
   {
      PSIO_PSSZ_IR_TRY(depth(n));
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(resolve(input, type));
      out = false;
      return std::visit(
          [&](const auto &t) -> codec_status
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, schema_types::List> ||
                           std::is_same_v<T, schema_types::BoundedList>)
             {
                out = true;
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Option>)
             {
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                PSIO_PSSZ_IR_TRY(can_be_empty(*child, out, n + 1));
                out = !out;
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Array>)
             {
                if (!t.len)
                {
                   out = true;
                   return codec_ok();
                }
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                Layout info;
                PSIO_PSSZ_IR_TRY(layout(*child, info, n + 1));
                out = info.fixed && info.size == 0;
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Struct> ||
                                std::is_same_v<T, schema_types::Object>)
             {
                if constexpr (std::is_same_v<T, schema_types::Object>)
                   if (!dwnc(t.attributes))
                      return codec_ok();
                Layout info;
                PSIO_PSSZ_IR_TRY(record_layout(t.members, info, n));
                out = info.fixed && info.size == 0;
                return codec_ok();
             }
             else if constexpr (std::is_same_v<T, schema_types::Custom>)
             {
                if (t.id == "bool" || t.id == "char")
                   return primitive_adapter(t, t.id == "bool" ? 1 : 8);
                if (t.id == "bitvector" || t.id == "bitlist")
                {
                   std::size_t count;
                   bool list;
                   PSIO_PSSZ_IR_TRY(bit_shape(t, count, list));
                   out = !list && !count;
                   return codec_ok();
                }
                if (byte_adapter_id(t.id))
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(inner(t.type, child));
                   return can_be_empty(*child, out, n + 1);
                }
                return fail("pssz IR: unsupported optional adapter");
             }
             else if constexpr (std::is_same_v<T, schema_types::Int> ||
                                std::is_same_v<T, schema_types::Float> ||
                                std::is_same_v<T, schema_types::Tuple> ||
                                std::is_same_v<T, schema_types::Variant>)
                return codec_ok();
             else
                return fail("pssz IR: unsupported optional inner schema");
          },
          type->value);
   }
   static codec_status read_offset(std::span<const char> bytes, std::size_t at, std::size_t w,
                                   std::size_t &out)
   {
      if (at > bytes.size() || w > bytes.size() - at)
         return fail("pssz IR: offset underrun");
      out = 0;
      for (std::size_t i = 0; i < w; ++i)
         out |= std::size_t(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
      return codec_ok();
   }
   static codec_status write_offset(std::vector<char> &bytes, std::size_t at, std::size_t value,
                                    std::size_t w)
   {
      const std::uint64_t max_offset = (std::uint64_t{1} << (w * 8)) - 1;
      if (value > max_offset || at > bytes.size() || w > bytes.size() - at)
         return fail("pssz IR: offset width overflow");
      for (std::size_t i = 0; i < w; ++i)
         bytes[at + i] = static_cast<char>(value >> (8 * i));
      return codec_ok();
   }
   codec_status bound_bytes(const schema_types::AnyType &input, std::size_t count,
                            bool check_count = true) const
   {
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(direct(input, type));
      const schema_types::AnyType *element = nullptr;
      if (auto list = std::get_if<schema_types::List>(&type->value))
         PSIO_PSSZ_IR_TRY(direct(list->type, element));
      else if (auto list = std::get_if<schema_types::BoundedList>(&type->value))
      {
         if (check_count && count > list->maxCount)
            return fail("pssz IR: byte count bound exceeded");
         PSIO_PSSZ_IR_TRY(direct(list->type, element));
      }
      else if (auto array = std::get_if<schema_types::Array>(&type->value))
      {
         if (check_count && count != array->len)
            return fail("pssz IR: byte array count mismatch");
         PSIO_PSSZ_IR_TRY(direct(array->type, element));
      }
      else
         return fail("pssz IR: byte adapter requires byte container");
      auto integer = std::get_if<schema_types::Int>(&element->value);
      if (auto custom = std::get_if<schema_types::Custom>(&element->value);
          custom && custom->id == "char")
         return primitive_adapter(*custom, 8);
      return integer && integer->bits == 8 && !integer->isSigned
                 ? codec_ok()
                 : fail("pssz IR: byte adapter requires unsigned bytes");
   }
   static codec_status encode_fields(const std::vector<Field> &fields, bool stable,
                                     std::size_t w, std::vector<char> &out)
   {
      std::size_t fixed = 0;
      for (const auto &field : fields)
         PSIO_PSSZ_IR_TRY(add(fixed, field.layout.fixed ? field.layout.size : w));
      const std::size_t header = stable ? 0 : w;
      std::size_t total = fixed;
      PSIO_PSSZ_IR_TRY(add(total, header));
      if (total > out.max_size())
         return fail("pssz IR: record size overflow");
      std::vector<char> bytes(total, 0);
      if (!stable)
         PSIO_PSSZ_IR_TRY(write_offset(bytes, 0, fixed, w));
      std::size_t cursor = header;
      for (const auto &field : fields)
      {
         if (field.layout.fixed)
         {
            if (field.bytes.size() != field.layout.size)
               return fail("pssz IR: fixed encoder size mismatch");
            std::copy(field.bytes.begin(), field.bytes.end(), bytes.begin() + cursor);
            cursor += field.layout.size;
         }
         else
         {
            PSIO_PSSZ_IR_TRY(write_offset(bytes, cursor, bytes.size() - header, w));
            PSIO_PSSZ_IR_TRY(append(bytes, field.bytes));
            cursor += w;
         }
      }
      return append(out, bytes);
   }
   codec_status encode_field(const schema_types::AnyType &type, const Value &value, std::size_t n,
                             Field &out) const
   {
      PSIO_PSSZ_IR_TRY(layout(type, out.layout));
      out.omit = out.layout.optional && none(value);
      return encode_at(type, value, out.bytes, n);
   }
   codec_status encode_members(const std::vector<schema_types::Member> &members,
                               const Record &values, bool stable, std::size_t w,
                               std::vector<char> &out, std::size_t n) const
   {
      for (const auto &[name, value] : values)
      {
         (void)value;
         if (std::count_if(values.begin(), values.end(),
                           [&](const auto &f) { return f.first == name; }) != 1 ||
             std::none_of(members.begin(), members.end(),
                          [&](const auto &m) { return m.name == name; }))
            return fail("pssz IR: duplicate or unknown record value");
      }
      const Value absent = Value::none();
      std::vector<const Value *> ordered;
      std::vector<const schema_types::AnyType *> types;
      std::size_t keep = 0;
      for (const auto &member : members)
      {
         const schema_types::AnyType *type;
         PSIO_PSSZ_IR_TRY(child_of(member.type, type));
         Layout info;
         PSIO_PSSZ_IR_TRY(layout(*type, info));
         auto found = std::find_if(values.begin(), values.end(),
                                   [&](const auto &f) { return f.first == member.name; });
         if (found == values.end() && !info.optional)
            return fail("pssz IR: missing required record value");
         const Value *value = found == values.end() ? &absent : &found->second;
         std::optional<LengthBound> bound;
         PSIO_PSSZ_IR_TRY(parse_length_bound(member.attributes, bound));
         if (bound && !bound_admits(*bound, *value))
            return fail("pssz IR: member length bound violated");
         ordered.push_back(value);
         types.push_back(type);
         if (stable || !info.optional || !none(*value))
            keep = ordered.size();
      }
      std::vector<Field> fields(keep);
      for (std::size_t i = 0; i < keep; ++i)
         PSIO_PSSZ_IR_TRY(encode_field(*types[i], *ordered[i], n + 1, fields[i]));
      return encode_fields(fields, stable, w, out);
   }
   codec_status encode_sequence(const schema_types::AnyType &type, const Sequence &values,
                                bool exact, std::uint64_t bound, bool bounded, std::size_t w,
                                std::vector<char> &out, std::size_t n) const
   {
      if (bounded && (exact ? values.size() != bound : values.size() > bound))
         return fail("pssz IR: sequence bound exceeded");
      Layout info;
      PSIO_PSSZ_IR_TRY(layout(type, info));
      if (!exact && info.fixed && !info.size && !values.empty())
         return fail("pssz IR: ambiguous zero-size list");
      std::vector<Field> fields(values.size());
      for (std::size_t i = 0; i < values.size(); ++i)
         PSIO_PSSZ_IR_TRY(encode_field(type, values[i], n + 1, fields[i]));
      return encode_fields(fields, true, w, out);
   }
   // Pruning is mandatory: the last slot a writer emits is never an absent
   // optional. Matches the typed codec (pssz.hpp record_has_unpruned_tail):
   // DWNC records are exempt, and a frame with unknown appended slots is not
   // judged because the reader cannot interpret the writer's last slot.
   static bool unpruned_tail(const std::vector<psio::detail::pssz_field_layout> &fields,
                             const std::vector<psio::detail::pssz_field_span> &spans,
                             std::size_t i, bool stable, bool unknown, const Value &value)
   {
      if (stable || unknown || !spans[i].present || !fields[i].optional)
         return false;
      if (i + 1 < spans.size() && spans[i + 1].present)
         return false;
      const auto *optional = std::get_if<Optional>(&value.value);
      return optional && !optional->some;
   }
   codec_status decode_members(const std::vector<schema_types::Member> &members,
                               std::span<const char> bytes, bool stable, std::size_t w,
                               Record &out, std::size_t n)
   {
      std::vector<const schema_types::AnyType *> types;
      std::vector<psio::detail::pssz_field_layout> fields;
      for (const auto &member : members)
      {
         const schema_types::AnyType *type;
         PSIO_PSSZ_IR_TRY(child_of(member.type, type));
         Layout info;
         PSIO_PSSZ_IR_TRY(layout(*type, info));
         types.push_back(type);
         fields.push_back({info.fixed ? info.size : w, !info.fixed, info.optional});
      }
      std::vector<psio::detail::pssz_field_span> spans(fields.size());
      bool unknown = false;
      PSIO_PSSZ_IR_TRY(spans_for(w, bytes, stable, fields, spans, unknown));
      if (unknown && !allow_unknown)
         return fail("pssz IR: unknown fields in exact decode");
      has_unknown |= unknown;
      for (std::size_t i = 0; i < members.size(); ++i)
      {
         Value value;
         if (spans[i].present)
            PSIO_PSSZ_IR_TRY(decode_at(*types[i],
                                       bytes.subspan(spans[i].begin, spans[i].end - spans[i].begin),
                                       value, n + 1));
         if (unpruned_tail(fields, spans, i, stable, unknown, value))
            return fail("pssz IR: trailing absent optional field was not pruned");
         std::optional<LengthBound> bound;
         PSIO_PSSZ_IR_TRY(parse_length_bound(members[i].attributes, bound));
         if (bound && !bound_admits(*bound, value))
            return fail("pssz IR: member length bound violated");
         out.emplace_back(members[i].name, std::move(value));
      }
      return codec_ok();
   }
   codec_status decode_sequence(const schema_types::AnyType &type, std::span<const char> bytes,
                                bool exact, std::uint64_t bound, bool bounded, std::size_t w,
                                Sequence &out, std::size_t n)
   {
      Layout info;
      PSIO_PSSZ_IR_TRY(layout(type, info));
      std::size_t count;
      if (exact)
      {
         if (bound > std::numeric_limits<std::size_t>::max())
            return fail("pssz IR: array count overflow");
         count = static_cast<std::size_t>(bound);
      }
      else if (bytes.empty())
         count = 0;
      else if (info.fixed)
      {
         if (!info.size || bytes.size() % info.size)
            return fail("pssz IR: fixed list span mismatch");
         count = bytes.size() / info.size;
      }
      else
      {
         std::size_t first;
         PSIO_PSSZ_IR_TRY(read_offset(bytes, 0, w, first));
         if (!first || first % w || first > bytes.size())
            return fail("pssz IR: invalid list offset table");
         count = first / w;
      }
      if (bounded && (exact ? count != bound : count > bound))
         return fail("pssz IR: sequence bound exceeded");
      if (count > out.max_size())
         return fail("pssz IR: sequence allocation overflow");
      if (info.fixed)
      {
         if (info.size && count > std::numeric_limits<std::size_t>::max() / info.size)
            return fail("pssz IR: fixed sequence size overflow");
         if (count * info.size != bytes.size())
            return fail("pssz IR: fixed sequence span mismatch");
         // Zero-size elements carry no bytes: never reserve for them.
         out.reserve(info.size ? count : 0);
         for (std::size_t i = 0; i < count; ++i)
         {
            Value value;
            PSIO_PSSZ_IR_TRY(
                decode_at(type, bytes.subspan(i * info.size, info.size), value, n + 1));
            out.push_back(std::move(value));
         }
      }
      else
      {
         if (count > bytes.size() / w || (!count && !bytes.empty()))
            return fail("pssz IR: list table span mismatch");
         const std::size_t table = count * w;
         std::size_t previous = table;
         out.reserve(count);
         for (std::size_t i = 0; i < count; ++i)
         {
            std::size_t begin, end = bytes.size();
            PSIO_PSSZ_IR_TRY(read_offset(bytes, i * w, w, begin));
            if (i + 1 < count)
               PSIO_PSSZ_IR_TRY(read_offset(bytes, (i + 1) * w, w, end));
            if ((!i && begin != table) || begin < previous || begin > end || end > bytes.size())
               return fail("pssz IR: invalid list offsets");
            Value value;
            PSIO_PSSZ_IR_TRY(decode_at(type, bytes.subspan(begin, end - begin), value, n + 1));
            out.push_back(std::move(value));
            previous = begin;
         }
      }
      return codec_ok();
   }

 public:
   bool has_unknown = false;
   bool allow_unknown = false;
   // The registry of custom handlers, or null: structure only.
   const CustomHandlers *customs = nullptr;
   // The status returned is a handler's (a violated invariant), not a
   // structural error of this codec.
   mutable bool check_failed = false;
   explicit Codec(const schema_types::Schema &schema, const CustomHandlers *customs = nullptr)
       : schema(schema), customs(customs && customs->checks_values() ? customs : nullptr)
   {
   }

   // The offset width `root` uses for its own header / slots / table.
   codec_status width(const schema_types::AnyType &root, std::size_t &out) const
   {
      const schema_types::AnyType *type;
      bool boxed = false;
      PSIO_PSSZ_IR_TRY(resolve(root, type, &boxed));
      // A box has no header of its own: the boxed type is encoded at its
      // own width. As a root, the typed trait reports the unbounded width.
      if constexpr (automatic)
         if (boxed)
         {
            out = 4;
            return codec_ok();
         }
      return width_of(*type, out);
   }

   codec_status metadata(const schema_types::AnyType &root) const
   {
      PSIO_PSSZ_IR_TRY(ir_version());
      std::vector<const schema_types::AnyType *> pending{&root}, seen;
      while (!pending.empty())
      {
         const schema_types::AnyType *type;
         PSIO_PSSZ_IR_TRY(resolve(*pending.back(), type));
         pending.pop_back();
         if (std::find(seen.begin(), seen.end(), type) != seen.end())
            continue;
         seen.push_back(type);
         auto push = [&](const auto &box) -> codec_status
         {
            if (!box.get())
               return fail("pssz IR: null schema box");
            pending.push_back(box.get());
            return codec_ok();
         };
         auto status = std::visit(
             [&](const auto &t) -> codec_status
             {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, schema_types::Object> ||
                              std::is_same_v<T, schema_types::Struct> ||
                              std::is_same_v<T, schema_types::Variant>)
                {
                   constexpr bool record = !std::is_same_v<T, schema_types::Variant>;
                   PSIO_PSSZ_IR_TRY(attributes(
                       t.attributes, record ? attribute_site::record : attribute_site::variant,
                       automatic));
                   if constexpr (!record)
                      if (t.members.empty() || t.members.size() > 256)
                         return fail("pssz IR: invalid variant schema");
                   for (const auto &member : t.members)
                   {
                      if (std::count_if(t.members.begin(), t.members.end(),
                                        [&](const auto &m) { return m.name == member.name; }) != 1)
                         return fail("pssz IR: duplicate schema member");
                      PSIO_PSSZ_IR_TRY(attributes(member.attributes,
                                                  record ? attribute_site::record_member
                                                         : attribute_site::variant_member,
                                                  automatic));
                      PSIO_PSSZ_IR_TRY(push(member.type));
                   }
                }
                else if constexpr (std::is_same_v<T, schema_types::Tuple>)
                {
                   for (const auto &member : t.members)
                      PSIO_PSSZ_IR_TRY(push(member));
                }
                else if constexpr (std::is_same_v<T, schema_types::List> ||
                                   std::is_same_v<T, schema_types::BoundedList> ||
                                   std::is_same_v<T, schema_types::Array> ||
                                   std::is_same_v<T, schema_types::Option>)
                {
                   PSIO_PSSZ_IR_TRY(push(t.type));
                }
                else if constexpr (std::is_same_v<T, schema_types::Custom>)
                {
                   if (t.id == "bool" || t.id == "char")
                      PSIO_PSSZ_IR_TRY(primitive_adapter(t, t.id == "bool" ? 1 : 8));
                   else if (t.id == "bitvector" || t.id == "bitlist")
                   {
                      std::size_t count;
                      bool list;
                      PSIO_PSSZ_IR_TRY(bit_shape(t, count, list));
                   }
                   else if (byte_adapter_id(t.id))
                   {
                      if (!t.type.get())
                         return fail("pssz IR: null schema box");
                      PSIO_PSSZ_IR_TRY(bound_bytes(*t.type, 0, false));
                   }
                   else
                      return fail("pssz IR: unsupported custom adapter");
                   PSIO_PSSZ_IR_TRY(push(t.type));
                }
                else if constexpr (std::is_same_v<T, schema_types::Int>)
                {
                   std::size_t size;
                   PSIO_PSSZ_IR_TRY(int_size(t, size));
                }
                else if constexpr (std::is_same_v<T, schema_types::Float>)
                {
                   std::size_t size;
                   PSIO_PSSZ_IR_TRY(float_size(t, size));
                }
                else
                   return fail("pssz IR: unsupported schema type");
                return codec_ok();
             },
             type->value);
         if (!status.ok())
            return status;
      }
      return codec_ok();
   }

   // Every value is encoded through here: its bytes (encode_shape), then
   // the checks of the custom nodes it is a value of.
   codec_status encode_at(const schema_types::AnyType &input, const Value &value,
                          std::vector<char> &out, std::size_t n = 0) const
   {
      if (!customs)
         return encode_shape(input, value, out, n);
      PSIO_PSSZ_IR_TRY(depth(n));
      std::vector<const CustomHandlers::Handler *> checks;
      PSIO_PSSZ_IR_TRY(custom_checks(input, checks));
      const std::size_t start = out.size();
      PSIO_PSSZ_IR_TRY(encode_shape(input, value, out, n));
      return run_checks(checks, input, value,
                        std::span<const char>{out}.subspan(start));
   }
   codec_status encode_shape(const schema_types::AnyType &input, const Value &value,
                             std::vector<char> &out, std::size_t n) const
   {
      PSIO_PSSZ_IR_TRY(depth(n));
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(resolve(input, type));
      return std::visit(
          [&](const auto &t) -> codec_status
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, schema_types::Int>)
             {
                std::size_t size;
                PSIO_PSSZ_IR_TRY(int_size(t, size));
                if (t.bits == 256)
                {
                   auto v = std::get_if<Bytes>(&value.value);
                   if (!v || v->size() != 32)
                      return fail("pssz IR: 256-bit integer needs 32 little-endian bytes");
                   return append(out, {reinterpret_cast<const char *>(v->data()), v->size()});
                }
                unsigned __int128 raw;
                if (t.isSigned)
                {
                   auto v = std::get_if<Signed>(&value.value);
                   if (!v)
                      return fail("pssz IR: signed integer value required");
                   if (t.bits < 128)
                   {
                      __int128 limit = __int128{1} << (t.bits - 1);
                      if (v->value < -limit || v->value >= limit)
                         return fail("pssz IR: signed integer overflow");
                   }
                   raw = static_cast<unsigned __int128>(v->value);
                }
                else
                {
                   auto v = std::get_if<Unsigned>(&value.value);
                   if (!v)
                      return fail("pssz IR: unsigned integer value required");
                   if (t.bits < 128 && v->value >= (static_cast<unsigned __int128>(1) << t.bits))
                      return fail("pssz IR: unsigned integer overflow");
                   raw = v->value;
                }
                for (std::size_t i = 0; i < size; ++i)
                   out.push_back(static_cast<char>(raw >> (8 * i)));
             }
             else if constexpr (std::is_same_v<T, schema_types::Float>)
             {
                std::size_t size;
                PSIO_PSSZ_IR_TRY(float_size(t, size));
                std::uint64_t raw;
                if (size == 4)
                {
                   auto v = std::get_if<float>(&value.value);
                   if (!v)
                      return fail("pssz IR: Float32 value required");
                   raw = std::bit_cast<std::uint32_t>(*v);
                }
                else
                {
                   auto v = std::get_if<double>(&value.value);
                   if (!v)
                      return fail("pssz IR: Float64 value required");
                   raw = std::bit_cast<std::uint64_t>(*v);
                }
                for (std::size_t i = 0; i < size; ++i)
                   out.push_back(static_cast<char>(raw >> (8 * i)));
             }
             else if constexpr (std::is_same_v<T, schema_types::Custom>)
             {
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                if (t.id == "bool")
                {
                   PSIO_PSSZ_IR_TRY(primitive_adapter(t, 1));
                   auto v = std::get_if<bool>(&value.value);
                   if (!v)
                      return fail("pssz IR: bool value required");
                   out.push_back(*v ? 1 : 0);
                }
                else if (t.id == "char")
                {
                   PSIO_PSSZ_IR_TRY(primitive_adapter(t, 8));
                   auto v = std::get_if<Unsigned>(&value.value);
                   if (!v || v->value > 255)
                      return fail("pssz IR: char byte value required");
                   out.push_back(static_cast<char>(v->value));
                }
                else if (text_adapter_id(t.id))
                {
                   auto v = std::get_if<std::string>(&value.value);
                   if (!v)
                      return fail("pssz IR: string value required");
                   PSIO_PSSZ_IR_TRY(bound_bytes(*child, v->size()));
                   // "utf8" is UTF-8; "string" is raw bytes.
                   if (t.id == "utf8")
                      PSIO_PSSZ_IR_TRY(utf8_spec::validate(std::span<const char>(*v)));
                   PSIO_PSSZ_IR_TRY(append(out, *v));
                }
                else if (t.id == "hex")
                {
                   auto v = std::get_if<Bytes>(&value.value);
                   if (!v)
                      return fail("pssz IR: bytes value required");
                   PSIO_PSSZ_IR_TRY(bound_bytes(*child, v->size()));
                   if (!v->empty())
                      PSIO_PSSZ_IR_TRY(depth(n + 1));
                   PSIO_PSSZ_IR_TRY(
                       append(out, {reinterpret_cast<const char *>(v->data()), v->size()}));
                }
                else if (t.id == "bitvector" || t.id == "bitlist")
                {
                   auto value_bits = std::get_if<Bits>(&value.value);
                   if (!value_bits)
                      return fail("pssz IR: explicit packed Bits value required");
                   std::size_t bound;
                   bool list;
                   PSIO_PSSZ_IR_TRY(bit_shape(t, bound, list));
                   if (value_bits->bit_count > std::numeric_limits<std::size_t>::max())
                      return fail("pssz IR: bit count exceeds platform capacity");
                   const auto count = static_cast<std::size_t>(value_bits->bit_count);
                   if (list ? count > bound : count != bound)
                      return fail("pssz IR: packed bit logical count mismatch");
                   PSIO_PSSZ_IR_TRY(validate_bit_data(value_bits->bytes, count));
                   PSIO_PSSZ_IR_TRY(
                       append(out, {reinterpret_cast<const char *>(value_bits->bytes.data()),
                                    value_bits->bytes.size()}));
                   if (list)
                   {
                      if (count % 8 == 0)
                         out.push_back(1);
                      else
                         out.back() = static_cast<char>(static_cast<unsigned char>(out.back()) |
                                                        (1u << (count % 8)));
                   }
                }
                else
                   return fail("pssz IR: unsupported custom adapter");
             }
             else if constexpr (std::is_same_v<T, schema_types::Option>)
             {
                auto v = std::get_if<Optional>(&value.value);
                if (!v)
                   return fail("pssz IR: explicit optional value required");
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(child_of(t.type, child));
                bool selector;
                PSIO_PSSZ_IR_TRY(can_be_empty(*child, selector));
                if (selector)
                   out.push_back(v->some ? 1 : 0);
                if (v->some)
                   PSIO_PSSZ_IR_TRY(encode_at(*child, *v->some, out, n + 1));
             }
             else if constexpr (std::is_same_v<T, schema_types::Struct> ||
                                std::is_same_v<T, schema_types::Object>)
             {
                auto v = std::get_if<Record>(&value.value);
                if (!v)
                   return fail("pssz IR: ordered record value required");
                bool stable = true;
                if constexpr (std::is_same_v<T, schema_types::Object>)
                   stable = dwnc(t.attributes);
                std::size_t w;
                PSIO_PSSZ_IR_TRY(width_of(*type, w));
                const std::size_t start = out.size();
                PSIO_PSSZ_IR_TRY(encode_members(t.members, *v, stable, w, out, n));
                PSIO_PSSZ_IR_TRY(check_cap(*type, out.size() - start));
             }
             else if constexpr (std::is_same_v<T, schema_types::Tuple>)
             {
                auto v = std::get_if<Sequence>(&value.value);
                if (!v || v->size() != t.members.size())
                   return fail("pssz IR: tuple value count mismatch");
                std::size_t keep = 0;
                std::vector<const schema_types::AnyType *> types;
                for (std::size_t i = 0; i < v->size(); ++i)
                {
                   const schema_types::AnyType *child;
                   PSIO_PSSZ_IR_TRY(child_of(t.members[i], child));
                   types.push_back(child);
                   Layout info;
                   PSIO_PSSZ_IR_TRY(layout(*child, info));
                   if (!info.optional || !none((*v)[i]))
                      keep = i + 1;
                }
                std::vector<Field> fields(keep);
                for (std::size_t i = 0; i < keep; ++i)
                   PSIO_PSSZ_IR_TRY(encode_field(*types[i], (*v)[i], n + 1, fields[i]));
                std::size_t w;
                PSIO_PSSZ_IR_TRY(width_of(*type, w));
                PSIO_PSSZ_IR_TRY(encode_fields(fields, false, w, out));
             }
             else if constexpr (std::is_same_v<T, schema_types::List> ||
                                std::is_same_v<T, schema_types::BoundedList> ||
                                std::is_same_v<T, schema_types::Array>)
             {
                auto v = std::get_if<Sequence>(&value.value);
                if (!v)
                   return fail("pssz IR: sequence value required");
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(child_of(t.type, child));
                std::size_t w;
                PSIO_PSSZ_IR_TRY(width_of(*type, w));
                if constexpr (std::is_same_v<T, schema_types::List>)
                   PSIO_PSSZ_IR_TRY(encode_sequence(*child, *v, false, 0, false, w, out, n));
                else if constexpr (std::is_same_v<T, schema_types::BoundedList>)
                   PSIO_PSSZ_IR_TRY(
                       encode_sequence(*child, *v, false, t.maxCount, true, w, out, n));
                else
                   PSIO_PSSZ_IR_TRY(encode_sequence(*child, *v, true, t.len, true, w, out, n));
             }
             else if constexpr (std::is_same_v<T, schema_types::Variant>)
             {
                auto v = std::get_if<Variant>(&value.value);
                if (!v || !v->payload || t.members.empty() || t.members.size() > 256 ||
                    v->selector >= t.members.size())
                   return fail("pssz IR: invalid explicit variant");
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(child_of(t.members[v->selector].type, child));
                out.push_back(static_cast<char>(v->selector));
                PSIO_PSSZ_IR_TRY(encode_at(*child, *v->payload, out, n + 1));
             }
             else if constexpr (std::is_same_v<T, schema_types::FracPack>)
                return fail("pssz IR: legacy FracPack marker is unsupported");
             else
                return fail("pssz IR: unsupported resource or schema type");
             return codec_ok();
          },
          type->value);
   }

   // Every value is decoded through here: its structure (decode_shape), then
   // the checks of the custom nodes it is a value of.
   codec_status decode_at(const schema_types::AnyType &input, std::span<const char> bytes,
                          Value &out, std::size_t n = 0)
   {
      if (!customs)
         return decode_shape(input, bytes, out, n);
      PSIO_PSSZ_IR_TRY(depth(n));
      std::vector<const CustomHandlers::Handler *> checks;
      PSIO_PSSZ_IR_TRY(custom_checks(input, checks));
      PSIO_PSSZ_IR_TRY(decode_shape(input, bytes, out, n));
      return run_checks(checks, input, out, bytes);
   }
   codec_status decode_shape(const schema_types::AnyType &input, std::span<const char> bytes,
                             Value &out, std::size_t n);
};

template <std::size_t W>
codec_status Codec<W>::decode_shape(const schema_types::AnyType &input,
                                    std::span<const char> bytes, Value &out, std::size_t n)
{
   PSIO_PSSZ_IR_TRY(depth(n));
   const schema_types::AnyType *type;
   PSIO_PSSZ_IR_TRY(resolve(input, type));
   return std::visit(
       [&](const auto &t) -> codec_status
       {
          using T = std::decay_t<decltype(t)>;
          if constexpr (std::is_same_v<T, schema_types::Int>)
          {
             std::size_t size;
             PSIO_PSSZ_IR_TRY(int_size(t, size));
             if (bytes.size() != size)
                return fail("pssz IR: integer span mismatch");
             if (t.bits == 256)
             {
                out = Bytes(bytes.begin(), bytes.end());
                return codec_ok();
             }
             unsigned __int128 raw = 0;
             for (std::size_t i = 0; i < size; ++i)
                raw |= static_cast<unsigned __int128>(static_cast<unsigned char>(bytes[i]))
                       << (8 * i);
             if (t.isSigned)
             {
                if (size < 16 && (static_cast<unsigned char>(bytes.back()) & 128))
                   raw |= ~static_cast<unsigned __int128>(0) << (size * 8);
                const auto value = std::bit_cast<__int128>(raw);
                if (t.bits == 1 && (value < -1 || value > 0))
                   return fail("pssz IR: signed one-bit overflow");
                out = Signed{value};
             }
             else
             {
                if (t.bits == 1 && raw > 1)
                   return fail("pssz IR: unsigned one-bit overflow");
                out = Unsigned{raw};
             }
          }
          else if constexpr (std::is_same_v<T, schema_types::Float>)
          {
             std::size_t size;
             PSIO_PSSZ_IR_TRY(float_size(t, size));
             if (bytes.size() != size)
                return fail("pssz IR: float span mismatch");
             std::uint64_t raw = 0;
             for (std::size_t i = 0; i < size; ++i)
                raw |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
             if (size == 4)
                out = std::bit_cast<float>(static_cast<std::uint32_t>(raw));
             else
                out = std::bit_cast<double>(raw);
          }
          else if constexpr (std::is_same_v<T, schema_types::Custom>)
          {
             const schema_types::AnyType *child;
             PSIO_PSSZ_IR_TRY(inner(t.type, child));
             if (t.id == "bool")
             {
                PSIO_PSSZ_IR_TRY(primitive_adapter(t, 1));
                if (bytes.size() != 1 || (bytes[0] != 0 && bytes[0] != 1))
                   return fail("pssz IR: invalid bool");
                out = bool(bytes[0]);
             }
             else if (t.id == "char")
             {
                PSIO_PSSZ_IR_TRY(primitive_adapter(t, 8));
                if (bytes.size() != 1)
                   return fail("pssz IR: char span mismatch");
                out = Unsigned{static_cast<unsigned char>(bytes[0])};
             }
             else if (text_adapter_id(t.id))
             {
                PSIO_PSSZ_IR_TRY(bound_bytes(*child, bytes.size()));
                if (t.id == "utf8")
                   PSIO_PSSZ_IR_TRY(utf8_spec::validate(bytes));
                out = bytes.empty() ? std::string{} : std::string(bytes.data(), bytes.size());
             }
             else if (t.id == "hex")
             {
                PSIO_PSSZ_IR_TRY(bound_bytes(*child, bytes.size()));
                if (!bytes.empty())
                   PSIO_PSSZ_IR_TRY(depth(n + 1));
                out = Bytes(bytes.begin(), bytes.end());
             }
             else if (t.id == "bitvector" || t.id == "bitlist")
             {
                std::size_t bound;
                bool list;
                PSIO_PSSZ_IR_TRY(bit_shape(t, bound, list));
                std::size_t count = bound;
                if (list)
                   PSIO_PSSZ_IR_TRY(bitlist_count(bytes, bound, count));
                else
                   PSIO_PSSZ_IR_TRY(validate_bit_data(
                       {reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()},
                       count));
                Bits value_bits;
                value_bits.bit_count = count;
                const auto payload = bytes.first(packed_byte_count(count));
                if (!payload.empty())
                   value_bits.bytes.assign(payload.begin(), payload.end());
                if (list && count % 8)
                   value_bits.bytes.back() &= static_cast<unsigned char>((1u << (count % 8)) - 1);
                out = std::move(value_bits);
             }
             else
                return fail("pssz IR: unsupported custom adapter");
          }
          else if constexpr (std::is_same_v<T, schema_types::Option>)
          {
             const schema_types::AnyType *child;
             PSIO_PSSZ_IR_TRY(child_of(t.type, child));
             bool selector;
             PSIO_PSSZ_IR_TRY(can_be_empty(*child, selector));
             if (selector)
             {
                if (bytes.empty())
                   return fail("pssz IR: optional selector missing");
                if (bytes[0] == 0)
                {
                   if (bytes.size() != 1)
                      return fail("pssz IR: trailing None bytes");
                   out = Value::none();
                   return codec_ok();
                }
                if (bytes[0] != 1)
                   return fail("pssz IR: invalid optional selector");
                bytes = bytes.subspan(1);
             }
             else if (bytes.empty())
             {
                out = Value::none();
                return codec_ok();
             }
             Value value;
             PSIO_PSSZ_IR_TRY(decode_at(*child, bytes, value, n + 1));
             out = Value::some(std::move(value));
          }
          else if constexpr (std::is_same_v<T, schema_types::Object> ||
                             std::is_same_v<T, schema_types::Struct>)
          {
             bool stable = true;
             if constexpr (std::is_same_v<T, schema_types::Object>)
                stable = dwnc(t.attributes);
             PSIO_PSSZ_IR_TRY(check_cap(*type, bytes.size()));
             std::size_t w;
             PSIO_PSSZ_IR_TRY(width_of(*type, w));
             Record values;
             PSIO_PSSZ_IR_TRY(decode_members(t.members, bytes, stable, w, values, n));
             out = std::move(values);
          }
          else if constexpr (std::is_same_v<T, schema_types::Tuple>)
          {
             std::vector<psio::detail::pssz_field_layout> fields;
             std::vector<const schema_types::AnyType *> types;
             std::size_t w;
             PSIO_PSSZ_IR_TRY(width_of(*type, w));
             for (const auto &member : t.members)
             {
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(child_of(member, child));
                Layout info;
                PSIO_PSSZ_IR_TRY(layout(*child, info));
                types.push_back(child);
                fields.push_back({info.fixed ? info.size : w, !info.fixed, info.optional});
             }
             std::vector<psio::detail::pssz_field_span> spans(fields.size());
             bool unknown = false;
             PSIO_PSSZ_IR_TRY(spans_for(w, bytes, false, fields, spans, unknown));
             if (unknown && !allow_unknown)
                return fail("pssz IR: unknown tuple fields in exact decode");
             has_unknown |= unknown;
             Sequence values;
             for (std::size_t i = 0; i < fields.size(); ++i)
             {
                Value value;
                if (spans[i].present)
                   PSIO_PSSZ_IR_TRY(decode_at(
                       *types[i], bytes.subspan(spans[i].begin, spans[i].end - spans[i].begin),
                       value, n + 1));
                if (unpruned_tail(fields, spans, i, false, unknown, value))
                   return fail("pssz IR: trailing absent optional field was not pruned");
                values.push_back(std::move(value));
             }
             out = std::move(values);
          }
          else if constexpr (std::is_same_v<T, schema_types::List> ||
                             std::is_same_v<T, schema_types::BoundedList> ||
                             std::is_same_v<T, schema_types::Array>)
          {
             const schema_types::AnyType *child;
             PSIO_PSSZ_IR_TRY(child_of(t.type, child));
             std::size_t w;
             PSIO_PSSZ_IR_TRY(width_of(*type, w));
             Sequence values;
             if constexpr (std::is_same_v<T, schema_types::List>)
                PSIO_PSSZ_IR_TRY(decode_sequence(*child, bytes, false, 0, false, w, values, n));
             else if constexpr (std::is_same_v<T, schema_types::BoundedList>)
                PSIO_PSSZ_IR_TRY(
                    decode_sequence(*child, bytes, false, t.maxCount, true, w, values, n));
             else
                PSIO_PSSZ_IR_TRY(decode_sequence(*child, bytes, true, t.len, true, w, values, n));
             out = std::move(values);
          }
          else if constexpr (std::is_same_v<T, schema_types::Variant>)
          {
             if (bytes.empty() || t.members.empty() || t.members.size() > 256)
                return fail("pssz IR: variant selector missing or invalid schema");
             const std::size_t selector = static_cast<unsigned char>(bytes[0]);
             if (selector >= t.members.size())
                return fail("pssz IR: variant selector out of range");
             const schema_types::AnyType *child;
             PSIO_PSSZ_IR_TRY(child_of(t.members[selector].type, child));
             Value value;
             PSIO_PSSZ_IR_TRY(decode_at(*child, bytes.subspan(1), value, n + 1));
             out = Value::variant(selector, std::move(value));
          }
          else if constexpr (std::is_same_v<T, schema_types::FracPack>)
             return fail("pssz IR: legacy FracPack marker is unsupported");
          else
             return fail("pssz IR: unsupported resource or schema type");
          return codec_ok();
       },
       type->value);
}

#undef PSIO_PSSZ_IR_TRY
} // namespace detail

// Each API publishes destination/output/unknown flag only after success.
template <std::size_t W>
[[nodiscard]] codec_status encode(const schema_types::Schema &schema,
                                  const schema_types::AnyType &root, const Value &value,
                                  std::vector<char> &destination,
                                  const CustomHandlers *custom = nullptr)
{
   detail::Codec<W> codec(schema, custom);
   std::vector<char> bytes;
   auto status = codec.metadata(root);
   if (!status.ok())
      return status;
   status = codec.encode_at(root, value, bytes);
   if (!status.ok())
      return status;
   if (status = codec.check_top_cap(root, bytes.size()); !status.ok())
      return status;
   return detail::append(destination, bytes);
}
template <std::size_t W>
[[nodiscard]] codec_status
decode_compatible(const schema_types::Schema &schema, const schema_types::AnyType &root,
                  std::span<const char> bytes, Value &output, bool &has_unknown,
                  const CustomHandlers *custom = nullptr)
{
   detail::Codec<W> codec(schema, custom);
   codec.allow_unknown = true;
   Value value;
   auto status = codec.metadata(root);
   if (!status.ok())
      return status;
   if (status = codec.check_top_cap(root, bytes.size()); !status.ok())
      return status;
   status = codec.decode_at(root, bytes, value);
   if (!status.ok())
      return status;
   output = std::move(value);
   has_unknown = codec.has_unknown;
   return codec_ok();
}
template <std::size_t W>
// Exact here means known schema and structural validity. Pruning is
// mandatory: a stored trailing None is rejected by every read (exact and
// compatible); validate_canonical_for_width remains the full re-encode check.
[[nodiscard]] codec_status decode_exact(const schema_types::Schema &schema,
                                        const schema_types::AnyType &root,
                                        std::span<const char> bytes, Value &output,
                                        const CustomHandlers *custom = nullptr)
{
   detail::Codec<W> codec(schema, custom);
   Value value;
   auto status = codec.metadata(root);
   if (!status.ok())
      return status;
   if (status = codec.check_top_cap(root, bytes.size()); !status.ok())
      return status;
   status = codec.decode_at(root, bytes, value);
   if (!status.ok())
      return status;
   output = std::move(value);
   return codec_ok();
}
// ── Automatic profile (psio::pssz): type-derived widths ────────────────────
//
// Byte-compatible with the typed psio::pssz codec for the type the IR
// describes: every record / array / list uses its own width, explicit caps
// and length bounds are enforced, pruning is mandatory.
[[nodiscard]] inline codec_status encode_auto(const schema_types::Schema &schema,
                                              const schema_types::AnyType &root,
                                              const Value &value, std::vector<char> &destination,
                                              const CustomHandlers *custom = nullptr)
{
   return encode<0>(schema, root, value, destination, custom);
}
// Accepts unknown appended optional fields and reports them (the typed
// `validate` / decode_pssz_checked rule).
[[nodiscard]] inline codec_status decode_auto(const schema_types::Schema &schema,
                                              const schema_types::AnyType &root,
                                              std::span<const char> bytes, Value &output,
                                              bool &has_unknown,
                                              const CustomHandlers *custom = nullptr)
{
   return decode_compatible<0>(schema, root, bytes, output, has_unknown, custom);
}
// Checks `bytes` against the schema without keeping the value: structure,
// and with a registry the invariants of its custom types.
[[nodiscard]] inline codec_status validate_auto(const schema_types::Schema &schema,
                                                const schema_types::AnyType &root,
                                                std::span<const char> bytes,
                                                bool *has_unknown = nullptr,
                                                const CustomHandlers *custom = nullptr)
{
   Value value;
   bool unknown = false;
   auto status = decode_compatible<0>(schema, root, bytes, value, unknown, custom);
   if (status.ok() && has_unknown)
      *has_unknown = unknown;
   return status;
}

// The offset width the automatic profile derives for `root`'s own header,
// slots or offset table: what auto_pssz_width_v<T> is for the typed codec.
[[nodiscard]] inline codec_status auto_width(const schema_types::Schema &schema,
                                             const schema_types::AnyType &root, std::size_t &width)
{
   detail::Codec<0> codec(schema);
   if (auto status = codec.metadata(root); !status.ok())
      return status;
   return codec.width(root, width);
}

template <std::size_t W>
[[nodiscard]] codec_status validate_canonical_for_width(const schema_types::Schema &schema,
                                                        const schema_types::AnyType &root,
                                                        std::span<const char> bytes,
                                                        const CustomHandlers *custom = nullptr)
{
   Value value;
   auto status = decode_exact<W>(schema, root, bytes, value, custom);
   if (!status.ok())
      return status;
   std::vector<char> canonical;
   status = encode<W>(schema, root, value, canonical);
   if (!status.ok())
      return status;
   if (canonical.size() != bytes.size() ||
       !std::equal(canonical.begin(), canonical.end(), bytes.begin()))
      return detail::fail("pssz IR: noncanonical bytes for explicit width");
   return codec_ok();
}
} // namespace psio::pssz_ir
