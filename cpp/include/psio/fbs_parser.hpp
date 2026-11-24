#pragma once
//
// psio/fbs_parser.hpp — FlatBuffers `.fbs` schema text → Schema IR.
//
// `psio::parse_fbs(text)` reads the schema language of `flatc` and keeps
// everything that decides the wire or the meaning of a buffer, in the form
// psio/fbs_layout.hpp documents ("How the Schema IR is read as
// FlatBuffers") and psio/emit_fbs.hpp writes back:
//
//   namespace a.b;            Schema::package.name (`a:b`) for the first
//                             one; a type declared in another namespace is
//                             named in full (`c.d.Name`)
//   table / struct            Object / Struct; names are kept as written
//   field:type = default (metadata);
//                             a Member; `= null` makes it an Option; the
//                             default (an enumerator's name as its value)
//                             is the `default` attribute; `(id: K)` is
//                             `field` = K + 1; `deprecated`, `required`,
//                             `key` and every other metadata entry are
//                             attributes of the same name
//   [T], [T:N]                List, Array
//   enum E : type { A, B = 4 }   Enum over that integer, every value
//                             (`bit_flags`: the flag, 1 << n)
//   union U { A, B, x:C }     Variant of references, named by alias or type
//   struct S (force_align: 8)    the `force_align` attribute
//   root_type, file_identifier, file_extension, include
//                             package attributes `fbs_root_type`,
//                             `fbs_file_identifier`, `fbs_file_extension`,
//                             `fbs_include` (one per file, not followed)
//   rpc_service               an Interface: one function per method, taking
//                             `request` and returning the response
//
// Not kept: comments (documentation too), `attribute` declarations (the
// emitter declares the ones a schema uses), metadata of enumerators.
// Refused: nested vectors, JSON data, ids on some fields of a table only or
// ids that are not 0, 1, 2, ... (as `flatc` refuses them), a struct with a
// member that is not fixed.
//
// The text is untrusted: the parser does not recurse, allocates in
// proportion to the text, and reports every malformed input as
// `fbs_parse_error` (line and column).

#include <psio/fbs_layout.hpp>
#include <psio/schema_ir.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace psio::schema_types
{

   struct fbs_parse_error : std::runtime_error
   {
      std::uint32_t line;
      std::uint32_t column;
      fbs_parse_error(const std::string& msg, std::uint32_t l, std::uint32_t c)
         : std::runtime_error("fbs:" + std::to_string(l) + ":" +
                              std::to_string(c) + ": " + msg),
           line{l},
           column{c}
      {
      }
   };

   namespace fbs_parse_detail
   {
      enum class tok : std::uint8_t
      {
         ident,
         number,  // digits, hex, a float: no sign
         string,  // the contents, escapes resolved
         punct,   // one character
         eof
      };

      struct token
      {
         tok           kind = tok::eof;
         std::string   text;
         std::uint32_t line   = 1;
         std::uint32_t column = 1;

         bool is(char c) const noexcept
         {
            return kind == tok::punct && text.size() == 1 && text[0] == c;
         }
         bool is_word(std::string_view w) const noexcept { return kind == tok::ident && text == w; }
      };

      inline bool ident_start(char c) noexcept
      {
         return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
      }
      inline bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
      inline bool ident_char(char c) noexcept { return ident_start(c) || digit(c); }

      class Lexer
      {
       public:
         explicit Lexer(std::string_view src) : _src{src} { next(); }

         const token& peek() const noexcept { return _tok; }

         token take()
         {
            token t = std::move(_tok);
            next();
            return t;
         }

       private:
         std::string_view _src;
         std::size_t      _pos    = 0;
         std::uint32_t    _line   = 1;
         std::uint32_t    _column = 1;
         token            _tok;

         bool eof() const noexcept { return _pos >= _src.size(); }
         char at(std::size_t off = 0) const noexcept
         {
            return _pos + off < _src.size() ? _src[_pos + off] : '\0';
         }
         void advance() noexcept
         {
            if (_src[_pos++] == '\n')
            {
               ++_line;
               _column = 1;
            }
            else
               ++_column;
         }
         [[noreturn]] void fail(const std::string& what) const
         {
            throw fbs_parse_error(what, _line, _column);
         }

         void skip_space()
         {
            while (!eof())
            {
               const char c = at();
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                  advance();
               else if (c == '/' && at(1) == '/')
                  while (!eof() && at() != '\n')
                     advance();
               else if (c == '/' && at(1) == '*')
               {
                  advance();
                  advance();
                  while (!eof() && !(at() == '*' && at(1) == '/'))
                     advance();
                  if (eof())
                     fail("a comment that does not end");
                  advance();
                  advance();
               }
               else
                  break;
            }
         }

         void next()
         {
            skip_space();
            _tok        = token{};
            _tok.line   = _line;
            _tok.column = _column;
            if (eof())
               return;
            const char c = at();
            if (ident_start(c))
            {
               _tok.kind = tok::ident;
               while (!eof() && ident_char(at()))
               {
                  _tok.text.push_back(at());
                  advance();
               }
            }
            else if (digit(c) || (c == '.' && digit(at(1))))
            {
               _tok.kind = tok::number;
               while (!eof())
               {
                  const char d = at();
                  const bool exponent_sign =
                     (d == '+' || d == '-') && !_tok.text.empty() &&
                     (_tok.text.back() == 'e' || _tok.text.back() == 'E' ||
                      _tok.text.back() == 'p' || _tok.text.back() == 'P') &&
                     !(_tok.text.size() > 1 && (_tok.text[1] == 'x' || _tok.text[1] == 'X') &&
                       (_tok.text.back() == 'e' || _tok.text.back() == 'E'));
                  if (!(ident_char(d) || d == '.' || exponent_sign))
                     break;
                  _tok.text.push_back(d);
                  advance();
               }
            }
            else if (c == '"' || c == '\'')
            {
               _tok.kind = tok::string;
               advance();
               while (true)
               {
                  if (eof() || at() == '\n')
                     fail("a string that does not end");
                  char d = at();
                  advance();
                  if (d == c)
                     break;
                  if (d == '\\')
                  {
                     if (eof())
                        fail("a string that does not end");
                     d = at();
                     advance();
                     switch (d)
                     {
                        case 'n': d = '\n'; break;
                        case 't': d = '\t'; break;
                        case 'r': d = '\r'; break;
                        case 'b': d = '\b'; break;
                        case 'f': d = '\f'; break;
                        case '"': case '\'': case '\\': case '/': break;
                        default: fail("an escape psio does not read");
                     }
                  }
                  _tok.text.push_back(d);
               }
            }
            else
            {
               _tok.kind = tok::punct;
               _tok.text.push_back(c);
               advance();
            }
         }
      };

      // FBS namespace `wasi.io` → IR package name `wasi:io`.
      inline std::string fbs_to_pkg(std::string_view ns)
      {
         std::string out;
         out.reserve(ns.size());
         for (char c : ns)
            out += (c == '.' ? ':' : c);
         return out;
      }

      // Attribute names the IR gives a meaning of its own: a `.fbs` file
      // may not use them for something else.
      inline bool reserved_attribute(std::string_view name) noexcept
      {
         return name == "field" || name == "default" || name == "value" ||
                name == "definitionWillNotChange" || name == "maxFields" ||
                name == "maxDynamicData" || name == "lengthBound";
      }

      class Parser
      {
       public:
         explicit Parser(std::string_view src) : _lex{src} {}

         Schema run()
         {
            while (_lex.peek().kind != tok::eof)
            {
               const token& t = _lex.peek();
               if (t.kind != tok::ident)
                  fail(t, t.is('{') ? "JSON data is not read" : "expected a declaration");
               if (t.text == "namespace")
                  parse_namespace();
               else if (t.text == "table" || t.text == "struct")
                  parse_record();
               else if (t.text == "enum")
                  parse_enum();
               else if (t.text == "union")
                  parse_union();
               else if (t.text == "root_type")
               {
                  _lex.take();
                  const token at = _lex.peek();
                  _root          = pending_name{dotted_name(), _namespace, at.line, at.column};
                  expect(';');
               }
               else if (t.text == "file_identifier" || t.text == "file_extension" ||
                        t.text == "include" || t.text == "native_include")
               {
                  const token key   = _lex.take();
                  const token value = _lex.take();
                  if (value.kind != tok::string)
                     fail(value, "expected a string");
                  if (key.text == "file_identifier" && value.text.size() != 4)
                     fail(value, "a file identifier is 4 characters");
                  expect(';');
                  if (key.text != "include" && key.text != "native_include")
                     drop_attribute(_s.package.attributes, "fbs_" + key.text);
                  _s.package.attributes.push_back(Attribute{"fbs_" + key.text, value.text});
               }
               else if (t.text == "attribute")
               {
                  _lex.take();
                  const token name = _lex.take();
                  if (name.kind != tok::string && name.kind != tok::ident)
                     fail(name, "expected an attribute name");
                  expect(';');
               }
               else if (t.text == "rpc_service")
                  parse_service();
               else
                  fail(t, "expected a declaration");
            }
            finish();
            return std::move(_s);
         }

       private:
         struct pending_name
         {
            std::string   written;
            std::string   ns;
            std::uint32_t line   = 0;
            std::uint32_t column = 0;
         };
         struct declaration
         {
            std::string   full_name;  // namespace.Name
            std::string   ns;
            std::uint32_t line   = 0;
            std::uint32_t column = 0;
         };

         Lexer                    _lex;
         Schema                   _s;
         std::string              _namespace;
         bool                     _have_package = false;
         std::vector<declaration> _decls;  // parallel to _s.types
         std::set<std::string>    _full_names, _ir_names, _service_names;
         std::optional<pending_name> _root;
         std::vector<std::string>    _service_ns;  // parallel to _s.interfaces

         [[noreturn]] static void fail(const token& at, const std::string& what)
         {
            throw fbs_parse_error(what, at.line, at.column);
         }

         static void drop_attribute(std::vector<Attribute>& attrs, std::string_view name)
         {
            std::erase_if(attrs, [&](const Attribute& a) { return a.name == name; });
         }

         void expect(char c)
         {
            const token t = _lex.take();
            if (!t.is(c))
               fail(t, std::string{"expected '"} + c + "'");
         }

         std::string ident(const char* what = "a name")
         {
            token t = _lex.take();
            if (t.kind != tok::ident)
               fail(t, std::string{"expected "} + what);
            return std::move(t.text);
         }

         std::string dotted_name()
         {
            std::string out = ident();
            while (_lex.peek().is('.'))
            {
               _lex.take();
               out += '.';
               out += ident();
            }
            return out;
         }

         void parse_namespace()
         {
            _lex.take();
            _namespace = dotted_name();
            expect(';');
            // The package is the namespace of the first declaration.
            if (!_have_package && _s.types.empty() && _s.interfaces.empty())
               _s.package.name = fbs_to_pkg(_namespace);
         }

         // The name a type declared now has in the IR.
         std::string declare(const token& at, const std::string& name, AnyType type,
                             std::vector<Attribute> attrs = {})
         {
            if (!_have_package)
            {
               _have_package   = true;
               _s.package.name = fbs_to_pkg(_namespace);
            }
            const std::string full = _namespace.empty() ? name : _namespace + "." + name;
            const std::string ir   = fbs_to_pkg(_namespace) == _s.package.name ? name : full;
            if (!_full_names.insert(full).second || !_ir_names.insert(ir).second)
               fail(at, "'" + name + "' is declared twice");
            _decls.push_back({full, _namespace, at.line, at.column});
            _s.types.push_back(Member{ir, Box<AnyType>{std::move(type)}, std::move(attrs)});
            return ir;
         }

         // `( name [: value], ... )`, or nothing.
         std::vector<Attribute> parse_metadata()
         {
            std::vector<Attribute> out;
            if (!_lex.peek().is('('))
               return out;
            _lex.take();
            std::set<std::string> names;
            while (true)
            {
               const token key = _lex.take();
               if (key.kind != tok::ident)
                  fail(key, "expected an attribute name");
               if (reserved_attribute(key.text))
                  fail(key, "the attribute name '" + key.text + "' is reserved");
               if (!names.insert(key.text).second)
                  fail(key, "attribute '" + key.text + "' is given twice");
               Attribute a{key.text, std::nullopt};
               if (_lex.peek().is(':'))
               {
                  _lex.take();
                  a.value = scalar_text("an attribute value");
               }
               out.push_back(std::move(a));
               if (_lex.peek().is(','))
               {
                  _lex.take();
                  continue;
               }
               expect(')');
               return out;
            }
         }

         // A constant: a string's contents, a name, or a number with its
         // sign.
         std::string scalar_text(const char* what)
         {
            token t = _lex.take();
            if (t.kind == tok::string || t.kind == tok::ident || t.kind == tok::number)
               return std::move(t.text);
            if (t.is('-') || t.is('+'))
            {
               token v = _lex.take();
               if (v.kind != tok::number && v.kind != tok::ident)
                  fail(v, std::string{"expected "} + what);
               return (t.is('-') ? "-" : "") + v.text;
            }
            fail(t, std::string{"expected "} + what);
         }

         static std::optional<AnyType> builtin(std::string_view name)
         {
            if (name == "bool")
               return AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}};
            if (name == "byte" || name == "int8")
               return AnyType{Int{8, true}};
            if (name == "ubyte" || name == "uint8")
               return AnyType{Int{8, false}};
            if (name == "short" || name == "int16")
               return AnyType{Int{16, true}};
            if (name == "ushort" || name == "uint16")
               return AnyType{Int{16, false}};
            if (name == "int" || name == "int32")
               return AnyType{Int{32, true}};
            if (name == "uint" || name == "uint32")
               return AnyType{Int{32, false}};
            if (name == "long" || name == "int64")
               return AnyType{Int{64, true}};
            if (name == "ulong" || name == "uint64")
               return AnyType{Int{64, false}};
            if (name == "float" || name == "float32")
               return AnyType{Float{8, 24}};
            if (name == "double" || name == "float64")
               return AnyType{Float{11, 53}};
            if (name == "string")
               return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
            return std::nullopt;
         }

         AnyType named_type()
         {
            const std::string name = dotted_name();
            if (auto b = builtin(name))
               return std::move(*b);
            return AnyType{Type{name}};
         }

         AnyType parse_type()
         {
            if (!_lex.peek().is('['))
               return named_type();
            _lex.take();
            if (_lex.peek().is('['))
               fail(_lex.peek(), "a vector of vectors is not FlatBuffers (wrap the inner one in a table)");
            AnyType inner = named_type();
            if (_lex.peek().is(':'))
            {
               _lex.take();
               const token n   = _lex.take();
               const auto  len = n.kind == tok::number ? fbs_layout_detail::parse_unsigned(n.text)
                                                       : std::nullopt;
               if (!len || *len < 1 || *len > 0xffff)
                  fail(n, "the length of a fixed array is 1 to 65535");
               expect(']');
               return AnyType{Array{Box<AnyType>{std::move(inner)}, *len}};
            }
            expect(']');
            return AnyType{List{Box<AnyType>{std::move(inner)}}};
         }

         void parse_record()
         {
            const bool  is_struct = _lex.take().text == "struct";
            const token at        = _lex.peek();
            const auto  name      = ident();
            auto        attrs     = parse_metadata();
            expect('{');
            std::vector<Member>   members;
            std::set<std::string> names;
            std::size_t           with_id = 0;
            while (!_lex.peek().is('}'))
            {
               const token field_at = _lex.peek();
               Member      m;
               m.name = ident("a field name");
               if (!names.insert(m.name).second)
                  fail(field_at, "field '" + m.name + "' is declared twice");
               expect(':');
               AnyType                    type = parse_type();
               std::optional<std::string> value;
               bool                       null = false;
               if (_lex.peek().is('='))
               {
                  _lex.take();
                  const token value_at = _lex.peek();
                  if (value_at.is('['))
                  {
                     // `= []`: an empty vector, which is what absent reads as.
                     _lex.take();
                     expect(']');
                  }
                  else if (value_at.kind == tok::string)
                     value = "\"" + _lex.take().text + "\"";
                  else
                  {
                     value = scalar_text("a default value");
                     if (*value == "null")
                        null = true, value.reset();
                  }
                  if (is_struct)
                     fail(value_at, "a struct member has no default");
               }
               m.attributes = parse_metadata();
               expect(';');
               for (auto& a : m.attributes)
                  if (a.name == "id")
                  {
                     const auto id = a.value ? fbs_layout_detail::parse_unsigned(*a.value)
                                             : std::nullopt;
                     if (!id || *id > 0x7fff)
                        fail(field_at, "a field id is a number");
                     a = Attribute{"field", std::to_string(*id + 1)};
                     ++with_id;
                  }
               // The attributes in their normal order: field, default,
               // then the rest as written.
               if (value)
               {
                  const bool numbered = !m.attributes.empty() && m.attributes.front().name == "field";
                  if (!numbered)
                     for (std::size_t i = 1; i < m.attributes.size(); ++i)
                        if (m.attributes[i].name == "field")
                           std::rotate(m.attributes.begin(), m.attributes.begin() + i,
                                       m.attributes.begin() + i + 1);
                  const bool first = !m.attributes.empty() && m.attributes.front().name == "field";
                  m.attributes.insert(m.attributes.begin() + (first ? 1 : 0),
                                      Attribute{"default", std::move(*value)});
               }
               *m.type = null ? AnyType{Option{Box<AnyType>{std::move(type)}}} : std::move(type);
               members.push_back(std::move(m));
            }
            _lex.take();
            if (members.empty() && is_struct)
               fail(at, "a struct has at least one member");
            // (a vtable is at most 65535 bytes: the layout counts a union
            // twice and refuses the table then)
            if (members.size() > 0x7ffd)
               fail(at, "a table has at most 32765 fields");
            if (with_id != 0 && with_id != members.size())
               fail(at, "either every field of a table has an id or none has");
            if (with_id != 0 && is_struct)
               fail(at, "struct members have no ids");
            if (is_struct)
               declare(at, name, AnyType{Struct{std::move(members), std::move(attrs)}});
            else
               declare(at, name, AnyType{Object{std::move(members), std::move(attrs)}});
            _explicit_ids.push_back(with_id != 0);
         }

         // An integer constant with its sign, as two's complement.
         std::int64_t integer(const char* what)
         {
            const token at   = _lex.peek();
            const auto  text = scalar_text(what);
            std::string_view digits = text;
            const bool       negative = !digits.empty() && digits.front() == '-';
            if (negative)
               digits.remove_prefix(1);
            std::uint64_t v    = 0;
            int           base = 10;
            if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
            {
               base = 16;
               digits.remove_prefix(2);
            }
            const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
            if (digits.empty() || r.ec != std::errc{} || r.ptr != digits.data() + digits.size() ||
                (negative && v > std::uint64_t{1} << 63))
               fail(at, std::string{"expected "} + what);
            return static_cast<std::int64_t>(negative ? std::uint64_t{0} - v : v);
         }

         void parse_enum()
         {
            _lex.take();
            const token at   = _lex.peek();
            const auto  name = ident();
            AnyType     under{Int{32, true}};  // without a type: `int`
            if (_lex.peek().is(':'))
            {
               _lex.take();
               const token type_at = _lex.peek();
               auto        b       = builtin(ident("an integer type"));
               if (!b || !std::holds_alternative<Int>(b->value))
                  fail(type_at, "the type of an enum is an integer type");
               under = std::move(*b);
            }
            const Int  width = std::get<Int>(under.value);
            auto       attrs = parse_metadata();
            const bool flags = fbs_layout_detail::has_attribute(attrs, "bit_flags");
            expect('{');
            Enum                  e;
            std::set<std::string> names;
            e.type = Box<AnyType>{std::move(under)};
            std::int64_t next       = 0;
            bool         first      = true;
            bool         last_value = false;  // the previous value has no successor
            while (!_lex.peek().is('}'))
            {
               const token value_at = _lex.peek();
               Enumerator  v;
               v.name = ident("an enumerator");
               if (!names.insert(v.name).second)
                  fail(value_at, "enumerator '" + v.name + "' is declared twice");
               std::int64_t n = next;
               if (!first && !_lex.peek().is('=') && last_value)
                  fail(value_at, "an enumerator past the last value of the enum's type");
               if (_lex.peek().is('='))
               {
                  _lex.take();
                  n = integer("an integer");
                  // (ascending as the enum's type orders its values)
                  if (!first && (width.isSigned || flags
                                    ? n < next
                                    : static_cast<std::uint64_t>(n) < static_cast<std::uint64_t>(next)))
                     fail(value_at, "enumerator values must ascend");
               }
               parse_metadata();
               if (flags)
               {
                  if (n < 0 || n >= static_cast<std::int64_t>(width.bits))
                     fail(value_at, "a bit flag outside the enum's type");
                  v.value = std::uint64_t{1} << n;
               }
               else
               {
                  // The value must be one the underlying type holds.
                  bool fits = true;
                  if (width.bits < 64)
                  {
                     const std::int64_t half = std::int64_t{1} << (width.bits - 1);
                     fits = width.isSigned ? n >= -half && n < half : n >= 0 && n < 2 * half;
                  }
                  if (!fits)
                     fail(value_at, "an enumerator outside the enum's type");
                  v.value = static_cast<std::uint64_t>(n);
               }
               e.members.push_back(std::move(v));
               first = false;
               last_value = width.isSigned || flags ? n == std::numeric_limits<std::int64_t>::max()
                                                    : n == -1;
               next       = last_value ? n : n + 1;
               if (_lex.peek().is(','))
                  _lex.take();
               else if (!_lex.peek().is('}'))
                  fail(_lex.peek(), "expected ',' or '}'");
            }
            _lex.take();
            declare(at, name, AnyType{std::move(e)}, std::move(attrs));
            _explicit_ids.push_back(false);
         }

         void parse_union()
         {
            _lex.take();
            const token at   = _lex.peek();
            const auto  name = ident();
            Variant               v;
            std::set<std::string> names;
            v.attributes = parse_metadata();
            expect('{');
            std::int64_t next = 1;
            while (!_lex.peek().is('}'))
            {
               const token member_at = _lex.peek();
               Member      m;
               std::string type = dotted_name();
               if (_lex.peek().is(':'))
               {
                  // alias:Type
                  if (type.find('.') != std::string::npos)
                     fail(member_at, "expected a name");
                  _lex.take();
                  m.name = std::move(type);
                  type   = dotted_name();
               }
               else
               {
                  // flatc names the member after the type, `.` as `_`.
                  m.name = type;
                  for (char& c : m.name)
                     if (c == '.')
                        c = '_';
               }
               if (m.name == "NONE")
                  fail(member_at, "NONE is the union's own first member");
               if (!names.insert(m.name).second)
                  fail(member_at, "union member '" + m.name + "' is declared twice");
               if (auto b = builtin(type))
                  *m.type = std::move(*b);
               else
                  *m.type = AnyType{Type{std::move(type)}};
               if (_lex.peek().is('='))
               {
                  _lex.take();
                  const std::int64_t n = integer("an integer");
                  if (n < next || n > 255)
                     fail(member_at, "union member values ascend from 1 to 255");
                  if (n != next)
                     m.attributes.push_back(Attribute{"value", std::to_string(n)});
                  next = n;
               }
               if (next > 255)
                  fail(member_at, "a union has at most 255 members");
               ++next;
               parse_metadata();
               v.members.push_back(std::move(m));
               if (_lex.peek().is(','))
                  _lex.take();
               else if (!_lex.peek().is('}'))
                  fail(_lex.peek(), "expected ',' or '}'");
            }
            _lex.take();
            if (v.members.empty())
               fail(at, "a union has at least one member");
            declare(at, name, AnyType{std::move(v)});
            _explicit_ids.push_back(false);
         }

         void parse_service()
         {
            _lex.take();
            Interface   iface;
            const token at = _lex.peek();
            iface.name     = ident();
            expect('{');
            while (!_lex.peek().is('}'))
            {
               Func f;
               f.name = ident("a method name");
               expect('(');
               f.params.push_back(Member{"request", Box<AnyType>{AnyType{Type{dotted_name()}}}, {}});
               expect(')');
               expect(':');
               f.result     = Box<AnyType>{AnyType{Type{dotted_name()}}};
               f.attributes = parse_metadata();
               expect(';');
               iface.funcs.push_back(std::move(f));
            }
            _lex.take();
            if (!_have_package)
            {
               _have_package   = true;
               _s.package.name = fbs_to_pkg(_namespace);
            }
            // Named like a type: in full outside the package's namespace.
            if (fbs_to_pkg(_namespace) != _s.package.name)
               iface.name = _namespace.empty() ? iface.name : _namespace + "." + iface.name;
            if (!_service_names.insert(iface.name).second)
               fail(at, "'" + iface.name + "' is declared twice");
            _s.interfaces.push_back(std::move(iface));
            _service_ns.push_back(_namespace);
         }

         // ── After the last declaration ──────────────────────────────────

         std::vector<bool> _explicit_ids;  // parallel to _s.types

         // The IR name of the type written as `written` in namespace `ns`:
         // looked up there, then in each enclosing namespace. Empty when no
         // such type is declared in this text.
         std::string lookup(const std::string& written, std::string ns) const
         {
            const Member* entry = lookup_entry(written, std::move(ns));
            return entry ? entry->name : std::string{};
         }
         const Member* lookup_entry(const std::string& written, std::string ns) const
         {
            while (true)
            {
               const std::string full = ns.empty() ? written : ns + "." + written;
               const auto        at   = _by_full_name.find(full);
               if (at != _by_full_name.end())
                  return &_s.types[at->second];
               if (ns.empty())
                  return nullptr;
               const auto dot = ns.rfind('.');
               ns             = dot == std::string::npos ? std::string{} : ns.substr(0, dot);
            }
         }

         std::map<std::string, std::size_t> _by_full_name;

         // A reference keeps its written name when the text does not
         // declare the type (an included file may): the layout refuses it.
         void resolve(AnyType& type, const std::string& ns) const
         {
            AnyType* at = &type;
            while (true)
            {
               if (auto* t = std::get_if<Type>(&at->value))
               {
                  if (auto ir = lookup(t->type, ns); !ir.empty())
                     t->type = std::move(ir);
                  return;
               }
               if (auto* l = std::get_if<List>(&at->value))
                  at = l->type.get();
               else if (auto* a = std::get_if<Array>(&at->value))
                  at = a->type.get();
               else if (auto* o = std::get_if<Option>(&at->value))
                  at = o->type.get();
               else
                  return;
            }
         }

         void finish()
         {
            for (std::size_t i = 0; i < _decls.size(); ++i)
               _by_full_name.emplace(_decls[i].full_name, i);

            for (std::size_t i = 0; i < _s.types.size(); ++i)
            {
               AnyType& node = *_s.types[i].type;
               if (auto* o = std::get_if<Object>(&node.value))
                  for (auto& m : o->members)
                     resolve(*m.type, _decls[i].ns);
               else if (auto* s = std::get_if<Struct>(&node.value))
                  for (auto& m : s->members)
                     resolve(*m.type, _decls[i].ns);
               else if (auto* v = std::get_if<Variant>(&node.value))
                  for (auto& m : v->members)
                     resolve(*m.type, _decls[i].ns);
            }
            for (std::size_t i = 0; i < _s.interfaces.size(); ++i)
               for (auto& f : _s.interfaces[i].funcs)
               {
                  resolve(*f.params[0].type, _service_ns[i]);
                  resolve(**f.result, _service_ns[i]);
               }
            if (_root)
            {
               const token at{tok::ident, {}, _root->line, _root->column};
               const Member* root = lookup_entry(_root->written, _root->ns);
               if (!root || !std::holds_alternative<Object>(root->type->value))
                  fail(at, "root_type names a table of this file");
               drop_attribute(_s.package.attributes, "fbs_root_type");
               _s.package.attributes.push_back(Attribute{"fbs_root_type", root->name});
            }

            // What needs every type: defaults that name an enumerator, the
            // ids of a table, the members of a struct.
            fbs_layout_detail::model types{_s};
            for (std::size_t i = 0; i < _s.types.size(); ++i)
            {
               const token at{tok::ident, {}, _decls[i].line, _decls[i].column};
               AnyType&    node = *_s.types[i].type;
               try
               {
                  if (auto* o = std::get_if<Object>(&node.value))
                     finish_table(types, *o, _explicit_ids[i], at);
                  else if (std::holds_alternative<Struct>(node.value))
                  {
                     if (types.shape_of(node).k != fbs_layout_detail::kind::structure)
                        fail(at, "struct '" + _s.types[i].name +
                                    "' has a member that is not a scalar, an enum, a fixed array or a struct");
                  }
               }
               catch (const fbs_parse_error&)
               {
                  throw;
               }
               catch (const std::runtime_error& e)
               {
                  // (the layout's own refusals: an unknown type is not one
                  // of them here — it may be declared by an included file)
                  std::string_view what = e.what();
                  if (what.find("unknown type") == std::string_view::npos)
                     fail(at, std::string{what});
               }
            }
         }

         void finish_table(fbs_layout_detail::model& types, Object& o, bool explicit_ids,
                           const token& at)
         {
            namespace d = fbs_layout_detail;
            std::vector<bool> used;
            for (auto& m : o.members)
            {
               d::shape e;
               bool     known = true;
               try
               {
                  e = types.shape_of(*m.type, 1);
               }
               catch (const std::runtime_error& x)
               {
                  if (std::string_view{x.what()}.find("unknown type") == std::string_view::npos)
                     throw;
                  known = false;
               }
               if (known && e.optional && !e.scalar())
                  fail(at, "field '" + m.name + "': only a scalar can be null");
               const bool is_union = known && d::union_field(types, e);
               if (explicit_ids)
               {
                  const auto id = *d::parse_unsigned(*d::attribute(m.attributes, "field")) - 1;
                  if (is_union && id == 0)
                     fail(at, "field '" + m.name + "': a union's id must leave room for its type");
                  for (std::uint64_t slot = is_union ? id - 1 : id; slot <= id; ++slot)
                  {
                     if (used.size() <= slot)
                        used.resize(slot + 1, false);
                     if (used[slot])
                        fail(at, "field id " + std::to_string(slot) + " is used twice");
                     used[slot] = true;
                  }
               }
               for (auto& a : m.attributes)
               {
                  if (a.name != "default" || !a.value)
                     continue;
                  if (!known)
                     continue;  // (a type of an included file: kept as written)
                  if (!e.scalar())
                  {
                     // A string default is kept as written (quoted).
                     if (e.k == d::kind::string && a.value->starts_with('"'))
                        continue;
                     fail(at, "field '" + m.name + "': only a scalar or a string has a default");
                  }
                  *a.value = scalar_default(types, m, e, *a.value, at);
               }
               // A default that is the type's zero is no default.
               std::erase_if(m.attributes, [](const Attribute& a) {
                  return a.name == "default" && a.value && a.value->empty();
               });
            }
            for (std::size_t slot = 0; slot < used.size(); ++slot)
               if (!used[slot])
                  fail(at, "field ids must be 0, 1, 2, ... without a gap (" + std::to_string(slot) +
                              " is missing)");
            if (!explicit_ids)
               return;
            // Ids in declaration order say nothing the order does not.
            bool          in_order = true;
            std::uint64_t last     = 0;
            for (const auto& m : o.members)
            {
               const auto n = *d::parse_unsigned(*d::attribute(m.attributes, "field"));
               if (n < last)
                  in_order = false;
               last = n;
            }
            if (in_order)
               for (auto& m : o.members)
                  drop_attribute(m.attributes, "field");
         }

         // The normal form of a scalar default: `true`, a decimal integer,
         // the shortest decimal float; empty for zero.
         std::string scalar_default(fbs_layout_detail::model& types, const Member& m,
                                    const fbs_layout_detail::shape& e, const std::string& text,
                                    const token& at) const
         {
            namespace d = fbs_layout_detail;
            const auto bad = [&]() -> std::string {
               fail(at, "field '" + m.name + "': '" + text + "' is not a value of its type");
            };
            if (e.k == d::kind::real)
            {
               const auto v = d::parse_real(text);
               if (!v)
                  return bad();
               if (*v != *v)
                  return "nan";
               if (*v == 0)
                  return std::signbit(*v) ? "-0" : "";
               char buffer[64];
               if (e.size == 4)
               {
                  const float f = static_cast<float>(*v);
                  if (f == std::numeric_limits<float>::infinity())
                     return "inf";
                  if (f == -std::numeric_limits<float>::infinity())
                     return "-inf";
                  const auto r = std::to_chars(buffer, buffer + sizeof(buffer), f);
                  return std::string(buffer, r.ptr);
               }
               if (*v == std::numeric_limits<double>::infinity())
                  return "inf";
               if (*v == -std::numeric_limits<double>::infinity())
                  return "-inf";
               const auto r = std::to_chars(buffer, buffer + sizeof(buffer), *v);
               return std::string(buffer, r.ptr);
            }
            if (e.k == d::kind::boolean)
            {
               if (text == "true" || text == "1")
                  return "true";
               if (text == "false" || text == "0")
                  return "";
               return bad();
            }
            std::int64_t value = 0;
            if (!text.empty() && ident_start(text.front()))
            {
               // An enumerator's name.
               const auto* en = e.k == d::kind::enumeration ? std::get_if<Enum>(&e.node->value)
                                                            : nullptr;
               const Enumerator* found = nullptr;
               if (en)
                  for (const auto& v : en->members)
                     if (v.name == text)
                        found = &v;
               if (!found)
                  return bad();
               value = static_cast<std::int64_t>(found->value);
            }
            else
            {
               std::string_view digits = text;
               const bool       negative = digits.front() == '-';
               if (negative)
                  digits.remove_prefix(1);
               int base = 10;
               if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
               {
                  base = 16;
                  digits.remove_prefix(2);
               }
               std::uint64_t u = 0;
               const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), u, base);
               if (digits.empty() || r.ec != std::errc{} || r.ptr != digits.data() + digits.size() ||
                   (negative && u > std::uint64_t{1} << 63))
                  return bad();
               if (negative && !e.is_signed && u != 0)
                  return bad();
               value = static_cast<std::int64_t>(negative ? std::uint64_t{0} - u : u);
               if (!negative && e.is_signed && e.size == 8 && u >= std::uint64_t{1} << 63)
                  return bad();
               if (e.size < 8)
               {
                  const std::int64_t lo = e.is_signed ? -(std::int64_t{1} << (8 * e.size - 1)) : 0;
                  const std::int64_t hi = e.is_signed ? (std::int64_t{1} << (8 * e.size - 1)) - 1
                                                      : (std::int64_t{1} << (8 * e.size)) - 1;
                  if (value < lo || value > hi)
                     return bad();
               }
            }
            (void)types;
            if (value == 0)
               return "";
            return e.is_signed ? std::to_string(value)
                               : std::to_string(static_cast<std::uint64_t>(value));
         }
      };

   }  // namespace fbs_parse_detail

   /// Parse FlatBuffers schema text into a Schema IR. Throws
   /// `fbs_parse_error` on text that is not a schema.
   inline Schema parse_fbs(std::string_view text)
   {
      fbs_parse_detail::Parser p{text};
      return p.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::fbs_parse_error;
   using schema_types::parse_fbs;
}
