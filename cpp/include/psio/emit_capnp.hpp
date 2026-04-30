#pragma once
//
// psio/emit_capnp.hpp — Schema IR → Cap'n Proto schema text.
//
// Walks a `psio::Schema` and produces `.capnp` source text.  The
// inverse (`.capnp` → Schema) lives in psio/capnp_parser.hpp.
//
// IR ↔ capnp mapping:
//
//   Object              → struct
//   Struct              → struct      (capnp doesn't distinguish; the
//                                      IR's Struct vs. Object split is
//                                      a binary-format concern from
//                                      v1's frac/ssz layer)
//   Variant             → struct with `union { … }` body (capnp
//                                      idiom: a struct containing a
//                                      single anonymous union)
//   Resource            → interface
//   Int{N, signed}      → Int{N} / UInt{N}      (N ∈ {8,16,32,64})
//   Float{8, 23}        → Float32
//   Float{11, 52}       → Float64
//   "bool"              → Bool
//   string / Custom-string → Text
//   list<u8> via Custom "hex" → Data
//   list<T>             → List(T)
//   Option<T>           → struct with `value :T` and an implicit
//                                      hasValue derived from
//                                      capnp's pointer-vs-default
//                                      convention; for primitives we
//                                      synthesise a tiny option-of
//                                      wrapper (named "Option<T>" in
//                                      a comment so a psio reader can
//                                      recover full fidelity)
//   Type{name}          → bare identifier (resolved by the schema
//                                      reader)
//   FunctionType        → method on the enclosing interface
//
// Field ordinals: each capnp field needs an explicit `@N`.  If the IR
// `Member` carries an `ordinal` attribute, we emit that; otherwise
// we assign sequential ordinals starting from 0 in declaration order.
//
// File-level `@<id>;` annotation: emitted from
// `Package.attributes["capnp_id"]` if present, else a deterministic
// stub `@0xABADC0DEABADC0DE` (capnp doesn't strictly require a real
// id for parsing — only for code generation — and a placeholder is
// strictly more honest than a fabricated random value).

#include <psio/schema_ir.hpp>

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace psio::schema_types
{

   namespace emit_capnp_detail
   {
      // Pascal-case identifier conversion: capnp uses upper-camel for
      // type names and lower-camel for fields.  IR names come in as
      // snake_case typically; we split on `_` and re-case.
      inline std::string to_pascal(std::string_view name)
      {
         std::string out;
         out.reserve(name.size());
         bool boundary = true;  // start of identifier
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

      inline std::string to_camel(std::string_view name)
      {
         auto p = to_pascal(name);
         if (!p.empty())
            p[0] = static_cast<char>(
               std::tolower(static_cast<unsigned char>(p[0])));
         return p;
      }

      // Look up an Attribute by name on a Member.  Returns nullopt
      // when absent.  Used for ordinal carriers.
      inline std::optional<std::string> attr_lookup(
         const std::vector<Attribute>& attrs, std::string_view name)
      {
         for (const auto& a : attrs)
            if (a.name == name)
               return a.value;
         return std::nullopt;
      }

      class Emitter
      {
       public:
         explicit Emitter(const Schema& s) : _s{s} {}

         std::string run()
         {
            emit_file_id();
            for (const auto& m : _s.types)
               emit_type_decl(m.name, *m.type);
            for (const auto& iface : _s.interfaces)
               emit_interface(iface);
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

         void emit_file_id()
         {
            std::string id = "@0xABADC0DEABADC0DE";
            for (const auto& a : _s.package.attributes)
               if (a.name == "capnp_id" && a.value)
                  id = *a.value;
            _out << id << ";\n";
            if (!_s.package.name.empty())
            {
               _out << "# package: " << _s.package.name;
               if (!_s.package.version.empty())
                  _out << "@" << _s.package.version;
               _out << "\n";
            }
            _out << "\n";
         }

         void emit_type_decl(const std::string& name, const AnyType& t)
         {
            std::visit(
               [&]<typename T>(const T& v) { emit_decl_impl(name, v); },
               t.value);
         }

         void emit_decl_impl(const std::string& name, const Object& o)
         {
            emit_type_attribute_comments(o.attributes);
            emit_struct(name, o.members);
         }
         void emit_decl_impl(const std::string& name, const Struct& s)
         {
            emit_type_attribute_comments(s.attributes);
            emit_struct(name, s.members);
         }

         // Capnp schema has no native attribute syntax for psio caps.
         // Surface them as preceding-line comments so readers can
         // recover the contract and tools can grep for it.
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
                  _out << "# @" << a.name;
                  if (!a.value.empty())
                     _out << "(" << a.value << ")";
                  _out << "\n";
               }
            }
         }
         void emit_decl_impl(const std::string& name, const Variant& v)
         {
            emit_variant_struct(name, v.members);
         }
         void emit_decl_impl(const std::string& name, const Resource& r)
         {
            emit_interface_decl(name, r);
         }
         template <typename T>
         void emit_decl_impl(const std::string& name, const T& t)
         {
            // capnp has no top-level alias for primitives or
            // collection types; render as an annotated comment so
            // round-trip readers can reconstruct, and skip emission.
            // (Custom IR shapes would need their own type form.)
            indent();
            _out << "# alias " << to_pascal(name) << " = "
                 << inline_type(t) << ";\n";
            (void)t;
         }

         void emit_struct(const std::string&         name,
                          const std::vector<Member>& members)
         {
            indent();
            _out << "struct " << to_pascal(name) << " {\n";
            ++_indent;
            std::uint32_t auto_ord = 0;
            for (const auto& m : members)
            {
               indent();
               _out << to_camel(m.name) << " ";
               auto user_ord = attr_lookup(m.attributes, "ordinal");
               _out << "@" << (user_ord ? *user_ord : std::to_string(auto_ord));
               _out << " :" << inline_any(*m.type) << ";\n";
               if (!user_ord)
                  ++auto_ord;
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         // capnp variant: a struct with a single anonymous union.  The
         // bare-case form (no payload) renders as `name @N :Void;`; a
         // payload case is `name @N :T;`.
         void emit_variant_struct(const std::string&         name,
                                  const std::vector<Member>& members)
         {
            indent();
            _out << "struct " << to_pascal(name) << " {\n";
            ++_indent;
            indent();
            _out << "union {\n";
            ++_indent;
            std::uint32_t auto_ord = 0;
            for (const auto& m : members)
            {
               indent();
               _out << to_camel(m.name) << " ";
               auto user_ord = attr_lookup(m.attributes, "ordinal");
               _out << "@" << (user_ord ? *user_ord : std::to_string(auto_ord));
               // Bare case: Tuple{} payload signals "no associated value".
               if (auto* tup = std::get_if<Tuple>(&m.type->value);
                   tup && tup->members.empty())
                  _out << " :Void;\n";
               else
                  _out << " :" << inline_any(*m.type) << ";\n";
               if (!user_ord)
                  ++auto_ord;
            }
            --_indent;
            indent();
            _out << "}\n";
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_interface_decl(const std::string& name,
                                  const Resource&    r)
         {
            indent();
            _out << "interface " << to_pascal(r.name.empty() ? name : r.name)
                 << " {\n";
            ++_indent;
            std::uint32_t auto_ord = 0;
            for (const auto& fn : r.methods)
            {
               emit_method(fn, auto_ord);
               ++auto_ord;
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_interface(const Interface& iface)
         {
            indent();
            _out << "interface " << to_pascal(iface.name) << " {\n";
            ++_indent;
            std::uint32_t auto_ord = 0;
            for (const auto& fn : iface.funcs)
            {
               emit_method(fn, auto_ord);
               ++auto_ord;
            }
            --_indent;
            indent();
            _out << "}\n\n";
         }

         void emit_method(const Func& fn, std::uint32_t auto_ord)
         {
            indent();
            _out << to_camel(fn.name) << " ";
            auto user_ord = attr_lookup(fn.attributes, "ordinal");
            _out << "@" << (user_ord ? *user_ord : std::to_string(auto_ord));
            _out << " (";
            for (std::size_t i = 0; i < fn.params.size(); ++i)
            {
               if (i > 0)
                  _out << ", ";
               _out << to_camel(fn.params[i].name) << " :"
                    << inline_any(*fn.params[i].type);
            }
            _out << ")";
            if (fn.result)
               _out << " -> (result :" << inline_any(**fn.result) << ")";
            _out << ";\n";
         }

         // ── Inline (use-site) type forms ──────────────────────────
         std::string inline_any(const AnyType& t)
         {
            return std::visit(
               [&]<typename T>(const T& v) { return inline_type(v); },
               t.value);
         }
         std::string inline_type(const Int& i)
         {
            std::string out = i.isSigned ? "Int" : "UInt";
            out += std::to_string(i.bits);
            return out;
         }
         std::string inline_type(const Float& f)
         {
            if (f.exp == 8 && f.mantissa == 23)
               return "Float32";
            if (f.exp == 11 && f.mantissa == 52)
               return "Float64";
            return "/* TODO: non-IEEE float */";
         }
         std::string inline_type(const Type& t)
         {
            if (t.type == "bool")
               return "Bool";
            if (t.type == "string")
               return "Text";
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
            // List(UInt8) renders as Data per capnp idiom.
            if (auto* inner = std::get_if<Int>(&l.type->value);
                inner && inner->bits == 8 && !inner->isSigned)
               return "Data";
            return "List(" + inline_any(*l.type) + ")";
         }
         std::string inline_type(const BoundedList& l)
         {
            // No bounded-list spelling; carry maxCount as a comment
            // for psio-aware readers.
            std::string out = "List(" + inline_any(*l.type) +
                              ")  # @psio:max=" +
                              std::to_string(l.maxCount);
            return out;
         }
         std::string inline_type(const Option& o)
         {
            // capnp has no native option<T>; emit as a comment-marked
            // List(T) of length 0 or 1 — same convention used by
            // several capnp-using codebases.
            return "List(" + inline_any(*o.type) +
                   ")  # @psio:option";
         }
         std::string inline_type(const Tuple& t)
         {
            if (t.members.empty())
               return "Void";
            // Anonymous tuples lack a native form; degrade to a
            // commented Object-like aside.
            std::string out = "Void  # @psio:tuple<";
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
            return "List(" + inline_any(*a.type) + ")  # @psio:array=" +
                   std::to_string(a.len);
         }
         std::string inline_type(const FracPack&)
         {
            return "/* TODO: fracpack wrapper */";
         }
         std::string inline_type(const Custom& c)
         {
            if (c.id == "bool")
               return "Bool";
            if (c.id == "string")
               return "Text";
            if (c.id == "char")
               return "UInt32  # @psio:char";
            // Tagged byte array → Data
            if (c.id == "hex")
            {
               if (auto* lst = std::get_if<List>(&c.type->value);
                   lst)
               {
                  if (auto* inner = std::get_if<Int>(&lst->type->value);
                      inner && inner->bits == 8 && !inner->isSigned)
                     return "Data";
               }
            }
            // Pass through to underlying inline form.
            return inline_any(*c.type);
         }
      };

   }  // namespace emit_capnp_detail

   /// Emit the full Cap'n Proto schema text representation of `schema`.
   /// Output ends with a trailing newline.
   inline std::string emit_capnp(const Schema& schema)
   {
      emit_capnp_detail::Emitter e{schema};
      return e.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_capnp;
}
