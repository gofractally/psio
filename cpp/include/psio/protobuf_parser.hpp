#pragma once
//
// psio/protobuf_parser.hpp — `.proto` text → Schema IR.
//
// `psio::parse_protobuf(text)` reads a proto2 or proto3 file and keeps what
// decides the wire or the meaning of a message, in the form
// psio/emit_protobuf.hpp documents ("How the Schema IR is read as
// protobuf") and writes back:
//
//   syntax                 package attribute `proto_syntax` (proto2 only;
//                          a file without the statement is proto2)
//   package a.b;           Schema::package.name (`a:b`)
//   import "x.proto";      package attribute `proto_import` (the statement's
//                          text; not followed — a type the file does not
//                          declare stays a Type reference by its name)
//   option …;              `proto_option` (`name = value` as written) on the
//                          package / message / named enum / service / method
//   message M { … }        Object `M`; nested `M.N`, to any depth
//   field                  member, attribute `field` = its number
//     int32 … bool, string, bytes, float, double: the types emit_protobuf
//     maps to them; sint* / fixed* / sfixed*: attribute `protobuf` = sint | fixed
//     optional T           Option{T}
//     repeated T           List{T}
//     required T           T with attribute `required`
//     map<K, V>            Custom{List{Tuple{K, V}}, "map"}
//                          (`protobuf_key` / `protobuf_value` = sint | fixed)
//     [deprecated = true]  attribute `deprecated`
//     [default = v]        attribute `default` (the text)
//     any other option     `proto_option` (`packed = false`, `json_name = "x"`, …)
//   oneof o { … }          member `o`: Variant{none: Tuple{}, alternatives…},
//                          each alternative with its `field`
//   enum E { A = 0; … }    Enum over Int{32, signed}, values as written;
//                          options of a value: `proto_option.A` on the enum
//   reserved / extensions  `proto_reserved` / `proto_extensions` (the text)
//   service S { rpc … }    Interface; a Func with one parameter `request`
//                          and a result (`proto_client_streaming`,
//                          `proto_server_streaming`)
//
// Names are kept as written; a reference is resolved by protobuf's scoping
// rules to the declaration's name. Refused, with line and column
// (`protobuf_parse_error`): `edition`, `extend`, groups, a field number
// protobuf refuses or uses twice in a message, a type that is not declared
// in a file without imports. Linear in the text; nesting is bounded (64).

#include <psio/schema_ir.hpp>

#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace psio::schema_types
{
   struct protobuf_parse_error : std::runtime_error
   {
      std::uint32_t line;
      std::uint32_t column;
      protobuf_parse_error(const std::string& msg, std::uint32_t l, std::uint32_t c)
         : std::runtime_error("proto:" + std::to_string(l) + ":" + std::to_string(c) + ": " + msg),
           line{l},
           column{c}
      {
      }
   };

   namespace protobuf_parse_detail
   {
      enum class tok : std::uint8_t { eof, ident, number, string, punct };

      struct token
      {
         tok              kind = tok::eof;
         std::string_view text;  // a string: with its quotes
         std::size_t      begin  = 0;
         std::size_t      end    = 0;
         std::uint32_t    line   = 1;
         std::uint32_t    column = 1;
         bool is(char c) const noexcept { return kind == tok::punct && text[0] == c; }
         bool is(std::string_view word) const noexcept { return kind == tok::ident && text == word; }
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
         const token&     peek() const noexcept { return _tok; }
         std::string_view source() const noexcept { return _src; }
         token            take()
         {
            const token t = _tok;
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
            throw protobuf_parse_error(what, _line, _column);
         }
         void skip_space()
         {
            while (!eof())
            {
               const char c = at();
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
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
            _tok.begin = _tok.end = _pos;
            if (eof())
               return;
            const char c = at();
            if (ident_start(c))
            {
               _tok.kind = tok::ident;
               while (!eof() && ident_char(at()))
                  advance();
            }
            else if (digit(c) || (c == '.' && digit(at(1))))
            {
               _tok.kind = tok::number;
               while (!eof())
               {
                  const char d    = at();
                  const char prev = _pos > _tok.begin ? _src[_pos - 1] : '\0';
                  const bool hex  = _pos - _tok.begin >= 2 && (_src[_tok.begin + 1] == 'x' ||
                                                               _src[_tok.begin + 1] == 'X');
                  const bool sign = (d == '+' || d == '-') && (prev == 'e' || prev == 'E') && !hex;
                  if (!(ident_char(d) || d == '.' || sign))
                     break;
                  advance();
               }
            }
            else if (c == '"' || c == '\'')
            {
               _tok.kind = tok::string;
               advance();
               for (;;)
               {
                  if (eof() || at() == '\n')
                     fail("a string that does not end");
                  const char d = at();
                  advance();
                  if (d == c)
                     break;
                  if (d == '\\')
                  {
                     if (eof() || at() == '\n')
                        fail("a string that does not end");
                     advance();
                  }
               }
            }
            else
            {
               _tok.kind = tok::punct;
               advance();
            }
            _tok.end  = _pos;
            _tok.text = _src.substr(_tok.begin, _tok.end - _tok.begin);
         }
      };

      inline AnyType bytes_type(const char* id)
      {
         return Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, id};
      }

      class Parser
      {
       public:
         explicit Parser(std::string_view src) : _lex{src} {}

         Schema run()
         {
            bool first = true;
            while (_lex.peek().kind != tok::eof)
            {
               const token t = _lex.peek();
               if (t.is(';'))
                  _lex.take();
               else if (t.is("syntax"))
               {
                  _lex.take();
                  expect('=');
                  const token v = _lex.take();
                  if (!first)
                     fail(t, "'syntax' is the first statement");
                  if (v.kind != tok::string || v.text.size() < 2)
                     fail(v, "expected \"proto2\" or \"proto3\"");
                  const std::string_view s = v.text.substr(1, v.text.size() - 2);
                  if (s == "proto3")
                     _proto3 = true;
                  else if (s != "proto2")
                     fail(v, "expected \"proto2\" or \"proto3\"");
                  expect(';');
               }
               else if (t.is("edition"))
                  fail(t, "editions are not read (proto2 and proto3 are)");
               else if (t.is("package"))
               {
                  _lex.take();
                  if (_have_package)
                     fail(t, "a second package");
                  _have_package   = true;
                  _package        = dotted_name(false);
                  _s.package.name = _package;
                  for (char& c : _s.package.name)
                     if (c == '.')
                        c = ':';
                  expect(';');
               }
               else if (t.is("import"))
               {
                  _lex.take();
                  const token from = _lex.peek();
                  if (_lex.peek().is("public") || _lex.peek().is("weak"))
                     _lex.take();
                  const token path = _lex.take();
                  if (path.kind != tok::string)
                     fail(path, "expected a file name");
                  expect(';');
                  _imports.push_back(std::string{slice(from, path)});
               }
               else if (t.is("option"))
                  _options.push_back(option_statement());
               else if (t.is("message"))
                  message(std::string{}, 0);
               else if (t.is("enum"))
                  enumeration(std::string{});
               else if (t.is("service"))
                  service();
               else if (t.is("extend"))
                  fail(t, "extensions are not read");
               else
                  fail(t, "expected a declaration");
               first = false;
            }
            finish();
            return std::move(_s);
         }

       private:
         struct reference
         {
            AnyType*      node;
            std::string   scope;  // the message the reference is written in
            std::uint32_t line;
            std::uint32_t column;
         };

         Lexer                    _lex;
         Schema                   _s;
         bool                     _proto3       = false;
         bool                     _have_package = false;
         std::string              _package;
         std::vector<std::string> _imports, _options;
         std::unordered_map<std::string, std::size_t> _declared;  // name → index in _s.types
         std::unordered_set<std::string>              _services;
         std::vector<reference>                       _references;

         [[noreturn]] static void fail(const token& at, const std::string& what)
         {
            throw protobuf_parse_error(what, at.line, at.column);
         }
         std::string_view slice(const token& from, const token& to) const
         {
            return _lex.source().substr(from.begin, to.end - from.begin);
         }
         void expect(char c)
         {
            const token t = _lex.take();
            if (!t.is(c))
               fail(t, std::string{"expected '"} + c + "'");
         }
         std::string ident(const char* what = "a name")
         {
            const token t = _lex.take();
            if (t.kind != tok::ident)
               fail(t, std::string{"expected "} + what);
            return std::string{t.text};
         }
         std::string dotted_name(bool absolute = true)
         {
            std::string out;
            if (absolute && _lex.peek().is('.'))
            {
               _lex.take();
               out = ".";
            }
            out += ident();
            while (_lex.peek().is('.'))
            {
               _lex.take();
               out += '.';
               out += ident();
            }
            return out;
         }

         // The tokens up to (not including) a `stop` outside brackets, as
         // written. Empty is an error.
         std::string raw_until(char stop, char stop2 = '\0')
         {
            const token first = _lex.peek();
            token       last  = first;
            unsigned    depth = 0;
            bool        any   = false;
            for (;;)
            {
               const token& t = _lex.peek();
               if (t.kind == tok::eof)
                  fail(t, "unexpected end of the file");
               if (depth == 0 && (t.is(stop) || (stop2 && t.is(stop2))))
                  break;
               if (t.is('{') || t.is('[') || t.is('('))
                  ++depth;
               else if (t.is('}') || t.is(']') || t.is(')'))
               {
                  if (depth == 0)
                     fail(t, "unbalanced bracket");
                  --depth;
               }
               last = _lex.take();
               any  = true;
            }
            if (!any)
               fail(first, "expected a value");
            return std::string{slice(first, last)};
         }

         // `option name = value;` → "name = value"
         std::string option_statement()
         {
            _lex.take();
            std::string out = raw_until(';');
            expect(';');
            if (out.find('=') == std::string::npos)
               fail(_lex.peek(), "an option is 'name = value'");
            return out;
         }

         // `[a = 1, b = "x"]` after a field / enum value.
         void bracket_options(std::vector<Attribute>& attrs, const std::string& prefix,
                              bool field)
         {
            if (!_lex.peek().is('['))
               return;
            _lex.take();
            for (;;)
            {
               const token at   = _lex.peek();
               std::string item = raw_until(',', ']');
               const auto  eq   = item.find('=');
               if (eq == std::string::npos)
                  fail(at, "an option is 'name = value'");
               std::string_view name  = std::string_view{item}.substr(0, eq);
               std::string_view value = std::string_view{item}.substr(eq + 1);
               while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\n' || name.back() == '\r'))
                  name.remove_suffix(1);
               while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n' || value.front() == '\r'))
                  value.remove_prefix(1);
               if (field && name == "deprecated" && value == "true")
                  attrs.push_back(Attribute{"deprecated", std::string{}});
               else if (field && name == "default")
                  attrs.push_back(Attribute{"default", std::string{value}});
               else
                  attrs.push_back(Attribute{prefix, std::move(item)});
               if (_lex.peek().is(','))
                  _lex.take();
               else
                  break;
            }
            expect(']');
         }

         std::size_t declare(const token& at, const std::string& name, AnyType type)
         {
            if (_services.count(name) || !_declared.emplace(name, _s.types.size()).second)
               fail(at, "'" + name + "' is declared twice");
            _s.types.push_back(Member{name, Box<AnyType>{std::move(type)}, {}});
            return _s.types.size() - 1;
         }

         static std::optional<std::uint64_t> integer(std::string_view text)
         {
            int base = 10;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
               base = 16;
               text.remove_prefix(2);
            }
            else if (text.size() > 1 && text[0] == '0')
            {
               base = 8;
               text.remove_prefix(1);
            }
            std::uint64_t v = 0;
            const auto    r = std::from_chars(text.data(), text.data() + text.size(), v, base);
            if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size())
               return std::nullopt;
            return v;
         }

         std::uint64_t field_number(std::unordered_set<std::uint64_t>& used)
         {
            const token t = _lex.take();
            const auto  n = t.kind == tok::number ? integer(t.text) : std::nullopt;
            if (!n)
               fail(t, "expected a field number");
            if (*n < 1 || *n > 536870911 || (*n >= 19000 && *n <= 19999))
               fail(t, "a field number is within 1..536870911 and outside 19000..19999");
            if (!used.insert(*n).second)
               fail(t, "the field number " + std::to_string(*n) + " is used twice");
            return *n;
         }

         // A field's type; `enc` is the `protobuf` attribute's value.
         AnyType type(const char*& enc)
         {
            enc           = nullptr;
            const token t = _lex.peek();
            if (t.kind == tok::ident && !_lex.peek().is('.'))
            {
               const std::string_view w = t.text;
               std::optional<AnyType> out;
               if (w == "double")
                  out = Float{11, 53};
               else if (w == "float")
                  out = Float{8, 24};
               else if (w == "int32" || w == "sint32" || w == "sfixed32")
                  out = Int{32, true};
               else if (w == "int64" || w == "sint64" || w == "sfixed64")
                  out = Int{64, true};
               else if (w == "uint32" || w == "fixed32")
                  out = Int{32, false};
               else if (w == "uint64" || w == "fixed64")
                  out = Int{64, false};
               else if (w == "bool")
                  out = Custom{Box<AnyType>{Int{1, false}}, "bool"};
               else if (w == "string")
                  out = bytes_type("utf8");
               else if (w == "bytes")
                  out = bytes_type("hex");
               if (out)
               {
                  _lex.take();
                  // A scalar's name may also begin a dotted type name.
                  if (_lex.peek().is('.'))
                     fail(_lex.peek(), "a type name that begins with a scalar's");
                  if (w.starts_with("sint"))
                     enc = "sint";
                  else if (w.starts_with("fixed") || w.starts_with("sfixed"))
                     enc = "fixed";
                  return std::move(*out);
               }
            }
            if (t.kind != tok::ident && !t.is('.'))
               fail(t, "expected a type");
            AnyType out{Type{dotted_name()}};
            return out;
         }

         // Notes a member's Type reference for resolution (after the move
         // into its Box: the node's address is final).
         void note(AnyType& node, const std::string& scope, const token& at)
         {
            if (std::holds_alternative<Type>(node.value))
               _references.push_back(reference{&node, scope, at.line, at.column});
         }

         Member field(const std::string& scope, std::unordered_set<std::uint64_t>& used,
                      bool in_oneof)
         {
            const token at = _lex.peek();
            enum { none, optional, required, repeated } label = none;
            if (!in_oneof)
            {
               if (at.is("optional"))
                  label = optional;
               else if (at.is("required"))
                  label = required;
               else if (at.is("repeated"))
                  label = repeated;
               if (label != none)
                  _lex.take();
            }
            if (label == required && _proto3)
               fail(at, "proto3 has no required fields");
            Member out;
            const token type_at = _lex.peek();
            if (type_at.is("group"))
               fail(type_at, "groups are not read");
            if (type_at.is("map") && label == none && !in_oneof)
            {
               _lex.take();
               if (_lex.peek().is('<'))
               {
                  _lex.take();
                  const char* key_enc   = nullptr;
                  const char* value_enc = nullptr;
                  const token key_at    = _lex.peek();
                  AnyType     key       = type(key_enc);
                  if (std::holds_alternative<Type>(key.value) ||
                      std::holds_alternative<Float>(key.value) ||
                      (std::holds_alternative<Custom>(key.value) &&
                       std::get<Custom>(key.value).id == "hex"))
                     fail(key_at, "a map's key is an integer, a bool or a string");
                  expect(',');
                  const token value_at = _lex.peek();
                  AnyType     value    = type(value_enc);
                  expect('>');
                  Tuple pair;
                  pair.members.push_back(Box<AnyType>{std::move(key)});
                  pair.members.push_back(Box<AnyType>{std::move(value)});
                  note(*pair.members[1], scope, value_at);
                  *out.type = Custom{Box<AnyType>{List{Box<AnyType>{std::move(pair)}}}, "map"};
                  // The reference noted above is inside boxes: moving the
                  // boxes keeps the node's address.
                  out.name = ident("a field name");
                  expect('=');
                  out.attributes.push_back(Attribute{"field", std::to_string(field_number(used))});
                  if (key_enc)
                     out.attributes.push_back(Attribute{"protobuf_key", key_enc});
                  if (value_enc)
                     out.attributes.push_back(Attribute{"protobuf_value", value_enc});
                  bracket_options(out.attributes, "proto_option", true);
                  expect(';');
                  return out;
               }
               // A type named `map`.
               *out.type = AnyType{Type{"map"}};
               note(*out.type, scope, type_at);
               out.name = ident("a field name");
               expect('=');
               out.attributes.push_back(Attribute{"field", std::to_string(field_number(used))});
               bracket_options(out.attributes, "proto_option", true);
               expect(';');
               return out;
            }
            const char* enc  = nullptr;
            AnyType     base = type(enc);
            if (label == none && !in_oneof && !_proto3)
               fail(at, "proto2: a field is optional, required or repeated");
            if (label == optional)
            {
               Option o;
               *o.type = std::move(base);
               note(*o.type, scope, type_at);
               *out.type = std::move(o);
            }
            else if (label == repeated)
            {
               List l;
               *l.type = std::move(base);
               note(*l.type, scope, type_at);
               *out.type = std::move(l);
            }
            else
            {
               *out.type = std::move(base);
               note(*out.type, scope, type_at);
            }
            out.name = ident("a field name");
            expect('=');
            out.attributes.push_back(Attribute{"field", std::to_string(field_number(used))});
            if (enc)
               out.attributes.push_back(Attribute{"protobuf", enc});
            if (label == required)
               out.attributes.push_back(Attribute{"required", std::nullopt});
            bracket_options(out.attributes, "proto_option", true);
            expect(';');
            return out;
         }

         void message(const std::string& scope, unsigned depth)
         {
            _lex.take();
            const token at   = _lex.peek();
            const std::string name = (scope.empty() ? "" : scope + ".") + ident();
            if (depth > 64)
               fail(at, "messages nested too deeply");
            // Declared before its body: the index is where the body goes.
            const std::size_t index = declare(at, name, Object{});
            expect('{');
            Object                            body;
            std::unordered_set<std::uint64_t> used;
            for (;;)
            {
               const token t = _lex.peek();
               if (t.is('}'))
                  break;
               if (t.kind == tok::eof)
                  fail(t, "a message that does not end");
               if (t.is(';'))
                  _lex.take();
               else if (t.is("message"))
                  message(name, depth + 1);
               else if (t.is("enum"))
                  enumeration(name);
               else if (t.is("option"))
                  body.attributes.push_back(Attribute{"proto_option", option_statement()});
               else if (t.is("reserved") || t.is("extensions"))
               {
                  _lex.take();
                  body.attributes.push_back(
                     Attribute{t.is("reserved") ? "proto_reserved" : "proto_extensions", raw_until(';')});
                  expect(';');
               }
               else if (t.is("extend"))
                  fail(t, "extensions are not read");
               else if (t.is("oneof"))
               {
                  _lex.take();
                  Member  m;
                  Variant v;
                  m.name = ident();
                  expect('{');
                  v.members.push_back(Member{"none", Box<AnyType>{AnyType{Tuple{}}}, {}});
                  while (!_lex.peek().is('}'))
                  {
                     if (_lex.peek().kind == tok::eof)
                        fail(_lex.peek(), "a oneof that does not end");
                     if (_lex.peek().is(';'))
                        _lex.take();
                     else if (_lex.peek().is("option"))
                        v.attributes.push_back(Attribute{"proto_option", option_statement()});
                     else
                        v.members.push_back(field(name, used, true));
                  }
                  _lex.take();
                  if (v.members.size() < 2)
                     fail(t, "a oneof without fields");
                  for (const auto& a : v.members[1].attributes)
                     if (a.name == "field")
                        m.attributes.push_back(a);
                  *m.type = std::move(v);
                  body.members.push_back(std::move(m));
               }
               else if (t.kind == tok::ident || t.is('.'))
                  body.members.push_back(field(name, used, false));
               else
                  fail(t, "expected a field");
            }
            _lex.take();
            *_s.types[index].type = std::move(body);
         }

         void enumeration(const std::string& scope)
         {
            _lex.take();
            const token       at   = _lex.peek();
            const std::string name = (scope.empty() ? "" : scope + ".") + ident();
            Enum              e;
            *e.type = Int{32, true};
            std::vector<Attribute> attrs;
            expect('{');
            for (;;)
            {
               const token t = _lex.peek();
               if (t.is('}'))
                  break;
               if (t.is(';'))
               {
                  _lex.take();
                  continue;
               }
               if (t.is("option"))
               {
                  attrs.push_back(Attribute{"proto_option", option_statement()});
                  continue;
               }
               if (t.is("reserved"))
               {
                  _lex.take();
                  attrs.push_back(Attribute{"proto_reserved", raw_until(';')});
                  expect(';');
                  continue;
               }
               if (t.kind != tok::ident)
                  fail(t, "expected an enum value name");
               _lex.take();
               expect('=');
               const bool negative = _lex.peek().is('-');
               if (negative)
                  _lex.take();
               const token v = _lex.take();
               const auto  n = v.kind == tok::number ? integer(v.text) : std::nullopt;
               if (!n || *n > (negative ? 2147483648ull : 2147483647ull))
                  fail(v, "an enum value is an int32");
               const std::int64_t value = negative ? -static_cast<std::int64_t>(*n)
                                                   : static_cast<std::int64_t>(*n);
               e.members.push_back(Enumerator{std::string{t.text}, static_cast<std::uint64_t>(value)});
               bracket_options(attrs, "proto_option." + std::string{t.text}, false);
               expect(';');
            }
            _lex.take();
            if (e.members.empty())
               fail(at, "an enum without values");
            if (_proto3 && e.members.front().value != 0)
               fail(at, "proto3: the first value of an enum is zero");
            const std::size_t index     = declare(at, name, std::move(e));
            _s.types[index].attributes = std::move(attrs);
         }

         void service()
         {
            _lex.take();
            const token at = _lex.peek();
            Interface   iface;
            iface.name = ident();
            if (_declared.count(iface.name) || !_services.insert(iface.name).second)
               fail(at, "'" + iface.name + "' is declared twice");
            expect('{');
            // The references are noted when the interface has its place.
            struct pending
            {
               std::size_t func;
               bool        result;
               token       at;
            };
            std::vector<pending> refs;
            for (;;)
            {
               const token t = _lex.peek();
               if (t.is('}'))
                  break;
               if (t.is(';'))
                  _lex.take();
               else if (t.is("option"))
                  iface.attributes.push_back(Attribute{"proto_option", option_statement()});
               else if (t.is("rpc"))
               {
                  _lex.take();
                  Func f;
                  f.name = ident();
                  expect('(');
                  if (_lex.peek().is("stream"))
                  {
                     // `stream` alone is a type name.
                     const token s = _lex.take();
                     if (_lex.peek().is(')'))
                     {
                        f.params.push_back(Member{"request", Box<AnyType>{AnyType{Type{"stream"}}}, {}});
                        refs.push_back({iface.funcs.size(), false, s});
                        goto request_done;
                     }
                     f.attributes.push_back(Attribute{"proto_client_streaming", std::nullopt});
                  }
                  {
                     const token r = _lex.peek();
                     f.params.push_back(
                        Member{"request", Box<AnyType>{AnyType{Type{dotted_name()}}}, {}});
                     refs.push_back({iface.funcs.size(), false, r});
                  }
               request_done:
                  expect(')');
                  if (!_lex.peek().is("returns"))
                     fail(_lex.peek(), "expected 'returns'");
                  _lex.take();
                  expect('(');
                  bool named = false;
                  if (_lex.peek().is("stream"))
                  {
                     const token s = _lex.take();
                     if (_lex.peek().is(')'))
                     {
                        f.result = Box<AnyType>{AnyType{Type{"stream"}}};
                        refs.push_back({iface.funcs.size(), true, s});
                        named = true;
                     }
                     else
                        f.attributes.push_back(Attribute{"proto_server_streaming", std::nullopt});
                  }
                  if (!named)
                  {
                     const token r = _lex.peek();
                     f.result      = Box<AnyType>{AnyType{Type{dotted_name()}}};
                     refs.push_back({iface.funcs.size(), true, r});
                  }
                  expect(')');
                  if (_lex.peek().is('{'))
                  {
                     _lex.take();
                     while (!_lex.peek().is('}'))
                     {
                        if (_lex.peek().is(';'))
                           _lex.take();
                        else if (_lex.peek().is("option"))
                           f.attributes.push_back(Attribute{"proto_option", option_statement()});
                        else
                           fail(_lex.peek(), "expected an option");
                     }
                     _lex.take();
                  }
                  else
                     expect(';');
                  iface.funcs.push_back(std::move(f));
               }
               else
                  fail(t, "expected an rpc");
            }
            _lex.take();
            for (const auto& r : refs)
            {
               Func& f = iface.funcs[r.func];
               note(r.result ? **f.result : *f.params[0].type, std::string{}, r.at);
            }
            _s.interfaces.push_back(std::move(iface));
         }

         // Gives every reference the name of the declaration protobuf's
         // scoping finds: the first component in the innermost scope that
         // declares it, the rest from there.
         void finish()
         {
            const std::string prefix = _package.empty() ? std::string{} : _package + ".";
            for (const reference& r : _references)
            {
               std::string&      written = std::get<Type>(r.node->value).type;
               const std::string original = written;
               std::string       found;
               bool              ok = false;
               if (written.starts_with('.'))
               {
                  std::string_view absolute = std::string_view{written}.substr(1);
                  if (!prefix.empty() && absolute.starts_with(prefix))
                     absolute.remove_prefix(prefix.size());
                  else if (!prefix.empty())
                     absolute = {};
                  found = std::string{absolute};
                  ok    = !found.empty() && _declared.count(found);
               }
               else
               {
                  const std::string head = written.substr(0, written.find('.'));
                  std::string       scope = r.scope;
                  for (;;)
                  {
                     if (_declared.count(scope.empty() ? head : scope + "." + head))
                     {
                        found = scope.empty() ? written : scope + "." + written;
                        ok    = _declared.count(found) != 0;
                        break;
                     }
                     if (scope.empty())
                        break;
                     const auto dot = scope.rfind('.');
                     scope          = dot == std::string::npos ? std::string{} : scope.substr(0, dot);
                  }
                  if (!ok && !prefix.empty() && written.starts_with(prefix))
                  {
                     found = written.substr(prefix.size());
                     ok    = _declared.count(found) != 0;
                  }
               }
               if (ok)
                  written = std::move(found);
               else if (_imports.empty())
                  throw protobuf_parse_error("'" + original + "' is not declared", r.line, r.column);
               // else: a type of an imported file, by its name as written
            }
            if (!_proto3)
               _s.package.attributes.push_back(Attribute{"proto_syntax", "proto2"});
            for (auto& i : _imports)
               _s.package.attributes.push_back(Attribute{"proto_import", std::move(i)});
            for (auto& o : _options)
               _s.package.attributes.push_back(Attribute{"proto_option", std::move(o)});
         }
      };
   }  // namespace protobuf_parse_detail

   /// The Schema IR of a `.proto` file. `protobuf_parse_error` (line and
   /// column) on text that is not one, or that uses what psio does not read.
   inline Schema parse_protobuf(std::string_view text)
   {
      protobuf_parse_detail::Parser p{text};
      return p.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::parse_protobuf;
   using schema_types::protobuf_parse_error;
}
