#pragma once
//
// psio/emit_fbs.hpp — Schema IR → FlatBuffers `.fbs` schema text.
//
// Walks a `psio::Schema` and produces FlatBuffers schema source.
// The inverse (`.fbs` → Schema) lives in psio/fbs_parser.hpp.
//
// IR ↔ fbs mapping:
//
//   Object              → table   (extensible record; the FBS default)
//   Struct              → struct  (fixed-size, scalar-only — fbs's
//                                  hard restriction; if the IR
//                                  Struct contains non-fixed members
//                                  we degrade to `table` with a
//                                  carrier comment)
//   Variant             → union <Name> { … }  (declared at top level
//                                  alongside a wrapping table whose
//                                  field carries the discriminator;
//                                  bare cases use the same trick as
//                                  capnp — Tuple{} payload renders
//                                  as a null-table reference)
//   Variant of empty cases (≈ enum)
//                       → enum   (bytes-typed, sequential values)
//   Resource            → table  (fbs has no native resource concept;
//                                  surface as a table with a comment
//                                  marker for psio-aware readers)
//   Int{N, signed}      → byte/short/int/long       or
//                         ubyte/ushort/uint/ulong
//   Float{8, 23}        → float
//   Float{11, 52}       → double
//   Custom{u8, "bool"}  → bool
//   Custom{List<u8>, "string"} → string
//   Custom{List<u8>, "hex"}    → [ubyte]
//   List<T>             → [T]
//   Option<T>           → field with default + comment marker
//   Type{name}          → bare identifier
//
// FlatBuffers field ids (`name:type (id: 3)` form) round-trip via
// the same `Member.attributes["fbs_id"]` carrier the IR uses for
// every format-specific ordinal annotation.  When absent the
// emitter assigns sequential ids matching FBS's default.
//
// Namespace: emitted from `Package.name` (FBS uses dot-separated
// namespaces; `wasi:io` → `wasi.io` for FBS compatibility).
//
// Top-level `root_type X;` is emitted from the first interface's
// first export type, when present.

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

   namespace emit_fbs_detail
   {
      // FBS allows snake_case directly — keep IR names as-is for
      // fields, but typed entries (table/struct/enum/union names)
      // are conventionally PascalCase upstream; we emit Pascal.
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
            {
               out += c;
            }
         }
         return out;
      }

      inline std::string fbs_namespace(std::string_view pkg)
      {
         std::string out;
         out.reserve(pkg.size());
         for (char c : pkg)
            out += (c == ':' ? '.' : c);
         return out;
      }

      inline std::optional<std::string> attr_lookup(
         const std::vector<Attribute>& attrs, std::string_view name)
      {
         for (const auto& a : attrs)
            if (a.name == name)
               return a.value;
         return std::nullopt;
      }

      // Detect "enum-like" Variant: every alternative is Tuple{}.
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

      class Emitter
      {
       public:
         explicit Emitter(const Schema& s) : _s{s} {}

         std::string run()
         {
            emit_namespace();
            // Collect all union/enum decls first so they're available
            // before the tables that reference them.
            for (const auto& m : _s.types)
               emit_decl(m.name, *m.type);
            for (const auto& iface : _s.interfaces)
               emit_interface_as_table(iface);
            emit_root_type();
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

         void emit_namespace()
         {
            if (_s.package.name.empty())
               return;
            _out << "namespace " << fbs_namespace(_s.package.name) << ";\n\n";
         }

         void emit_root_type()
         {
            for (const auto& iface : _s.interfaces)
               for (const auto& tn : iface.type_names)
               {
                  _out << "root_type " << to_pascal(tn) << ";\n";
                  return;
               }
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
            emit_table(name, o.members);
         }
         void emit_decl_impl(const std::string& name, const Struct& s)
         {
            emit_type_attribute_comments(s.attributes);
            // FBS struct must be all-scalar fixed; otherwise degrade
            // to table with a comment marker.
            bool all_scalar = true;
            for (const auto& m : s.members)
               if (!is_fbs_struct_field(*m.type))
               {
                  all_scalar = false;
                  break;
               }
            if (all_scalar)
               emit_struct(name, s.members);
            else
               emit_table(name, s.members,
                          "// @psio:struct  (degraded to table — "
                          "non-scalar member)");
         }

         // FBS schemas have no native attribute syntax for psio caps;
         // emit them as preceding-line comments per the same convention
         // used elsewhere in this header for psio-specific metadata.
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
               emit_union_pair(name, v.members);
         }
         void emit_decl_impl(const std::string& name, const Resource& r)
         {
            // No native resource form — surface as a table with a
            // marker; methods aren't FBS-emittable, drop them with a
            // comment so a psio reader knows to look elsewhere.
            indent();
            _out << "// @psio:resource " << to_pascal(r.name.empty() ? name
                                                                    : r.name)
                 << " (methods omitted — see WIT/capnp for the full surface)\n";
            std::vector<Member> empty;
            emit_table(r.name.empty() ? name : r.name, empty);
         }
         template <typename T>
         void emit_decl_impl(const std::string& name, const T& t)
         {
            // Top-level alias for primitives etc. — FBS has no
            // typedef syntax; leave a comment so round-trip readers
            // can reconstruct.
            indent();
            _out << "// alias " << to_pascal(name) << " = "
                 << inline_type(t) << ";\n";
            (void)t;
         }

         bool is_fbs_struct_field(const AnyType& a) const
         {
            return std::visit(
               []<typename T>(const T& v) {
                  if constexpr (std::is_same_v<T, Int> ||
                                std::is_same_v<T, Float>)
                     return true;
                  else if constexpr (std::is_same_v<T, Type>)
                     // Conservatively allow named refs; the consumer
                     // will catch invalid struct fields at the FBS
                     // compiler stage.
                     return true;
                  else
                     return false;
                  (void)v;
               },
               a.value);
         }

         void emit_table(const std::string&         name,
                         const std::vector<Member>& members,
                         std::string_view           leading_comment = "")
         {
            indent();
            if (!leading_comment.empty())
            {
               _out << leading_comment << "\n";
               indent();
            }
            _out << "table " << to_pascal(name) << " {\n";
            ++_indent;
            std::uint32_t auto_id = 0;
            for (const auto& m : members)
            {
               indent();
               _out << m.name << ":" << inline_any(*m.type);
               if (auto fid = attr_lookup(m.attributes, "fbs_id"))
                  _out << " (id: " << *fid << ")";
               (void)auto_id;
               _out << ";\n";
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_struct(const std::string&         name,
                          const std::vector<Member>& members)
         {
            indent();
            _out << "struct " << to_pascal(name) << " {\n";
            ++_indent;
            for (const auto& m : members)
            {
               indent();
               _out << m.name << ":" << inline_any(*m.type) << ";\n";
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_enum(const std::string&         name,
                        const std::vector<Member>& members)
         {
            indent();
            _out << "enum " << to_pascal(name) << " : ubyte {\n";
            ++_indent;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               indent();
               _out << members[i].name;
               if (i + 1 < members.size())
                  _out << ",";
               _out << "\n";
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         // Variant-with-payload: emit a union over Pascal-cased case
         // names plus a wrapping table "Name" with a single
         // discriminated `value:Name_Union` field, mirroring v1's
         // FBS convention for tagged unions.
         void emit_union_pair(const std::string&         name,
                              const std::vector<Member>& members)
         {
            indent();
            _out << "union " << to_pascal(name) << "Union {\n";
            ++_indent;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               indent();
               // Bare cases need a synthetic empty-table reference;
               // emit one ahead of the union if so.  For now use the
               // case name itself as the table reference and emit a
               // tiny empty table afterwards if it's a Tuple{}.
               _out << to_pascal(members[i].name);
               if (i + 1 < members.size())
                  _out << ",";
               _out << "\n";
            }
            --_indent;
            indent();
            _out << "}\n\n";

            // Emit the empty tables for Tuple{} cases; for typed
            // cases with a non-Object payload, wrap in a small
            // single-field table.
            for (const auto& m : members)
            {
               auto* tup = std::get_if<Tuple>(&m.type->value);
               if (tup && tup->members.empty())
               {
                  indent();
                  _out << "table " << to_pascal(m.name) << " {}\n\n";
               }
               else if (auto* ty = std::get_if<Type>(&m.type->value))
               {
                  // Already-named type; assume it's an Object/table
                  // declared elsewhere.  No wrapper needed if the
                  // case name matches the type name; otherwise emit
                  // a forwarder.
                  if (to_pascal(m.name) != to_pascal(ty->type))
                  {
                     indent();
                     _out << "table " << to_pascal(m.name) << " { value:"
                          << inline_any(*m.type) << "; }\n\n";
                  }
                  (void)ty;
               }
               else
               {
                  // Primitive / list / etc. — wrap in a single-field
                  // table since FBS unions only hold tables.
                  indent();
                  _out << "table " << to_pascal(m.name) << " { value:"
                       << inline_any(*m.type) << "; }\n\n";
               }
            }

            // Wrapping table that hosts the union field.
            indent();
            _out << "table " << to_pascal(name) << " {\n";
            ++_indent;
            indent();
            _out << "value:" << to_pascal(name) << "Union;\n";
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_interface_as_table(const Interface& iface)
         {
            indent();
            _out << "// @psio:interface " << to_pascal(iface.name)
                 << " (methods omitted — see WIT/capnp for the full surface)\n";
            std::vector<Member> empty;
            emit_table(iface.name, empty);
            (void)iface;
         }

         // ── Inline type forms ─────────────────────────────────────
         std::string inline_any(const AnyType& t)
         {
            return std::visit(
               [&]<typename T>(const T& v) { return inline_type(v); },
               t.value);
         }
         std::string inline_type(const Int& i)
         {
            // FBS maps:
            //   8/16/32/64 signed   → byte/short/int/long
            //   8/16/32/64 unsigned → ubyte/ushort/uint/ulong
            switch (i.bits)
            {
               case 8:  return i.isSigned ? "byte"  : "ubyte";
               case 16: return i.isSigned ? "short" : "ushort";
               case 32: return i.isSigned ? "int"   : "uint";
               case 64: return i.isSigned ? "long"  : "ulong";
               default: return "/* TODO: " +
                               std::string{i.isSigned ? "i" : "u"} +
                               std::to_string(i.bits) + " */";
            }
         }
         std::string inline_type(const Float& f)
         {
            if (f.exp == 8 && f.mantissa == 23)
               return "float";
            if (f.exp == 11 && f.mantissa == 52)
               return "double";
            return "/* TODO: non-IEEE float */";
         }
         std::string inline_type(const Type& t)
         {
            if (t.type == "bool")
               return "bool";
            if (t.type == "string")
               return "string";
            if (t.type == "char")
               return "uint  // @psio:char";
            if (!t.type.empty() && t.type.front() == '@')
            {
               if (auto* resolved = _s.get(t.type))
                  return inline_any(*resolved);
               return "/* TODO: unresolved " + t.type + " */";
            }
            return to_pascal(t.type);
         }
         std::string inline_type(const List& l)
         {
            return "[" + inline_any(*l.type) + "]";
         }
         std::string inline_type(const BoundedList& l)
         {
            return "[" + inline_any(*l.type) + "]  // @psio:max=" +
                   std::to_string(l.maxCount);
         }
         std::string inline_type(const Option& o)
         {
            // FBS table fields with a default behave like options;
            // emit as the inner type with a marker for psio readers.
            return inline_any(*o.type) + "  // @psio:option";
         }
         std::string inline_type(const Tuple& t)
         {
            if (t.members.empty())
               return "void  // @psio:unit";
            std::string out = "void  // @psio:tuple<";
            for (std::size_t i = 0; i < t.members.size(); ++i)
            {
               if (i > 0)
                  out += ", ";
               out += inline_any(*t.members[i]);
            }
            out += ">";
            return out;
         }
         std::string inline_type(const Object&)
         {
            return "/* TODO: anonymous record */";
         }
         std::string inline_type(const Struct&)
         {
            return "/* TODO: anonymous struct */";
         }
         std::string inline_type(const Variant&)
         {
            return "/* TODO: anonymous variant */";
         }
         std::string inline_type(const Resource& r)
         {
            return to_pascal(r.name);
         }
         std::string inline_type(const Array& a)
         {
            return "[" + inline_any(*a.type) + "]  // @psio:array=" +
                   std::to_string(a.len);
         }
         std::string inline_type(const FracPack&)
         {
            return "/* TODO: fracpack wrapper */";
         }
         std::string inline_type(const Custom& c)
         {
            if (c.id == "bool")
               return "bool";
            if (c.id == "string")
               return "string";
            if (c.id == "char")
               return "uint  // @psio:char";
            if (c.id == "hex")
            {
               if (auto* lst = std::get_if<List>(&c.type->value))
                  if (auto* inner = std::get_if<Int>(&lst->type->value);
                      inner && inner->bits == 8 && !inner->isSigned)
                     return "[ubyte]";
            }
            return inline_any(*c.type);
         }
      };

   }  // namespace emit_fbs_detail

   /// Emit the full FlatBuffers schema text representation of `schema`.
   /// Output ends with a trailing newline.
   inline std::string emit_fbs(const Schema& schema)
   {
      emit_fbs_detail::Emitter e{schema};
      return e.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_fbs;
}
