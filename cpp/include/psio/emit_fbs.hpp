#pragma once
//
// psio/emit_fbs.hpp — Schema IR → FlatBuffers `.fbs` schema text.
//
// `psio::emit_fbs(schema)` writes the declarations psio/fbs_layout.hpp
// describes ("How the Schema IR is read as FlatBuffers"): for a schema the
// schema builder made from reflected C++ types, `flatc` accepts the text and
// computes the layout psio/flatbuf.hpp uses for those types
// (tests/flatbuf_schema_interop_tests.cpp exchanges buffers with the code
// `flatc` generates from it); for a schema psio/fbs_parser.hpp read, the
// text declares what the original text declared.
//
// What the text looks like:
//
//   * names are the IR's names; a character FlatBuffers does not allow in a
//     name becomes `_` (`a::Config` is `a__Config`). A union, an enum or a
//     map's entry table that has no name in the IR (an `@N` type of the
//     schema builder) is named after the first member that uses it:
//     `Order_payment`, `Order_stock`;
//   * a declaration is written where the IR has it, after the enums,
//     structs and unions it uses that were not written yet (flatc wants
//     those declared first; a table may be used before it is declared);
//   * `namespace` is the package (`a:b` as `a.b`); a type whose IR name is
//     dotted (`c.d.Name`) is declared in that namespace;
//   * field ids are written (on every field of the table) only where they
//     differ from the declaration order;
//   * a default is written only when it is not zero; an enumerator only
//     gets `= value` when it is not the previous value + 1;
//   * attributes psio does not know are written as metadata and declared
//     with `attribute "name";`;
//   * an Interface whose functions take one table and return one is an
//     `rpc_service`; other functions are not FlatBuffers and are left out.
//
// Throws std::runtime_error ("emit_fbs: ...") for a type FlatBuffers does
// not have: a vector of vectors, a tuple, an integer wider than 64 bits, a
// union of something that is not a table, an optional of an optional.
//
// The normal form: `emit_fbs(parse_fbs(text))` declares what `text`
// declares (flatc's binary schemas of the two are equal), and
// `emit_fbs(parse_fbs(emit_fbs(s))) == emit_fbs(s)` for every schema.

#include <psio/fbs_layout.hpp>
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

   namespace emit_fbs_detail
   {
      namespace d = fbs_layout_detail;

      [[noreturn]] inline void fail(const std::string& what)
      {
         throw std::runtime_error("emit_fbs: " + what);
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
         out.reserve(name.size() + 1);
         for (char c : name)
            out += ident_char(c) ? c : '_';
         if (out.empty() || (out.front() >= '0' && out.front() <= '9'))
            out.insert(out.begin(), '_');
         return out;
      }

      inline std::string fbs_namespace(std::string_view pkg)
      {
         std::string out;
         out.reserve(pkg.size());
         for (char c : pkg)
            out += c == ':' ? '.' : ident_char(c) || c == '.' ? c : '_';
         return out;
      }

      // Attributes that are the IR's own or another format's: not metadata.
      inline bool not_metadata(std::string_view name) noexcept
      {
         static constexpr std::string_view names[] = {
            "field", "default", "value", "lengthBound", "jsonInt", "ijsonInt", "doc",
            "description", "visible", "definitionWillNotChange", "maxFields", "maxDynamicData",
            "tuple", "untagged", "ordinal", "since", "unstable"};
         for (auto n : names)
            if (n == name)
               return true;
         return name.starts_with("capnp_") || name.starts_with("fbs_") ||
                name.starts_with("proto_") || !is_identifier(name);
      }

      // The attributes flatc knows without a declaration.
      inline bool builtin_attribute(std::string_view name) noexcept
      {
         static constexpr std::string_view names[] = {
            "id", "deprecated", "required", "force_align", "bit_flags", "original_order",
            "nested_flatbuffer", "flexbuffer", "key", "hash", "csharp_partial", "streaming",
            "idempotent", "cpp_type", "cpp_ptr_type", "cpp_ptr_type_get", "cpp_str_type",
            "cpp_str_flex_ctor", "cpp_vec_type", "native_inline", "native_default",
            "native_custom_alloc", "native_type", "native_type_pack_name", "shared", "private",
            "offset64", "vector64"};
         for (auto n : names)
            if (n == name)
               return true;
         return false;
      }

      inline std::string string_literal(std::string_view text)
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
               case '\b': out += "\\b"; break;
               case '\f': out += "\\f"; break;
               default: out += c;
            }
         }
         return out + "\"";
      }

      class Emitter
      {
       public:
         explicit Emitter(const Schema& s)
            : _s{s}, _types{s}, _package{fbs_namespace(s.package.name)}
         {
         }

         std::string run()
         {
            // Names first: a declaration may be referred to before it is
            // written.
            for (const auto& entry : _s.types)
               if (declarable(*entry.type) && !entry.name.starts_with('@'))
               {
                  name_entry(entry);
                  _own.insert(entry.type.get());
               }
            for (const auto& entry : _s.types)
            {
               if (declarable(*entry.type) && !entry.name.starts_with('@'))
                  declare(*entry.type);
               drain();
            }
            for (const auto& iface : _s.interfaces)
            {
               service(iface);
               drain();
            }

            std::string head;
            for (const auto& a : _s.package.attributes)
               if (a.name == "fbs_include" && a.value)
                  head += "include " + string_literal(*a.value) + ";\n";
            if (!head.empty())
               head += "\n";
            for (const auto& name : _user_attributes)
               head += "attribute " + string_literal(name) + ";\n";
            if (!_user_attributes.empty())
               head += "\n";

            if (const std::string* root = d::attribute(_s.package.attributes, "fbs_root_type"))
            {
               const Member*  entry = _types.find(*root);
               const AnyType* node  = entry ? entry->type.get() : nullptr;
               if (!node || !_names.count(node))
                  fail("the root type '" + *root + "' is not a table of the schema");
               enter(_names.at(node).ns);
               _out += "root_type " + _names.at(node).name + ";\n";
            }
            if (const std::string* id = d::attribute(_s.package.attributes, "fbs_file_identifier"))
            {
               if (id->size() != 4)
                  fail("a file identifier is 4 characters");
               _out += "file_identifier " + string_literal(*id) + ";\n";
            }
            if (const std::string* ext = d::attribute(_s.package.attributes, "fbs_file_extension"))
               _out += "file_extension " + string_literal(*ext) + ";\n";
            return head + _out;
         }

       private:
         struct declared_name
         {
            std::string ns;
            std::string name;
         };

         const Schema& _s;
         d::model      _types;
         std::string   _package;
         std::string   _out;
         std::string   _current;       // the namespace `_out` is in
         bool          _in_namespace = false;

         std::map<const AnyType*, declared_name> _names;
         std::set<std::string>                   _taken;    // ns.name
         std::set<const AnyType*>                _own;      // the schema's named declarations
         std::set<const AnyType*>                _written;  // (or being written)
         std::vector<const AnyType*>             _queue;    // tables to write
         std::set<std::string>                   _user_attributes;
         // Entry tables of maps: by the map's node.
         std::map<const AnyType*, declared_name> _entries;

         static bool declarable(const AnyType& node)
         {
            return std::holds_alternative<Object>(node.value) ||
                   std::holds_alternative<Struct>(node.value) ||
                   std::holds_alternative<Enum>(node.value) ||
                   std::holds_alternative<Variant>(node.value);
         }

         declared_name unique(std::string ns, std::string name)
         {
            std::string candidate = name;
            for (unsigned n = 2; !_taken.insert(ns + "." + candidate).second; ++n)
               candidate = name + "_" + std::to_string(n);
            return {std::move(ns), std::move(candidate)};
         }

         void name_entry(const Member& entry)
         {
            const declared_name n = split_name(entry.name);
            _names.emplace(entry.type.get(), unique(n.ns, n.name));
         }

         // `c.d.Name` is Name of namespace c.d; any other name is of the
         // package's namespace.
         declared_name split_name(const std::string& ir) const
         {
            const auto dot = ir.rfind('.');
            if (dot != std::string::npos)
            {
               bool        ok = true;
               std::size_t at = 0;
               while (ok && at <= ir.size())
               {
                  const auto end = std::min(ir.find('.', at), ir.size());
                  ok             = is_identifier(std::string_view{ir}.substr(at, end - at));
                  at             = end + 1;
               }
               if (ok)
                  return {ir.substr(0, dot), ir.substr(dot + 1)};
            }
            return {_package, identifier(ir)};
         }

         // The name of a declaration; one without a name in the IR is named
         // after its first use.
         const declared_name& name_of(const AnyType& node, const declared_name& owner,
                                      std::string_view member)
         {
            auto at = _names.find(&node);
            if (at == _names.end())
               at = _names
                       .emplace(&node,
                                unique(owner.ns, owner.name + "_" + identifier(member)))
                       .first;
            return at->second;
         }

         static std::string reference(const declared_name& to, const declared_name& from)
         {
            if (to.ns == from.ns)
               return to.name;
            // (a type of the global namespace cannot be named from inside
            // another one that declares the same name: left to flatc)
            return to.ns.empty() ? to.name : to.ns + "." + to.name;
         }

         void enter(const std::string& ns)
         {
            if (_in_namespace ? ns == _current : ns.empty())
               return;
            if (ns.empty())
               fail("a type of the global namespace after a namespace was entered");
            _out += "namespace " + ns + ";\n\n";
            _current      = ns;
            _in_namespace = true;
         }

         void drain()
         {
            // (in the order they were met; a declaration may add more)
            for (std::size_t i = 0; i < _queue.size(); ++i)
               declare(*_queue[i]);
            _queue.clear();
         }

         // The tables a declaration written ahead of its user refers to
         // follow it, as they do when that declaration is one of the
         // schema's own (which is what the parser makes of this text). Not
         // past a nesting no schema written by hand has: those tables wait
         // for the enclosing declaration.
         void write_queued_from(std::size_t mark)
         {
            if (_depth > 48)
               return;
            ++_depth;
            for (std::size_t i = mark; i < _queue.size(); ++i)
               declare(*_queue[i]);
            _queue.resize(mark);
            --_depth;
         }
         unsigned _depth = 0;

         // Writes the declaration of `node` (which has a name) unless it is
         // written. Recursion is through structs inside structs and enums /
         // unions used by a record: bounded by the model's depth.
         void declare(const AnyType& node)
         {
            if (!_written.insert(&node).second)
               return;
            const declared_name self = _names.at(&node);
            std::string         text;
            if (const auto* e = std::get_if<Enum>(&node.value))
               text = enumeration(self, node, *e);
            else if (const auto* v = std::get_if<Variant>(&node.value))
               text = _types.enum_shaped(*v) ? plain_enumeration(self, *v) : union_type(self, node, *v);
            else
            {
               const auto* o       = std::get_if<Object>(&node.value);
               const auto& members = o ? o->members : std::get<Struct>(node.value).members;
               const auto& attrs   = o ? o->attributes : std::get<Struct>(node.value).attributes;
               text = _types.shape_of(node).k == d::kind::structure
                         ? structure(self, members, attrs)
                         : table(self, members, attrs);
            }
            enter(self.ns);
            _out += text;
         }

         // ── Types ───────────────────────────────────────────────────────

         static const char* scalar_name(const d::shape& e)
         {
            if (e.k == d::kind::boolean)
               return "bool";
            if (e.k == d::kind::real)
               return e.size == 4 ? "float" : "double";
            switch (e.size)
            {
               case 1: return e.is_signed ? "byte" : "ubyte";
               case 2: return e.is_signed ? "short" : "ushort";
               case 4: return e.is_signed ? "int" : "uint";
               default: return e.is_signed ? "long" : "ulong";
            }
         }

         // A scalar, enum, struct, table, string or union, by name.
         std::string simple(const d::shape& e, const declared_name& owner, std::string_view member)
         {
            switch (e.k)
            {
               case d::kind::integer:
               case d::kind::real:
               case d::kind::boolean:
                  return scalar_name(e);
               case d::kind::string:
                  return "string";
               case d::kind::enumeration:
               case d::kind::structure:
               case d::kind::union_type:
               {
                  // Written before whatever uses it (flatc needs the size
                  // of a struct and the type of an enum where they are used).
                  const declared_name to   = name_of(*e.node, owner, member);
                  const std::size_t   mark = _queue.size();
                  declare(*e.node);
                  write_queued_from(mark);
                  return reference(to, owner);
               }
               case d::kind::table:
               {
                  const declared_name to = name_of(*e.node, owner, member);
                  // One of the schema's own declarations is written where
                  // the schema has it (a table may be used before it is
                  // declared); one without a name of its own, after its user.
                  if (!_written.count(e.node) && !_own.count(e.node))
                     _queue.push_back(e.node);
                  return reference(to, owner);
               }
               default:
                  fail("member '" + std::string{member} + "' of '" + owner.name +
                       "': a vector of vectors has no FlatBuffers type");
            }
         }

         // The element type of the vector `node` is (a node shape_of()
         // calls a vector), written as `[T]`.
         std::string vector(const d::shape& e, const declared_name& owner, std::string_view member)
         {
            const AnyType* node = e.node;
            const AnyType* elem = nullptr;
            if (const auto* c = std::get_if<Custom>(&node->value))
            {
               if (c->id == "map")
                  return "[" + map_entry(*node, *c, owner, member) + "]";
               node = &_types.resolve(*c->type);  // hex, set: over a list
            }
            if (const auto* l = std::get_if<List>(&node->value))
               elem = l->type.get();
            else if (const auto* b = std::get_if<BoundedList>(&node->value))
               elem = b->type.get();
            else if (const auto* a = std::get_if<Array>(&node->value))
               elem = a->type.get();
            else if (std::holds_alternative<Nested>(node->value))
               return "[ubyte]";
            else
               fail("member '" + std::string{member} + "' of '" + owner.name +
                    "': not a list");
            const d::shape s = _types.shape_of(*elem, 2);
            if (s.optional || s.k == d::kind::unit || s.k == d::kind::array)
               fail("member '" + std::string{member} + "' of '" + owner.name +
                    "': the elements have no FlatBuffers type");
            return "[" + simple(s, owner, member) + "]";
         }

         // `table Owner_member { key:K (key); value:V; }` for a map.
         std::string map_entry(const AnyType& map, const Custom& c, const declared_name& owner,
                               std::string_view member)
         {
            auto at = _entries.find(&map);
            if (at == _entries.end())
            {
               const AnyType* list  = &_types.resolve(*c.type);
               const AnyType* entry = nullptr;
               if (const auto* l = std::get_if<List>(&list->value))
                  entry = &_types.resolve(*l->type);
               else if (const auto* b = std::get_if<BoundedList>(&list->value))
                  entry = &_types.resolve(*b->type);
               const auto* pair = entry ? std::get_if<Tuple>(&entry->value) : nullptr;
               if (!pair || pair->members.size() != 2)
                  fail("member '" + std::string{member} + "' of '" + owner.name +
                       "': a map is over a list of (key, value)");
               const declared_name self = unique(owner.ns, owner.name + "_" + identifier(member));
               at                       = _entries.emplace(&map, self).first;
               const d::shape key       = _types.shape_of(*pair->members[0], 2);
               if (!(key.k == d::kind::string || (key.scalar() && key.k != d::kind::boolean)) ||
                   key.optional)
                  fail("member '" + std::string{member} + "' of '" + owner.name +
                       "': a map's key is an integer, a float, an enum or a string");
               std::string text = "table " + self.name + " {\n  key:" +
                                  simple(key, self, "key") + " (key);\n";
               const std::size_t mark = _queue.size();
               text += "  value:" + field_type(*pair->members[1], self, "value", false).text + ";\n}\n\n";
               enter(self.ns);
               _out += text;
               write_queued_from(mark);
            }
            return reference(at->second, owner);
         }

         struct field_text
         {
            std::string text;
            d::shape    shape;
         };

         field_text field_type(const AnyType& written, const declared_name& owner,
                               std::string_view member, bool in_struct)
         {
            const d::shape e = _types.shape_of(written, 1);
            switch (e.k)
            {
               case d::kind::unit:
                  fail("member '" + std::string{member} + "' of '" + owner.name + "' has no value");
               case d::kind::vector:
                  return {vector(e, owner, member), e};
               case d::kind::array:
               {
                  if (!in_struct)
                     return {vector(e, owner, member), e};
                  const auto&    a = std::get<Array>(e.node->value);
                  const d::shape s = _types.shape_of(*a.type, 2);
                  if (s.k == d::kind::array)
                     fail("member '" + std::string{member} + "' of '" + owner.name +
                          "': an array of arrays has no FlatBuffers type");
                  return {"[" + simple(s, owner, member) + ":" + std::to_string(a.len) + "]", e};
               }
               default:
                  return {simple(e, owner, member), e};
            }
         }

         // ── Declarations ────────────────────────────────────────────────

         std::string metadata(const std::vector<Attribute>& attrs,
                              std::vector<std::string>      first = {})
         {
            for (const auto& a : attrs)
            {
               if (not_metadata(a.name) || a.name == "id")
                  continue;
               if (!builtin_attribute(a.name))
                  _user_attributes.insert(a.name);
               // `deprecated` carries a version in the IR of a C++ type.
               if (!a.value || a.name == "deprecated" || a.name == "required" || a.name == "key")
                  first.push_back(a.name);
               else if (d::parse_integer(*a.value))
                  first.push_back(a.name + ": " + *a.value);
               else
                  first.push_back(a.name + ": " + string_literal(*a.value));
            }
            if (first.empty())
               return {};
            std::string out = " (";
            for (std::size_t i = 0; i < first.size(); ++i)
               out += (i ? ", " : "") + first[i];
            return out + ")";
         }

         // Whether flatc takes a field of the enum `e` whose default is the
         // member's: the default (zero when there is none) must be one of
         // the enumerators. Where it is not — a C++ enum without a zero
         // that a member leaves at zero — the field is written as the
         // enum's integer, which is the same wire.
         bool enum_field(const Member* m, const d::shape& e)
         {
            if (e.k != d::kind::enumeration)
               return false;
            if (e.optional)
               return true;
            std::int64_t value = 0;
            if (m)
               if (const std::string* text = d::attribute(m->attributes, "default"))
               {
                  if (*text == "true")
                     value = 1;
                  else if (const auto v = d::parse_integer(*text))
                     value = *v;
               }
            if (const auto* en = std::get_if<Enum>(&e.node->value))
            {
               if (value == 0 && d::has_attribute(entry_attributes(*e.node), "bit_flags"))
                  return true;
               for (const auto& v : en->members)
                  if (static_cast<std::int64_t>(v.value) == value)
                     return true;
               return false;
            }
            // A variant of alternatives without values: 0 .. n-1.
            const auto& v = std::get<Variant>(e.node->value);
            return value >= 0 && static_cast<std::uint64_t>(value) < v.members.size();
         }

         std::string default_text(const Member& m, const d::shape& e)
         {
            if (e.optional)
               return e.scalar() ? " = null" : "";
            const std::string* text = d::attribute(m.attributes, "default");
            if (!text || text->empty())
               return {};
            if (e.k == d::kind::string)
               return text->starts_with('"') ? " = " + *text : " = " + string_literal(*text);
            if (!e.scalar())
               return {};
            if (e.k == d::kind::real)
            {
               const auto v = d::parse_real(*text);
               if (!v)
                  fail("member '" + m.name + "': '" + *text + "' is not a float");
               return *v == 0 && !std::signbit(*v) ? "" : " = " + *text;
            }
            std::int64_t value = 0;
            if (*text == "true" || *text == "false")
               value = *text == "true";
            else if (const auto v = d::parse_integer(*text))
               value = *v;
            else
               fail("member '" + m.name + "': '" + *text + "' is not an integer");
            if (value == 0)
               return {};
            if (e.k == d::kind::boolean)
               return " = true";
            if (e.k == d::kind::enumeration)
               if (const auto* en = std::get_if<Enum>(&e.node->value))
                  for (const auto& v : en->members)
                     if (static_cast<std::int64_t>(v.value) == value)
                        return " = " + identifier(v.name);
            return " = " + (e.is_signed ? std::to_string(value)
                                        : std::to_string(static_cast<std::uint64_t>(value)));
         }

         std::string table(const declared_name& self, const std::vector<Member>& members,
                           const std::vector<Attribute>& attrs)
         {
            // The slots, and whether they are the declaration order.
            fbs_type_layout layout;
            layout.name = self.name;
            d::table_fields(_types, members, layout);
            bool          in_order = true;
            std::uint32_t next     = 0;
            for (const auto& f : layout.fields)
            {
               if (f.slot != next)
                  in_order = false;
               next = f.slot + (f.is_union ? 2 : 1);
            }
            std::string body;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               const Member& m = members[i];
               if (!is_identifier(m.name))
                  fail("'" + m.name + "' of '" + self.name + "' is not a field name");
               field_text t = field_type(*m.type, self, m.name, false);
               if (t.shape.k == d::kind::enumeration && !enum_field(&m, t.shape))
                  t.text = scalar_name(t.shape), t.shape.k = d::kind::integer;
               std::vector<std::string> first;
               if (!in_order)
                  first.push_back("id: " + std::to_string(layout.fields[i].slot +
                                                          (layout.fields[i].is_union ? 1 : 0)));
               body += "  " + m.name + ":" + t.text + default_text(m, t.shape) +
                       metadata(m.attributes, std::move(first)) + ";\n";
            }
            return "table " + self.name + metadata(attrs) + " {\n" + body + "}\n\n";
         }

         std::string structure(const declared_name& self, const std::vector<Member>& members,
                               const std::vector<Attribute>& attrs)
         {
            std::string body;
            for (const auto& m : members)
            {
               if (!is_identifier(m.name))
                  fail("'" + m.name + "' of '" + self.name + "' is not a field name");
               // (a struct member has no default: its enum needs a zero)
               field_text t = field_type(*m.type, self, m.name, true);
               if (t.shape.k == d::kind::enumeration && !enum_field(nullptr, t.shape))
                  t.text = scalar_name(t.shape);
               body += "  " + m.name + ":" + t.text + metadata(m.attributes) + ";\n";
            }
            return "struct " + self.name + metadata(attrs) + " {\n" + body + "}\n\n";
         }

         const std::vector<Attribute>& entry_attributes(const AnyType& node) const
         {
            static const std::vector<Attribute> none;
            const Member*                       entry = _types.entry_of(node);
            return entry ? entry->attributes : none;
         }

         std::string enumeration(const declared_name& self, const AnyType& node, const Enum& e)
         {
            const d::shape s     = _types.shape_of(node);
            const auto&    attrs = entry_attributes(node);
            const bool     flags = d::has_attribute(attrs, "bit_flags");
            std::string    body;
            std::int64_t   next  = 0;
            if (e.members.empty())
               fail("enum '" + self.name + "' has no enumerators");
            for (std::size_t i = 0; i < e.members.size(); ++i)
            {
               // The value as the underlying type holds it.
               std::int64_t value = static_cast<std::int64_t>(e.members[i].value);
               if (flags)
               {
                  const std::uint64_t bits = e.members[i].value;
                  if (bits == 0 || (bits & (bits - 1)) != 0)
                     fail("enum '" + self.name + "': a bit flag that is not one bit");
                  value = 0;
                  while ((bits >> value) != 1)
                     ++value;
               }
               body += "  " + identifier(e.members[i].name);
               if (value != next || (i == 0 && value != 0))
                  body += " = " + (s.is_signed || flags
                                      ? std::to_string(value)
                                      : std::to_string(static_cast<std::uint64_t>(value)));
               body += i + 1 < e.members.size() ? ",\n" : "\n";
               next = value == std::numeric_limits<std::int64_t>::max() ? value : value + 1;
            }
            return "enum " + self.name + " : " + scalar_name(s) + metadata(attrs) + " {\n" + body +
                   "}\n\n";
         }

         std::string plain_enumeration(const declared_name& self, const Variant& v)
         {
            if (v.members.size() > 256)
               fail("enum '" + self.name + "' has more than 256 enumerators");
            std::string body;
            for (std::size_t i = 0; i < v.members.size(); ++i)
               body += "  " + identifier(v.members[i].name) +
                       (i + 1 < v.members.size() ? ",\n" : "\n");
            return "enum " + self.name + " : ubyte {\n" + body + "}\n\n";
         }

         std::string union_type(const declared_name& self, const AnyType& node, const Variant& v)
         {
            // (the discriminants are checked by the layout)
            d::enum_layout(_types, self.name, node);
            std::string body;
            for (std::size_t i = 0; i < v.members.size(); ++i)
            {
               const Member& m = v.members[i];
               if (_types.is_unit(*m.type))
                  continue;  // NONE
               const d::shape e = _types.shape_of(*m.type, 1);
               if ((e.k != d::kind::table && e.k != d::kind::structure &&
                    e.k != d::kind::string) ||
                   e.optional)
                  fail("union '" + self.name + "': member '" + m.name + "' is not a table");
               const std::string type = simple(e, self, m.name);
               std::string       plain = type;
               for (char& c : plain)
                  if (c == '.')
                     c = '_';
               if (!body.empty())
                  body += ",\n";
               body += "  " + (plain == m.name ? type : identifier(m.name) + ":" + type);
               if (const std::string* value = d::attribute(m.attributes, "value"))
                  body += " = " + *value;
            }
            return "union " + self.name + metadata(v.attributes) + " {\n" + body + "\n}\n\n";
         }

         void service(const Interface& iface)
         {
            const declared_name self = split_name(iface.name);
            std::string         body;
            for (const auto& f : iface.funcs)
            {
               // A method takes one table and returns one.
               if (f.params.size() != 1 || !f.result)
                  continue;
               const d::shape request  = _types.shape_of(*f.params[0].type, 1);
               const d::shape response = _types.shape_of(**f.result, 1);
               if (request.k != d::kind::table || response.k != d::kind::table ||
                   request.optional || response.optional)
                  continue;
               body += "  " + identifier(f.name) + "(" + simple(request, self, f.name) + "):" +
                       simple(response, self, f.name) + metadata(f.attributes) + ";\n";
            }
            if (body.empty())
               return;
            drain();
            enter(self.ns);
            _out += "rpc_service " + self.name + " {\n" + body + "}\n\n";
         }
      };

   }  // namespace emit_fbs_detail

   /// The FlatBuffers schema text of `schema`. Throws std::runtime_error
   /// for a schema FlatBuffers cannot express.
   inline std::string emit_fbs(const Schema& schema)
   {
      try
      {
         emit_fbs_detail::Emitter e{schema};
         return e.run();
      }
      catch (const std::runtime_error& e)
      {
         std::string_view what = e.what();
         if (what.starts_with("emit_fbs: "))
            throw;
         if (what.starts_with("fbs_layout: "))
            what.remove_prefix(12);
         throw std::runtime_error("emit_fbs: " + std::string{what});
      }
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_fbs;
}
