#pragma once
//
// psio/emit_protobuf.hpp — Schema IR → `.proto` text.
//
// `psio::emit_protobuf(schema)` writes the file `protoc` compiles to the
// messages psio::protobuf (protobuf.hpp) reads and writes for the types the
// schema was built from, and writes back what `psio::parse_protobuf` read.
// User documentation: docs/protobuf.md.
//
// How the Schema IR is read as protobuf
// -------------------------------------
//
//   Object / Struct (named)        message; `A.B` is nested in the message `A`
//   member                         field; its number is the `field`
//                                  attribute, else its 1-based position
//     Int 8/16/32 signed           int32   (`protobuf` = sint: sint32, fixed: sfixed32)
//     Int 8/16/32 unsigned, char   uint32  (fixed: fixed32)
//     Int 64                       int64 / uint64 (sint64, sfixed64 / fixed64)
//     Float{8,24} / Float{11,53}   float / double
//     Custom bool                  bool
//     Custom string / utf8         string
//     Custom hex, Nested           bytes
//     Enum                         enum (its values as int32)
//     Type                         the message / enum it names
//     Option{X}                    optional X  (proto2: optional)
//     List / BoundedList / set     repeated X
//     Custom map over List<(K,V)>  map<K, V>
//     Variant                      oneof; alternative j has the number
//                                  (member's number + j), or its own
//                                  `field` attribute; a leading Tuple{}
//                                  alternative (std::monostate) is "none set"
//     Variant of Tuple{} only      enum, values 0, 1, …
//   attributes
//     deprecated                   [deprecated = true]
//     default                      proto2: [default = …]; proto3 has none
//                                  (an absent field is zero: not written)
//     required                     proto2: required
//     proto_option                 `name = value`, as an option of the file /
//                                  message / field / enum / service / method
//     proto_reserved, proto_extensions   the statement's text
//     proto_option.NAME (named enum)     options of the enum value NAME
//   package attributes             proto_syntax (proto3 when absent),
//                                  proto_import
//   Interface                      service; a Func of one message parameter
//                                  and a message result is an rpc
//                                  (proto_client_streaming, proto_server_streaming)
//
// What protobuf does not have is an error (std::runtime_error): a list of
// lists, an optional list, a list of optionals or variants, a tuple, a
// fixed array that is not bytes, an integer wider than 64 bits, a function
// that is not message → message, a field number protobuf refuses.

#include <psio/fbs_layout.hpp>
#include <psio/schema_ir.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace psio::schema_types
{
   namespace emit_protobuf_detail
   {
      namespace d = fbs_layout_detail;

      [[noreturn]] inline void fail(const std::string& what)
      {
         throw std::runtime_error("emit_protobuf: " + what);
      }

      inline bool ident_char(char c) noexcept
      {
         return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_';
      }
      inline bool is_identifier(std::string_view name) noexcept
      {
         if (name.empty() || (name.front() >= '0' && name.front() <= '9'))
            return false;
         for (char c : name)
            if (!ident_char(c))
               return false;
         return true;
      }
      inline std::string identifier(std::string_view name)
      {
         std::string out;
         for (char c : name)
            out += ident_char(c) ? c : '_';
         if (out.empty() || (out.front() >= '0' && out.front() <= '9'))
            out.insert(out.begin(), '_');
         return out;
      }
      inline bool valid_number(std::uint64_t n) noexcept
      {
         return n >= 1 && n <= 536870911 && !(n >= 19000 && n <= 19999);
      }

      class Emitter
      {
       public:
         explicit Emitter(const Schema& s) : _s{s}, _types{s}
         {
            for (char c : s.package.name)
               _package += c == ':' || c == '/' ? '.' : ident_char(c) || c == '.' ? c : '_';
            if (const std::string* syntax = d::attribute(s.package.attributes, "proto_syntax"))
               _proto2 = *syntax == "proto2";
         }

         std::string run()
         {
            // The declarations the schema names, and where each one is.
            for (const auto& entry : _s.types)
               if (declarable(*entry.type) && !entry.name.starts_with('@'))
                  _by_ir_name.emplace(entry.name, entry.type.get());
            for (const auto& entry : _s.types)
               if (declarable(*entry.type) && !entry.name.starts_with('@') &&
                   _by_ir_name.at(entry.name) == entry.type.get())
                  place(entry);
            std::string services;
            for (const auto& iface : _s.interfaces)
               services += service(iface);
            std::string body;
            for (std::size_t i = 0; i < _top.size(); ++i)  // grows as members are read
               body += declaration(*_top[i], 0);
            body += services;

            std::string head = _proto2 ? "syntax = \"proto2\";\n" : "syntax = \"proto3\";\n";
            if (!_package.empty())
               head += "\npackage " + _package + ";\n";
            bool any = false;
            for (const auto& a : _s.package.attributes)
               if (a.name == "proto_import" && a.value)
               {
                  head += std::string{any ? "" : "\n"} + "import " + *a.value + ";\n";
                  any = true;
               }
            any = false;
            for (const auto& a : _s.package.attributes)
               if (a.name == "proto_option" && a.value)
               {
                  head += std::string{any ? "" : "\n"} + "option " + *a.value + ";\n";
                  any = true;
               }
            return head + "\n" + body;
         }

       private:
         struct decl
         {
            const AnyType*              node = nullptr;
            const Member*               entry = nullptr;  // the schema's entry, if it has one
            std::string                 name;             // in its scope
            std::string                 full;             // from the package
            std::vector<const AnyType*> children;
         };

         const Schema& _s;
         d::model      _types;
         std::string   _package;
         bool          _proto2 = false;
         bool          _nested = false;
         std::map<std::string, const AnyType*, std::less<>> _by_ir_name;
         std::map<const AnyType*, decl>                     _decls;
         std::vector<const AnyType*>                        _top;
         std::set<std::string>                              _taken;  // scope + "." + name
         unsigned                                           _depth = 0;

         static bool declarable(const AnyType& node)
         {
            return std::holds_alternative<Object>(node.value) ||
                   std::holds_alternative<Struct>(node.value) ||
                   std::holds_alternative<Enum>(node.value);
         }
         static bool is_message(const AnyType& node)
         {
            return std::holds_alternative<Object>(node.value) ||
                   std::holds_alternative<Struct>(node.value);
         }

         std::string unique(const std::string& scope, const std::string& name)
         {
            std::string candidate = name;
            for (unsigned n = 2; !_taken.insert(scope + "." + candidate).second; ++n)
               candidate = name + "_" + std::to_string(n);
            return candidate;
         }

         // Puts a named entry in its scope: inside the message its name is
         // prefixed with, else at the top.
         decl& place(const Member& entry)
         {
            if (auto at = _decls.find(entry.type.get()); at != _decls.end())
               return at->second;
            decl out;
            out.node  = entry.type.get();
            out.entry = &entry;
            const auto dot = entry.name.rfind('.');
            if (dot != std::string::npos && is_identifier(std::string_view{entry.name}.substr(dot + 1)))
               if (auto parent = _by_ir_name.find(std::string_view{entry.name}.substr(0, dot));
                   parent != _by_ir_name.end() && is_message(*parent->second) &&
                   parent->second != entry.type.get())
               {
                  const Member* parent_entry = _types.entry_of(*parent->second);
                  decl&         p            = place(*parent_entry);
                  out.name = unique(p.full, entry.name.substr(dot + 1));
                  out.full = p.full + "." + out.name;
                  p.children.push_back(out.node);
                  _nested = true;
                  return _decls.emplace(out.node, std::move(out)).first->second;
               }
            out.name = unique("", identifier(entry.name));
            out.full = out.name;
            _top.push_back(out.node);
            return _decls.emplace(out.node, std::move(out)).first->second;
         }

         // A declaration without a name of its own: named after its first user.
         decl& place_anonymous(const AnyType& node, const std::string& owner,
                               std::string_view member)
         {
            if (auto at = _decls.find(&node); at != _decls.end())
               return at->second;
            if (const Member* entry = _types.entry_of(node); entry && !entry->name.starts_with('@'))
               return place(*entry);
            decl out;
            out.node  = &node;
            out.entry = _types.entry_of(node);
            out.name  = unique("", identifier(owner + "_" + std::string{member}));
            out.full  = out.name;
            _top.push_back(out.node);
            return _decls.emplace(out.node, std::move(out)).first->second;
         }

         std::string reference(const decl& to) const
         {
            if (!_nested)
               return to.full;
            return "." + (_package.empty() ? std::string{} : _package + ".") + to.full;
         }

         // ── Types ───────────────────────────────────────────────────
         struct single
         {
            std::string text;
            bool        integer   = false;  // may be a map key
            bool        is_signed = false;
            bool        is_string = false;
            bool        is_bool   = false;
         };

         static std::string where(const std::string& owner, std::string_view member)
         {
            return "member '" + std::string{member} + "' of '" + owner + "': ";
         }

         single integer(const Int& i, std::string_view enc, const std::string& at)
         {
            if (i.bits != 8 && i.bits != 16 && i.bits != 32 && i.bits != 64)
               fail(at + "an integer of " + std::to_string(i.bits) + " bits has no protobuf type");
            const char* w = i.bits == 64 ? "64" : "32";
            single      out;
            out.integer   = true;
            out.is_signed = i.isSigned;
            if (enc == "fixed")
               out.text = std::string{i.isSigned ? "sfixed" : "fixed"} + w;
            else if (enc == "sint")
            {
               if (!i.isSigned)
                  fail(at + "sint is for signed integers");
               out.text = std::string{"sint"} + w;
            }
            else if (enc.empty())
               out.text = std::string{i.isSigned ? "int" : "uint"} + w;
            else
               fail(at + "the 'protobuf' attribute is 'fixed' or 'sint'");
            return out;
         }

         // A type of an imported file: a reference the schema does not
         // declare, written as it is.
         bool foreign(const AnyType& written) const
         {
            const auto* ref = std::get_if<Type>(&written.value);
            return ref && !_types.find(ref->type) &&
                   d::has_attribute(_s.package.attributes, "proto_import");
         }

         // A value with one wire form. `enc` is consumed by an integer.
         single one(const AnyType& written, std::string_view enc, const std::string& owner,
                    std::string_view member, unsigned depth = 0)
         {
            const std::string at = where(owner, member);
            if (depth > 64)
               fail(at + "types nested too deeply");
            single out;
            if (foreign(written))
            {
               if (!enc.empty())
                  fail(at + "the 'protobuf' attribute is for integers");
               out.text = std::get<Type>(written.value).type;
               return out;
            }
            const AnyType& type = _types.resolve(written);
            if (const auto* i = std::get_if<Int>(&type.value))
               return integer(*i, enc, at);
            if (!enc.empty())
            {
               const auto* c = std::get_if<Custom>(&type.value);
               if (!c || c->id == "bool" || c->id == "string" || c->id == "utf8" ||
                   c->id == "hex" || c->id == "map" || c->id == "set")
                  fail(at + "the 'protobuf' attribute is for integers");
            }
            if (const auto* f = std::get_if<Float>(&type.value))
            {
               if (f->exp != 8 && f->exp != 11)
                  fail(at + "a float that is not 32 or 64 bits has no protobuf type");
               out.text = f->exp == 8 ? "float" : "double";
            }
            else if (const auto* c = std::get_if<Custom>(&type.value))
            {
               if (c->id == "bool")
               {
                  out.text    = "bool";
                  out.integer = out.is_bool = true;
               }
               else if (c->id == "char")
                  return integer(Int{32, false}, enc, at);
               else if (c->id == "string" || c->id == "utf8")
               {
                  out.text      = "string";
                  out.is_string = true;
               }
               else if (c->id == "hex")
                  out.text = "bytes";
               else if (c->id == "map" || c->id == "set")
                  fail(at + "a list of lists has no protobuf type");
               else
                  return one(*c->type, enc, owner, member, depth + 1);
            }
            else if (std::holds_alternative<Nested>(type.value))
               out.text = "bytes";
            else if (declarable(type))
               out.text = reference(place_anonymous(type, owner, member));
            else if (const auto* v = std::get_if<Variant>(&type.value); v && _types.enum_shaped(*v))
               out.text = reference(place_anonymous(type, owner, member));
            else if (std::holds_alternative<List>(type.value) ||
                     std::holds_alternative<BoundedList>(type.value))
               fail(at + "a list of lists has no protobuf type");
            else if (std::holds_alternative<Option>(type.value))
               fail(at + "an optional here has no protobuf type");
            else if (std::holds_alternative<Variant>(type.value))
               fail(at + "a variant here has no protobuf type (a oneof is a member of a message)");
            else if (std::holds_alternative<Array>(type.value))
               fail(at + "a fixed array that is not bytes has no protobuf type");
            else if (std::holds_alternative<Tuple>(type.value))
               fail(at + "a tuple has no protobuf type");
            else
               fail(at + "this type has no protobuf form");
            return out;
         }

         // The element of a list / set, or null.
         const AnyType* list_element(const AnyType& type) const
         {
            const AnyType* node = &type;
            if (const auto* c = std::get_if<Custom>(&node->value); c && c->id == "set")
               node = &_types.resolve(*c->type);
            else if (c)
               return nullptr;
            if (const auto* l = std::get_if<List>(&node->value))
               return l->type.get();
            if (const auto* b = std::get_if<BoundedList>(&node->value))
               return b->type.get();
            return nullptr;
         }

         // ── Declarations ────────────────────────────────────────────
         static std::string pad(unsigned indent) { return std::string(indent * 2, ' '); }

         static void statements(std::string& out, const std::vector<Attribute>& attrs,
                                unsigned indent)
         {
            for (const auto& a : attrs)
            {
               if (!a.value)
                  continue;
               if (a.name == "proto_option")
                  out += pad(indent) + "option " + *a.value + ";\n";
               else if (a.name == "proto_reserved")
                  out += pad(indent) + "reserved " + *a.value + ";\n";
               else if (a.name == "proto_extensions")
                  out += pad(indent) + "extensions " + *a.value + ";\n";
            }
         }

         static std::string bracket(const std::vector<std::string>& options)
         {
            if (options.empty())
               return {};
            std::string out = " [";
            for (std::size_t i = 0; i < options.size(); ++i)
               out += (i ? ", " : "") + options[i];
            return out + "]";
         }

         std::vector<std::string> field_options(const std::vector<Attribute>& attrs) const
         {
            std::vector<std::string> out;
            for (const auto& a : attrs)
            {
               if (a.name == "deprecated")
                  out.push_back("deprecated = true");
               else if (a.name == "default" && a.value && _proto2)
                  out.push_back("default = " + *a.value);
               else if (a.name == "proto_option" && a.value)
                  out.push_back(*a.value);
            }
            return out;
         }

         std::string declaration(const AnyType& node, unsigned indent)
         {
            if (++_depth > 64)
               fail("declarations nested too deeply");
            // Copied: reading the members may add declarations.
            const decl  self = _decls.at(&node);
            std::string out;
            if (const auto* e = std::get_if<Enum>(&node.value))
               out = enumeration(self, *e, indent);
            else if (const auto* v = std::get_if<Variant>(&node.value))
               out = unit_enumeration(self, *v, indent);
            else
               out = message(self, indent);
            --_depth;
            return out;
         }

         std::string scope_of(const decl& self) const
         {
            const auto dot = self.full.rfind('.');
            return dot == std::string::npos ? std::string{} : self.full.substr(0, dot);
         }

         std::string enumeration(const decl& self, const Enum& e, unsigned indent)
         {
            const AnyType& under = _types.resolve(*e.type);
            std::uint32_t  bits  = 32;
            bool           sign  = true;
            if (const auto* i = std::get_if<Int>(&under.value))
            {
               bits = i->bits;
               sign = i->isSigned;
            }
            else if (const auto* c = std::get_if<Custom>(&under.value); !c || c->id != "bool")
               fail("enum '" + self.full + "' is not over an integer");
            std::vector<std::pair<std::string, std::int64_t>> values;
            bool                                              alias = false;
            std::set<std::int64_t>                            seen;
            for (const auto& m : e.members)
            {
               std::int64_t v = static_cast<std::int64_t>(m.value);
               if (bits < 64)
               {
                  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
                  std::uint64_t       u    = m.value & mask;
                  if ((sign || bits == 32) && (u >> (bits - 1)))
                     u |= ~mask;
                  v = static_cast<std::int64_t>(u);
               }
               if (v < INT32_MIN || v > INT32_MAX)
                  fail("enum '" + self.full + "': the value of '" + m.name +
                       "' is not an int32");
               alias |= !seen.insert(v).second;
               values.emplace_back(m.name, v);
            }
            return enum_text(self, std::move(values), alias, indent);
         }

         std::string unit_enumeration(const decl& self, const Variant& v, unsigned indent)
         {
            std::vector<std::pair<std::string, std::int64_t>> values;
            for (std::size_t i = 0; i < v.members.size(); ++i)
               values.emplace_back(v.members[i].name, static_cast<std::int64_t>(i));
            return enum_text(self, std::move(values), false, indent);
         }

         std::string enum_text(const decl& self, std::vector<std::pair<std::string, std::int64_t>> values,
                               bool alias, unsigned indent)
         {
            const std::string scope = scope_of(self);
            static const std::vector<Attribute> none;
            const auto& attrs = self.entry ? self.entry->attributes : none;
            std::string out   = pad(indent) + "enum " + self.name + " {\n";
            bool        stated = false;
            for (const auto& a : attrs)
               if (a.name == "proto_option" && a.value && a.value->starts_with("allow_alias"))
                  stated = true;
            if (alias && !stated)
               out += pad(indent + 1) + "option allow_alias = true;\n";
            statements(out, attrs, indent + 1);
            if (!_proto2)
            {
               // proto3: the first value is zero (what an absent field reads as).
               std::size_t zero = values.size();
               for (std::size_t i = 0; i < values.size() && zero == values.size(); ++i)
                  if (values[i].second == 0)
                     zero = i;
               if (zero == values.size())
                  values.insert(values.begin(), {self.name + "_UNSPECIFIED", 0});
               else if (zero)
                  std::rotate(values.begin(), values.begin() + zero, values.begin() + zero + 1);
            }
            else if (values.empty())
               fail("enum '" + self.full + "' has no values");
            for (const auto& [name, value] : values)
            {
               // Enum values are names of the enclosing scope.
               std::string id = identifier(name);
               if (!_taken.insert(scope + "." + id).second)
                  id = unique(scope, self.name + "_" + id);
               std::vector<std::string> options;
               for (const auto& a : attrs)
                  if (a.value && a.name == "proto_option." + name)
                     options.push_back(*a.value);
               out += pad(indent + 1) + id + " = " + std::to_string(value) + bracket(options) + ";\n";
            }
            return out + pad(indent) + "}\n\n";
         }

         std::string message(const decl& self, unsigned indent)
         {
            const auto* o       = std::get_if<Object>(&self.node->value);
            const auto& members = o ? o->members : std::get<Struct>(self.node->value).members;
            const auto& attrs   = o ? o->attributes : std::get<Struct>(self.node->value).attributes;

            std::string             fields;
            std::set<std::uint64_t> numbers;
            // Field names are unique in a message, a oneof's among them.
            std::set<std::string> names;
            const auto            name_field = [&](std::string id, const std::string& fallback) {
               if (names.count(id) && !fallback.empty())
                  id = fallback;
               const std::string base = id;
               for (unsigned n = 2; !names.insert(id).second; ++n)
                  id = base + "_" + std::to_string(n);
               return id;
            };
            const auto              take = [&](std::uint64_t n, std::string_view member) {
               if (!valid_number(n))
                  fail(where(self.full, member) + "the field number " + std::to_string(n) +
                       " is not one protobuf allows");
               if (!numbers.insert(n).second)
                  fail(where(self.full, member) + "the field number " + std::to_string(n) +
                       " is used twice (a variant takes one number per alternative)");
            };
            const auto number_of = [&](const std::vector<Attribute>& a, std::uint64_t otherwise,
                                       std::string_view member) {
               bool present = false;
               const std::string* text = d::attribute(a, "field", &present);
               if (!present)
                  return otherwise;
               const auto n = text ? d::parse_unsigned(*text) : std::nullopt;
               if (!n)
                  fail(where(self.full, member) + "the 'field' attribute is not a number");
               return *n;
            };

            for (std::size_t i = 0; i < members.size(); ++i)
            {
               const Member&       m      = members[i];
               const std::string   id     = name_field(identifier(m.name), {});
               const std::string   at     = where(self.full, m.name);
               const std::uint64_t number = number_of(m.attributes, i + 1, m.name);
               const std::string*  enc_at = d::attribute(m.attributes, "protobuf");
               const std::string_view enc = enc_at ? std::string_view{*enc_at} : std::string_view{};
               auto                options = field_options(m.attributes);
               const AnyType&      type    = foreign(*m.type) ? *m.type : _types.resolve(*m.type);
               const std::string   in      = pad(indent + 1);
               const auto          enc_of  = [&](const std::vector<Attribute>& a, const char* name) {
                  const std::string* e = d::attribute(a, name);
                  return e ? std::string_view{*e} : std::string_view{};
               };

               if (const auto* v = std::get_if<Variant>(&type.value); v && !_types.enum_shaped(*v))
               {
                  if (!enc.empty())
                     fail(at + "the 'protobuf' attribute does not apply to a variant");
                  fields += in + "oneof " + id + " {\n";
                  statements(fields, v->attributes, indent + 2);
                  std::uint64_t j = 0;
                  for (std::size_t a = 0; a < v->members.size(); ++a)
                  {
                     const Member& alt = v->members[a];
                     if (a == 0 && _types.is_unit(*alt.type))
                        continue;  // std::monostate: none set
                     const std::uint64_t n = number_of(alt.attributes, number + j, m.name);
                     ++j;
                     take(n, m.name);
                     const single s = one(*alt.type, enc_of(alt.attributes, "protobuf"), self.full,
                                          m.name + "_" + alt.name);
                     fields += in + "  " + s.text + " " +
                               name_field(identifier(alt.name), identifier(m.name + "_" + alt.name)) + " = " +
                               std::to_string(n) + bracket(field_options(alt.attributes)) + ";\n";
                  }
                  if (!j)
                     fail(at + "a variant without alternatives");
                  fields += in + "}\n";
                  continue;
               }
               take(number, m.name);
               std::string text;
               if (const auto* opt = std::get_if<Option>(&type.value))
                  text = "optional " + one(*opt->type, enc, self.full, m.name).text;
               else if (const auto* c = std::get_if<Custom>(&type.value); c && c->id == "map")
               {
                  if (!enc.empty())
                     fail(at + "a map has 'protobuf_key' and 'protobuf_value'");
                  const AnyType* entry = list_element(_types.resolve(*c->type));
                  const auto*    pair  = entry ? std::get_if<Tuple>(&_types.resolve(*entry).value) : nullptr;
                  if (!pair || pair->members.size() != 2)
                     fail(at + "a map is over a list of (key, value)");
                  const single key = one(*pair->members[0], enc_of(m.attributes, "protobuf_key"),
                                         self.full, m.name + "_key");
                  if (!key.integer && !key.is_string)
                     fail(at + "a map's key is an integer, a bool or a string");
                  const single value = one(*pair->members[1], enc_of(m.attributes, "protobuf_value"),
                                           self.full, m.name + "_value");
                  text = "map<" + key.text + ", " + value.text + ">";
               }
               else if (const AnyType* element = list_element(type))
                  text = "repeated " + one(*element, enc, self.full, m.name).text;
               else
               {
                  const single s = one(*m.type, enc, self.full, m.name);
                  if (_proto2)
                     text = d::has_attribute(m.attributes, "required") ? "required " : "optional ";
                  text += s.text;
               }
               fields += in + text + " " + id + " = " + std::to_string(number) + bracket(options) +
                         ";\n";
            }

            std::string out = pad(indent) + "message " + self.name + " {\n";
            statements(out, attrs, indent + 1);
            // The members may have added nested declarations? No: an
            // anonymous declaration goes to the top. Children are fixed.
            for (const AnyType* child : _decls.at(self.node).children)
               out += declaration(*child, indent + 1);
            return out + fields + pad(indent) + "}\n\n";
         }

         std::string service(const Interface& iface)
         {
            std::string out = "service " + identifier(iface.name) + " {\n";
            statements(out, iface.attributes, 1);
            for (const auto& f : iface.funcs)
            {
               const std::string at = "function '" + f.name + "' of '" + iface.name + "': ";
               if (f.params.size() != 1 || !f.result)
                  fail(at + "an rpc takes one message and returns one");
               const auto message_name = [&](const AnyType& written, const char* role) {
                  if (foreign(written))
                     return std::get<Type>(written.value).type;
                  const AnyType& type = _types.resolve(written);
                  if (!is_message(type))
                     fail(at + "an rpc takes one message and returns one");
                  return reference(place_anonymous(type, iface.name, f.name + role));
               };
               out += "  rpc " + identifier(f.name) + " (" +
                      (d::has_attribute(f.attributes, "proto_client_streaming") ? "stream " : "") +
                      message_name(*f.params[0].type, "_request") + ") returns (" +
                      (d::has_attribute(f.attributes, "proto_server_streaming") ? "stream " : "") +
                      message_name(**f.result, "_response") + ")";
               std::string options;
               statements(options, f.attributes, 2);
               out += options.empty() ? ";\n" : " {\n" + options + "  }\n";
            }
            return out + "}\n\n";
         }
      };
   }  // namespace emit_protobuf_detail

   /// The `.proto` file of a schema. std::runtime_error for what protobuf
   /// cannot say.
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
