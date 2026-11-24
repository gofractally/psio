#pragma once
//
// psio/capnp_layout.hpp — the Cap'n Proto wire layout of a Schema IR.
//
// `psio::capnp_layout(schema)` computes, for every struct of a schema — one
// the schema builder made from reflected C++ types, or one
// psio/capnp_parser.hpp read from `.capnp` text — what `capnp compile`
// computes for the corresponding declarations and what psio/capnp.hpp uses
// for the corresponding C++ types: the sizes of the data and pointer
// sections, where every field is, its ordinal and default, the position of
// every union's discriminant and the value of each of its members (their
// rank by ordinal).
//
// tests/capnp_schema_tests.cpp compares all of it with the
// CodeGeneratorRequest `capnp compile -o-` writes, on hand-written and on
// generated schemas, and with the codec's compile-time layout.
//
// ── How the Schema IR is read as Cap'n Proto ─────────────────────────────
//
// This is the one statement of the mapping: psio/emit_capnp.hpp writes the
// declarations it describes, psio/capnp_parser.hpp produces the IR it reads.
//
//   Object, Struct (a named type)   a struct
//   Int 8/16/32/64, Float, `bool`, `char`     the data types
//   Enum{Int, enumerators}          its integer (a Cap'n Proto `enum` is the
//                                   Enum over UInt16 with values 0, 1, ...)
//   `string`, `utf8`                Text
//   `hex`, Nested                   Data
//   List, BoundedList, Array        List(T)
//   a reference to a struct         a pointer to it
//   Resource                        an interface; a reference to one is a
//                                   capability pointer
//   Tuple{}                         Void
//   Custom{.., "AnyPointer"}        AnyPointer (`AnyStruct`, `AnyList`,
//                                   `Capability` likewise)
//   any other Custom                its inner type
//
// Members of a struct:
//
//   a member of one of the types above         a field
//   Option<a pointer type>          that field; null is "no value"
//   Option<a data type>             `name :union { none @n :Void; some @n+1 :T; }`
//   a Variant (written in the member or by name)
//                                   `name :union { ... }`, one member per
//                                   alternative: a field, or a group when
//                                   the alternative is an Object written in
//                                   place. The member named "" is the
//                                   struct's unnamed union.
//   an Object written in place      `name :group { ... }`
//
// A Variant that is a named type of its own is, in addition, the struct
// that holds nothing but that union. A named Variant whose alternatives all
// have no value is an `enum`.
//
// Attributes:
//
//   ordinal = N      on a member / an alternative: its `@N`. On a Variant:
//                    the ordinal of the union itself (when its discriminant
//                    was added). Either every field of a struct has one or
//                    none has; without them ordinals follow the members in
//                    ascending order of `field` (absent: the 1-based
//                    position; psio/detail/field_number.hpp), a union taking
//                    one per alternative — the codec's rule.
//   default = text   a data field's default (`true`, a decimal integer —
//                    an enumerator's value —, a decimal float, `nan`,
//                    `inf`, `-inf`); the wire holds value XOR default. For a
//                    pointer field, the value as Cap'n Proto writes it.
//   capnp_id         on a named type / the package: its 64-bit id (`0x...`)

#include <psio/detail/capnp_holes.hpp>
#include <psio/fbs_layout.hpp>
#include <psio/schema_ir.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace psio::schema_types
{

   enum class capnp_slot_kind : std::uint8_t
   {
      none,     // Void
      data,
      pointer,
      group     // a group or a named union: `group` holds its members
   };

   struct capnp_scope_layout;

   struct capnp_field_layout
   {
      std::string     name;
      capnp_slot_kind kind = capnp_slot_kind::none;
      std::uint8_t    lg   = 0;  // data: log2 of the field's size in bits
      // data: in units of the field's size; pointer: its index
      std::uint32_t                offset = 0;
      std::optional<std::uint16_t> ordinal;       // a group has none; a union may
      std::optional<std::uint16_t> discriminant;  // a member of its scope's union
      std::uint64_t                default_bits = 0;  // data: XORed with the value
      std::vector<capnp_scope_layout> group;      // kind == group: one scope

      std::uint64_t bit_offset() const noexcept { return std::uint64_t{offset} << lg; }
   };

   // The members of a struct or of a group, and the union among them.
   struct capnp_scope_layout
   {
      std::vector<capnp_field_layout> fields;
      std::uint16_t                   discriminant_count  = 0;
      std::uint32_t                   discriminant_offset = 0;  // in 16-bit units

      const capnp_field_layout* field(std::string_view field_name) const noexcept
      {
         for (const auto& f : fields)
            if (f.name == field_name)
               return &f;
         return nullptr;
      }
   };

   struct capnp_struct_layout : capnp_scope_layout
   {
      std::string   name;
      std::uint16_t data_words = 0;
      std::uint16_t pointers   = 0;
   };

   struct capnp_layouts
   {
      std::vector<capnp_struct_layout> structs;

      const capnp_struct_layout* find(std::string_view name) const noexcept
      {
         for (const auto& t : structs)
            if (t.name == name)
               return &t;
         return nullptr;
      }
   };

   namespace capnp_layout_detail
   {
      using fbs_layout_detail::attribute;
      using fbs_layout_detail::has_attribute;
      using fbs_layout_detail::parse_integer;
      using fbs_layout_detail::parse_real;
      using fbs_layout_detail::parse_unsigned;
      using hole_set = ::psio::detail::capnp_hole_set;

      [[noreturn]] inline void fail(const std::string& what)
      {
         throw std::runtime_error("capnp_layout: " + what);
      }

      // ── The official compiler's allocator (StructLayout of capnp's
      // node-translator), for any nesting of groups and unions. Fields are
      // placed in ordinal order; a data field takes the smallest hole that
      // fits or a new word; the members of a union share space, each
      // reusing (and if possible growing) the locations earlier members
      // claimed; the 16-bit discriminant is allocated when the union's
      // second member is, or at the union's own ordinal.

      struct scope
      {
         virtual ~scope()                                                           = default;
         virtual void          add_void()                                           = 0;
         virtual std::uint32_t add_data(std::uint32_t lg)                           = 0;
         virtual std::uint32_t add_pointer()                                        = 0;
         virtual bool try_expand_data(std::uint32_t old_lg, std::uint32_t old_offset,
                                      std::uint32_t factor)                         = 0;
      };

      struct top_scope final : scope
      {
         std::uint32_t data_words = 0;
         std::uint32_t pointers   = 0;
         hole_set      holes;

         void          add_void() override {}
         std::uint32_t add_data(std::uint32_t lg) override
         {
            if (auto hole = holes.try_allocate(lg))
               return *hole;
            if (data_words >= 0xffff)
               fail("a struct section holds at most 65535 words");
            const std::uint32_t offset = data_words++ << (6 - lg);
            holes.add_holes_at_end(lg, offset + 1);
            return offset;
         }
         std::uint32_t add_pointer() override
         {
            if (pointers >= 0xffff)
               fail("a struct section holds at most 65535 words");
            return pointers++;
         }
         bool try_expand_data(std::uint32_t old_lg, std::uint32_t old_offset,
                              std::uint32_t factor) override
         {
            return holes.try_expand(old_lg, old_offset, factor);
         }
      };

      struct union_scope
      {
         struct data_location
         {
            std::uint32_t lg;
            std::uint32_t offset;

            bool try_expand_to(union_scope& u, std::uint32_t new_lg)
            {
               if (new_lg <= lg)
                  return true;
               if (u.parent.try_expand_data(lg, offset, new_lg - lg))
               {
                  offset >>= (new_lg - lg);
                  lg = new_lg;
                  return true;
               }
               return false;
            }
         };

         scope&                       parent;
         std::uint32_t                groups = 0;
         std::optional<std::uint32_t> discriminant;  // offset in 16-bit units
         std::vector<data_location>   data;
         std::vector<std::uint32_t>   pointers;

         explicit union_scope(scope& p) : parent(p) {}

         std::uint32_t add_new_data_location(std::uint32_t lg)
         {
            const std::uint32_t offset = parent.add_data(lg);
            data.push_back({lg, offset});
            return offset;
         }
         std::uint32_t add_new_pointer_location()
         {
            pointers.push_back(parent.add_pointer());
            return pointers.back();
         }
         bool add_discriminant()
         {
            if (discriminant)
               return false;
            discriminant = parent.add_data(4);
            return true;
         }
         void new_group_adding_first_member()
         {
            if (++groups == 2)
               add_discriminant();
         }
      };

      // One member of a union: a field (the group the compiler makes for
      // it) or a group.
      struct group_scope final : scope
      {
         struct usage
         {
            bool          used    = false;
            std::uint32_t lg_used = 0;
            hole_set      holes;

            std::optional<std::uint32_t> smallest_hole_at_least(
               const union_scope::data_location& loc, std::uint32_t lg) const
            {
               if (!used)
                  return lg <= loc.lg ? std::optional<std::uint32_t>{loc.lg} : std::nullopt;
               if (lg >= lg_used)
                  return lg < loc.lg ? std::optional<std::uint32_t>{lg} : std::nullopt;
               if (auto r = holes.smallest_at_least(lg))
                  return r;
               return lg_used < loc.lg ? std::optional<std::uint32_t>{lg_used} : std::nullopt;
            }
            std::uint32_t allocate_from_hole(const union_scope::data_location& loc,
                                             std::uint32_t                     lg)
            {
               std::uint32_t result = 0;
               if (!used)
               {
                  used    = true;
                  lg_used = lg;
               }
               else if (lg >= lg_used)
               {
                  holes.add_holes_at_end(lg_used, 1, lg);
                  lg_used = lg + 1;
                  result  = 1;
               }
               else if (auto hole = holes.try_allocate(lg))
                  result = *hole;
               else
               {
                  result = std::uint32_t{1} << (lg_used - lg);
                  holes.add_holes_at_end(lg, result + 1, lg_used);
                  lg_used += 1;
               }
               return (loc.offset << (loc.lg - lg)) + result;
            }
            bool try_expand_usage(union_scope& u, union_scope::data_location& loc,
                                  std::uint32_t desired, bool new_holes)
            {
               if (desired > loc.lg && !loc.try_expand_to(u, desired))
                  return false;
               if (new_holes)
                  holes.add_holes_at_end(lg_used, 1, desired);
               lg_used = desired;
               return true;
            }
            std::optional<std::uint32_t> try_allocate_by_expanding(
               union_scope& u, union_scope::data_location& loc, std::uint32_t lg)
            {
               if (!used)
               {
                  if (!loc.try_expand_to(u, lg))
                     return std::nullopt;
                  used    = true;
                  lg_used = lg;
                  return loc.offset << (loc.lg - lg);
               }
               const std::uint32_t desired = std::max(lg_used, lg) + 1;
               if (!try_expand_usage(u, loc, desired, true))
                  return std::nullopt;
               const auto hole = holes.try_allocate(lg);
               return (loc.offset << (loc.lg - lg)) + *hole;
            }
            bool try_expand(union_scope& u, union_scope::data_location& loc, std::uint32_t old_lg,
                            std::uint32_t old_offset, std::uint32_t factor)
            {
               // Exactly the requested data: the whole location grows.
               if (old_offset == 0 && lg_used == old_lg)
                  return try_expand_usage(u, loc, old_lg + factor, false);
               // The data and other things: it can only take holes.
               return holes.try_expand(old_lg, old_offset, factor);
            }
         };

         union_scope&       parent;
         std::vector<usage> data_usage;
         std::uint32_t      pointer_usage = 0;
         bool               has_members   = false;

         explicit group_scope(union_scope& u) : parent(u) {}

         void add_member()
         {
            if (!has_members)
            {
               has_members = true;
               parent.new_group_adding_first_member();
            }
         }
         void add_void() override
         {
            add_member();
            // A union inside a union: the outer one learns of the member
            // although it takes no space.
            parent.parent.add_void();
         }
         std::uint32_t add_data(std::uint32_t lg) override
         {
            add_member();
            std::uint32_t              best_size = ~std::uint32_t{0};
            std::optional<std::size_t> best;
            for (std::size_t i = 0; i < parent.data.size(); ++i)
            {
               if (data_usage.size() == i)
                  data_usage.emplace_back();
               if (auto hole = data_usage[i].smallest_hole_at_least(parent.data[i], lg))
                  if (*hole < best_size)
                  {
                     best_size = *hole;
                     best      = i;
                  }
            }
            if (best)
               return data_usage[*best].allocate_from_hole(parent.data[*best], lg);
            for (std::size_t i = 0; i < parent.data.size(); ++i)
               if (auto r = data_usage[i].try_allocate_by_expanding(parent, parent.data[i], lg))
                  return *r;
            const std::uint32_t offset = parent.add_new_data_location(lg);
            usage               u;
            u.used    = true;
            u.lg_used = lg;
            data_usage.push_back(u);
            return offset;
         }
         std::uint32_t add_pointer() override
         {
            add_member();
            if (pointer_usage < parent.pointers.size())
               return parent.pointers[pointer_usage++];
            ++pointer_usage;
            return parent.add_new_pointer_location();
         }
         bool try_expand_data(std::uint32_t old_lg, std::uint32_t old_offset,
                              std::uint32_t factor) override
         {
            if (old_lg + factor > 6 || (old_offset & ((std::uint32_t{1} << factor) - 1)) != 0)
               return false;
            for (std::size_t i = 0; i < data_usage.size(); ++i)
            {
               auto& loc = parent.data[i];
               if (loc.lg >= old_lg && (old_offset >> (loc.lg - old_lg)) == loc.offset)
               {
                  const std::uint32_t local = old_offset - (loc.offset << (loc.lg - old_lg));
                  return data_usage[i].try_expand(parent, loc, old_lg, local, factor);
               }
            }
            return false;
         }
      };

      // ── The members of a struct, as the allocator sees them ────────────

      struct item
      {
         enum class what_t : std::uint8_t
         {
            field,
            group,
            union_  // a union: its children are its members
         };
         what_t                       what = what_t::field;
         std::string                  name;
         capnp_slot_kind              kind = capnp_slot_kind::none;  // field
         std::uint8_t                 lg   = 0;
         std::uint64_t                default_bits = 0;
         std::optional<std::uint64_t> ordinal;  // field; union: its own
         std::vector<item>            children;
         bool                         named = true;  // union: `name :union`

         // Filled by the allocation.
         std::uint32_t                offset = 0;
         scope*                       where  = nullptr;  // field: its scope
         union_scope*                 owner  = nullptr;  // union
         std::optional<std::uint16_t> discriminant;
      };

      struct data_shape
      {
         capnp_slot_kind kind = capnp_slot_kind::none;
         std::uint8_t    lg   = 0;
         bool            real = false;
      };

      class builder
      {
       public:
         explicit builder(const Schema& s) : _types{s} {}

         // Whether the schema entry `entry` is a struct of its own.
         static bool is_struct_entry(const Member& entry)
         {
            return std::holds_alternative<Object>(entry.type->value) ||
                   std::holds_alternative<Struct>(entry.type->value);
         }

         // A named variant whose alternatives all have no value: an enum.
         bool is_enum_variant(const AnyType& node, const std::string* name) const
         {
            const auto* v = std::get_if<Variant>(&node.value);
            return v && name && !name->starts_with('@') && _types.enum_shaped(*v);
         }

         // What a value of `written` takes in a struct: nothing, data, or a
         // pointer. Null for a union, a group, an optional of data.
         std::optional<data_shape> simple_shape(const AnyType& written, unsigned depth = 0) const
         {
            if (depth > 64)
               fail("types nested too deeply");
            const std::string* name = nullptr;
            const AnyType&     type = _types.resolve(written, &name);
            if (const auto* i = std::get_if<Int>(&type.value))
            {
               switch (i->bits)
               {
                  case 8: return data_shape{capnp_slot_kind::data, 3};
                  case 16: return data_shape{capnp_slot_kind::data, 4};
                  case 32: return data_shape{capnp_slot_kind::data, 5};
                  case 64: return data_shape{capnp_slot_kind::data, 6};
                  default:
                     fail("an integer of " + std::to_string(i->bits) +
                          " bits has no Cap'n Proto type");
               }
            }
            if (const auto* f = std::get_if<Float>(&type.value))
            {
               if (f->exp == 8)
                  return data_shape{capnp_slot_kind::data, 5, true};
               if (f->exp == 11)
                  return data_shape{capnp_slot_kind::data, 6, true};
               fail("a float that is not 32 or 64 bits has no Cap'n Proto type");
            }
            if (const auto* c = std::get_if<Custom>(&type.value))
            {
               if (c->id == "bool")
                  return data_shape{capnp_slot_kind::data, 0};
               if (c->id == "string" || c->id == "utf8" || c->id == "hex" ||
                   c->id == "AnyPointer" || c->id == "AnyStruct" || c->id == "AnyList" ||
                   c->id == "Capability")
                  return data_shape{capnp_slot_kind::pointer, 0};
               if (c->id == "map" || c->id == "set")
                  fail("a " + c->id + " has no Cap'n Proto type");
               return simple_shape(*c->type, depth + 1);
            }
            if (const auto* e = std::get_if<Enum>(&type.value))
            {
               const auto under = simple_shape(*e->type, depth + 1);
               if (!under || under->kind != capnp_slot_kind::data || under->real)
                  fail("an enum that is not over an integer");
               return under;
            }
            if (std::holds_alternative<List>(type.value) ||
                std::holds_alternative<BoundedList>(type.value) ||
                std::holds_alternative<Array>(type.value) ||
                std::holds_alternative<Nested>(type.value) ||
                std::holds_alternative<Resource>(type.value))
               return data_shape{capnp_slot_kind::pointer, 0};
            if (std::holds_alternative<Object>(type.value) ||
                std::holds_alternative<Struct>(type.value))
            {
               // By name: a pointer to the struct. Written in place: a group.
               if (std::holds_alternative<Type>(written.value))
                  return data_shape{capnp_slot_kind::pointer, 0};
               return std::nullopt;
            }
            if (const auto* t = std::get_if<Tuple>(&type.value))
            {
               if (!t->members.empty())
                  fail("a tuple has no Cap'n Proto type");
               return data_shape{capnp_slot_kind::none, 0};
            }
            if (std::holds_alternative<Variant>(type.value))
            {
               if (is_enum_variant(type, name))
                  return data_shape{capnp_slot_kind::data, 4};
               return std::nullopt;
            }
            if (const auto* o = std::get_if<Option>(&type.value))
            {
               const auto inner = simple_shape(*o->type, depth + 1);
               if (inner && inner->kind == capnp_slot_kind::pointer &&
                   !std::holds_alternative<Option>(_types.resolve(*o->type).value))
                  return inner;
               return std::nullopt;
            }
            fail("a type without a Cap'n Proto counterpart");
         }

         // The default of a data field as the bits XORed with its value.
         static std::uint64_t default_bits(const Member& m, const data_shape& shape)
         {
            const std::string* text = attribute(m.attributes, "default");
            if (!text || shape.kind != capnp_slot_kind::data)
               return 0;
            if (shape.real)
            {
               const auto v = parse_real(*text);
               if (!v)
                  fail("member '" + m.name + "': '" + *text + "' is not a float");
               if (shape.lg == 5)
                  return std::bit_cast<std::uint32_t>(static_cast<float>(*v));
               return std::bit_cast<std::uint64_t>(*v);
            }
            std::uint64_t bits = 0;
            if (*text == "true" || *text == "false")
               bits = *text == "true";
            else if (const auto v = parse_integer(*text))
               bits = static_cast<std::uint64_t>(*v);
            else
               fail("member '" + m.name + "': '" + *text + "' is not an integer");
            return shape.lg == 6 ? bits : bits & ((std::uint64_t{1} << (1u << shape.lg)) - 1);
         }

         static std::optional<std::uint64_t> ordinal_of(const std::vector<Attribute>& attrs,
                                                        const std::string&            name)
         {
            bool               present = false;
            const std::string* text    = attribute(attrs, "ordinal", &present);
            if (!present)
               return std::nullopt;
            const auto n = text ? parse_unsigned(*text) : std::nullopt;
            if (!n || *n > 0xffff)
               fail("'" + name + "': an ordinal is a number below 65536");
            return n;
         }

         // The members of a record as items, in declaration order.
         std::vector<item> items_of(const std::vector<Member>& members, unsigned depth) const
         {
            if (depth > 32)
               fail("groups nested too deeply");
            std::vector<item> out;
            out.reserve(members.size());
            for (const auto& m : members)
               out.push_back(item_of(m, depth, false));
            return out;
         }

         item item_of(const Member& m, unsigned depth, bool in_union) const
         {
            item it;
            it.name = m.name;
            if (const auto shape = simple_shape(*m.type))
            {
               it.kind         = shape->kind;
               it.lg           = shape->lg;
               it.default_bits = default_bits(m, *shape);
               it.ordinal      = ordinal_of(m.attributes, m.name);
               return it;
            }
            const std::string* name = nullptr;
            const AnyType&     type = _types.resolve(*m.type, &name);
            if (const auto* o = std::get_if<Option>(&type.value))
            {
               // Option<data>: union { none :Void; some :T; }
               const auto inner = simple_shape(*o->type);
               if (in_union || !inner || inner->kind != capnp_slot_kind::data)
                  fail("member '" + m.name + "': this optional has no Cap'n Proto type");
               it.what = item::what_t::union_;
               item none, some;
               none.name = "none";
               some.name = "some";
               some.kind = inner->kind;
               some.lg   = inner->lg;
               // (explicit ordinals: `ordinal` is the first of the two)
               if (const auto n = ordinal_of(m.attributes, m.name))
               {
                  none.ordinal = *n;
                  some.ordinal = *n + 1;
               }
               it.children.push_back(std::move(none));
               it.children.push_back(std::move(some));
               return it;
            }
            if (const auto* v = std::get_if<Variant>(&type.value))
            {
               if (in_union)
                  fail("member '" + m.name + "': a union cannot hold a union");
               if (v->members.size() < 2)
                  fail("member '" + m.name + "': a Cap'n Proto union has at least two members");
               it.what    = item::what_t::union_;
               it.named   = !m.name.empty();
               it.ordinal = ordinal_of(v->attributes, m.name);
               for (const auto& alt : v->members)
                  it.children.push_back(item_of(alt, depth + 1, true));
               return it;
            }
            const auto* o = std::get_if<Object>(&type.value);
            const auto* s = std::get_if<Struct>(&type.value);
            if (!o && !s)
               fail("member '" + m.name + "' has no Cap'n Proto type");
            it.what     = item::what_t::group;
            it.children = items_of(o ? o->members : s->members, depth + 1);
            if (it.children.empty())
               fail("group '" + m.name + "' has no members");
            return it;
         }

         // ── Ordinals ────────────────────────────────────────────────────

         static void count_fields(const std::vector<item>& items, std::size_t& fields,
                                  std::size_t& with_ordinal)
         {
            for (const auto& it : items)
            {
               if (it.what == item::what_t::field)
               {
                  ++fields;
                  with_ordinal += it.ordinal.has_value();
               }
               else
                  count_fields(it.children, fields, with_ordinal);
            }
         }

         static void number(item& it, std::uint64_t& next)
         {
            if (it.what == item::what_t::field)
               it.ordinal = next++;
            else
               for (auto& child : it.children)
                  number(child, next);
         }

         // Without ordinals: ascending field number, a union one per member.
         static void assign_ordinals(std::vector<item>& items, const std::vector<Member>& members)
         {
            std::uint64_t next = 0;
            for (const std::size_t i : fbs_layout_detail::members_by_field_number(members))
               number(items[i], next);
         }

         // ── Allocation ──────────────────────────────────────────────────

         struct pending
         {
            std::uint64_t ordinal;
            item*         it;
            bool          discriminant;  // a union's own ordinal
         };

         static std::uint64_t lowest_ordinal(const item& it)
         {
            if (it.what == item::what_t::field)
               return *it.ordinal;
            std::uint64_t lowest = ~std::uint64_t{0};
            for (const auto& child : it.children)
               lowest = std::min(lowest, lowest_ordinal(child));
            return lowest;
         }

         void attach(std::vector<item>& items, scope& where, std::vector<pending>& order)
         {
            for (auto& it : items)
            {
               switch (it.what)
               {
                  case item::what_t::field:
                     it.where = &where;
                     order.push_back({*it.ordinal, &it, false});
                     break;
                  case item::what_t::group:
                     attach(it.children, where, order);
                     break;
                  case item::what_t::union_:
                  {
                     _unions.push_back(std::make_unique<union_scope>(where));
                     union_scope& u = *_unions.back();
                     it.owner       = &u;
                     if (it.ordinal)
                        order.push_back({*it.ordinal, &it, true});
                     // A member's value in the discriminant: its rank by the
                     // lowest ordinal it holds (the order the members were
                     // added to the schema over time).
                     std::vector<std::pair<std::uint64_t, std::size_t>> rank;
                     for (std::size_t i = 0; i < it.children.size(); ++i)
                        rank.emplace_back(lowest_ordinal(it.children[i]), i);
                     std::sort(rank.begin(), rank.end());
                     for (std::size_t i = 0; i < rank.size(); ++i)
                        it.children[rank[i].second].discriminant = static_cast<std::uint16_t>(i);
                     for (auto& member : it.children)
                     {
                        _groups.push_back(std::make_unique<group_scope>(u));
                        if (member.what == item::what_t::field)
                        {
                           member.where = _groups.back().get();
                           order.push_back({*member.ordinal, &member, false});
                        }
                        else
                           attach(member.children, *_groups.back(), order);
                     }
                     break;
                  }
               }
            }
         }

         static void emit(const std::vector<item>& items, capnp_scope_layout& out)
         {
            for (const auto& it : items)
            {
               if (it.what == item::what_t::union_ && !it.named)
               {
                  // The unnamed union: its members are the scope's.
                  out.discriminant_count  = static_cast<std::uint16_t>(it.children.size());
                  out.discriminant_offset = *it.owner->discriminant;
                  emit(it.children, out);
                  continue;
               }
               capnp_field_layout f;
               f.name         = it.name;
               f.discriminant = it.discriminant;
               if (it.what == item::what_t::field)
               {
                  f.kind         = it.kind;
                  f.lg           = it.lg;
                  f.offset       = it.offset;
                  f.ordinal      = static_cast<std::uint16_t>(*it.ordinal);
                  f.default_bits = it.default_bits;
               }
               else
               {
                  f.kind = capnp_slot_kind::group;
                  f.group.emplace_back();
                  if (it.what == item::what_t::union_)
                  {
                     // (a union's own ordinal is the ordinal of its member)
                     if (it.ordinal)
                        f.ordinal = static_cast<std::uint16_t>(*it.ordinal);
                     f.group[0].discriminant_count  = static_cast<std::uint16_t>(it.children.size());
                     f.group[0].discriminant_offset = *it.owner->discriminant;
                  }
                  emit(it.children, f.group[0]);
               }
               out.fields.push_back(std::move(f));
            }
         }

         static void collect_unions(std::vector<item>& items, std::vector<item*>& out)
         {
            for (auto& it : items)
            {
               if (it.what == item::what_t::union_)
                  out.push_back(&it);
               collect_unions(it.children, out);
            }
         }

         capnp_struct_layout layout(const std::string& name, const std::vector<Member>& members)
         {
            capnp_struct_layout out;
            out.name = name;
            std::vector<item> items = items_of(members, 0);

            std::size_t unnamed = 0;
            for (const auto& it : items)
               unnamed += it.what == item::what_t::union_ && !it.named;
            if (unnamed > 1)
               fail("struct '" + name + "' has more than one unnamed union");

            std::size_t fields = 0, with_ordinal = 0;
            count_fields(items, fields, with_ordinal);
            if (fields > 0x10000)
               fail("struct '" + name + "' has too many fields");
            if (with_ordinal == 0)
               assign_ordinals(items, members);
            else if (with_ordinal != fields)
               fail("struct '" + name + "': either every field has an ordinal or none has");

            _unions.clear();
            _groups.clear();
            top_scope            top;
            std::vector<pending> order;
            order.reserve(fields);
            attach(items, top, order);
            std::stable_sort(order.begin(), order.end(),
                             [](const pending& a, const pending& b) { return a.ordinal < b.ordinal; });
            for (std::size_t i = 0; i < order.size(); ++i)
               if (order[i].ordinal != i)
                  fail("struct '" + name + "': ordinals must be 0, 1, 2, ... without a gap (@" +
                       std::to_string(i) + " is " +
                       (i > 0 && order[i].ordinal == order[i - 1].ordinal ? "used twice" : "missing") +
                       ")");
            for (const pending& p : order)
            {
               item& it = *p.it;
               if (p.discriminant)
               {
                  if (!it.owner->add_discriminant())
                     fail("struct '" + name + "': union '" + it.name +
                          "' has its ordinal after its second member");
               }
               else if (it.kind == capnp_slot_kind::none)
                  it.where->add_void();
               else if (it.kind == capnp_slot_kind::data)
                  it.offset = it.where->add_data(it.lg);
               else
                  it.offset = it.where->add_pointer();
            }
            // (every union has had two members: its discriminant exists)
            std::vector<item*> unions;
            collect_unions(items, unions);
            for (item* u : unions)
               u->owner->add_discriminant();

            emit(items, out);
            out.data_words = static_cast<std::uint16_t>(top.data_words);
            out.pointers   = static_cast<std::uint16_t>(top.pointers);
            _unions.clear();
            _groups.clear();
            return out;
         }

         fbs_layout_detail::model& types() const noexcept { return _types; }

       private:
         mutable fbs_layout_detail::model          _types;
         std::vector<std::unique_ptr<union_scope>> _unions;
         std::vector<std::unique_ptr<group_scope>> _groups;
      };
   }  // namespace capnp_layout_detail

   // The layout of every struct of `schema` (a named Object or Struct; a
   // named Variant, as the struct that holds that union). Throws
   // std::runtime_error for a schema Cap'n Proto cannot lay out.
   inline capnp_layouts capnp_layout(const Schema& schema)
   {
      capnp_layout_detail::builder b{schema};
      capnp_layouts                out;
      for (const auto& entry : schema.types)
      {
         const AnyType& node = *entry.type;
         if (const auto* o = std::get_if<Object>(&node.value))
            out.structs.push_back(b.layout(entry.name, o->members));
         else if (const auto* s = std::get_if<Struct>(&node.value))
            out.structs.push_back(b.layout(entry.name, s->members));
         else if (const auto* v = std::get_if<Variant>(&node.value);
                  v && !entry.name.starts_with('@') && !b.types().enum_shaped(*v))
         {
            // The struct that holds nothing but the union.
            std::vector<Member> members;
            members.push_back(Member{"", Box<AnyType>{AnyType{Type{entry.name}}}, {}});
            out.structs.push_back(b.layout(entry.name, members));
         }
      }
      return out;
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::capnp_layout;
   using schema_types::capnp_layouts;
   using schema_types::capnp_slot_kind;
}
