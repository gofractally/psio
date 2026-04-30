#pragma once
//
// psio/emit_wit.hpp — Schema IR → WIT text emitter.
//
// First member of the symmetric format-emitter family.  Walks a
// `psio::Schema` (built by SchemaBuilder, by parsing WIT/fbs/capnp,
// or by direct construction) and produces standards-compliant WIT
// source text.  Lets psio hand any reflected schema to external
// component-model tooling (wit-bindgen, wasmtime, …) and forms the
// inverse of `psio/wit_parser.hpp`.
//
// Coverage (matching v1):
//
//   Envelope: Package, Use, Interface, World
//   Types:    Int, Float, bool, char, string,
//             List (`list<T>`), Option (`option<T>`),
//             Tuple (`tuple<...>`), named Type refs,
//             Object/Struct (as `record`),
//             Variant (as `variant`),
//             Resource (as `resource`).
//
//   Lossy on the wire today (rendered with a /* @psio:* */ hint when
//   no native WIT spelling exists): BoundedList (max-bound on a
//   list<T>), Array (fixed-size), FracPack wrappers.  Custom
//   annotations surface as their canonical built-in name when one
//   exists ("bool", "string", "char"); otherwise the underlying
//   shape passes through.
//
// Companion / inverse: psio/wit_parser.hpp (WIT → Schema) — already
// ported.

#include <psio/schema_ir.hpp>

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace psio::schema_types
{

   namespace emit_wit_detail
   {
      // ── Name mangling ────────────────────────────────────────────
      //
      // WIT identifiers are lowercase-kebab.  Convert C++ names by:
      //   • turning `_` into `-`
      //   • splitting camel/Pascal on lowercase→uppercase transitions
      //   • lowercasing everything
      inline std::string to_kebab(std::string_view name)
      {
         std::string out;
         out.reserve(name.size() + 2);
         for (std::size_t i = 0; i < name.size(); ++i)
         {
            char c = name[i];
            if (c == '_')
            {
               out += '-';
               continue;
            }
            if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
                std::islower(static_cast<unsigned char>(name[i - 1])))
            {
               out += '-';
            }
            out += static_cast<char>(
               std::tolower(static_cast<unsigned char>(c)));
         }
         return out;
      }

      inline std::string format_package_id(const std::string& name,
                                           const std::string& version)
      {
         std::string out = name;
         if (!version.empty())
         {
            out += '@';
            out += version;
         }
         return out;
      }

      class Emitter
      {
       public:
         explicit Emitter(const Schema& schema) : _schema{schema} {}

         std::string run()
         {
            emit_package();
            emit_uses();
            emit_interfaces();
            emit_worlds();
            emit_orphan_types();
            return _out.str();
         }

       private:
         const Schema&      _schema;
         std::ostringstream _out;
         int                _indent = 0;

         void indent()
         {
            for (int i = 0; i < _indent; ++i)
               _out << "  ";
         }

         // ── Envelope ──────────────────────────────────────────────
         void emit_package()
         {
            const auto& p = _schema.package;
            if (p.name.empty())
               return;
            _out << "package " << format_package_id(p.name, p.version)
                 << ";\n\n";
         }

         void emit_uses()
         {
            if (_schema.uses.empty())
               return;
            for (const auto& u : _schema.uses)
            {
               _out << "use ";
               if (!u.package.empty())
                  _out << u.package << '/';
               _out << to_kebab(u.interface_name);
               if (!u.version.empty())
                  _out << '@' << u.version;
               _out << ";\n";
            }
            _out << "\n";
         }

         void emit_interfaces()
         {
            for (const auto& iface : _schema.interfaces)
               emit_interface(iface);
         }

         void emit_interface(const Interface& iface)
         {
            _out << "interface " << to_kebab(iface.name) << " {\n";
            ++_indent;
            for (const auto& tname : iface.type_names)
               emit_named_type(tname);
            for (const auto& fn : iface.funcs)
               emit_func(fn);
            --_indent;
            _out << "}\n\n";
         }

         void emit_worlds()
         {
            for (const auto& w : _schema.worlds)
               emit_world(w);
         }

         void emit_world(const World& w)
         {
            _out << "world " << to_kebab(w.name) << " {\n";
            ++_indent;
            for (const auto& imp : w.imports)
            {
               indent();
               _out << "import ";
               if (!imp.package.empty())
                  _out << imp.package << '/';
               _out << to_kebab(imp.interface_name) << ";\n";
            }
            for (const auto& exp : w.exports)
            {
               indent();
               _out << "export " << to_kebab(exp) << ";\n";
            }
            --_indent;
            _out << "}\n\n";
         }

         // Schema-level types not claimed by any interface.  Names
         // starting with `@` are SchemaBuilder interning artifacts —
         // resolved inline at the use site, never emitted as
         // top-level aliases.
         void emit_orphan_types()
         {
            std::unordered_set<std::string> in_iface;
            for (const auto& iface : _schema.interfaces)
               for (const auto& tn : iface.type_names)
                  in_iface.insert(tn);
            for (const auto& m : _schema.types)
            {
               if (in_iface.contains(m.name))
                  continue;
               if (!m.name.empty() && m.name.front() == '@')
                  continue;
               emit_type_decl(m.name, *m.type);
            }
         }

         // ── Type emission ─────────────────────────────────────────
         void emit_named_type(const std::string& tname)
         {
            const AnyType* type = _schema.get(tname);
            if (!type)
            {
               indent();
               _out << "/* TODO: missing type \"" << tname << "\" */\n";
               return;
            }
            emit_type_decl(tname, *type);
         }

         void emit_type_decl(const std::string& tname, const AnyType& type)
         {
            std::visit([&]<typename T>(const T& t)
                       { emit_type_decl_impl(tname, t); },
                       type.value);
         }

         void emit_type_decl_impl(const std::string& tname, const Object& o)
         {
            emit_type_attribute_lines(o.attributes);
            emit_record(tname, o.members);
         }
         void emit_type_decl_impl(const std::string& tname, const Struct& s)
         {
            emit_type_attribute_lines(s.attributes);
            emit_record(tname, s.members);
         }

         // WIT supports @-attribute syntax on type declarations.  Emit
         // psio caps as native attributes so downstream WIT tooling
         // (parsers, doc generators, codegen) sees them.  Unknown
         // attributes are by spec ignored, so this is forward-safe.
         void emit_type_attribute_lines(
            const std::vector<Attribute>& attrs)
         {
            for (const auto& a : attrs)
            {
               if (a.name == "definitionWillNotChange" ||
                   a.name == "maxFields" ||
                   a.name == "maxDynamicData")
               {
                  indent();
                  _out << "@" << a.name;
                  if (!a.value.empty())
                     _out << "(" << a.value << ")";
                  _out << "\n";
               }
            }
         }
         void emit_type_decl_impl(const std::string& tname, const Variant& v)
         {
            emit_variant_decl(tname, v.members);
         }
         void emit_type_decl_impl(const std::string&, const Resource& r)
         {
            emit_resource_decl(r);
         }
         template <typename T>
         void emit_type_decl_impl(const std::string& tname, const T& t)
         {
            indent();
            _out << "type " << to_kebab(tname) << " = " << inline_type(t)
                 << ";\n";
         }

         void emit_record(const std::string&         tname,
                          const std::vector<Member>& members)
         {
            indent();
            _out << "record " << to_kebab(tname) << " {\n";
            ++_indent;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               const auto& m = members[i];
               indent();
               _out << to_kebab(m.name) << ": " << inline_any(*m.type);
               if (i + 1 < members.size())
                  _out << ",";
               _out << "\n";
            }
            --_indent;
            indent();
            _out << "}\n";
         }

         void emit_variant_decl(const std::string&         tname,
                                const std::vector<Member>& members)
         {
            indent();
            _out << "variant " << to_kebab(tname) << " {\n";
            ++_indent;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               const auto& m = members[i];
               indent();
               _out << to_kebab(m.name);
               // A Tuple{} payload with zero members serves as the
               // bare-case form (no associated type); anything else
               // gets parenthesised.
               if (auto* tup = std::get_if<Tuple>(&m.type->value);
                   !(tup && tup->members.empty()))
               {
                  _out << "(" << inline_any(*m.type) << ")";
               }
               if (i + 1 < members.size())
                  _out << ",";
               _out << "\n";
            }
            --_indent;
            indent();
            _out << "}\n";
         }

         void emit_resource_decl(const Resource& r)
         {
            indent();
            _out << "resource " << to_kebab(r.name) << " {\n";
            ++_indent;
            for (const auto& method : r.methods)
               emit_func(method);
            --_indent;
            indent();
            _out << "}\n";
         }

         void emit_func(const Func& fn)
         {
            indent();
            _out << to_kebab(fn.name) << ": func(";
            for (std::size_t i = 0; i < fn.params.size(); ++i)
            {
               const auto& p = fn.params[i];
               if (i > 0)
                  _out << ", ";
               _out << to_kebab(p.name) << ": " << inline_any(*p.type);
            }
            _out << ")";
            if (fn.result)
               _out << " -> " << inline_any(**fn.result);
            _out << ";\n";
         }

         // ── Inline type forms (no declaration) ────────────────────
         std::string inline_any(const AnyType& t)
         {
            return std::visit(
               [&]<typename T>(const T& v) { return inline_type(v); },
               t.value);
         }

         std::string inline_type(const Int& i)
         {
            std::string out;
            out += (i.isSigned ? 's' : 'u');
            out += std::to_string(i.bits);
            return out;
         }
         std::string inline_type(const Float& f)
         {
            if (f.exp == 8 && f.mantissa == 23)
               return "f32";
            if (f.exp == 11 && f.mantissa == 52)
               return "f64";
            return "/* TODO: non-IEEE float */";
         }
         std::string inline_type(const Type& t)
         {
            // Built-in spellings, then SchemaBuilder interning
            // artifacts (always inlined), then user-defined kebab.
            if (t.type == "bool")
               return "bool";
            if (t.type == "string")
               return "string";
            if (t.type == "char")
               return "char";
            if (!t.type.empty() && t.type.front() == '@')
            {
               if (const AnyType* resolved = _schema.get(t.type))
                  return inline_any(*resolved);
               return "/* TODO: unresolved " + t.type + " */";
            }
            return to_kebab(t.type);
         }
         std::string inline_type(const List& l)
         {
            return "list<" + inline_any(*l.type) + ">";
         }
         std::string inline_type(const BoundedList& l)
         {
            // WIT has no native bounded-list syntax; emit as list<T>
            // with a psio convention comment carrying the bound — a
            // psio-aware reader can recover it on round-trip.
            return "list<" + inline_any(*l.type) +
                   ">/* @psio:max=" + std::to_string(l.maxCount) + " */";
         }
         std::string inline_type(const Option& o)
         {
            return "option<" + inline_any(*o.type) + ">";
         }
         std::string inline_type(const Tuple& t)
         {
            if (t.members.empty())
               return "tuple<>";
            std::string out = "tuple<";
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
            return "/* TODO: inline record not supported */";
         }
         std::string inline_type(const Struct&)
         {
            return "/* TODO: inline record not supported */";
         }
         std::string inline_type(const Variant&)
         {
            return "/* TODO: inline variant not supported */";
         }
         std::string inline_type(const Resource& r)
         {
            // Inline resource references render as own<name>; borrow<>
            // form will follow once the IR carries the
            // owned-vs-borrowed distinction explicitly.
            return "own<" + to_kebab(r.name) + ">";
         }
         std::string inline_type(const Array&)
         {
            return "/* TODO: fixed-size array */";
         }
         std::string inline_type(const FracPack&)
         {
            return "/* TODO: fracpack wrapper */";
         }
         std::string inline_type(const Custom& c)
         {
            // Surface the annotation where a built-in spelling exists,
            // otherwise pass through to the underlying type
            // (`Custom{List<u8>, "hex"}` → `list<u8>`).
            if (c.id == "bool")
               return "bool";
            if (c.id == "string")
               return "string";
            if (c.id == "char")
               return "char";
            return inline_any(*c.type);
         }
      };

   }  // namespace emit_wit_detail

   /// Emit the full WIT text representation of `schema`.
   /// Output ends with a trailing newline.
   inline std::string emit_wit(const Schema& schema)
   {
      emit_wit_detail::Emitter e{schema};
      return e.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_wit;
}
