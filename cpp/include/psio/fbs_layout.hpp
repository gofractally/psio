#pragma once
//
// psio/fbs_layout.hpp — the FlatBuffers wire layout of a Schema IR.
//
// `psio::fbs_layout(schema)` computes, for a schema — one the schema builder
// made from reflected C++ types, or one psio/fbs_parser.hpp read from `.fbs`
// text — what `flatc` computes for the corresponding declarations and what
// psio/flatbuf.hpp uses for the corresponding C++ types:
//
//   table    the field id (vtable slot) of every member; whether it is a
//            union (two slots: the type byte, then the value), deprecated,
//            required, a key, an optional scalar; the default of a scalar;
//   struct   the byte offset of every member, the struct's size and
//            alignment;
//   enum     the underlying integer and every enumerator's value;
//   union    the discriminant of every member (`NONE` is 0);
//   file     the root type and the file identifier.
//
// tests/fbs_schema_tests.cpp compares all of it with the binary schema
// `flatc --binary --schema` writes, and tests/flatbuf_tests.cpp with the
// codec's compile-time layout.
//
// ── How the Schema IR is read as FlatBuffers ─────────────────────────────
//
// This is the one statement of the mapping: psio/emit_fbs.hpp writes the
// declarations it describes, psio/fbs_parser.hpp produces the IR it reads.
//
//   Object                  a table. With `definitionWillNotChange` and
//                           nothing but fixed members (below) it is a
//                           struct — the codec's rule for a C++ record.
//   Struct                  a struct (fixed members only); a table if a
//                           member is not fixed.
//   Enum{Int, enumerators}  an enum of that integer.
//   Variant                 a union; each member is a table (a `Type`
//                           naming one). A first member of type `Tuple{}`
//                           is `NONE` (C++ `std::monostate`). A variant
//                           whose members are all `Tuple{}` is an enum of
//                           `ubyte` with the values 0, 1, ...
//   Int 8/16/32/64, Float, `bool`, `char`          the scalars
//   `string`, `utf8`        string
//   List, BoundedList, `hex`, Nested               a vector
//   `set` over List<K>      a vector (sorted)
//   `map` over List<Tuple[K, V]>   a vector of entry tables
//                           `{ key:K (key); value:V; }`
//   Array                   in a struct, a fixed array `[T:N]`; in a table,
//                           a vector
//   Option<scalar or enum>  an optional scalar (`= null`)
//   Option<anything else>   that field (every such field may be absent)
//   any other Custom        its inner type
//
// Fixed (may be a struct member): a scalar, an enum, an Array of a fixed
// type, a struct. A record that holds itself is never fixed.
//
// Member attributes:
//
//   field = N        the member's field number (absent: its 1-based position
//                    — psio/detail/field_number.hpp). Vtable slots are
//                    assigned in ascending order of it, a union taking two;
//                    `(id: K)` in `.fbs` text is field = K + 1.
//   default = text   a scalar's default: `true` / `false`, a decimal
//                    integer (an enum's value), a decimal float, `nan`,
//                    `inf`, `-inf`
//   deprecated       the member keeps its slot and is neither read nor
//                    written
//   required, key    FlatBuffers' own
//
// On a struct: `force_align = N`. On a variant member: `value = N`, its
// discriminant when it is not the previous one + 1. On the package:
// `fbs_root_type`, `fbs_file_identifier`, `fbs_file_extension`.

#include <psio/schema_ir.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace psio::schema_types
{

   struct fbs_field_layout
   {
      std::string   name;
      // Tables: the field id, and its position in the vtable (4 + 2 * id).
      // For a union this is the type byte; the value is at slot + 1.
      std::uint16_t slot          = 0;
      std::uint16_t vtable_offset = 0;
      bool          is_union      = false;
      bool          deprecated    = false;
      bool          required      = false;
      bool          key           = false;
      bool          optional      = false;  // an optional scalar (`= null`)
      // Structs: the member's byte offset.
      std::uint32_t offset = 0;
      // The bytes the member occupies inline (4 for an offset) and their
      // alignment.
      std::uint32_t size  = 0;
      std::uint32_t align = 0;
      // The default of a scalar member of a table: of an integer, bool or
      // enum (an unsigned 64-bit value as its two's complement), of a float.
      std::int64_t default_integer = 0;
      double       default_real    = 0;
   };

   struct fbs_type_layout
   {
      std::string                   name;
      bool                          is_struct = false;
      std::vector<fbs_field_layout> fields;
      std::uint16_t                 slots = 0;  // tables: vtable slots in use
      std::uint32_t                 size  = 0;  // structs
      std::uint32_t                 align = 0;  // structs

      const fbs_field_layout* field(std::string_view field_name) const noexcept
      {
         for (const auto& f : fields)
            if (f.name == field_name)
               return &f;
         return nullptr;
      }
   };

   struct fbs_enum_value
   {
      std::string  name;
      std::int64_t value = 0;
   };

   // An enum, or a union (whose values are its discriminants, `NONE` first).
   struct fbs_enum_layout
   {
      std::string                 name;
      bool                        is_union  = false;
      std::uint32_t               size      = 1;  // bytes of the underlying integer
      bool                        is_signed = false;
      std::vector<fbs_enum_value> values;

      const fbs_enum_value* value(std::string_view value_name) const noexcept
      {
         for (const auto& v : values)
            if (v.name == value_name)
               return &v;
         return nullptr;
      }
   };

   struct fbs_layouts
   {
      std::vector<fbs_type_layout> types;
      std::vector<fbs_enum_layout> enums;
      std::string                  root_type;
      std::string                  file_identifier;

      const fbs_type_layout* find(std::string_view name) const noexcept
      {
         for (const auto& t : types)
            if (t.name == name)
               return &t;
         return nullptr;
      }
      const fbs_enum_layout* find_enum(std::string_view name) const noexcept
      {
         for (const auto& e : enums)
            if (e.name == name)
               return &e;
         return nullptr;
      }
   };

   namespace fbs_layout_detail
   {
      enum class kind : std::uint8_t
      {
         integer,
         real,
         boolean,
         enumeration,
         array,      // a fixed array of fixed elements
         structure,
         table,
         string,
         vector,
         union_type,
         unit        // Tuple{}: no value (a union's NONE)
      };

      struct shape
      {
         kind          k         = kind::table;
         std::uint32_t size      = 4;
         std::uint32_t align     = 4;
         bool          fixed     = false;  // may be a struct member
         bool          optional  = false;  // Option<...>
         bool          is_signed = false;  // integers
         bool          is_map    = false;  // vector: `map`
         // The node the type resolves to, and the name of the schema entry
         // it was last reached through (null: written inline).
         const AnyType*     node = nullptr;
         const std::string* name = nullptr;

         bool scalar() const noexcept
         {
            return k == kind::integer || k == kind::real || k == kind::boolean ||
                   k == kind::enumeration;
         }
      };

      [[noreturn]] inline void fail(const std::string& what)
      {
         throw std::runtime_error("fbs_layout: " + what);
      }

      // The value of attribute `name`: null when it is absent or has no
      // value; `*present` says which.
      inline const std::string* attribute(const std::vector<Attribute>& attrs,
                                          std::string_view              name,
                                          bool*                         present = nullptr)
      {
         for (const auto& a : attrs)
            if (a.name == name)
            {
               if (present)
                  *present = true;
               return a.value ? &*a.value : nullptr;
            }
         return nullptr;
      }
      inline bool has_attribute(const std::vector<Attribute>& attrs, std::string_view name)
      {
         bool present = false;
         attribute(attrs, name, &present);
         return present;
      }

      inline std::optional<std::uint64_t> parse_unsigned(std::string_view text)
      {
         std::uint64_t v = 0;
         const auto    r = std::from_chars(text.data(), text.data() + text.size(), v);
         if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size())
            return std::nullopt;
         return v;
      }

      // A decimal integer of up to 64 bits, signed or unsigned, as its
      // two's complement.
      inline std::optional<std::int64_t> parse_integer(std::string_view text)
      {
         if (!text.empty() && text.front() == '+')
            text.remove_prefix(1);
         if (!text.empty() && text.front() == '-')
         {
            std::int64_t v = 0;
            const auto   r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
               return std::nullopt;
            return v;
         }
         if (auto u = parse_unsigned(text))
            return static_cast<std::int64_t>(*u);
         return std::nullopt;
      }

      inline std::optional<double> parse_real(std::string_view text)
      {
         bool negative = false;
         if (!text.empty() && (text.front() == '+' || text.front() == '-'))
         {
            negative = text.front() == '-';
            text.remove_prefix(1);
         }
         double v = 0;
         if (text == "nan")
            v = std::numeric_limits<double>::quiet_NaN();
         else if (text == "inf" || text == "infinity")
            v = std::numeric_limits<double>::infinity();
         else
         {
            if (text.empty() || !((text.front() >= '0' && text.front() <= '9') || text.front() == '.'))
               return std::nullopt;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec == std::errc::result_out_of_range)
               v = std::numeric_limits<double>::infinity();
            else if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
               return std::nullopt;
         }
         return negative ? -v : v;
      }

      // The shapes of a schema's types. Records are examined once each, so
      // the work is linear in the schema whatever its sharing; a struct
      // that contains itself is refused.
      class model
      {
       public:
         // (the schema must not gain or lose types while the model is used)
         explicit model(const Schema& s) : _s{s}
         {
            for (const auto& m : s.types)
            {
               _by_name.emplace(m.name, &m);  // the first of a name, as Schema::get
               _by_node.emplace(m.type.get(), &m);
            }
         }

         const Schema& schema() const noexcept { return _s; }

         // The schema entry named `name` (the first, as Schema::get), and the
         // entry whose type is the node `node`; null when there is none.
         const Member* find(std::string_view name) const
         {
            const auto at = _by_name.find(name);
            return at == _by_name.end() ? nullptr : at->second;
         }
         const Member* entry_of(const AnyType& node) const
         {
            const auto at = _by_node.find(&node);
            return at == _by_node.end() ? nullptr : at->second;
         }

         // `Tuple{}`, directly or by name: no value.
         bool is_unit(const AnyType& t) const
         {
            const AnyType* at = &t;
            for (std::size_t hops = 0; hops <= _s.types.size(); ++hops)
            {
               const auto* ref = std::get_if<Type>(&at->value);
               if (!ref)
               {
                  const auto* tuple = std::get_if<Tuple>(&at->value);
                  return tuple && tuple->members.empty();
               }
               const auto found = _by_name.find(std::string_view{ref->type});
               if (found == _by_name.end())
                  return false;
               at = found->second->type.get();
            }
            return false;
         }

         // A variant whose members all have no value: an enumeration.
         bool enum_shaped(const Variant& v)
         {
            const auto [at, inserted] = _enum_shaped.try_emplace(&v, false);
            if (inserted)
            {
               bool all = !v.members.empty();
               for (const auto& m : v.members)
                  if (!is_unit(*m.type))
                  {
                     all = false;
                     break;
                  }
               at->second = all;
            }
            return at->second;
         }

         // `type`, with named references followed. `name` receives the last
         // name followed (unchanged if none).
         const AnyType& resolve(const AnyType& type, const std::string** name = nullptr) const
         {
            const AnyType* at = &type;
            for (std::size_t hops = 0; hops <= _s.types.size(); ++hops)
            {
               const auto* ref = std::get_if<Type>(&at->value);
               if (!ref)
                  return *at;
               const auto found = _by_name.find(std::string_view{ref->type});
               if (found == _by_name.end())
                  fail("unknown type '" + ref->type + "'");
               const Member* entry = found->second;
               if (name)
                  *name = &entry->name;
               at = entry->type.get();
            }
            fail("a type name that refers to itself");
         }

         shape shape_of(const AnyType& written, unsigned depth = 0)
         {
            if (depth > 64)
               fail("types nested too deeply");
            const std::string* name = nullptr;
            const AnyType&     type = resolve(written, &name);
            shape              out;
            out.node = &type;
            out.name = name;
            if (const auto* i = std::get_if<Int>(&type.value))
            {
               if (i->bits != 8 && i->bits != 16 && i->bits != 32 && i->bits != 64)
                  fail("an integer of " + std::to_string(i->bits) + " bits has no FlatBuffers type");
               out.k    = kind::integer;
               out.size = out.align = i->bits / 8;
               out.is_signed        = i->isSigned;
               out.fixed            = true;
            }
            else if (const auto* f = std::get_if<Float>(&type.value))
            {
               if (f->exp != 8 && f->exp != 11)
                  fail("a float that is not 32 or 64 bits has no FlatBuffers type");
               out.k    = kind::real;
               out.size = out.align = f->exp == 8 ? 4 : 8;
               out.fixed            = true;
            }
            else if (const auto* c = std::get_if<Custom>(&type.value))
            {
               if (c->id == "bool")
               {
                  out.k    = kind::boolean;
                  out.size = out.align = 1;
                  out.fixed            = true;
               }
               else if (c->id == "string" || c->id == "utf8")
                  out.k = kind::string;
               else if (c->id == "hex" || c->id == "set")
                  out.k = kind::vector;
               else if (c->id == "map")
               {
                  out.k      = kind::vector;
                  out.is_map = true;
               }
               else
               {
                  out      = shape_of(*c->type, depth + 1);
                  out.name = name ? name : out.name;
               }
            }
            else if (const auto* e = std::get_if<Enum>(&type.value))
            {
               const shape under = shape_of(*e->type, depth + 1);
               if (under.k != kind::integer && under.k != kind::boolean)
                  fail("an enum that is not over an integer");
               out.k    = kind::enumeration;
               out.size = out.align = under.size;
               out.is_signed        = under.is_signed;
               out.fixed            = true;
            }
            else if (const auto* o = std::get_if<Option>(&type.value))
            {
               const shape inner = shape_of(*o->type, depth + 1);
               if (inner.optional)
                  fail("an optional of an optional has no FlatBuffers type");
               if (inner.k == kind::union_type || inner.k == kind::unit)
                  fail("an optional union has no FlatBuffers type");
               out          = inner;
               out.optional = true;
               out.fixed    = false;
               if (inner.k == kind::array)
                  out.k = kind::vector, out.size = out.align = 4;
            }
            else if (const auto* a = std::get_if<Array>(&type.value))
            {
               const shape e = shape_of(*a->type, depth + 1);
               if (e.fixed && a->len >= 1 && a->len <= 0xffff &&
                   std::uint64_t{e.size} * a->len <= 0x7fffffff)
               {
                  out.k     = kind::array;
                  out.size  = static_cast<std::uint32_t>(e.size * a->len);
                  out.align = e.align;
                  out.fixed = true;
               }
               else
                  out.k = kind::vector;
            }
            else if (std::holds_alternative<List>(type.value) ||
                     std::holds_alternative<BoundedList>(type.value) ||
                     std::holds_alternative<Nested>(type.value))
               out.k = kind::vector;
            else if (const auto* st = std::get_if<Struct>(&type.value))
               record(type, st->members, st->attributes, true, depth, out);
            else if (const auto* ob = std::get_if<Object>(&type.value))
               record(type, ob->members, ob->attributes,
                      has_attribute(ob->attributes, "definitionWillNotChange"), depth, out);
            else if (const auto* v = std::get_if<Variant>(&type.value))
            {
               if (enum_shaped(*v))
               {
                  out.k    = kind::enumeration;
                  out.size = out.align = 1;
                  out.fixed            = true;
               }
               else
                  out.k = kind::union_type;
            }
            else if (const auto* t = std::get_if<Tuple>(&type.value))
            {
               if (!t->members.empty())
                  fail("a tuple has no FlatBuffers type");
               out.k    = kind::unit;
               out.size = out.align = 0;
            }
            else
               fail("a resource has no FlatBuffers type");
            return out;
         }

         // The members of a struct with their offsets; `out` gets size and
         // alignment. Only for a record shape_of() calls a structure.
         void struct_fields(const std::vector<Member>& members, const std::vector<Attribute>& attrs,
                            fbs_type_layout& out)
         {
            std::uint32_t pos = 0, align = 1;
            for (const auto& m : members)
            {
               const shape e = shape_of(*m.type, 1);
               pos           = (pos + e.align - 1) & ~(e.align - 1);
               fbs_field_layout f;
               f.name       = m.name;
               f.offset     = pos;
               f.size       = e.size;
               f.align      = e.align;
               f.deprecated = has_attribute(m.attributes, "deprecated");
               out.fields.push_back(std::move(f));
               pos += e.size;
               if (e.align > align)
                  align = e.align;
            }
            align         = forced_alignment(attrs, align);
            out.is_struct = true;
            out.size      = (pos + align - 1) & ~(align - 1);
            out.align     = align;
         }

       private:
         struct record_state
         {
            bool          done      = false;
            bool          is_struct = false;
            std::uint32_t size      = 0;
            std::uint32_t align     = 0;
         };

         static std::uint32_t forced_alignment(const std::vector<Attribute>& attrs,
                                               std::uint32_t                 natural)
         {
            const std::string* text = attribute(attrs, "force_align");
            if (!text)
               return natural;
            const auto n = parse_unsigned(*text);
            // flatc: a power of two from the natural alignment to 16.
            if (!n || *n < natural || *n > 16 || (*n & (*n - 1)) != 0)
               fail("force_align must be a power of two from the struct's alignment to 16");
            return static_cast<std::uint32_t>(*n);
         }

         void record(const AnyType& node, const std::vector<Member>& members,
                     const std::vector<Attribute>& attrs, bool may_be_struct, unsigned depth,
                     shape& out)
         {
            out.k = kind::table;
            if (!may_be_struct || members.empty())
               return;
            auto [at, inserted] = _records.try_emplace(&node);
            if (!inserted)
            {
               // Reached again while its members are examined: it holds
               // itself (behind an optional), so it is not fixed.
               if (!at->second.done)
                  return;
            }
            else
            {
               record_state  r;
               std::uint64_t pos = 0;
               std::uint32_t align = 1;
               r.is_struct = true;
               for (const auto& m : members)
               {
                  const shape e = shape_of(*m.type, depth + 1);
                  if (!e.fixed)
                  {
                     r.is_struct = false;
                     break;
                  }
                  pos = (pos + e.align - 1) & ~std::uint64_t{e.align - 1};
                  pos += e.size;
                  if (pos > 0x7fffffff)
                     fail("a struct larger than a buffer");
                  if (e.align > align)
                     align = e.align;
               }
               if (r.is_struct)
               {
                  align   = forced_alignment(attrs, align);
                  r.align = align;
                  r.size  = static_cast<std::uint32_t>((pos + align - 1) & ~std::uint64_t{align - 1});
               }
               r.done     = true;
               at->second = r;  // (std::map: `at` survived the insertions)
            }
            if (at->second.is_struct)
            {
               out.k     = kind::structure;
               out.size  = at->second.size;
               out.align = at->second.align;
               out.fixed = true;
            }
         }

         const Schema&                           _s;
         std::map<std::string_view, const Member*> _by_name;
         std::map<const AnyType*, const Member*>   _by_node;
         std::map<const AnyType*, record_state>    _records;
         std::map<const Variant*, bool>            _enum_shaped;
      };

      // A member's field number: `field`, else its 1-based position.
      inline std::uint64_t field_number(const Member& m, std::size_t index)
      {
         bool               present = false;
         const std::string* text    = attribute(m.attributes, "field", &present);
         if (!present)
            return index + 1;
         const auto n = text ? parse_unsigned(*text) : std::nullopt;
         if (!n)
            fail("member '" + m.name + "': a field attribute that is not a number");
         return *n;
      }

      // The members of a record in ascending order of field number
      // (declaration order among equal numbers): member index by rank.
      inline std::vector<std::size_t> members_by_field_number(const std::vector<Member>& members)
      {
         std::vector<std::uint64_t> number(members.size());
         std::vector<std::size_t>   order(members.size());
         bool                       sorted = true;
         for (std::size_t i = 0; i < members.size(); ++i)
         {
            number[i] = field_number(members[i], i);
            order[i]  = i;
            if (i > 0 && number[i] < number[i - 1])
               sorted = false;
         }
         if (!sorted)
            std::stable_sort(order.begin(), order.end(),
                             [&](std::size_t a, std::size_t b) { return number[a] < number[b]; });
         return order;
      }

      // The default of a scalar member, from its `default` attribute.
      inline void scalar_default(const Member& m, const shape& e, fbs_field_layout& f)
      {
         const std::string* text = attribute(m.attributes, "default");
         if (!text || !e.scalar() || e.optional)
            return;
         if (e.k == kind::real)
         {
            const auto v = parse_real(*text);
            if (!v)
               fail("member '" + m.name + "': '" + *text + "' is not a float");
            f.default_real = e.size == 4 ? static_cast<double>(static_cast<float>(*v)) : *v;
            return;
         }
         if (*text == "true" || *text == "false")
         {
            f.default_integer = *text == "true";
            return;
         }
         const auto v = parse_integer(*text);
         if (!v)
            fail("member '" + m.name + "': '" + *text + "' is not an integer");
         f.default_integer = *v;
      }

      // Whether a table member takes two slots: a union (its type, its
      // value) or a vector of unions (two vectors: types and values).
      inline bool union_field(model& types, const shape& e)
      {
         if (e.k == kind::union_type)
            return true;
         if (e.k != kind::vector)
            return false;
         if (const auto* l = std::get_if<List>(&e.node->value))
            return types.shape_of(*l->type, 2).k == kind::union_type;
         if (const auto* b = std::get_if<BoundedList>(&e.node->value))
            return types.shape_of(*b->type, 2).k == kind::union_type;
         return false;
      }

      inline void table_fields(model& types, const std::vector<Member>& members,
                               fbs_type_layout& out)
      {
         out.fields.resize(members.size());
         std::uint32_t next = 0;
         for (const std::size_t i : members_by_field_number(members))
         {
            const Member& m = members[i];
            shape         e = types.shape_of(*m.type, 1);
            if (e.k == kind::unit)
               fail("member '" + m.name + "' has no value");
            const bool is_union = union_field(types, e);
            const bool by_offset = !(e.scalar() || e.k == kind::structure);
            fbs_field_layout& f = out.fields[i];
            f.name              = m.name;
            f.is_union          = is_union;
            f.size              = e.k == kind::union_type ? 1 : by_offset ? 4 : e.size;
            f.align             = e.k == kind::union_type ? 1 : by_offset ? 4 : e.align;
            f.deprecated        = has_attribute(m.attributes, "deprecated");
            f.required          = has_attribute(m.attributes, "required");
            f.key               = has_attribute(m.attributes, "key");
            // flatc: a key that is a string is required.
            if (f.key && e.k == kind::string)
               f.required = true;
            f.optional          = e.optional && e.scalar();
            scalar_default(m, e, f);
            const std::uint32_t slot = next;
            next += is_union ? 2 : 1;
            // flatc: a vtable is at most 65535 bytes.
            if (next > 0x7ffd)
               fail("table '" + out.name + "' has too many fields");
            f.slot          = static_cast<std::uint16_t>(slot);
            f.vtable_offset = static_cast<std::uint16_t>(4 + 2 * slot);
         }
         out.slots = static_cast<std::uint16_t>(next);
      }

      inline fbs_enum_layout enum_layout(model& types, const std::string& name, const AnyType& node)
      {
         fbs_enum_layout out;
         out.name = name;
         if (const auto* e = std::get_if<Enum>(&node.value))
         {
            const shape s = types.shape_of(node);
            out.size      = s.size;
            out.is_signed = s.is_signed;
            for (const auto& v : e->members)
               out.values.push_back({v.name, static_cast<std::int64_t>(v.value)});
            return out;
         }
         const auto&  v    = std::get<Variant>(node.value);
         std::int64_t next = 0;
         if (types.enum_shaped(v))
         {
            for (const auto& m : v.members)
               out.values.push_back({m.name, next++});
            return out;
         }
         out.is_union = true;
         out.values.push_back({"NONE", 0});
         for (std::size_t i = 0; i < v.members.size(); ++i)
         {
            const Member& m = v.members[i];
            if (types.is_unit(*m.type))
            {
               if (i != 0)
                  fail("union '" + name + "': only the first member may have no value");
               continue;
            }
            if (const std::string* text = attribute(m.attributes, "value"))
            {
               const auto n = parse_unsigned(*text);
               if (!n || static_cast<std::int64_t>(*n) <= next)
                  fail("union '" + name + "': member values must ascend");
               next = static_cast<std::int64_t>(*n);
            }
            else
               ++next;
            if (next > 255)
               fail("union '" + name + "' has too many members");
            out.values.push_back({m.name, next});
         }
         return out;
      }
   }  // namespace fbs_layout_detail

   // The layout of every table, struct, enum and union of `schema`. Throws
   // std::runtime_error for a schema FlatBuffers cannot lay out (an unknown
   // type name, a type FlatBuffers does not have, a struct that contains
   // itself).
   inline fbs_layouts fbs_layout(const Schema& schema)
   {
      namespace d = fbs_layout_detail;
      d::model    types{schema};
      fbs_layouts out;
      for (const auto& entry : schema.types)
      {
         const AnyType& node = *entry.type;
         if (std::holds_alternative<Object>(node.value) ||
             std::holds_alternative<Struct>(node.value))
         {
            const auto*     o       = std::get_if<Object>(&node.value);
            const auto&     members = o ? o->members : std::get<Struct>(node.value).members;
            const auto&     attrs   = o ? o->attributes : std::get<Struct>(node.value).attributes;
            fbs_type_layout t;
            t.name = entry.name;
            if (types.shape_of(node).k == d::kind::structure)
               types.struct_fields(members, attrs, t);
            else
               d::table_fields(types, members, t);
            out.types.push_back(std::move(t));
         }
         else if (std::holds_alternative<Enum>(node.value) ||
                  std::holds_alternative<Variant>(node.value))
            out.enums.push_back(d::enum_layout(types, entry.name, node));
      }
      if (const std::string* root = d::attribute(schema.package.attributes, "fbs_root_type"))
         out.root_type = *root;
      if (const std::string* id = d::attribute(schema.package.attributes, "fbs_file_identifier"))
         out.file_identifier = *id;
      return out;
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::fbs_enum_layout;
   using schema_types::fbs_layout;
   using schema_types::fbs_layouts;
}
