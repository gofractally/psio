#pragma once
//
// psio/detail/pssz_ir_assoc.hpp — maps, sets and the `key` encoding in the
// schema-driven codec (included by pssz_ir.hpp).
//
// Schema IR (docs/schema-builder.md):
//
//    Custom{ List{ Tuple{[K, V]} }, "map" }      a map
//    Custom{ List{ K }, "set" }                  a set
//
// (BoundedList in place of List when the type carries a max count.) Both
// ids are part of the binary form: the list's entries are in strictly
// ascending order of the `key` encoding of K (docs/key-spec.md). A dynamic
// map value is the Sequence of its entries, each a Sequence {key, value}; a
// set value is the Sequence of its elements. The encoder sorts them and
// refuses duplicates; the decoder requires the order.
//
// Malformed schemas: "map" not directly on a List / BoundedList whose
// element is (an alias of) a Tuple of exactly two members; "set" not
// directly on a List / BoundedList; a key type with no key encoding.
//
// Dynamic key encoder: `Codec::key_at` writes the `key` encoding of a
// dynamic value from its schema type — the bytes the typed psio::key writes
// for the type the schema describes (a type with a sortable_binary_category
// adapter excepted: its key bytes are not in the schema). Public form:
// psio::pssz_ir::to_key.

#include <algorithm>
#include <bit>
#include <cstring>
#include <numeric>

namespace psio::pssz_ir
{
namespace detail
{
#define PSIO_PSSZ_IR_TRY(expression)                                                               \
   do                                                                                              \
   {                                                                                               \
      auto psio_ir_status_ = (expression);                                                         \
      if (!psio_ir_status_.ok())                                                                   \
         return psio_ir_status_;                                                                   \
   } while (false)

inline void key_escaped(std::span<const unsigned char> bytes, std::vector<char> &out)
{
   for (unsigned char byte : bytes)
   {
      out.push_back(static_cast<char>(byte));
      if (byte == 0)
         out.push_back(1);
   }
   out.push_back(0);
   out.push_back(0);
}

template <std::size_t W>
codec_status Codec<W>::assoc_shape(const schema_types::Custom &custom, AssocShape &out,
                                   bool check_key) const
{
   out = {};
   out.map = custom.id == "map";
   PSIO_PSSZ_IR_TRY(direct(custom.type, out.list));
   if (auto list = std::get_if<schema_types::List>(&out.list->value))
      out.element = &list->type;
   else if (auto bounded = std::get_if<schema_types::BoundedList>(&out.list->value))
   {
      out.element = &bounded->type;
      out.max = bounded->maxCount;
   }
   else
      return fail(out.map ? "pssz IR: a map must sit directly on a list of two-member tuples"
                          : "pssz IR: a set must sit directly on a list");
   if (!out.element->get())
      return fail("pssz IR: null schema box");
   if (!out.map)
      out.key = out.element->get();
   else
   {
      const schema_types::AnyType *entry;
      PSIO_PSSZ_IR_TRY(alias(**out.element, entry));
      auto tuple = std::get_if<schema_types::Tuple>(&entry->value);
      if (!tuple || tuple->members.size() != 2)
         return fail("pssz IR: a map must sit directly on a list of two-member tuples");
      if (!tuple->members[0].get() || !tuple->members[1].get())
         return fail("pssz IR: null schema box");
      out.key = tuple->members[0].get();
      out.value = tuple->members[1].get();
   }
   // The key type is checked once per schema node, by metadata().
   if (!check_key)
      return codec_ok();
   std::vector<const schema_types::AnyType *> active;
   return has_key(*out.key, active);
}

// Whether a list element is one key byte (docs/key-spec.md: lists of octets
// share the string framing): u8, i8 or char, reached through aliases only.
template <std::size_t W>
codec_status Codec<W>::key_octet(const schema_types::AnyType &element, bool &octet,
                                 bool &is_signed) const
{
   const schema_types::AnyType *type;
   PSIO_PSSZ_IR_TRY(alias(element, type));
   octet = is_signed = false;
   if (auto integer = std::get_if<schema_types::Int>(&type->value))
   {
      octet = integer->bits == 8;
      is_signed = integer->isSigned;
   }
   else if (auto custom = std::get_if<schema_types::Custom>(&type->value))
      octet = custom->id == "char";
   return codec_ok();
}

template <std::size_t W>
codec_status Codec<W>::has_key(const schema_types::AnyType &input,
                               std::vector<const schema_types::AnyType *> &active) const
{
   static constexpr const char *none = "pssz IR: the key type of a map / set has no key encoding";
   const schema_types::AnyType *type;
   bool boxed = false;
   PSIO_PSSZ_IR_TRY(resolve(input, type, &boxed));
   if (boxed)
      return fail(none);
   if (std::find(active.begin(), active.end(), type) != active.end())
      return codec_ok();
   PSIO_PSSZ_IR_TRY(depth(active.size()));
   active.push_back(type);
   auto child = [&](const schema_types::Box<schema_types::AnyType> &box) -> codec_status
   {
      if (!box.get())
         return fail("pssz IR: null schema box");
      return has_key(*box, active);
   };
   auto status = std::visit(
       [&](const auto &t) -> codec_status
       {
          using T = std::decay_t<decltype(t)>;
          if constexpr (std::is_same_v<T, schema_types::Int>)
          {
             std::size_t size;
             PSIO_PSSZ_IR_TRY(int_size(t, size));
             return t.bits == 1 ? fail(none) : codec_ok();
          }
          else if constexpr (std::is_same_v<T, schema_types::Float>)
          {
             std::size_t size;
             return float_size(t, size);
          }
          else if constexpr (std::is_same_v<T, schema_types::Option> ||
                             std::is_same_v<T, schema_types::List> ||
                             std::is_same_v<T, schema_types::BoundedList> ||
                             std::is_same_v<T, schema_types::Array>)
             return child(t.type);
          else if constexpr (std::is_same_v<T, schema_types::Tuple>)
          {
             for (const auto &member : t.members)
                PSIO_PSSZ_IR_TRY(child(member));
             return codec_ok();
          }
          else if constexpr (std::is_same_v<T, schema_types::Struct> ||
                             std::is_same_v<T, schema_types::Object> ||
                             std::is_same_v<T, schema_types::Variant>)
          {
             for (const auto &member : t.members)
                PSIO_PSSZ_IR_TRY(child(member.type));
             return codec_ok();
          }
          else if constexpr (std::is_same_v<T, schema_types::Custom>)
          {
             if (t.id == "bool" || t.id == "char" || t.id == "bitvector" ||
                 byte_adapter_id(t.id))
                return codec_ok();
             if (assoc_id(t.id))
             {
                AssocShape shape;
                PSIO_PSSZ_IR_TRY(assoc_shape(t, shape));
                return shape.value ? has_key(*shape.value, active) : codec_ok();
             }
             return fail(none);
          }
          else if constexpr (std::is_same_v<T, schema_types::Nested>)
             // The key of a byte list, whatever the inner type is.
             return t.type.get() ? codec_ok() : fail("pssz IR: null schema box");
          else
             return fail(none);
       },
       type->value);
   active.pop_back();
   return status;
}

template <std::size_t W>
codec_status Codec<W>::key_at(const schema_types::AnyType &input, const Value &value,
                              std::vector<char> &out, std::size_t n) const
{
   PSIO_PSSZ_IR_TRY(depth(n));
   const schema_types::AnyType *type;
   bool boxed = false;
   PSIO_PSSZ_IR_TRY(resolve(input, type, &boxed));
   if (boxed)
      return fail("pssz IR: a boxed type has no key encoding");
   auto big_endian = [&](unsigned __int128 raw, std::size_t size)
   {
      for (std::size_t i = size; i-- > 0;)
         out.push_back(static_cast<char>(raw >> (8 * i)));
   };
   // A list / map / set payload: the octet framing, or 01 + element ... 00.
   auto list = [&](const schema_types::Box<schema_types::AnyType> &element,
                   const Sequence &items, const std::vector<std::size_t> *order) -> codec_status
   {
      if (!element.get())
         return fail("pssz IR: null schema box");
      bool octet, is_signed;
      PSIO_PSSZ_IR_TRY(key_octet(*element, octet, is_signed));
      std::vector<char> one;
      for (std::size_t i = 0; i < items.size(); ++i)
      {
         const Value &item = items[order ? (*order)[i] : i];
         if (!octet)
         {
            out.push_back(1);
            PSIO_PSSZ_IR_TRY(key_at(*element, item, out, n + 1));
            continue;
         }
         one.clear();
         PSIO_PSSZ_IR_TRY(key_at(*element, item, one, n + 1));
         out.push_back(one[0]);
         if (one[0] == 0)
            out.push_back(1);
      }
      out.push_back(0);
      if (octet)
         out.push_back(0);
      return codec_ok();
   };
   return std::visit(
       [&](const auto &t) -> codec_status
       {
          using T = std::decay_t<decltype(t)>;
          if constexpr (std::is_same_v<T, schema_types::Int>)
          {
             std::size_t size;
             PSIO_PSSZ_IR_TRY(int_size(t, size));
             if (t.bits == 1)
                return fail("pssz IR: a one-bit integer has no key encoding");
             if (t.bits == 256)
             {
                auto v = std::get_if<Bytes>(&value.value);
                if (!v || v->size() != 32)
                   return fail("pssz IR: 256-bit integer needs 32 little-endian bytes");
                out.insert(out.end(), v->rbegin(), v->rend());
                return codec_ok();
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
                raw = static_cast<unsigned __int128>(v->value) ^
                      (static_cast<unsigned __int128>(1) << (t.bits - 1));
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
             big_endian(raw, size);
          }
          else if constexpr (std::is_same_v<T, schema_types::Float>)
          {
             // -0 becomes +0; negative: every bit flipped; else the sign bit.
             std::size_t size;
             PSIO_PSSZ_IR_TRY(float_size(t, size));
             std::uint64_t bits;
             if (size == 4)
             {
                auto v = std::get_if<float>(&value.value);
                if (!v)
                   return fail("pssz IR: Float32 value required");
                bits = std::bit_cast<std::uint32_t>(*v);
             }
             else
             {
                auto v = std::get_if<double>(&value.value);
                if (!v)
                   return fail("pssz IR: Float64 value required");
                bits = std::bit_cast<std::uint64_t>(*v);
             }
             const std::uint64_t sign = std::uint64_t{1} << (size * 8 - 1);
             const std::uint64_t mask = size == 4 ? 0xffffffffull : ~std::uint64_t{0};
             if (bits == sign)
                bits = 0;
             bits = (bits & sign) ? (~bits & mask) : (bits ^ sign);
             big_endian(bits, size);
          }
          else if constexpr (std::is_same_v<T, schema_types::Custom>)
          {
             if (t.id == "bool")
             {
                auto v = std::get_if<bool>(&value.value);
                if (!v)
                   return fail("pssz IR: bool value required");
                out.push_back(*v ? 1 : 0);
             }
             else if (t.id == "char")
             {
                auto v = std::get_if<Unsigned>(&value.value);
                if (!v || v->value > 255)
                   return fail("pssz IR: char byte value required");
                out.push_back(static_cast<char>(v->value));
             }
             else if (byte_adapter_id(t.id))
             {
                // A string / byte list: escaped and terminated. A fixed
                // byte array: its bytes.
                std::span<const unsigned char> bytes;
                if (auto text = std::get_if<std::string>(&value.value);
                    text && text_adapter_id(t.id))
                   bytes = {reinterpret_cast<const unsigned char *>(text->data()), text->size()};
                else if (auto raw = std::get_if<Bytes>(&value.value); raw && t.id == "hex")
                   bytes = {raw->data(), raw->size()};
                else
                   return fail(t.id == "hex" ? "pssz IR: bytes value required"
                                             : "pssz IR: string value required");
                const schema_types::AnyType *container;
                PSIO_PSSZ_IR_TRY(inner(t.type, container));
                PSIO_PSSZ_IR_TRY(bound_bytes(*t.type, bytes.size()));
                if (std::holds_alternative<schema_types::Array>(container->value))
                   out.insert(out.end(), bytes.begin(), bytes.end());
                else
                   key_escaped(bytes, out);
             }
             else if (t.id == "bitvector")
             {
                auto v = std::get_if<Bits>(&value.value);
                std::size_t count;
                bool is_list;
                PSIO_PSSZ_IR_TRY(bit_shape(t, count, is_list));
                if (!v || v->bit_count != count)
                   return fail("pssz IR: packed bit logical count mismatch");
                PSIO_PSSZ_IR_TRY(validate_bit_data(v->bytes, count));
                out.insert(out.end(), v->bytes.begin(), v->bytes.end());
             }
             else if (assoc_id(t.id))
             {
                auto v = std::get_if<Sequence>(&value.value);
                if (!v)
                   return fail("pssz IR: sequence value required");
                AssocShape shape;
                PSIO_PSSZ_IR_TRY(assoc_shape(t, shape));
                std::vector<std::size_t> order;
                PSIO_PSSZ_IR_TRY(assoc_order(t, *v, order));
                return list(*shape.element, *v, &order);
             }
             else
                return fail("pssz IR: this type has no key encoding");
          }
          else if constexpr (std::is_same_v<T, schema_types::Option>)
          {
             auto v = std::get_if<Optional>(&value.value);
             if (!v)
                return fail("pssz IR: explicit optional value required");
             out.push_back(v->some ? 1 : 0);
             if (v->some)
             {
                if (!t.type.get())
                   return fail("pssz IR: null schema box");
                return key_at(*t.type, *v->some, out, n + 1);
             }
          }
          else if constexpr (std::is_same_v<T, schema_types::List> ||
                             std::is_same_v<T, schema_types::BoundedList>)
          {
             auto v = std::get_if<Sequence>(&value.value);
             if (!v)
                return fail("pssz IR: sequence value required");
             if constexpr (std::is_same_v<T, schema_types::BoundedList>)
                if (v->size() > t.maxCount)
                   return fail("pssz IR: sequence bound exceeded");
             return list(t.type, *v, nullptr);
          }
          else if constexpr (std::is_same_v<T, schema_types::Array>)
          {
             auto v = std::get_if<Sequence>(&value.value);
             if (!v || v->size() != t.len || !t.type.get())
                return fail("pssz IR: sequence bound exceeded");
             for (const Value &item : *v)
                PSIO_PSSZ_IR_TRY(key_at(*t.type, item, out, n + 1));
          }
          else if constexpr (std::is_same_v<T, schema_types::Tuple>)
          {
             auto v = std::get_if<Sequence>(&value.value);
             if (!v || v->size() != t.members.size())
                return fail("pssz IR: tuple value count mismatch");
             for (std::size_t i = 0; i < v->size(); ++i)
             {
                if (!t.members[i].get())
                   return fail("pssz IR: null schema box");
                PSIO_PSSZ_IR_TRY(key_at(*t.members[i], (*v)[i], out, n + 1));
             }
          }
          else if constexpr (std::is_same_v<T, schema_types::Struct> ||
                             std::is_same_v<T, schema_types::Object>)
          {
             // The members in schema order; an absent Option member is None.
             auto v = std::get_if<Record>(&value.value);
             if (!v)
                return fail("pssz IR: ordered record value required");
             for (const auto &[name, field] : *v)
             {
                (void)field;
                if (std::count_if(v->begin(), v->end(),
                                  [&](const auto &f) { return f.first == name; }) != 1 ||
                    std::none_of(t.members.begin(), t.members.end(),
                                 [&](const auto &m) { return m.name == name; }))
                   return fail("pssz IR: duplicate or unknown record value");
             }
             const Value absent = Value::none();
             for (const auto &member : t.members)
             {
                const schema_types::AnyType *member_type;
                PSIO_PSSZ_IR_TRY(inner(member.type, member_type));
                auto found = std::find_if(v->begin(), v->end(),
                                          [&](const auto &f) { return f.first == member.name; });
                if (found == v->end() &&
                    !std::holds_alternative<schema_types::Option>(member_type->value))
                   return fail("pssz IR: missing required record value");
                PSIO_PSSZ_IR_TRY(key_at(*member.type, found == v->end() ? absent : found->second,
                                        out, n + 1));
             }
          }
          else if constexpr (std::is_same_v<T, schema_types::Variant>)
          {
             auto v = std::get_if<Variant>(&value.value);
             if (!v || !v->payload || t.members.empty() || t.members.size() > 256 ||
                 v->selector >= t.members.size() || !t.members[v->selector].type.get())
                return fail("pssz IR: invalid explicit variant");
             out.push_back(static_cast<char>(v->selector));
             return key_at(*t.members[v->selector].type, *v->payload, out, n + 1);
          }
          else if constexpr (std::is_same_v<T, schema_types::Nested>)
          {
             // The key of the byte list holding the inner value's canonical
             // encoding (the octet framing): the typed key of nested<T>.
             if (!t.type.get())
                return fail("pssz IR: null schema box");
             std::vector<char> bytes;
             if (opaque_nested)
             {
                auto v = std::get_if<Bytes>(&value.value);
                if (!v)
                   return fail("pssz IR: bytes value required for an opaque nested value");
                bytes.assign(v->begin(), v->end());
             }
             else
                PSIO_PSSZ_IR_TRY(nested_encode(*t.type, value, bytes, n + 1));
             key_escaped({reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()},
                         out);
          }
          else
             return fail("pssz IR: this type has no key encoding");
          return codec_ok();
       },
       type->value);
}

// The key bytes of every entry's key, in `items` order.
template <std::size_t W>
codec_status Codec<W>::assoc_keys(const AssocShape &shape, const Sequence &items,
                                  std::vector<std::vector<char>> &keys) const
{
   keys.clear();
   keys.resize(items.size());
   for (std::size_t i = 0; i < items.size(); ++i)
   {
      const Value *key = &items[i];
      if (shape.map)
      {
         auto entry = std::get_if<Sequence>(&items[i].value);
         if (!entry || entry->size() != 2)
            return fail("pssz IR: a map entry is a {key, value} pair");
         key = &(*entry)[0];
      }
      PSIO_PSSZ_IR_TRY(key_at(*shape.key, *key, keys[i]));
   }
   return codec_ok();
}

inline bool key_bytes_less(const std::vector<char> &a, const std::vector<char> &b) noexcept
{
   const std::size_t n = std::min(a.size(), b.size());
   if (n)
      if (const int c = std::memcmp(a.data(), b.data(), n))
         return c < 0;
   return a.size() < b.size();
}

// The canonical order of a map / set value's entries: indices into `items`,
// ascending by key bytes. Duplicate keys are an error (`duplicate` tells that
// failure from a malformed entry).
template <std::size_t W>
codec_status Codec<W>::assoc_order(const schema_types::Custom &custom, const Sequence &items,
                                   std::vector<std::size_t> &order, bool *duplicate) const
{
   AssocShape shape;
   PSIO_PSSZ_IR_TRY(assoc_shape(custom, shape));
   std::vector<std::vector<char>> keys;
   PSIO_PSSZ_IR_TRY(assoc_keys(shape, items, keys));
   order.resize(items.size());
   std::iota(order.begin(), order.end(), std::size_t{0});
   std::sort(order.begin(), order.end(),
             [&](std::size_t a, std::size_t b) { return key_bytes_less(keys[a], keys[b]); });
   for (std::size_t i = 1; i < order.size(); ++i)
      if (keys[order[i - 1]] == keys[order[i]])
      {
         if (duplicate)
            *duplicate = true;
         return fail(shape.map ? "pssz IR: map has duplicate keys"
                               : "pssz IR: set has duplicate elements");
      }
   return codec_ok();
}

template <std::size_t W>
codec_status Codec<W>::encode_assoc(const schema_types::Custom &custom, const Value &value,
                                    std::vector<char> &out, std::size_t n) const
{
   auto items = std::get_if<Sequence>(&value.value);
   if (!items)
      return fail("pssz IR: sequence value required");
   AssocShape shape;
   PSIO_PSSZ_IR_TRY(assoc_shape(custom, shape));
   std::vector<std::size_t> order;
   PSIO_PSSZ_IR_TRY(assoc_order(custom, *items, order));
   const schema_types::AnyType *element;
   PSIO_PSSZ_IR_TRY(inner(*shape.element, element));
   std::size_t w;
   PSIO_PSSZ_IR_TRY(width_of(*shape.list, w));
   const std::uint64_t bound = shape.max ? *shape.max : 0;
   // Values that are already in canonical order (anything a decoder or a
   // text reader produced) are encoded as they are.
   if (std::is_sorted(order.begin(), order.end()))
      return encode_sequence(*element, *items, false, bound, shape.max.has_value(), w, out, n);
   Sequence sorted;
   sorted.reserve(items->size());
   for (std::size_t index : order)
      sorted.push_back((*items)[index]);
   return encode_sequence(*element, sorted, false, bound, shape.max.has_value(), w, out, n);
}

template <std::size_t W>
codec_status Codec<W>::decode_assoc(const schema_types::Custom &custom,
                                    std::span<const char> bytes, Value &out, std::size_t n)
{
   AssocShape shape;
   PSIO_PSSZ_IR_TRY(assoc_shape(custom, shape));
   const schema_types::AnyType *element;
   PSIO_PSSZ_IR_TRY(inner(*shape.element, element));
   std::size_t w;
   PSIO_PSSZ_IR_TRY(width_of(*shape.list, w));
   Sequence items;
   PSIO_PSSZ_IR_TRY(decode_sequence(*element, bytes, false, shape.max ? *shape.max : 0,
                                    shape.max.has_value(), w, items, n));
   std::vector<std::vector<char>> keys;
   PSIO_PSSZ_IR_TRY(assoc_keys(shape, items, keys));
   for (std::size_t i = 1; i < keys.size(); ++i)
      if (!key_bytes_less(keys[i - 1], keys[i]))
         return fail(shape.map ? "pssz IR: map entries are not in strictly ascending key order"
                               : "pssz IR: set elements are not in strictly ascending order");
   out = std::move(items);
   return codec_ok();
}

#undef PSIO_PSSZ_IR_TRY
} // namespace detail

// ── Schema-driven key encoding ─────────────────────────────────────────────
//
// Appends the `key` encoding (docs/key-spec.md) of `value`, a dynamic value
// of `root`: the bytes psio::encode(psio::key{}, v) writes for the typed
// value. `value` is first checked against the schema (integer ranges,
// bounds, UTF-8 of "utf8" strings, map / set uniqueness); the entries of a
// map / set are written in canonical order whatever order `value` holds
// them in. `destination` is unchanged on failure.
//
// A type that has a sortable_binary_category adapter in C++ is described by
// its wire type in the schema: its key here is the wire type's, not the
// adapter's.
[[nodiscard]] inline codec_status to_key(const schema_types::Schema &schema,
                                         const schema_types::AnyType &root, const Value &value,
                                         std::vector<char> &destination)
{
   detail::Codec<0> codec(schema);
   if (auto status = codec.metadata(root); !status.ok())
      return status;
   std::vector<char> scratch;
   if (auto status = codec.encode_at(root, value, scratch); !status.ok())
      return status;
   std::vector<char> bytes;
   if (auto status = codec.key_at(root, value, bytes); !status.ok())
      return status;
   return detail::append(destination, bytes);
}

} // namespace psio::pssz_ir
