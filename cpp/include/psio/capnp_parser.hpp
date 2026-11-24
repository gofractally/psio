#pragma once
//
// psio/capnp_parser.hpp — Cap'n Proto schema text → Schema IR.
//
// `psio::parse_capnp(text)` reads the schema language of `capnp compile`
// and keeps everything that decides the wire or the meaning of a message,
// in the form psio/capnp_layout.hpp documents ("How the Schema IR is read
// as Cap'n Proto") and psio/emit_capnp.hpp writes back:
//
//   @0x...;                    the package attribute `capnp_id`
//   struct Name @0x... { }     Object; a nested declaration is named in
//                              full (`Outer.Inner`); the id, when written,
//                              is the type's `capnp_id` attribute
//   name @N :Type = default;   a Member with `ordinal` and `default`: a
//                              data field's default in its normal form (an
//                              enumerant as its value), a pointer field's as
//                              written
//   name :union { ... }        a Member holding a Variant in place; the
//                              unnamed union is the member named ""; a
//                              union's own ordinal is the Variant's
//                              `ordinal`. `{ none @n :Void; some @n+1 :T; }`
//                              of a data type T is Option<T> — what the
//                              codec writes for std::optional
//   name :group { ... }        a Member holding an Object in place
//   enum Name { a @0; ... }    Enum over UInt16, each value its ordinal
//   interface Name { }         Resource; a method's parameters with their
//                              ordinals are its params, its results an
//                              Object in place (one result named `result`:
//                              that type)
//   using Name = Type;         a named alias
//   $Json.name("text")         (capnp/compat/json.capnp) the member's name
//                              in the IR; the Cap'n Proto name, when it is
//                              not the camelCase of that, is `capnp_name`
//   any other $annotation      an attribute `$name` with the value as
//                              written
//   const, annotation, using ... import
//                              kept as written (`capnp_decl` attributes of
//                              the package or of the enclosing struct) and
//                              written back; imports are not followed
//
// Refused: generic types and methods, a struct whose ordinals are not
// 0, 1, 2, ... (as the compiler refuses them).
//
// The text is untrusted: nesting is bounded (64 levels), memory is
// proportional to the text, and every malformed input is a
// `capnp_parse_error` (line and column).

#include <psio/capnp_layout.hpp>
#include <psio/schema_ir.hpp>

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

   struct capnp_parse_error : std::runtime_error
   {
      std::uint32_t line;
      std::uint32_t column;
      capnp_parse_error(const std::string& msg, std::uint32_t l,
                        std::uint32_t c)
         : std::runtime_error("capnp:" + std::to_string(l) + ":" +
                              std::to_string(c) + ": " + msg),
           line{l},
           column{c}
      {
      }
   };

   namespace capnp_parse_detail
   {
      enum class tok : std::uint8_t
      {
         ident,
         number,
         string,  // text: the contents, escapes resolved
         punct,
         eof
      };

      struct token
      {
         tok           kind = tok::eof;
         std::string   text;
         std::size_t   begin  = 0;  // in the source
         std::size_t   end    = 0;
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

         const token&     peek() const noexcept { return _tok; }
         std::string_view source() const noexcept { return _src; }

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
            throw capnp_parse_error(what, _line, _column);
         }

         void next()
         {
            while (!eof())
            {
               const char c = at();
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\xef' || c == '\xbb' ||
                   c == '\xbf')
                  advance();
               else if (c == '#')
                  while (!eof() && at() != '\n')
                     advance();
               else
                  break;
            }
            _tok        = token{};
            _tok.line   = _line;
            _tok.column = _column;
            _tok.begin  = _pos;
            if (eof())
            {
               _tok.end = _pos;
               return;
            }
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
                  const char d    = at();
                  const bool hex  = _tok.text.size() > 1 && (_tok.text[1] == 'x' || _tok.text[1] == 'X');
                  const bool sign = (d == '+' || d == '-') && !_tok.text.empty() && !hex &&
                                    (_tok.text.back() == 'e' || _tok.text.back() == 'E');
                  if (!(ident_char(d) || d == '.' || sign))
                     break;
                  _tok.text.push_back(d);
                  advance();
               }
            }
            else if (c == '"')
            {
               _tok.kind = tok::string;
               advance();
               while (true)
               {
                  if (eof() || at() == '\n')
                     fail("a string that does not end");
                  char d = at();
                  advance();
                  if (d == '"')
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
                        case 'a': d = '\a'; break;
                        case 'b': d = '\b'; break;
                        case 'f': d = '\f'; break;
                        case 'v': d = '\v'; break;
                        case '"': case '\'': case '\\': break;
                        default:
                           // (\x, octal: only where the text is kept as
                           // written; a name does not use them)
                           _tok.text.push_back('\\');
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
            _tok.end = _pos;
         }
      };

      // `unit_price`, `UnitPrice`, `unit-price` → `unitPrice`: the name
      // Cap'n Proto requires of a field (no underscores, lower case first).
      // A name that does not start with a letter gets `alt` in front.
      inline std::string capnp_camel(std::string_view name, bool upper_first = false)
      {
         std::string out;
         bool        boundary = upper_first;
         for (char c : name)
         {
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c);
            if (!alnum)
            {
               boundary = !out.empty() || upper_first;
               continue;
            }
            if (out.empty() && digit(c))
               out = upper_first ? "T" : "alt";
            if (boundary && c >= 'a' && c <= 'z')
               out += static_cast<char>(c - 'a' + 'A');
            else if (out.empty() && !upper_first && c >= 'A' && c <= 'Z')
               out += static_cast<char>(c - 'A' + 'a');
            else
               out += c;
            boundary = false;
         }
         if (out.empty())
            out = upper_first ? "T" : "alt";
         return out;
      }

      inline constexpr unsigned max_depth = 64;

      class Parser
      {
       public:
         explicit Parser(std::string_view src) : _lex{src} {}

         Schema run()
         {
            if (_lex.peek().is('@'))
            {
               _s.package.attributes.push_back(Attribute{"capnp_id", id()});
               expect(';');
            }
            while (_lex.peek().kind != tok::eof)
               declaration({}, nullptr, 0);
            finish();
            return std::move(_s);
         }

       private:
         struct declared
         {
            std::string   scope;  // the names its members are looked up in
            std::uint32_t line   = 0;
            std::uint32_t column = 0;
         };

         Lexer                 _lex;
         Schema                _s;
         std::vector<declared> _decls;  // parallel to _s.types
         std::set<std::string> _names;
         // Names given to `import "/capnp/compat/json.capnp"`.
         std::set<std::string> _json;

         [[noreturn]] static void fail(const token& at, const std::string& what)
         {
            throw capnp_parse_error(what, at.line, at.column);
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

         // `@0x...`: a 64-bit id, as `0x` and 16 hex digits.
         std::string id()
         {
            expect('@');
            const token   t = _lex.take();
            std::uint64_t v = 0;
            bool          ok = t.kind == tok::number && t.text.size() > 2 && t.text[0] == '0' &&
                               (t.text[1] == 'x' || t.text[1] == 'X');
            if (ok)
            {
               const auto r = std::from_chars(t.text.data() + 2, t.text.data() + t.text.size(), v, 16);
               ok           = r.ec == std::errc{} && r.ptr == t.text.data() + t.text.size();
            }
            if (!ok || (v >> 63) == 0)
               fail(t, "an id is a 64-bit hexadecimal number with its first bit set");
            static const char digits[] = "0123456789abcdef";
            std::string       out      = "0x";
            for (int shift = 60; shift >= 0; shift -= 4)
               out += digits[(v >> shift) & 15];
            return out;
         }

         // `@N`, or nothing.
         std::optional<std::string> ordinal()
         {
            if (!_lex.peek().is('@'))
               return std::nullopt;
            _lex.take();
            const token t = _lex.take();
            const auto  n = t.kind == tok::number ? fbs_layout_detail::parse_unsigned(t.text)
                                                  : std::nullopt;
            if (!n || *n > 0xffff)
               fail(t, "an ordinal is a number below 65536");
            // (`@3!`: what the compiler wants after a union's own ordinal)
            if (_lex.peek().is('!'))
               _lex.take();
            return std::to_string(*n);
         }

         // The source from `begin` up to the token that is next, trimmed.
         std::string raw_from(std::size_t begin) const
         {
            std::string_view text = _lex.source().substr(begin, _lex.peek().begin - begin);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
                                     text.back() == '\t' || text.back() == '\r'))
               text.remove_suffix(1);
            return std::string{text};
         }

         // Skips a value: up to `;`, `$`, `,` or a closing bracket outside
         // of every bracket of the value. Comments inside are skipped by the
         // lexer and stay in the text as written.
         void skip_value()
         {
            std::size_t depth = 0;
            while (true)
            {
               const token& t = _lex.peek();
               if (t.kind == tok::eof)
                  fail(t, "a value that does not end");
               if (t.is('(') || t.is('['))
                  ++depth;
               else if (t.is(')') || t.is(']'))
               {
                  if (depth == 0)
                     return;
                  --depth;
               }
               else if (depth == 0 && (t.is(';') || t.is('$') || t.is(',') || t.is('{')))
                  return;
               _lex.take();
            }
         }

         // `$name(value)`...: each an attribute `$name`, except the JSON
         // name, which is returned through `json_name`.
         void annotations(std::vector<Attribute>& out, std::optional<std::string>* json_name)
         {
            while (_lex.peek().is('$'))
            {
               _lex.take();
               const token at = _lex.peek();
               if (at.is_word("import"))
                  fail(at, "an annotation named through an import is not read");
               std::string name = ident("an annotation");
               std::string head = name;
               while (_lex.peek().is('.'))
               {
                  _lex.take();
                  name += ".";
                  name += ident("an annotation");
               }
               std::optional<std::string> value;
               std::optional<std::string> text;  // when the value is one string
               if (_lex.peek().is('('))
               {
                  _lex.take();
                  const std::size_t begin = _lex.peek().begin;
                  if (_lex.peek().kind == tok::string)
                     text = _lex.peek().text;
                  const std::size_t first_end = _lex.peek().end;
                  skip_value();
                  if (_lex.peek().begin != first_end && raw_from(begin).size() != first_end - begin)
                     text.reset();
                  value = raw_from(begin);
                  expect(')');
               }
               if (json_name && _json.count(head) && name == head + ".name" && text)
               {
                  *json_name = *text;
                  continue;
               }
               out.push_back(Attribute{"$" + name, std::move(value)});
            }
         }

         // Declares a named type; `scope` is the enclosing declaration's
         // name. Returns the name in the IR.
         std::string declare(const token& at, const std::string& scope, const std::string& name,
                             AnyType type, std::vector<Attribute> attrs = {})
         {
            std::string full = scope.empty() ? name : scope + "." + name;
            if (!_names.insert(full).second)
               fail(at, "'" + name + "' is declared twice");
            _decls.push_back({full, at.line, at.column});
            _s.types.push_back(Member{full, Box<AnyType>{std::move(type)}, std::move(attrs)});
            return full;
         }

         // One declaration of a file or of a struct / interface body:
         // `raw` receives what is kept as written.
         void declaration(const std::string& scope, std::vector<Attribute>* raw, unsigned depth)
         {
            const token at = _lex.peek();
            if (at.is_word("struct"))
               return struct_decl(scope, depth);
            if (at.is_word("enum"))
               return enum_decl(scope);
            if (at.is_word("interface"))
               return interface_decl(scope, depth);
            if (at.is('$') && !raw)
            {
               // An annotation of the file: `$Cxx.namespace("x");`
               const std::size_t begin = at.begin;
               std::vector<Attribute> ignored;
               annotations(ignored, nullptr);
               const std::string text = raw_from(begin);
               expect(';');
               _s.package.attributes.push_back(Attribute{"capnp_decl", text + ";"});
               return;
            }
            if (at.is_word("using") || at.is_word("const") || at.is_word("annotation"))
            {
               const std::size_t begin = at.begin;
               _lex.take();
               bool        alias = false;
               std::string name;
               token       name_at;
               AnyType     target;
               if (at.text == "using" && _lex.peek().kind == tok::ident &&
                   !_lex.peek().is_word("import"))
               {
                  name_at = _lex.peek();
                  name    = ident();
                  expect('=');
                  if (_lex.peek().is_word("import"))
                  {
                     _lex.take();
                     if (_lex.peek().kind == tok::string &&
                         _lex.peek().text == "/capnp/compat/json.capnp")
                        _json.insert(name);
                  }
                  else
                  {
                     // `using Name = Type;`
                     target = type(depth);
                     alias  = _lex.peek().is(';');
                  }
               }
               // Up to the `;` (a const's value may hold brackets).
               while (!_lex.peek().is(';'))
               {
                  if (_lex.peek().kind == tok::eof)
                     fail(_lex.peek(), "a declaration that does not end");
                  if (_lex.peek().is('(') || _lex.peek().is('['))
                  {
                     _lex.take();
                     skip_bracketed();
                  }
                  else
                     _lex.take();
               }
               _lex.take();
               if (alias)
                  declare(name_at, scope, name, std::move(target));
               else
               {
                  // (the text up to the token after the `;`: with it)
                  auto& attrs = raw ? *raw : _s.package.attributes;
                  attrs.push_back(Attribute{"capnp_decl", raw_from(begin)});
               }
               return;
            }
            fail(at, "expected a declaration");
         }

         // After an opening bracket: up to and including its closing one.
         void skip_bracketed()
         {
            std::size_t depth = 1;
            while (depth != 0)
            {
               const token t = _lex.take();
               if (t.kind == tok::eof)
                  fail(t, "a bracket that does not close");
               if (t.is('(') || t.is('['))
                  ++depth;
               else if (t.is(')') || t.is(']'))
                  --depth;
            }
         }

         void no_generics()
         {
            if (_lex.peek().is('('))
               fail(_lex.peek(), "generic types are not read");
         }

         void struct_decl(const std::string& scope, unsigned depth)
         {
            if (depth > max_depth)
               fail(_lex.peek(), "declarations nested too deeply");
            _lex.take();
            const token       at   = _lex.peek();
            const std::string name = ident();
            std::vector<Attribute> entry_attrs;
            if (_lex.peek().is('@'))
               entry_attrs.push_back(Attribute{"capnp_id", id()});
            no_generics();
            Object o;
            annotations(o.attributes, nullptr);
            expect('{');
            // (declared before its body: a nested type's name starts with it)
            const std::string full  = declare(at, scope, name, AnyType{Object{}}, std::move(entry_attrs));
            const std::size_t index = _s.types.size() - 1;
            members(o.members, &o.attributes, full, depth + 1, false);
            *_s.types[index].type = AnyType{std::move(o)};
         }

         // The members of a struct, group or union, up to and including the
         // closing brace.
         void members(std::vector<Member>& out, std::vector<Attribute>* raw,
                      const std::string& scope, unsigned depth, bool in_union)
         {
            if (depth > max_depth)
               fail(_lex.peek(), "declarations nested too deeply");
            std::set<std::string> names;
            bool                  unnamed = false;
            while (!_lex.peek().is('}'))
            {
               const token at = _lex.peek();
               if (at.kind != tok::ident)
                  fail(at, "expected a member");
               if (at.is_word("union"))
               {
                  // The unnamed union.
                  if (in_union)
                     fail(at, "a union cannot hold a union");
                  _lex.take();
                  Variant v;
                  if (_lex.peek().is('@'))
                     fail(_lex.peek(), "the unnamed union has no ordinal of its own");
                  annotations(v.attributes, nullptr);
                  expect('{');
                  members(v.members, nullptr, scope, depth + 1, true);
                  if (unnamed)
                     fail(at, "a struct has one unnamed union");
                  unnamed = true;
                  out.push_back(Member{"", Box<AnyType>{AnyType{std::move(v)}}, {}});
                  continue;
               }
               if (raw && (at.is_word("struct") || at.is_word("enum") || at.is_word("interface") ||
                           at.is_word("using") || at.is_word("const") || at.is_word("annotation")))
               {
                  declaration(scope, raw, depth);
                  continue;
               }
               Member      m;
               std::string written = ident("a member name");
               auto        number  = ordinal();
               expect(':');
               std::optional<std::string> json_name;
               if (_lex.peek().is_word("group") || _lex.peek().is_word("union"))
               {
                  const bool is_union = _lex.take().text == "union";
                  if (is_union && in_union)
                     fail(at, "a union cannot hold a union");
                  std::vector<Attribute> attrs;
                  annotations(attrs, &json_name);
                  expect('{');
                  if (is_union)
                  {
                     Variant v;
                     if (number)
                        v.attributes.push_back(Attribute{"ordinal", std::move(*number)});
                     members(v.members, nullptr, scope, depth + 1, true);
                     *m.type      = optional_scalar(v) ? std::move(*optional_scalar(v))
                                                       : AnyType{std::move(v)};
                     if (std::holds_alternative<Option>(m.type->value))
                        m.attributes.push_back(Attribute{"ordinal", optional_ordinal});
                  }
                  else
                  {
                     if (number)
                        fail(at, "a group has no ordinal");
                     Object g;
                     members(g.members, nullptr, scope, depth + 1, false);
                     if (g.members.empty())
                        fail(at, "a group has at least one member");
                     *m.type = AnyType{std::move(g)};
                  }
                  for (auto& a : attrs)
                     m.attributes.push_back(std::move(a));
               }
               else
               {
                  if (!number)
                     fail(at, "a field has an ordinal (@N)");
                  m.attributes.push_back(Attribute{"ordinal", std::move(*number)});
                  *m.type = type(depth);
                  if (_lex.peek().is('='))
                  {
                     _lex.take();
                     const std::size_t begin = _lex.peek().begin;
                     if (_lex.peek().is(';') || _lex.peek().is('$'))
                        fail(_lex.peek(), "expected a default value");
                     skip_value();
                     m.attributes.push_back(Attribute{"default", raw_from(begin)});
                  }
                  annotations(m.attributes, &json_name);
                  expect(';');
               }
               set_name(m, written, json_name);
               if (!names.insert(m.name).second)
                  fail(at, "member '" + m.name + "' is declared twice");
               out.push_back(std::move(m));
            }
            _lex.take();
         }

         // The name of a member in the IR: its JSON name when it has one.
         // The Cap'n Proto name is kept when it is not derivable.
         static void set_name(Member& m, const std::string& written,
                              const std::optional<std::string>& json_name)
         {
            m.name = json_name && !json_name->empty() ? *json_name : written;
            if (capnp_camel(m.name) != written)
               m.attributes.push_back(Attribute{"capnp_name", written});
         }

         // `{ none @n :Void; some @n+1 :T; }`, T a data type: Option<T>
         // (sets `optional_ordinal` to n).
         std::string            optional_ordinal;
         std::optional<AnyType> optional_scalar(const Variant& v)
         {
            if (v.members.size() != 2 || !v.attributes.empty() || v.members[0].name != "none" ||
                v.members[1].name != "some" || v.members[0].attributes.size() != 1 ||
                v.members[1].attributes.size() != 1)
               return std::nullopt;
            const auto* unit = std::get_if<Tuple>(&v.members[0].type->value);
            const auto& some = v.members[1].type->value;
            const bool  data = std::holds_alternative<Int>(some) || std::holds_alternative<Float>(some) ||
                               (std::holds_alternative<Custom>(some) &&
                                std::get<Custom>(some).id == "bool");
            const auto a = fbs_layout_detail::parse_unsigned(*v.members[0].attributes[0].value);
            const auto b = fbs_layout_detail::parse_unsigned(*v.members[1].attributes[0].value);
            if (!unit || !data || !a || !b || *b != *a + 1)
               return std::nullopt;
            optional_ordinal = *v.members[0].attributes[0].value;
            return AnyType{Option{Box<AnyType>{*v.members[1].type}}};
         }

         AnyType type(unsigned depth)
         {
            if (depth > max_depth)
               fail(_lex.peek(), "types nested too deeply");
            const token at   = _lex.peek();
            std::string name = ident("a type");
            while (_lex.peek().is('.'))
            {
               _lex.take();
               name += ".";
               name += ident("a type");
            }
            if (name == "List")
            {
               expect('(');
               AnyType inner = type(depth + 1);
               expect(')');
               return AnyType{List{Box<AnyType>{std::move(inner)}}};
            }
            if (_lex.peek().is('('))
               fail(at, "generic types are not read");
            if (name == "Void")
               return AnyType{Tuple{}};
            if (name == "Bool")
               return AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}};
            if (name == "Int8")
               return AnyType{Int{8, true}};
            if (name == "Int16")
               return AnyType{Int{16, true}};
            if (name == "Int32")
               return AnyType{Int{32, true}};
            if (name == "Int64")
               return AnyType{Int{64, true}};
            if (name == "UInt8")
               return AnyType{Int{8, false}};
            if (name == "UInt16")
               return AnyType{Int{16, false}};
            if (name == "UInt32")
               return AnyType{Int{32, false}};
            if (name == "UInt64")
               return AnyType{Int{64, false}};
            if (name == "Float32")
               return AnyType{Float{8, 24}};
            if (name == "Float64")
               return AnyType{Float{11, 53}};
            if (name == "Text")
               return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
            if (name == "Data")
               return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "hex"}};
            if (name == "AnyPointer" || name == "AnyStruct" || name == "AnyList" ||
                name == "Capability")
               return AnyType{Custom{Box<AnyType>{Tuple{}}, name}};
            return AnyType{Type{std::move(name)}};
         }

         void enum_decl(const std::string& scope)
         {
            _lex.take();
            const token            at   = _lex.peek();
            const std::string      name = ident();
            std::vector<Attribute> attrs;
            if (_lex.peek().is('@'))
               attrs.push_back(Attribute{"capnp_id", id()});
            annotations(attrs, nullptr);
            expect('{');
            Enum e;
            e.type = Box<AnyType>{AnyType{Int{16, false}}};
            std::vector<bool>     used;
            std::set<std::string> names;
            while (!_lex.peek().is('}'))
            {
               const token value_at = _lex.peek();
               Enumerator  v;
               v.name          = ident("an enumerant");
               const auto n    = ordinal();
               if (!n)
                  fail(value_at, "an enumerant has an ordinal (@N)");
               v.value = *fbs_layout_detail::parse_unsigned(*n);
               std::vector<Attribute>     ignored;
               std::optional<std::string> json_name;
               annotations(ignored, &json_name);
               if (json_name && !json_name->empty())
               {
                  // (an enumerator has no attributes: the Cap'n Proto name
                  // that its IR name does not give is kept on the enum)
                  if (capnp_camel(*json_name) != v.name)
                     attrs.push_back(Attribute{"capnp_name." + *n, v.name});
                  v.name = *json_name;
               }
               else if (capnp_camel(v.name) != v.name)
                  attrs.push_back(Attribute{"capnp_name." + *n, v.name});
               expect(';');
               if (used.size() <= v.value)
                  used.resize(v.value + 1, false);
               if (used[v.value])
                  fail(value_at, "ordinal @" + *n + " is used twice");
               used[v.value] = true;
               if (!names.insert(v.name).second)
                  fail(value_at, "enumerant '" + v.name + "' is declared twice");
               e.members.push_back(std::move(v));
            }
            _lex.take();
            for (std::size_t i = 0; i < used.size(); ++i)
               if (!used[i])
                  fail(at, "ordinals must be 0, 1, 2, ... without a gap (@" + std::to_string(i) +
                              " is missing)");
            declare(at, scope, name, AnyType{std::move(e)}, std::move(attrs));
         }

         // `(name :Type = default, ...)` — or a struct's name.
         std::vector<Member> parameters(unsigned depth, bool& by_name)
         {
            std::vector<Member> out;
            by_name = false;
            if (!_lex.peek().is('('))
            {
               by_name = true;
               out.push_back(Member{"", Box<AnyType>{type(depth)}, {}});
               return out;
            }
            _lex.take();
            std::set<std::string> names;
            while (!_lex.peek().is(')'))
            {
               const token at = _lex.peek();
               Member      m;
               const std::string written = ident("a parameter");
               expect(':');
               *m.type = type(depth);
               if (_lex.peek().is('='))
               {
                  _lex.take();
                  const std::size_t begin = _lex.peek().begin;
                  skip_value();
                  m.attributes.push_back(Attribute{"default", raw_from(begin)});
               }
               std::optional<std::string> json_name;
               annotations(m.attributes, &json_name);
               set_name(m, written, json_name);
               if (!names.insert(m.name).second)
                  fail(at, "parameter '" + m.name + "' is declared twice");
               out.push_back(std::move(m));
               if (_lex.peek().is(','))
                  _lex.take();
               else if (!_lex.peek().is(')'))
                  fail(_lex.peek(), "expected ',' or ')'");
            }
            _lex.take();
            return out;
         }

         void interface_decl(const std::string& scope, unsigned depth)
         {
            if (depth > max_depth)
               fail(_lex.peek(), "declarations nested too deeply");
            _lex.take();
            const token            at   = _lex.peek();
            const std::string      name = ident();
            std::vector<Attribute> entry_attrs;
            if (_lex.peek().is('@'))
               entry_attrs.push_back(Attribute{"capnp_id", id()});
            no_generics();
            Resource r;
            if (_lex.peek().is_word("extends"))
            {
               _lex.take();
               expect('(');
               const std::size_t begin = _lex.peek().begin;
               while (!_lex.peek().is(')'))
               {
                  if (_lex.peek().kind == tok::eof || _lex.peek().is('('))
                     fail(_lex.peek(), "expected the interfaces extended");
                  _lex.take();
               }
               r.attributes.push_back(Attribute{"capnp_extends", raw_from(begin)});
               _lex.take();
            }
            annotations(r.attributes, nullptr);
            expect('{');
            const std::string full  = declare(at, scope, name, AnyType{Resource{}}, std::move(entry_attrs));
            const std::size_t index = _s.types.size() - 1;
            r.name                  = full;
            std::set<std::string> method_names;
            while (!_lex.peek().is('}'))
            {
               const token member_at = _lex.peek();
               if (member_at.is_word("struct") || member_at.is_word("enum") ||
                   member_at.is_word("interface") || member_at.is_word("using") ||
                   member_at.is_word("const") || member_at.is_word("annotation"))
               {
                  declaration(full, &r.attributes, depth + 1);
                  continue;
               }
               Func f;
               f.name       = ident("a method name");
               const auto n = ordinal();
               if (!n)
                  fail(member_at, "a method has an ordinal (@N)");
               f.attributes.push_back(Attribute{"ordinal", *n});
               if (_lex.peek().is('['))
                  fail(_lex.peek(), "generic methods are not read");
               bool by_name = false;
               f.params     = parameters(depth, by_name);
               if (by_name)
               {
                  // `method @0 Params -> Results`: the struct is the
                  // parameter list.
                  f.params[0].name = "params";
                  f.attributes.push_back(Attribute{"capnp_params", "struct"});
               }
               if (_lex.peek().is('-'))
               {
                  _lex.take();
                  expect('>');
                  if (_lex.peek().is_word("stream"))
                  {
                     _lex.take();
                     f.attributes.push_back(Attribute{"capnp_results", "stream"});
                  }
                  else
                  {
                     bool   results_by_name = false;
                     auto   results         = parameters(depth, results_by_name);
                     if (results_by_name)
                     {
                        f.result = Box<AnyType>{std::move(*results[0].type)};
                        f.attributes.push_back(Attribute{"capnp_results", "struct"});
                     }
                     else if (results.size() == 1 && results[0].name == "result" &&
                              results[0].attributes.empty())
                        f.result = Box<AnyType>{std::move(*results[0].type)};
                     else
                        f.result = Box<AnyType>{AnyType{Object{std::move(results), {}}}};
                  }
               }
               annotations(f.attributes, nullptr);
               expect(';');
               if (!method_names.insert(f.name).second)
                  fail(member_at, "method '" + f.name + "' is declared twice");
               r.methods.push_back(std::move(f));
            }
            _lex.take();
            *_s.types[index].type = AnyType{std::move(r)};
         }

         // ── After the last declaration ──────────────────────────────────

         // The IR name of the type written as `written` in `scope`: looked
         // up there, then in each enclosing scope. Empty when this text
         // does not declare it (an imported file may).
         std::string lookup(const std::string& written, std::string scope) const
         {
            while (true)
            {
               const std::string full = scope.empty() ? written : scope + "." + written;
               if (_names.count(full))
                  return full;
               if (scope.empty())
                  return {};
               const auto dot = scope.rfind('.');
               scope          = dot == std::string::npos ? std::string{} : scope.substr(0, dot);
            }
         }

         // (the depth of a type is bounded by the parser's own limit)
         void resolve(AnyType& type, const std::string& scope) const
         {
            if (auto* t = std::get_if<Type>(&type.value))
            {
               if (auto ir = lookup(t->type, scope); !ir.empty())
                  t->type = std::move(ir);
            }
            else if (auto* l = std::get_if<List>(&type.value))
               resolve(*l->type, scope);
            else if (auto* o = std::get_if<Option>(&type.value))
               resolve(*o->type, scope);
            else if (auto* v = std::get_if<Variant>(&type.value))
               for (auto& m : v->members)
                  resolve(*m.type, scope);
            else if (auto* g = std::get_if<Object>(&type.value))
               for (auto& m : g->members)
                  resolve(*m.type, scope);
         }

         void finish()
         {
            for (std::size_t i = 0; i < _s.types.size(); ++i)
            {
               AnyType&           node  = *_s.types[i].type;
               const std::string& scope = _decls[i].scope;
               if (auto* r = std::get_if<Resource>(&node.value))
               {
                  for (auto& f : r->methods)
                  {
                     for (auto& p : f.params)
                        resolve(*p.type, scope);
                     if (f.result)
                        resolve(**f.result, scope);
                  }
               }
               else if (std::holds_alternative<Type>(node.value) ||
                        std::holds_alternative<List>(node.value))
               {
                  // An alias: looked up where it is declared.
                  const auto dot = scope.rfind('.');
                  resolve(node, dot == std::string::npos ? std::string{} : scope.substr(0, dot));
               }
               else
                  resolve(node, scope);
            }

            capnp_layout_detail::builder layout{_s};
            for (std::size_t i = 0; i < _s.types.size(); ++i)
            {
               const token at{tok::ident, {}, 0, 0, _decls[i].line, _decls[i].column};
               auto*       o = std::get_if<Object>(&_s.types[i].type->value);
               if (!o)
                  continue;
               try
               {
                  defaults(layout, o->members, at);
                  layout.layout(_s.types[i].name, o->members);
               }
               catch (const capnp_parse_error&)
               {
                  throw;
               }
               catch (const std::runtime_error& e)
               {
                  // (a type of an imported file is not an error here)
                  std::string_view what = e.what();
                  if (what.find("unknown type") != std::string_view::npos)
                     continue;
                  if (what.starts_with("capnp_layout: ") || what.starts_with("fbs_layout: "))
                     what.remove_prefix(what.find(": ") + 2);
                  fail(at, std::string{what});
               }
            }
         }

         // The defaults of data fields in their normal form.
         void defaults(capnp_layout_detail::builder& layout, std::vector<Member>& members,
                       const token& at)
         {
            for (auto& m : members)
            {
               if (auto* v = std::get_if<Variant>(&m.type->value))
                  defaults(layout, v->members, at);
               else if (auto* g = std::get_if<Object>(&m.type->value))
                  defaults(layout, g->members, at);
               for (auto& a : m.attributes)
               {
                  if (a.name != "default" || !a.value)
                     continue;
                  const auto shape = layout.simple_shape(*m.type);
                  if (!shape || shape->kind == capnp_slot_kind::pointer)
                     continue;  // as written
                  *a.value = scalar_default(layout, m, *shape, *a.value, at);
               }
               std::erase_if(m.attributes, [](const Attribute& a) {
                  return a.name == "default" && a.value && a.value->empty();
               });
            }
         }

         std::string scalar_default(capnp_layout_detail::builder&       layout,
                                    const Member&                       m,
                                    const capnp_layout_detail::data_shape& shape,
                                    std::string text, const token& at)
         {
            const auto bad = [&]() -> std::string {
               fail(at, "field '" + m.name + "': '" + text + "' is not a value of its type");
            };
            if (shape.kind == capnp_slot_kind::none)
               return text == "void" ? std::string{} : bad();
            // (a sign and its number may be apart)
            std::erase(text, ' ');
            const AnyType& type = layout.types().resolve(*m.type);
            if (shape.real)
            {
               const auto v = fbs_layout_detail::parse_real(text);
               if (!v)
                  return bad();
               if (*v != *v)
                  return "nan";
               if (*v == 0)
                  return std::signbit(*v) ? "-0" : "";
               char buffer[64];
               if (shape.lg == 5)
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
            if (shape.lg == 0)
               return text == "true" ? "true" : text == "false" ? "" : bad();
            if (!text.empty() && ident_start(text.front()))
            {
               // An enumerant.
               const AnyType* at_type = &type;
               while (const auto* c = std::get_if<Custom>(&at_type->value))
                  at_type = &layout.types().resolve(*c->type);
               const auto* e = std::get_if<Enum>(&at_type->value);
               if (e)
               {
                  const Member* entry = layout.types().entry_of(*at_type);
                  const std::vector<Attribute>* names = entry ? &entry->attributes : nullptr;
                  for (const auto& v : e->members)
                  {
                     const std::string* written =
                        names ? fbs_layout_detail::attribute(*names, "capnp_name." + std::to_string(v.value))
                              : nullptr;
                     if ((written ? *written : capnp_camel(v.name)) == text)
                        return v.value == 0 ? std::string{} : std::to_string(v.value);
                  }
               }
               return bad();
            }
            std::string_view digits   = text;
            const bool       negative = !digits.empty() && digits.front() == '-';
            if (negative)
               digits.remove_prefix(1);
            int base = 10;
            if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
            {
               base = 16;
               digits.remove_prefix(2);
            }
            else if (digits.size() > 1 && digits[0] == '0')
            {
               base = 8;
               digits.remove_prefix(1);
            }
            std::uint64_t u = 0;
            const auto    r = std::from_chars(digits.data(), digits.data() + digits.size(), u, base);
            if (digits.empty() || r.ec != std::errc{} || r.ptr != digits.data() + digits.size())
               return bad();
            const Int*     i     = std::get_if<Int>(&type.value);
            const bool     sign  = i && i->isSigned;
            const unsigned bits  = 1u << shape.lg;
            if (negative && (!sign || u > std::uint64_t{1} << (bits - 1)))
               return bad();
            if (!negative)
            {
               const std::uint64_t hi = sign ? (std::uint64_t{1} << (bits - 1)) - 1
                                             : bits == 64 ? ~std::uint64_t{0}
                                                          : (std::uint64_t{1} << bits) - 1;
               if (u > hi)
                  return bad();
            }
            if (u == 0)
               return {};
            return (negative ? "-" : "") + std::to_string(u);
         }
      };

   }  // namespace capnp_parse_detail

   /// Parse Cap'n Proto schema text into a Schema IR. Throws
   /// `capnp_parse_error` on text that is not a schema.
   inline Schema parse_capnp(std::string_view text)
   {
      capnp_parse_detail::Parser p{text};
      return p.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::capnp_parse_error;
   using schema_types::parse_capnp;
}
