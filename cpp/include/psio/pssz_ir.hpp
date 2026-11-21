#pragma once

// Checked explicit-width PSSZ runtime codec for the actual schema_ir.hpp IR.
// This does not reinterpret the older lowercase psio::schema artifact.
#include <psio/detail/pssz_record_layout.hpp>
#include <psio/detail/validate_depth.hpp>
#include <psio/schema_ir.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
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
   using Storage = std::variant<bool, Signed, Unsigned, float, double, std::string, Bytes, Sequence,
                                Record, Optional, Variant>;
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
inline codec_status attributes(const std::vector<schema_types::Attribute> &values, bool layout)
{
   for (const auto &value : values)
   {
      if (value.name == "doc" || value.name == "description" || value.name == "visible")
         continue;
      if (layout && value.name == "definitionWillNotChange")
         continue;
      return fail("pssz IR: unsupported schema constraint or annotation");
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

#define PSIO_PSSZ_IR_TRY(expression)                                                               \
   do                                                                                              \
   {                                                                                               \
      auto psio_ir_status_ = (expression);                                                         \
      if (!psio_ir_status_.ok())                                                                   \
         return psio_ir_status_;                                                                   \
   } while (false)

template <std::size_t W> class Codec
{
   static_assert(W == 1 || W == 2 || W == 4);
   const schema_types::Schema &schema;
   static constexpr std::uint64_t max_offset = (std::uint64_t{1} << (W * 8)) - 1;

   codec_status resolve(const schema_types::AnyType &input, const schema_types::AnyType *&out) const
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
   codec_status inner(const schema_types::Box<schema_types::AnyType> &box,
                      const schema_types::AnyType *&out) const
   {
      if (!box.get())
         return fail("pssz IR: null schema box");
      return resolve(*box, out);
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
      PSIO_PSSZ_IR_TRY(inner(custom.type, type));
      auto integer = std::get_if<schema_types::Int>(&type->value);
      return integer && integer->bits == bits && !integer->isSigned
                 ? codec_ok()
                 : fail("pssz IR: primitive adapter has incompatible inner schema");
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
                if (t.id == "string" || t.id == "hex")
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
                if (t.id == "string" || t.id == "hex")
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
   static codec_status read_offset(std::span<const char> bytes, std::size_t at, std::size_t &out)
   {
      if (at > bytes.size() || W > bytes.size() - at)
         return fail("pssz IR: offset underrun");
      out = 0;
      for (std::size_t i = 0; i < W; ++i)
         out |= std::size_t(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
      return codec_ok();
   }
   static codec_status write_offset(std::vector<char> &bytes, std::size_t at, std::size_t value)
   {
      if (value > max_offset || at > bytes.size() || W > bytes.size() - at)
         return fail("pssz IR: offset width overflow");
      for (std::size_t i = 0; i < W; ++i)
         bytes[at + i] = static_cast<char>(value >> (8 * i));
      return codec_ok();
   }
   codec_status bound_bytes(const schema_types::AnyType &input, std::size_t count,
                            bool check_count = true) const
   {
      const schema_types::AnyType *type;
      PSIO_PSSZ_IR_TRY(resolve(input, type));
      const schema_types::AnyType *element = nullptr;
      if (auto list = std::get_if<schema_types::List>(&type->value))
         PSIO_PSSZ_IR_TRY(inner(list->type, element));
      else if (auto list = std::get_if<schema_types::BoundedList>(&type->value))
      {
         if (check_count && count > list->maxCount)
            return fail("pssz IR: byte count bound exceeded");
         PSIO_PSSZ_IR_TRY(inner(list->type, element));
      }
      else if (auto array = std::get_if<schema_types::Array>(&type->value))
      {
         if (check_count && count != array->len)
            return fail("pssz IR: byte array count mismatch");
         PSIO_PSSZ_IR_TRY(inner(array->type, element));
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
                                     std::vector<char> &out)
   {
      std::size_t fixed = 0;
      for (const auto &field : fields)
         PSIO_PSSZ_IR_TRY(add(fixed, field.layout.fixed ? field.layout.size : W));
      const std::size_t header = stable ? 0 : W;
      std::size_t total = fixed;
      PSIO_PSSZ_IR_TRY(add(total, header));
      if (total > out.max_size())
         return fail("pssz IR: record size overflow");
      std::vector<char> bytes(total, 0);
      if (!stable)
         PSIO_PSSZ_IR_TRY(write_offset(bytes, 0, fixed));
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
            PSIO_PSSZ_IR_TRY(write_offset(bytes, cursor, bytes.size() - header));
            PSIO_PSSZ_IR_TRY(append(bytes, field.bytes));
            cursor += W;
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
                               const Record &values, bool stable, std::vector<char> &out,
                               std::size_t n) const
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
         PSIO_PSSZ_IR_TRY(inner(member.type, type));
         Layout info;
         PSIO_PSSZ_IR_TRY(layout(*type, info));
         auto found = std::find_if(values.begin(), values.end(),
                                   [&](const auto &f) { return f.first == member.name; });
         if (found == values.end() && !info.optional)
            return fail("pssz IR: missing required record value");
         const Value *value = found == values.end() ? &absent : &found->second;
         ordered.push_back(value);
         types.push_back(type);
         if (stable || !info.optional || !none(*value))
            keep = ordered.size();
      }
      std::vector<Field> fields(keep);
      for (std::size_t i = 0; i < keep; ++i)
         PSIO_PSSZ_IR_TRY(encode_field(*types[i], *ordered[i], n + 1, fields[i]));
      return encode_fields(fields, stable, out);
   }
   codec_status encode_sequence(const schema_types::AnyType &type, const Sequence &values,
                                bool exact, std::uint64_t bound, bool bounded,
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
      return encode_fields(fields, true, out);
   }
   codec_status decode_members(const std::vector<schema_types::Member> &members,
                               std::span<const char> bytes, bool stable, Record &out, std::size_t n)
   {
      std::vector<const schema_types::AnyType *> types;
      std::vector<psio::detail::pssz_field_layout> fields;
      for (const auto &member : members)
      {
         const schema_types::AnyType *type;
         PSIO_PSSZ_IR_TRY(inner(member.type, type));
         Layout info;
         PSIO_PSSZ_IR_TRY(layout(*type, info));
         types.push_back(type);
         fields.push_back({info.fixed ? info.size : W, !info.fixed, info.optional});
      }
      std::vector<psio::detail::pssz_field_span> spans(fields.size());
      bool unknown = false;
      PSIO_PSSZ_IR_TRY(psio::detail::pssz_record_spans<W>(bytes, 0, bytes.size(), stable, fields,
                                                          spans, unknown));
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
         out.emplace_back(members[i].name, std::move(value));
      }
      return codec_ok();
   }
   codec_status decode_sequence(const schema_types::AnyType &type, std::span<const char> bytes,
                                bool exact, std::uint64_t bound, bool bounded, Sequence &out,
                                std::size_t n)
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
         PSIO_PSSZ_IR_TRY(read_offset(bytes, 0, first));
         if (!first || first % W || first > bytes.size())
            return fail("pssz IR: invalid list offset table");
         count = first / W;
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
         out.reserve(count);
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
         if (count > bytes.size() / W || (!count && !bytes.empty()))
            return fail("pssz IR: list table span mismatch");
         const std::size_t table = count * W;
         std::size_t previous = table;
         out.reserve(count);
         for (std::size_t i = 0; i < count; ++i)
         {
            std::size_t begin, end = bytes.size();
            PSIO_PSSZ_IR_TRY(read_offset(bytes, i * W, begin));
            if (i + 1 < count)
               PSIO_PSSZ_IR_TRY(read_offset(bytes, (i + 1) * W, end));
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
   explicit Codec(const schema_types::Schema &schema) : schema(schema) {}

   codec_status metadata(const schema_types::AnyType &root) const
   {
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
                   PSIO_PSSZ_IR_TRY(attributes(t.attributes, record));
                   if constexpr (!record)
                      if (t.members.empty() || t.members.size() > 256)
                         return fail("pssz IR: invalid variant schema");
                   for (const auto &member : t.members)
                   {
                      if (std::count_if(t.members.begin(), t.members.end(),
                                        [&](const auto &m) { return m.name == member.name; }) != 1)
                         return fail("pssz IR: duplicate schema member");
                      PSIO_PSSZ_IR_TRY(attributes(member.attributes, false));
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
                   else if (t.id == "string" || t.id == "hex")
                   {
                      const schema_types::AnyType *child;
                      PSIO_PSSZ_IR_TRY(inner(t.type, child));
                      PSIO_PSSZ_IR_TRY(bound_bytes(*child, 0, false));
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

   codec_status encode_at(const schema_types::AnyType &input, const Value &value,
                          std::vector<char> &out, std::size_t n = 0) const
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
                else if (t.id == "string")
                {
                   auto v = std::get_if<std::string>(&value.value);
                   if (!v)
                      return fail("pssz IR: string value required");
                   PSIO_PSSZ_IR_TRY(bound_bytes(*child, v->size()));
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
                else
                   return fail("pssz IR: unsupported custom adapter");
             }
             else if constexpr (std::is_same_v<T, schema_types::Option>)
             {
                auto v = std::get_if<Optional>(&value.value);
                if (!v)
                   return fail("pssz IR: explicit optional value required");
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
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
                PSIO_PSSZ_IR_TRY(encode_members(t.members, *v, stable, out, n));
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
                   PSIO_PSSZ_IR_TRY(inner(t.members[i], child));
                   types.push_back(child);
                   Layout info;
                   PSIO_PSSZ_IR_TRY(layout(*child, info));
                   if (!info.optional || !none((*v)[i]))
                      keep = i + 1;
                }
                std::vector<Field> fields(keep);
                for (std::size_t i = 0; i < keep; ++i)
                   PSIO_PSSZ_IR_TRY(encode_field(*types[i], (*v)[i], n + 1, fields[i]));
                PSIO_PSSZ_IR_TRY(encode_fields(fields, false, out));
             }
             else if constexpr (std::is_same_v<T, schema_types::List> ||
                                std::is_same_v<T, schema_types::BoundedList> ||
                                std::is_same_v<T, schema_types::Array>)
             {
                auto v = std::get_if<Sequence>(&value.value);
                if (!v)
                   return fail("pssz IR: sequence value required");
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.type, child));
                if constexpr (std::is_same_v<T, schema_types::List>)
                   PSIO_PSSZ_IR_TRY(encode_sequence(*child, *v, false, 0, false, out, n));
                else if constexpr (std::is_same_v<T, schema_types::BoundedList>)
                   PSIO_PSSZ_IR_TRY(encode_sequence(*child, *v, false, t.maxCount, true, out, n));
                else
                   PSIO_PSSZ_IR_TRY(encode_sequence(*child, *v, true, t.len, true, out, n));
             }
             else if constexpr (std::is_same_v<T, schema_types::Variant>)
             {
                auto v = std::get_if<Variant>(&value.value);
                if (!v || !v->payload || t.members.empty() || t.members.size() > 256 ||
                    v->selector >= t.members.size())
                   return fail("pssz IR: invalid explicit variant");
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(t.members[v->selector].type, child));
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

   codec_status decode_at(const schema_types::AnyType &input, std::span<const char> bytes,
                          Value &out, std::size_t n = 0);
};

template <std::size_t W>
codec_status Codec<W>::decode_at(const schema_types::AnyType &input, std::span<const char> bytes,
                                 Value &out, std::size_t n)
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
             else if (t.id == "string")
             {
                PSIO_PSSZ_IR_TRY(bound_bytes(*child, bytes.size()));
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
             else
                return fail("pssz IR: unsupported custom adapter");
          }
          else if constexpr (std::is_same_v<T, schema_types::Option>)
          {
             const schema_types::AnyType *child;
             PSIO_PSSZ_IR_TRY(inner(t.type, child));
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
             Record values;
             PSIO_PSSZ_IR_TRY(decode_members(t.members, bytes, stable, values, n));
             out = std::move(values);
          }
          else if constexpr (std::is_same_v<T, schema_types::Tuple>)
          {
             std::vector<psio::detail::pssz_field_layout> fields;
             std::vector<const schema_types::AnyType *> types;
             for (const auto &member : t.members)
             {
                const schema_types::AnyType *child;
                PSIO_PSSZ_IR_TRY(inner(member, child));
                Layout info;
                PSIO_PSSZ_IR_TRY(layout(*child, info));
                types.push_back(child);
                fields.push_back({info.fixed ? info.size : W, !info.fixed, info.optional});
             }
             std::vector<psio::detail::pssz_field_span> spans(fields.size());
             bool unknown = false;
             PSIO_PSSZ_IR_TRY(psio::detail::pssz_record_spans<W>(bytes, 0, bytes.size(), false,
                                                                 fields, spans, unknown));
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
                values.push_back(std::move(value));
             }
             out = std::move(values);
          }
          else if constexpr (std::is_same_v<T, schema_types::List> ||
                             std::is_same_v<T, schema_types::BoundedList> ||
                             std::is_same_v<T, schema_types::Array>)
          {
             const schema_types::AnyType *child;
             PSIO_PSSZ_IR_TRY(inner(t.type, child));
             Sequence values;
             if constexpr (std::is_same_v<T, schema_types::List>)
                PSIO_PSSZ_IR_TRY(decode_sequence(*child, bytes, false, 0, false, values, n));
             else if constexpr (std::is_same_v<T, schema_types::BoundedList>)
                PSIO_PSSZ_IR_TRY(
                    decode_sequence(*child, bytes, false, t.maxCount, true, values, n));
             else
                PSIO_PSSZ_IR_TRY(decode_sequence(*child, bytes, true, t.len, true, values, n));
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
             PSIO_PSSZ_IR_TRY(inner(t.members[selector].type, child));
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
                                  std::vector<char> &destination)
{
   detail::Codec<W> codec(schema);
   std::vector<char> bytes;
   auto status = codec.metadata(root);
   if (!status.ok())
      return status;
   status = codec.encode_at(root, value, bytes);
   if (!status.ok())
      return status;
   return detail::append(destination, bytes);
}
template <std::size_t W>
[[nodiscard]] codec_status
decode_compatible(const schema_types::Schema &schema, const schema_types::AnyType &root,
                  std::span<const char> bytes, Value &output, bool &has_unknown)
{
   detail::Codec<W> codec(schema);
   codec.allow_unknown = true;
   Value value;
   auto status = codec.metadata(root);
   if (!status.ok())
      return status;
   status = codec.decode_at(root, bytes, value);
   if (!status.ok())
      return status;
   output = std::move(value);
   has_unknown = codec.has_unknown;
   return codec_ok();
}
template <std::size_t W>
// Exact here means known schema and structural validity. Well-formed
// unpruned None is accepted; canonicality is a separate explicit check.
[[nodiscard]] codec_status decode_exact(const schema_types::Schema &schema,
                                        const schema_types::AnyType &root,
                                        std::span<const char> bytes, Value &output)
{
   detail::Codec<W> codec(schema);
   Value value;
   auto status = codec.metadata(root);
   if (!status.ok())
      return status;
   status = codec.decode_at(root, bytes, value);
   if (!status.ok())
      return status;
   output = std::move(value);
   return codec_ok();
}
template <std::size_t W>
[[nodiscard]] codec_status validate_canonical_for_width(const schema_types::Schema &schema,
                                                        const schema_types::AnyType &root,
                                                        std::span<const char> bytes)
{
   Value value;
   auto status = decode_exact<W>(schema, root, bytes, value);
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
