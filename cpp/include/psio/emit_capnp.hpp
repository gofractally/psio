#pragma once
//
// psio/emit_capnp.hpp — Schema IR → Cap'n Proto schema text.
//
// `psio::emit_capnp(schema)` writes the declarations psio/capnp_layout.hpp
// describes ("How the Schema IR is read as Cap'n Proto"): for a schema the
// schema builder made from reflected C++ types, `capnp compile` accepts the
// text and computes the layout psio/capnp.hpp uses for those types
// (tests/capnp_schema_interop_tests.cpp exchanges messages with the code it
// generates); for a schema psio/capnp_parser.hpp read, the text declares
// what the original text declared.
//
// What the text looks like:
//
//   * Cap'n Proto requires `TypeName` and `fieldName` without underscores.
//     A type is named by the PascalCase of its IR name (`a::Config` is
//     `AConfig`); a member, enumerant or union member by the camelCase of
//     its IR name, with `$Json.name("the_ir_name")`
//     (capnp/compat/json.capnp, the official annotation for a name in text)
//     where the two differ — which is what parse_capnp reads the IR name
//     back from. The file then imports that schema as `Json`.
//   * a type whose IR name is `Outer.Inner`, Outer a struct or interface of
//     the schema, is declared inside Outer;
//   * every field has its ordinal: the IR's, or the codec's assignment
//     (ascending field number, a union one per member);
//   * a default is written when it is not zero;
//   * an Enum is a Cap'n Proto `enum` when it is one on the wire (over
//     UInt16, its values 0, 1, 2, ...); any other enum is written as its
//     integer type, which is what the codec puts on the wire for a C++ enum;
//   * `@0x...` ids are written where the IR has them (`capnp_id`); a schema
//     without a file id gets one derived from its package and type names;
//   * a function of a reflected interface (Schema::interfaces) whose
//     parameters or result have no Cap'n Proto type is left out, with a
//     comment that says so;
//   * a type name the schema does not declare is written as it is (an
//     imported file may declare it) where no layout depends on it: in a
//     method's parameters and results.
//
// Throws std::runtime_error ("emit_capnp: ...") for a type Cap'n Proto does
// not have: a map, a set, a tuple, an integer wider than 64 bits, a list of
// unions or of optional scalars, an optional of an optional.
//
// The normal form: `emit_capnp(parse_capnp(text))` declares what `text`
// declares (the compiler computes the same layout, names, ordinals and
// defaults for the two), and `emit_capnp(parse_capnp(emit_capnp(s))) ==
// emit_capnp(s)` for every schema.

#include <psio/capnp_layout.hpp>
#include <psio/capnp_parser.hpp>
#include <psio/schema_ir.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace psio::schema_types
{

   namespace emit_capnp_detail
   {
      namespace d = capnp_layout_detail;
      using capnp_parse_detail::capnp_camel;

      [[noreturn]] inline void fail(const std::string& what)
      {
         throw std::runtime_error("emit_capnp: " + what);
      }

      inline std::string text_literal(std::string_view text)
      {
         std::string out = "\"";
         for (char c : text)
         {
            switch (c)
            {
               case '"': out += "\\\""; break;
               case '\\': out += "\\\\"; break;
               case '\n': out += "\\n"; break;
               case '\t': out += "\\t"; break;
               case '\r': out += "\\r"; break;
               default: out += c;
            }
         }
         return out + "\"";
      }

      class Emitter
      {
       public:
         explicit Emitter(const Schema& s) : _s{s}, _layout{s} {}

         std::string run()
         {
            name_types();
            std::string decls, body;
            for (const auto& a : _s.package.attributes)
               if (a.name == "capnp_decl" && a.value)
               {
                  decls += *a.value + "\n";
                  // An import of the JSON annotations the text already has.
                  static constexpr std::string_view import_json =
                     "import \"/capnp/compat/json.capnp\"";
                  std::string_view decl = *a.value;
                  if (decl.starts_with("using ") && decl.find(import_json) != std::string_view::npos)
                  {
                     decl.remove_prefix(6);
                     const auto end = decl.find_first_of(" =");
                     if (end != std::string_view::npos)
                        _json = std::string{decl.substr(0, end)};
                  }
               }
            for (const auto& entry : _s.types)
               if (const auto at = _declared.find(entry.type.get());
                   at != _declared.end() && at->second.parent == nullptr)
                  body += declaration(entry, 0, 0);
            for (const auto& iface : _s.interfaces)
               body += interface(iface.name, iface.funcs, {}, nullptr, 0, 0);

            std::string head = "@" + file_id() + ";\n\n";
            if (_json_used && !_json_declared())
               decls = "using " + _json + " = import \"/capnp/compat/json.capnp\";\n" + decls;
            if (!decls.empty())
               decls += "\n";
            return head + decls + body;
         }

       private:
         struct declared
         {
            std::string    path;    // `Outer.Inner`: how the text refers to it
            std::string    name;    // `Inner`
            const AnyType* parent = nullptr;
         };

         const Schema& _s;
         d::builder    _layout;
         std::map<const AnyType*, declared>                    _declared;
         std::map<const AnyType*, std::vector<const Member*>>  _nested;  // by parent
         std::map<const AnyType*, std::set<std::string>>       _taken;   // names of a scope
         std::string                                           _json      = "Json";
         bool                                                  _json_used = false;

         bool _json_declared() const
         {
            for (const auto& a : _s.package.attributes)
               if (a.name == "capnp_decl" && a.value &&
                   a.value->find("import \"/capnp/compat/json.capnp\"") != std::string::npos &&
                   a.value->starts_with("using " + _json + " "))
                  return true;
            return false;
         }

         // ── Names ───────────────────────────────────────────────────────

         // Whether an Enum is a Cap'n Proto enum on the wire: over UInt16,
         // its values a permutation of 0 .. n-1.
         bool capnp_enum(const Enum& e) const
         {
            const AnyType& under = _layout.types().resolve(*e.type);
            const auto*    i     = std::get_if<Int>(&under.value);
            if (!i || i->bits != 16 || i->isSigned || e.members.empty())
               return false;
            std::vector<bool> seen(e.members.size(), false);
            for (const auto& v : e.members)
            {
               if (v.value >= seen.size() || seen[v.value])
                  return false;
               seen[v.value] = true;
            }
            return true;
         }

         bool declarable(const Member& entry)
         {
            if (entry.name.starts_with('@'))
               return false;
            const AnyType& node = *entry.type;
            if (const auto* e = std::get_if<Enum>(&node.value))
               return capnp_enum(*e);
            if (const auto* t = std::get_if<Type>(&node.value))
            {
               // An alias of a declaration.
               return _layout.types().find(t->type) != nullptr;
            }
            return std::holds_alternative<Object>(node.value) ||
                   std::holds_alternative<Struct>(node.value) ||
                   std::holds_alternative<Variant>(node.value) ||
                   std::holds_alternative<Resource>(node.value);
         }

         std::string unique(const AnyType* scope, std::string name)
         {
            auto&       taken     = _taken[scope];
            std::string candidate = name;
            for (unsigned n = 2; !taken.insert(candidate).second; ++n)
               candidate = name + std::to_string(n);
            return candidate;
         }

         void name_types()
         {
            std::map<std::string_view, const Member*> by_name;
            for (const auto& entry : _s.types)
               if (declarable(entry))
                  by_name.emplace(entry.name, &entry);
            // (a nested type follows its parent in a parsed schema; a parent
            // that comes later is found too, as long as it is not nested in
            // a still later one)
            for (int pass = 0; pass < 2; ++pass)
               for (const auto& [name, entry] : by_name)
               {
                  if (_declared.count(entry->type.get()))
                     continue;
                  const auto    dot    = name.rfind('.');
                  const Member* parent = nullptr;
                  if (dot != std::string_view::npos)
                     if (const auto at = by_name.find(name.substr(0, dot)); at != by_name.end())
                        if (std::holds_alternative<Object>(at->second->type->value) ||
                            std::holds_alternative<Struct>(at->second->type->value) ||
                            std::holds_alternative<Resource>(at->second->type->value))
                           parent = at->second;
                  if (parent && !_declared.count(parent->type.get()))
                  {
                     if (pass == 0)
                        continue;
                     parent = nullptr;
                  }
                  declared decl;
                  if (parent)
                  {
                     decl.parent = parent->type.get();
                     decl.name   = unique(decl.parent, capnp_camel(name.substr(dot + 1), true));
                     decl.path   = _declared.at(decl.parent).path + "." + decl.name;
                  }
                  else
                  {
                     decl.name = unique(nullptr, capnp_camel(name, true));
                     decl.path = decl.name;
                  }
                  _declared.emplace(entry->type.get(), std::move(decl));
               }
            for (const auto& entry : _s.types)
               if (const auto at = _declared.find(entry.type.get());
                   at != _declared.end() && at->second.parent)
                  _nested[at->second.parent].push_back(&entry);
         }

         std::string file_id() const
         {
            if (const std::string* id = d::attribute(_s.package.attributes, "capnp_id"))
               return *id;
            // FNV-1a over what identifies the schema; the first bit set, as
            // the compiler requires of an id.
            std::uint64_t h    = 0xcbf29ce484222325ull;
            const auto    feed = [&](std::string_view text) {
               for (unsigned char c : text)
                  h = (h ^ c) * 0x100000001b3ull;
               h = (h ^ 0xff) * 0x100000001b3ull;
            };
            feed(_s.package.name);
            feed(_s.package.version);
            for (const auto& entry : _s.types)
               if (!entry.name.starts_with('@'))
                  feed(entry.name);
            for (const auto& iface : _s.interfaces)
               feed(iface.name);
            h |= std::uint64_t{1} << 63;
            static const char digits[] = "0123456789abcdef";
            std::string       out      = "0x";
            for (int shift = 60; shift >= 0; shift -= 4)
               out += digits[(h >> shift) & 15];
            return out;
         }

         // ── Types ───────────────────────────────────────────────────────

         // The type of a field, parameter or list element.
         std::string type_text(const AnyType& written, unsigned depth = 0)
         {
            if (depth > 64)
               fail("types nested too deeply");
            // A declared name is written as that name (an alias too); a
            // name the schema does not declare, as it is: an imported file
            // may declare it.
            if (const auto* ref = std::get_if<Type>(&written.value))
            {
               const Member* direct = _layout.types().find(ref->type);
               if (!direct)
               {
                  if (!type_path(ref->type))
                     fail("unknown type '" + ref->type + "'");
                  return ref->type;
               }
               if (const auto at = _declared.find(direct->type.get()); at != _declared.end())
               {
                  const auto* v = std::get_if<Variant>(&direct->type->value);
                  if (!v || _layout.types().enum_shaped(*v))
                     return at->second.path;
               }
            }
            const std::string* name = nullptr;
            const AnyType&     type = _layout.types().resolve(written, &name);
            if (const auto at = _declared.find(&type); at != _declared.end())
            {
               // By name: a struct, an interface, an enum. (A variant that
               // is not an enum is a union where it is used, not a type.)
               const auto* v = std::get_if<Variant>(&type.value);
               if (!v || _layout.types().enum_shaped(*v))
                  if (std::holds_alternative<Type>(written.value))
                     return at->second.path;
            }
            if (const auto* i = std::get_if<Int>(&type.value))
            {
               if (i->bits != 8 && i->bits != 16 && i->bits != 32 && i->bits != 64)
                  fail("an integer of " + std::to_string(i->bits) + " bits has no Cap'n Proto type");
               return (i->isSigned ? "Int" : "UInt") + std::to_string(i->bits);
            }
            if (const auto* f = std::get_if<Float>(&type.value))
            {
               if (f->exp != 8 && f->exp != 11)
                  fail("a float that is not 32 or 64 bits has no Cap'n Proto type");
               return f->exp == 8 ? "Float32" : "Float64";
            }
            if (const auto* c = std::get_if<Custom>(&type.value))
            {
               if (c->id == "bool")
                  return "Bool";
               if (c->id == "string" || c->id == "utf8")
                  return "Text";
               if (c->id == "hex")
                  return "Data";
               if (c->id == "AnyPointer" || c->id == "AnyStruct" || c->id == "AnyList" ||
                   c->id == "Capability")
                  return c->id;
               if (c->id == "map" || c->id == "set")
                  fail("a " + c->id + " has no Cap'n Proto type");
               return type_text(*c->type, depth + 1);
            }
            if (const auto* e = std::get_if<Enum>(&type.value))
               return type_text(*e->type, depth + 1);  // (not a Cap'n Proto enum)
            if (const auto* l = std::get_if<List>(&type.value))
               return "List(" + element_text(*l->type, depth) + ")";
            if (const auto* b = std::get_if<BoundedList>(&type.value))
               return "List(" + element_text(*b->type, depth) + ")";
            if (const auto* a = std::get_if<Array>(&type.value))
               return "List(" + element_text(*a->type, depth) + ")";
            if (std::holds_alternative<Nested>(type.value))
               return "Data";
            if (const auto* t = std::get_if<Tuple>(&type.value))
            {
               if (!t->members.empty())
                  fail("a tuple has no Cap'n Proto type");
               return "Void";
            }
            if (const auto* o = std::get_if<Option>(&type.value))
            {
               const auto inner = _layout.simple_shape(*o->type);
               if (!inner || inner->kind != capnp_slot_kind::pointer ||
                   std::holds_alternative<Option>(_layout.types().resolve(*o->type).value))
                  fail("this optional has no Cap'n Proto type here");
               return type_text(*o->type, depth + 1);
            }
            if (std::holds_alternative<Variant>(type.value))
               fail("a union is a member of a struct, not a type of its own");
            fail("a record without a name is a group, not a type of its own");
         }

         // `Name` or `Outer.Inner`, as Cap'n Proto writes a type's name.
         static bool type_path(std::string_view name)
         {
            bool start = true;
            for (char c : name)
            {
               if (c == '.')
               {
                  if (start)
                     return false;
                  start = true;
                  continue;
               }
               const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
               if (!(letter || (!start && c >= '0' && c <= '9')))
                  return false;
               start = false;
            }
            return !start;
         }

         // Whether a value of `type` is one field: not a union, a group or
         // an optional scalar. (A type the schema does not declare is taken
         // to be one.)
         bool one_field(const AnyType& type) const
         {
            try
            {
               return _layout.simple_shape(type).has_value();
            }
            catch (const std::runtime_error& e)
            {
               if (std::string_view{e.what()}.find("unknown type") == std::string_view::npos)
                  throw;
               return true;
            }
         }

         std::string element_text(const AnyType& element, unsigned depth)
         {
            if (!one_field(element))
               fail("a list of unions, groups or optional scalars has no Cap'n Proto type");
            return type_text(element, depth + 1);
         }

         // ── Members ─────────────────────────────────────────────────────

         std::string annotations(const std::vector<Attribute>& attrs)
         {
            std::string out;
            for (const auto& a : attrs)
               if (a.name.size() > 1 && a.name.front() == '$')
                  out += " " + a.name + (a.value ? "(" + *a.value + ")" : "");
            return out;
         }

         // The Cap'n Proto name of a member, and the annotation that keeps
         // its IR name when the two differ.
         std::string member_name(const std::string& ir, const std::vector<Attribute>& attrs,
                                 std::set<std::string>& taken, std::string& annotation)
         {
            const std::string* kept    = d::attribute(attrs, "capnp_name");
            std::string        written = kept ? *kept : capnp_camel(ir);
            for (unsigned n = 2; !taken.insert(written).second; ++n)
               written = (kept ? *kept : capnp_camel(ir)) + std::to_string(n);
            annotation.clear();
            if (written != ir)
            {
               _json_used = true;
               annotation = " $" + _json + ".name(" + text_literal(ir) + ")";
            }
            return written;
         }

         std::string default_text(const Member& m)
         {
            const std::string* text = d::attribute(m.attributes, "default");
            if (!text || text->empty())
               return {};
            const auto shape = _layout.simple_shape(*m.type);
            if (!shape || shape->kind == capnp_slot_kind::none)
               return {};
            if (shape->kind == capnp_slot_kind::pointer)
               return " = " + *text;  // as Cap'n Proto writes it
            if (shape->real)
            {
               const auto v = d::parse_real(*text);
               if (!v)
                  fail("member '" + m.name + "': '" + *text + "' is not a float");
               if (*v == 0)
                  return std::signbit(*v) ? " = -0.0" : "";
               return " = " + *text;
            }
            if (*text == "false" || *text == "0")
               return {};
            if (shape->lg == 0)
               return " = true";
            const auto v = d::parse_integer(*text);
            if (!v)
               fail("member '" + m.name + "': '" + *text + "' is not an integer");
            // An enumerant by its name.
            const std::string* name = nullptr;
            const AnyType&     type = _layout.types().resolve(*m.type, &name);
            if (_declared.count(&type) && std::holds_alternative<Type>(m.type->value))
            {
               if (const auto* e = std::get_if<Enum>(&type.value))
                  for (const auto& en : e->members)
                     if (static_cast<std::int64_t>(en.value) == *v)
                     {
                        const Member* entry = _layout.types().entry_of(type);
                        const std::vector<Attribute>* attrs = entry ? &entry->attributes : nullptr;
                        const std::string* kept =
                           attrs ? d::attribute(*attrs, "capnp_name." + std::to_string(en.value))
                                 : nullptr;
                        return " = " + (kept ? *kept : capnp_camel(en.name));
                     }
               if (const auto* var = std::get_if<Variant>(&type.value))
                  if (*v >= 0 && static_cast<std::uint64_t>(*v) < var->members.size())
                     return " = " + capnp_camel(var->members[*v].name);
            }
            return " = " + *text;
         }

         // The Cap'n Proto name an enum keeps for an enumerator, as the
         // `capnp_name` attribute member_name() reads.
         static std::vector<Attribute> enumerant_name(const std::vector<Attribute>& attrs,
                                                      const Enumerator&             v)
         {
            if (const std::string* kept = d::attribute(attrs, "capnp_name." + std::to_string(v.value)))
               return {Attribute{"capnp_name", *kept}};
            return {};
         }

         static std::string pad(unsigned indent) { return std::string(2 * indent, ' '); }

         // One member (or union member) and the layout the compiler gives
         // it.
         std::string member(const Member& m, const capnp_field_layout& f, unsigned indent,
                            std::set<std::string>& taken, unsigned depth)
         {
            std::string       annotation;
            const std::string name = member_name(m.name, m.attributes, taken, annotation);
            if (f.kind != capnp_slot_kind::group)
               return pad(indent) + name + " @" + std::to_string(*f.ordinal) + " :" +
                      type_text(*m.type) + default_text(m) + annotation +
                      annotations(m.attributes) + ";\n";
            const AnyType& type  = _layout.types().resolve(*m.type);
            const auto&    scope = f.group.at(0);
            if (std::holds_alternative<Option>(type.value))
            {
               const auto& some = std::get<Option>(type.value).type;
               return pad(indent) + name + " :union" + annotation + annotations(m.attributes) +
                      " {\n" + pad(indent + 1) + "none @" +
                      std::to_string(*scope.fields.at(0).ordinal) + " :Void;\n" + pad(indent + 1) +
                      "some @" + std::to_string(*scope.fields.at(1).ordinal) + " :" +
                      type_text(*some) + ";\n" + pad(indent) + "}\n";
            }
            if (const auto* v = std::get_if<Variant>(&type.value))
            {
               std::string out = pad(indent) + name;
               if (const std::string* own = d::attribute(v->attributes, "ordinal"))
                  out += " @" + *own + "!";
               out += " :union" + annotation + annotations(m.attributes) + " {\n";
               std::set<std::string> inner;
               for (std::size_t i = 0; i < v->members.size(); ++i)
                  out += member(v->members[i], scope.fields.at(i), indent + 1, inner, depth + 1);
               return out + pad(indent) + "}\n";
            }
            const auto* o       = std::get_if<Object>(&type.value);
            const auto& members = o ? o->members : std::get<Struct>(type.value).members;
            return pad(indent) + name + " :group" + annotation + annotations(m.attributes) + " {\n" +
                   members_text(members, scope, indent + 1, depth + 1) + pad(indent) + "}\n";
         }

         std::string members_text(const std::vector<Member>& members,
                                  const capnp_scope_layout& scope, unsigned indent, unsigned depth)
         {
            if (depth > 64)
               fail("groups nested too deeply");
            std::string           out;
            std::set<std::string> taken;
            std::size_t           at = 0;  // in scope.fields
            for (const auto& m : members)
            {
               const AnyType& type = _layout.types().resolve(*m.type);
               const auto*    v    = std::get_if<Variant>(&type.value);
               if (m.name.empty() && v && !_layout.simple_shape(*m.type))
               {
                  // The unnamed union: its members are the scope's.
                  out += pad(indent) + "union";
                  if (d::has_attribute(v->attributes, "ordinal"))
                     fail("the unnamed union cannot have an ordinal of its own");
                  out += annotations(v->attributes) + " {\n";
                  for (const auto& alternative : v->members)
                     out += member(alternative, scope.fields.at(at++), indent + 1, taken, depth);
                  out += pad(indent) + "}\n";
                  continue;
               }
               out += member(m, scope.fields.at(at++), indent, taken, depth);
            }
            return out;
         }

         // ── Declarations ────────────────────────────────────────────────

         std::string head(const Member& entry, const char* keyword)
         {
            std::string out = std::string{keyword} + " " + _declared.at(entry.type.get()).name;
            if (const std::string* id = d::attribute(entry.attributes, "capnp_id"))
               out += " @" + *id;
            return out;
         }

         std::string nested(const AnyType& parent, const std::vector<Attribute>& attrs,
                            unsigned indent, unsigned depth)
         {
            std::string out;
            for (const auto& a : attrs)
               if (a.name == "capnp_decl" && a.value)
                  out += pad(indent) + *a.value + "\n";
            if (const auto at = _nested.find(&parent); at != _nested.end())
               for (const Member* child : at->second)
                  out += declaration(*child, indent, depth + 1);
            return out;
         }

         // (declarations end with an empty line; the last one inside a body
         // does not)
         static std::string inside(std::string text)
         {
            if (text.ends_with("\n\n"))
               text.pop_back();
            return text;
         }

         std::string declaration(const Member& entry, unsigned indent, unsigned depth)
         {
            if (depth > 64)
               fail("declarations nested too deeply");
            const AnyType& node = *entry.type;
            if (const auto* e = std::get_if<Enum>(&node.value))
            {
               std::string out = pad(indent) + head(entry, "enum") + annotations(entry.attributes) +
                                 " {\n";
               std::set<std::string> taken;
               for (const auto& v : e->members)
               {
                  std::string       annotation;
                  const std::string name =
                     member_name(v.name, enumerant_name(entry.attributes, v), taken, annotation);
                  out += pad(indent + 1) + name + " @" + std::to_string(v.value) + annotation + ";\n";
               }
               return out + pad(indent) + "}\n\n";
            }
            if (std::holds_alternative<Type>(node.value))
               return pad(indent) + "using " + _declared.at(&node).name + " = " + type_text(node) +
                      ";\n\n";
            if (const auto* r = std::get_if<Resource>(&node.value))
               return interface(entry.name, r->methods, r->attributes, &entry, indent, depth);
            if (const auto* v = std::get_if<Variant>(&node.value))
            {
               if (_layout.types().enum_shaped(*v))
               {
                  std::string out = pad(indent) + head(entry, "enum") + " {\n";
                  std::set<std::string> taken;
                  for (std::size_t i = 0; i < v->members.size(); ++i)
                  {
                     std::string       annotation;
                     const std::string name = member_name(v->members[i].name, {}, taken, annotation);
                     out += pad(indent + 1) + name + " @" + std::to_string(i) + annotation + ";\n";
                  }
                  return out + pad(indent) + "}\n\n";
               }
               // The struct that holds nothing but the union.
               std::vector<Member> members;
               members.push_back(Member{"", Box<AnyType>{AnyType{Type{entry.name}}}, {}});
               const auto layout = _layout.layout(entry.name, members);
               std::string out = pad(indent) + head(entry, "struct") + " {\n" + pad(indent + 1) +
                                 "union {\n";
               std::set<std::string> taken;
               for (std::size_t i = 0; i < v->members.size(); ++i)
                  out += member(v->members[i], layout.fields.at(i), indent + 2, taken, depth);
               return out + pad(indent + 1) + "}\n" + pad(indent) + "}\n\n";
            }
            const auto* o       = std::get_if<Object>(&node.value);
            const auto& members = o ? o->members : std::get<Struct>(node.value).members;
            const auto& attrs   = o ? o->attributes : std::get<Struct>(node.value).attributes;
            const auto  layout  = _layout.layout(entry.name, members);
            return pad(indent) + head(entry, "struct") + annotations(attrs) + " {\n" +
                   members_text(members, layout, indent + 1, depth) +
                   inside(nested(node, attrs, indent + 1, depth)) + pad(indent) + "}\n\n";
         }

         std::string parameters(const std::vector<Member>& params)
         {
            std::string           out = "(";
            std::set<std::string> taken;
            for (std::size_t i = 0; i < params.size(); ++i)
            {
               const Member&     p = params[i];
               std::string       annotation;
               const std::string name =
                  member_name(parameter_identifier(p.name), p.attributes, taken, annotation);
               // (a parameter's own name is positional: no annotation for
               // the `arg` psio puts in front of a number)
               if (parameter_identifier(p.name) != p.name)
                  annotation.clear();
               if (!one_field(*p.type))
                  fail("a parameter that is a union or a group");
               out += (i ? ", " : "") + name + " :" + type_text(*p.type) + default_text(p) +
                      annotation + annotations(p.attributes);
            }
            return out + ")";
         }

         std::string interface(const std::string& ir_name, const std::vector<Func>& methods,
                               const std::vector<Attribute>& attrs, const Member* entry,
                               unsigned indent, unsigned depth)
         {
            std::string out = pad(indent);
            if (entry)
               out += head(*entry, "interface");
            else
               out += "interface " + unique(nullptr, capnp_camel(ir_name, true));
            if (const std::string* extends = d::attribute(attrs, "capnp_extends"))
               out += " extends(" + *extends + ")";
            out += annotations(attrs) + " {\n";
            std::set<std::string> taken;
            std::size_t           next = 0;
            for (const auto& f : methods)
            {
               std::string line;
               try
               {
                  std::string             annotation;
                  std::vector<Attribute>  none;
                  std::set<std::string>   probe = taken;
                  const std::string name = member_name(f.name, none, probe, annotation);
                  const std::string* ordinal = d::attribute(f.attributes, "ordinal");
                  line = pad(indent + 1) + name + " @" + (ordinal ? *ordinal : std::to_string(next));
                  const std::string* params  = d::attribute(f.attributes, "capnp_params");
                  const std::string* results = d::attribute(f.attributes, "capnp_results");
                  if (params && *params == "struct" && f.params.size() == 1)
                     line += " " + type_text(*f.params[0].type);
                  else
                     line += " " + parameters(f.params);
                  if (results && *results == "stream")
                     line += " -> stream";
                  else if (f.result)
                  {
                     const AnyType& result = **f.result;
                     if (results && *results == "struct")
                        line += " -> " + type_text(result);
                     else if (const auto* o = std::get_if<Object>(&result.value))
                        line += " -> " + parameters(o->members);
                     else
                     {
                        if (!one_field(result))
                           fail("a result that is a union or a group");
                        line += " -> (result :" + type_text(result) + ")";
                     }
                  }
                  line += annotation + annotations(f.attributes) + ";\n";
                  taken = std::move(probe);
                  ++next;
               }
               catch (const std::runtime_error& e)
               {
                  // An interface read from Cap'n Proto text has only
                  // methods Cap'n Proto has: leaving one out would change it.
                  if (entry)
                     throw;
                  std::string_view what = e.what();
                  for (std::string_view prefix : {"emit_capnp: ", "capnp_layout: ", "fbs_layout: "})
                     if (what.starts_with(prefix))
                        what.remove_prefix(prefix.size());
                  line = pad(indent + 1) + "# " + f.name + ": not Cap'n Proto (" + std::string{what} +
                         ")\n";
               }
               out += line;
            }
            if (entry)
               out += inside(nested(*entry->type, attrs, indent + 1, depth));
            return out + pad(indent) + "}\n\n";
         }
      };

   }  // namespace emit_capnp_detail

   /// The Cap'n Proto schema text of `schema`. Throws std::runtime_error
   /// for a schema Cap'n Proto cannot express.
   inline std::string emit_capnp(const Schema& schema)
   {
      try
      {
         emit_capnp_detail::Emitter e{schema};
         return e.run();
      }
      catch (const std::runtime_error& e)
      {
         std::string_view what = e.what();
         if (what.starts_with("emit_capnp: "))
            throw;
         for (std::string_view prefix : {"capnp_layout: ", "fbs_layout: "})
            if (what.starts_with(prefix))
               what.remove_prefix(prefix.size());
         throw std::runtime_error("emit_capnp: " + std::string{what});
      }
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_capnp;
}
