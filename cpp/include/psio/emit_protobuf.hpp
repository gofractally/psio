#pragma once
//
// psio/emit_protobuf.hpp — Schema IR → Protocol Buffers `.proto` text.
//
// Walks a `psio::Schema` and produces `.proto3` schema source.  The
// inverse (`.proto` → Schema) lives in psio/protobuf_parser.hpp (TBD).
//
// IR ↔ proto3 mapping:
//
//   Object              → message  (extensible record)
//   Struct              → message  (proto3 has no fixed-size record;
//                                   structs degrade to messages, with
//                                   a `// @psio:struct` comment so the
//                                   round-trip parser can recover it)
//   Variant of empty cases  → enum
//   Variant with payload(s) → message wrapping a `oneof` (proto3 has
//                                   no top-level oneof)
//   Resource            → message with `// @psio:resource` comment;
//                                   methods aren't proto3-emittable
//                                   and are omitted
//   Int{N,signed}       → int32/int64/uint32/uint64 by default;
//                                   sint32/sint64 when the field has
//                                   `pb_sint` in its attributes;
//                                   sfixed32/fixed32/sfixed64/fixed64
//                                   when `pb_fixed`.  (8/16-bit Int
//                                   widens to 32-bit on the wire — the
//                                   smallest sizes proto3 supports.)
//   Float{8,23}         → float
//   Float{11,52}        → double
//   Custom{u8, "bool"}  → bool
//   Custom{List<u8>, "string"} → string
//   Custom{List<u8>, "hex"}    → bytes
//   List<u8>            → bytes  (well-known proto3 alias)
//   List<T>             → `repeated T`
//   Option<T>           → `optional T`  (proto3 syntax, since v3.15)
//   Tuple{}             → omitted with `// @psio:unit` (no proto3
//                                   void/unit type)
//   Type{name}          → bare identifier
//
// Field numbers come from the canonical `field` attribute on each
// Member (the same channel `attr(member, field<N>)` writes through);
// when absent, sequential 1..N matches proto3's positional default.
//
// Per-field encoding hints carried as attribute names:
//   `pb_fixed`  — emit fixed/sfixed instead of varint
//   `pb_sint`   — emit zigzag (sint32/sint64) for signed integers
//
// Output ends with a trailing newline and a `syntax = "proto3";`
// header.  Package is derived from `Package.name` (proto3 uses dot-
// separated namespaces; `wasi:io` → `wasi.io`).

#include <psio/schema_ir.hpp>

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace psio::schema_types
{

   namespace emit_protobuf_detail
   {
      // proto3 conventionally PascalCase's message / enum names and
      // snake_case's fields.  Field names already come from C++
      // identifiers (snake_case-friendly); leave them alone.
      inline std::string to_pascal(std::string_view name)
      {
         std::string out;
         out.reserve(name.size());
         bool boundary = true;
         for (char c : name)
         {
            if (c == '_' || c == '-')
            {
               boundary = true;
               continue;
            }
            if (boundary)
            {
               out += static_cast<char>(
                  std::toupper(static_cast<unsigned char>(c)));
               boundary = false;
            }
            else
               out += c;
         }
         return out;
      }

      inline std::string proto_package(std::string_view pkg)
      {
         std::string out;
         out.reserve(pkg.size());
         for (char c : pkg)
            out += (c == ':' ? '.' : c);
         return out;
      }

      inline bool has_attr(const std::vector<Attribute>& attrs,
                           std::string_view              name)
      {
         for (const auto& a : attrs)
            if (a.name == name)
               return true;
         return false;
      }

      inline std::optional<std::string> attr_lookup(
         const std::vector<Attribute>& attrs, std::string_view name)
      {
         for (const auto& a : attrs)
            if (a.name == name && a.value)
               return *a.value;
         return std::nullopt;
      }

      inline bool variant_is_enum_shape(const Variant& v)
      {
         for (const auto& m : v.members)
         {
            auto* tup = std::get_if<Tuple>(&m.type->value);
            if (!(tup && tup->members.empty()))
               return false;
         }
         return !v.members.empty();
      }

      // Detect "List<u8>" — proto3's bytes alias.
      inline bool is_byte_list(const AnyType& t)
      {
         if (auto* lst = std::get_if<List>(&t.value))
            if (auto* inner = std::get_if<Int>(&lst->type->value))
               return inner->bits == 8 && !inner->isSigned;
         return false;
      }

      class Emitter
      {
       public:
         explicit Emitter(const Schema& s) : _s{s} {}

         std::string run()
         {
            _out << "syntax = \"proto3\";\n\n";
            if (!_s.package.name.empty())
               _out << "package " << proto_package(_s.package.name)
                    << ";\n\n";
            for (const auto& m : _s.types)
               emit_decl(m.name, *m.type);
            return _out.str();
         }

       private:
         const Schema&      _s;
         std::ostringstream _out;
         int                _indent = 0;

         void indent()
         {
            for (int i = 0; i < _indent; ++i)
               _out << "  ";
         }

         void emit_decl(const std::string& name, const AnyType& t)
         {
            std::visit(
               [&]<typename T>(const T& v) { emit_decl_impl(name, v); },
               t.value);
         }

         void emit_decl_impl(const std::string& name, const Object& o)
         {
            emit_type_attribute_comments(o.attributes);
            emit_message(name, o.members);
         }
         void emit_decl_impl(const std::string& name, const Struct& s)
         {
            emit_type_attribute_comments(s.attributes);
            emit_message(name, s.members,
                         "// @psio:struct  (proto3 has no fixed-size record)");
         }

         // Protobuf has no native type-level attribute syntax for psio
         // caps; emit them as preceding-line comments so `protoc`-style
         // tools can carry the metadata, even if it's not enforced
         // wire-side.
         void emit_type_attribute_comments(
            const std::vector<Attribute>& attrs)
         {
            for (const auto& a : attrs)
            {
               if (a.name == "definitionWillNotChange" ||
                   a.name == "maxFields" ||
                   a.name == "maxDynamicData")
               {
                  indent();
                  _out << "// @" << a.name;
                  if (!a.value.empty())
                     _out << "(" << a.value << ")";
                  _out << "\n";
               }
            }
         }
         void emit_decl_impl(const std::string& name, const Variant& v)
         {
            if (variant_is_enum_shape(v))
               emit_enum(name, v.members);
            else
               emit_oneof_message(name, v.members);
         }
         void emit_decl_impl(const std::string& name, const Resource& r)
         {
            indent();
            _out << "// @psio:resource " << to_pascal(r.name.empty()
                                                         ? name
                                                         : r.name)
                 << " (methods omitted — see WIT/capnp for the surface)\n";
            std::vector<Member> empty;
            emit_message(r.name.empty() ? name : r.name, empty);
         }
         template <typename T>
         void emit_decl_impl(const std::string& name, const T& t)
         {
            indent();
            _out << "// alias " << to_pascal(name) << " = "
                 << inline_any_default(t) << ";\n\n";
            (void)t;
         }

         void emit_message(const std::string&         name,
                           const std::vector<Member>& members,
                           std::string_view           leading = "")
         {
            indent();
            if (!leading.empty())
            {
               _out << leading << "\n";
               indent();
            }
            _out << "message " << to_pascal(name) << " {\n";
            ++_indent;
            std::uint32_t auto_id = 1;
            for (const auto& m : members)
            {
               indent();
               emit_member_line(m, auto_id);
               ++auto_id;
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_enum(const std::string&         name,
                        const std::vector<Member>& members)
         {
            indent();
            _out << "enum " << to_pascal(name) << " {\n";
            ++_indent;
            //  proto3 enums must have a zero value; the first listed
            //  member fills that slot.
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               indent();
               _out << members[i].name << " = " << i << ";\n";
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_oneof_message(const std::string&         name,
                                 const std::vector<Member>& members)
         {
            indent();
            _out << "message " << to_pascal(name) << " {\n";
            ++_indent;
            indent();
            _out << "oneof value {\n";
            ++_indent;
            std::uint32_t auto_id = 1;
            for (const auto& m : members)
            {
               indent();
               //  oneof bodies disallow `repeated` and `optional`,
               //  but the type-printer never emits those for variant
               //  cases since the oneof itself is the discriminant.
               auto* tup = std::get_if<Tuple>(&m.type->value);
               if (tup && tup->members.empty())
                  _out << "// @psio:bare-case " << m.name << " (no payload)\n";
               else
                  _out << inline_any(*m.type, m.attributes) << " " << m.name
                       << " = " << auto_id << ";\n";
               ++auto_id;
            }
            --_indent;
            indent();
            _out << "}\n";
            --_indent;
            indent();
            _out << "}\n\n";
         }

         //  Emit one `<type> <name> = <id>;` line, honouring repeated /
         //  optional / bytes / encoding-hint annotations.
         void emit_member_line(const Member& m, std::uint32_t auto_id)
         {
            std::uint32_t fid = auto_id;
            if (auto v = attr_lookup(m.attributes, "field"))
               fid = static_cast<std::uint32_t>(std::stoul(*v));

            const AnyType& t       = *m.type;
            bool           printed = false;

            //  Option<T> → `optional T`.
            if (auto* opt = std::get_if<Option>(&t.value))
            {
               _out << "optional " << inline_any(*opt->type, m.attributes);
               printed = true;
            }
            //  List / BoundedList of u8 → bytes (no `repeated`).
            else if (is_byte_list(t))
            {
               _out << "bytes";
               printed = true;
            }
            else if (auto* lst = std::get_if<List>(&t.value))
            {
               _out << "repeated " << inline_any(*lst->type, m.attributes);
               printed = true;
            }
            else if (auto* blst = std::get_if<BoundedList>(&t.value))
            {
               _out << "repeated " << inline_any(*blst->type, m.attributes);
               printed = true;
            }
            else if (auto* arr = std::get_if<Array>(&t.value))
            {
               _out << "repeated " << inline_any(*arr->type, m.attributes);
               printed = true;
            }

            if (!printed)
               _out << inline_any(t, m.attributes);

            _out << " " << m.name << " = " << fid << ";\n";
         }

         // ── Inline type forms (encoding-hint aware) ───────────────
         std::string inline_any(const AnyType&                t,
                                const std::vector<Attribute>& attrs)
         {
            return std::visit(
               [&]<typename U>(const U& v) {
                  return inline_type(v, attrs);
               },
               t.value);
         }

         //  Default-attribute overload for use from emit_decl_impl
         //  fallback (alias comment).
         template <typename U>
         std::string inline_any_default(const U& v)
         {
            std::vector<Attribute> none;
            return inline_type(v, none);
         }

         std::string inline_type(const Int&                    i,
                                 const std::vector<Attribute>& attrs)
         {
            //  proto3 has no narrower-than-32-bit types.  8/16-bit
            //  Int widens; signedness preserved.
            const std::uint32_t bits = i.bits <= 32 ? 32u : 64u;
            const bool          fix  = has_attr(attrs, "pb_fixed");
            const bool          zig  = has_attr(attrs, "pb_sint");

            if (fix)
            {
               if (bits == 32)
                  return i.isSigned ? "sfixed32" : "fixed32";
               return i.isSigned ? "sfixed64" : "fixed64";
            }
            if (zig && i.isSigned)
               return bits == 32 ? "sint32" : "sint64";
            if (i.isSigned)
               return bits == 32 ? "int32" : "int64";
            return bits == 32 ? "uint32" : "uint64";
         }
         std::string inline_type(const Float& f,
                                 const std::vector<Attribute>&)
         {
            if (f.exp == 8 && f.mantissa == 23)
               return "float";
            if (f.exp == 11 && f.mantissa == 52)
               return "double";
            return "/* TODO: non-IEEE float */";
         }
         std::string inline_type(const Type& t,
                                 const std::vector<Attribute>&)
         {
            if (t.type == "bool")
               return "bool";
            if (t.type == "string")
               return "string";
            if (t.type == "char")
               return "uint32  // @psio:char";
            if (!t.type.empty() && t.type.front() == '@')
            {
               if (auto* resolved = _s.get(t.type))
                  return inline_any(*resolved, {});
               return "/* TODO: unresolved " + t.type + " */";
            }
            return to_pascal(t.type);
         }
         std::string inline_type(const List& l,
                                 const std::vector<Attribute>& attrs)
         {
            //  Bare List<u8> already collapsed to `bytes` upstream;
            //  this path is only hit for nested lists, where proto3
            //  has no syntax — wrap in a comment.
            return "/* TODO: nested list of " +
                   inline_any(*l.type, attrs) + " */";
         }
         std::string inline_type(const BoundedList& l,
                                 const std::vector<Attribute>& attrs)
         {
            return "/* TODO: nested bounded-list of " +
                   inline_any(*l.type, attrs) + " */";
         }
         std::string inline_type(const Option& o,
                                 const std::vector<Attribute>& attrs)
         {
            //  Option as a value-position type (e.g. inside a list of
            //  optionals): proto3 can't express that directly.
            return "/* TODO: nested option of " +
                   inline_any(*o.type, attrs) + " */";
         }
         std::string inline_type(const Tuple& t,
                                 const std::vector<Attribute>&)
         {
            if (t.members.empty())
               return "/* @psio:unit */";
            return "/* TODO: tuple */";
         }
         std::string inline_type(const Array& a,
                                 const std::vector<Attribute>& attrs)
         {
            return "/* TODO: nested array of " +
                   inline_any(*a.type, attrs) + " */";
         }
         std::string inline_type(const Object&,
                                 const std::vector<Attribute>&)
         {
            return "/* TODO: anonymous record */";
         }
         std::string inline_type(const Struct&,
                                 const std::vector<Attribute>&)
         {
            return "/* TODO: anonymous struct */";
         }
         std::string inline_type(const Variant&,
                                 const std::vector<Attribute>&)
         {
            return "/* TODO: anonymous variant */";
         }
         std::string inline_type(const Resource& r,
                                 const std::vector<Attribute>&)
         {
            return to_pascal(r.name);
         }
         std::string inline_type(const FracPack&,
                                 const std::vector<Attribute>&)
         {
            return "/* TODO: fracpack wrapper */";
         }
         std::string inline_type(const Custom& c,
                                 const std::vector<Attribute>& attrs)
         {
            if (c.id == "bool")
               return "bool";
            if (c.id == "string")
               return "string";
            if (c.id == "char")
               return "uint32  // @psio:char";
            if (c.id == "hex")
            {
               if (auto* lst = std::get_if<List>(&c.type->value))
                  if (auto* inner = std::get_if<Int>(&lst->type->value);
                      inner && inner->bits == 8 && !inner->isSigned)
                     return "bytes";
            }
            return inline_any(*c.type, attrs);
         }
      };

   }  // namespace emit_protobuf_detail

   /// Emit the full `.proto3` schema text representation of `schema`.
   /// Output starts with `syntax = "proto3";` and ends with a trailing
   /// newline.
   inline std::string emit_protobuf(const Schema& schema)
   {
      emit_protobuf_detail::Emitter e{schema};
      return e.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_protobuf;
}
