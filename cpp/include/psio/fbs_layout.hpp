#pragma once
//
// psio/fbs_layout.hpp — the FlatBuffers wire layout of a Schema IR.
//
// `psio::fbs_layout(schema)` computes, for every table and struct of a
// schema (as psio/fbs_parser.hpp produces from `.fbs` text, or the schema
// builder from reflected types), what `flatc` computes for the same
// declarations and what psio/flatbuf.hpp uses for the same C++ types:
//
//   table    the field id (vtable slot) of every member — declaration order,
//            or the `(id: N)` attributes; a union takes two consecutive
//            slots (its type byte, then its value), and with explicit ids
//            the union's id names the value and the type is at id - 1; a
//            `deprecated` member keeps its slot;
//   struct   the byte offset of every member — declaration order, each at
//            its natural alignment — and the struct's size and alignment.
//
// This is what a reader needs to walk a buffer from a schema alone (the
// dynamic view of stage F4), and what tests/flatbuf_tests.cpp and
// tests/flatbuf_interop_tests.cpp compare with the codec's compile-time
// layout and with the constants `flatc` generates.

#include <psio/schema_ir.hpp>

#include <cstdint>
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
      // Structs: the member's byte offset.
      std::uint32_t offset = 0;
      // The bytes the member occupies inline (4 for an offset) and their
      // alignment.
      std::uint32_t size  = 0;
      std::uint32_t align = 0;
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

   struct fbs_layouts
   {
      std::vector<fbs_type_layout> types;

      const fbs_type_layout* find(std::string_view name) const noexcept
      {
         for (const auto& t : types)
            if (t.name == name)
               return &t;
         return nullptr;
      }
   };

   namespace fbs_layout_detail
   {
      struct inline_shape
      {
         std::uint32_t size     = 4;
         std::uint32_t align    = 4;
         bool          is_union = false;
         bool          fixed    = false;  // may be a struct member
      };

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

      inline std::uint32_t enum_width(const std::vector<Attribute>& attrs)
      {
         // fbs_parser records the underlying type of an enum unless it is
         // `ubyte`, the one emit_fbs writes.
         const std::string* u = attribute(attrs, "fbs_underlying");
         if (!u)
            return 1;
         if (*u == "byte" || *u == "ubyte" || *u == "bool" || *u == "int8" || *u == "uint8")
            return 1;
         if (*u == "short" || *u == "ushort" || *u == "int16" || *u == "uint16")
            return 2;
         if (*u == "long" || *u == "ulong" || *u == "int64" || *u == "uint64")
            return 8;
         return 4;
      }

      inline inline_shape struct_shape(const Schema& schema, const Struct& s, unsigned depth,
                                       fbs_type_layout* out);

      inline inline_shape shape_of(const Schema& schema, const AnyType& type, unsigned depth)
      {
         if (depth > 32)
            throw std::runtime_error("fbs_layout: types nested too deeply (a struct cycle?)");
         if (const auto* i = std::get_if<Int>(&type.value))
         {
            const std::uint32_t n = i->bits <= 8 ? 1 : i->bits / 8;
            return {n, n, false, true};
         }
         if (const auto* f = std::get_if<Float>(&type.value))
         {
            const std::uint32_t n = f->exp == 8 ? 4 : 8;
            return {n, n, false, true};
         }
         if (const auto* c = std::get_if<Custom>(&type.value))
         {
            if (c->id == "bool")
               return {1, 1, false, true};
            if (c->id == "string" || c->id == "hex")
               return {4, 4, false, false};
            return shape_of(schema, *c->type, depth + 1);
         }
         if (const auto* e = std::get_if<Enum>(&type.value))
            return shape_of(schema, *e->type, depth + 1);
         if (const auto* o = std::get_if<Option>(&type.value))
            return shape_of(schema, *o->type, depth + 1);
         if (const auto* a = std::get_if<Array>(&type.value))
         {
            const inline_shape e = shape_of(schema, *a->type, depth + 1);
            if (!e.fixed)
               throw std::runtime_error("fbs_layout: a fixed array of a type that is not fixed");
            return {static_cast<std::uint32_t>(e.size * a->len), e.align, false, true};
         }
         if (const auto* s = std::get_if<Struct>(&type.value))
            return struct_shape(schema, *s, depth + 1, nullptr);
         if (const auto* v = std::get_if<Variant>(&type.value))
         {
            bool is_enum = true;
            for (const auto& m : v->members)
               if (const auto* t = std::get_if<Tuple>(&m.type->value); !t || !t->members.empty())
                  is_enum = false;
            if (is_enum)
            {
               const std::uint32_t n = enum_width(v->attributes);
               return {n, n, false, true};
            }
            return {4, 4, true, false};
         }
         if (const auto* t = std::get_if<Type>(&type.value))
         {
            const AnyType* target = schema.get(t->type);
            if (!target)
               throw std::runtime_error("fbs_layout: unknown type '" + t->type + "'");
            return shape_of(schema, *target, depth + 1);
         }
         // Tables, vectors, everything else held behind an offset.
         return {4, 4, false, false};
      }

      inline inline_shape struct_shape(const Schema& schema, const Struct& s, unsigned depth,
                                       fbs_type_layout* out)
      {
         // FlatBuffers structs hold fixed members only; an IR Struct with
         // anything else is emitted as a table (emit_fbs.hpp).
         std::uint32_t pos = 0, align = 1;
         bool          fixed = !s.members.empty();
         for (const auto& m : s.members)
         {
            const inline_shape e = shape_of(schema, *m.type, depth + 1);
            if (!e.fixed)
            {
               fixed = false;
               break;
            }
            pos = (pos + e.align - 1) & ~(e.align - 1);
            if (out)
            {
               fbs_field_layout f;
               f.name   = m.name;
               f.offset = pos;
               f.size   = e.size;
               f.align  = e.align;
               out->fields.push_back(std::move(f));
            }
            pos += e.size;
            if (e.align > align)
               align = e.align;
         }
         if (!fixed)
         {
            if (out)
               out->fields.clear();
            return {4, 4, false, false};
         }
         const std::uint32_t size = (pos + align - 1) & ~(align - 1);
         if (out)
         {
            out->is_struct = true;
            out->size      = size;
            out->align     = align;
         }
         return {size, align, false, true};
      }

      inline void table_layout(const Schema& schema, const std::vector<Member>& members,
                               fbs_type_layout& out)
      {
         std::size_t with_id = 0;
         for (const auto& m : members)
         {
            bool present = false;
            attribute(m.attributes, "fbs_id", &present);
            with_id += present;
         }
         if (with_id != 0 && with_id != members.size())
            throw std::runtime_error("fbs_layout: table '" + out.name +
                                     "': either every field has an id or none has");
         std::uint32_t next = 0, slots = 0;
         for (const auto& m : members)
         {
            const inline_shape e = shape_of(schema, *m.type, 0);
            fbs_field_layout   f;
            f.name     = m.name;
            f.is_union = e.is_union;
            f.size     = e.is_union ? 1 : e.size;
            f.align    = e.is_union ? 1 : e.align;
            bool flag  = false;
            attribute(m.attributes, "deprecated", &flag);
            f.deprecated = flag;
            std::uint32_t slot;
            if (with_id)
            {
               const std::string* id = attribute(m.attributes, "fbs_id");
               if (!id)
                  throw std::runtime_error("fbs_layout: an id attribute without a value");
               const std::uint32_t n = static_cast<std::uint32_t>(std::stoul(*id));
               // A union's id is its value field; the type byte precedes it.
               if (e.is_union && n == 0)
                  throw std::runtime_error("fbs_layout: a union's id must leave room for its type");
               slot = e.is_union ? n - 1 : n;
            }
            else
            {
               slot = next;
               next += e.is_union ? 2 : 1;
            }
            const std::uint32_t end = slot + (e.is_union ? 2 : 1);
            if (end > slots)
               slots = end;
            if (end > 0x7ffd)
               throw std::runtime_error("fbs_layout: field id out of range");
            f.slot          = static_cast<std::uint16_t>(slot);
            f.vtable_offset = static_cast<std::uint16_t>(4 + 2 * slot);
            out.fields.push_back(std::move(f));
         }
         out.slots = static_cast<std::uint16_t>(slots);
      }
   }  // namespace fbs_layout_detail

   // The layout of every table and struct of `schema`. Throws
   // std::runtime_error for a schema FlatBuffers cannot lay out (an unknown
   // type name, ids on some fields only, a struct that contains itself).
   inline fbs_layouts fbs_layout(const Schema& schema)
   {
      fbs_layouts out;
      for (const auto& entry : schema.types)
      {
         fbs_type_layout t;
         t.name = entry.name;
         if (const auto* o = std::get_if<Object>(&entry.type->value))
            fbs_layout_detail::table_layout(schema, o->members, t);
         else if (const auto* s = std::get_if<Struct>(&entry.type->value))
         {
            const auto shape = fbs_layout_detail::struct_shape(schema, *s, 0, &t);
            if (!shape.fixed)  // emitted as a table
               fbs_layout_detail::table_layout(schema, s->members, t);
         }
         else
            continue;
         out.types.push_back(std::move(t));
      }
      return out;
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::fbs_layout;
   using schema_types::fbs_layouts;
}
