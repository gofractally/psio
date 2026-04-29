#pragma once
//
// psio/capnp_parser.hpp — Cap'n Proto schema text → Schema IR.
//
// Recursive-descent parser for the .capnp schema language; the
// inverse of psio/emit_capnp.hpp.
//
// Coverage matches the emitter:
//   - file id (`@0x…;`) and a leading `# package: ns:name@version`
//     comment as carried by emit_capnp
//   - struct ↔ Object
//   - struct-with-anonymous-union ↔ Variant (union body lifted into
//     the IR as a Variant rather than nesting under Object)
//   - enum ↔ Variant of empty cases
//   - interface ↔ Resource (carries methods)
//   - field ordinals (`@N`) round-trip via the
//     Member.attributes["ordinal"] / Func.attributes["ordinal"]
//     carrier
//   - primitive types Int8/16/32/64, UInt8/16/32/64, Float32/64,
//     Bool, Text, Data, Void
//   - List(T) compound type; Data alone is treated as
//     `Custom{List<u8>, "hex"}` to match emit_capnp.

#include <psio/schema_ir.hpp>

#include <cctype>
#include <cstdint>
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
      struct Lexer
      {
         std::string_view src;
         std::size_t      pos    = 0;
         std::uint32_t    line   = 1;
         std::uint32_t    column = 1;

         bool eof() const noexcept { return pos >= src.size(); }
         char peek() const noexcept { return eof() ? '\0' : src[pos]; }
         char peek(std::size_t off) const noexcept
         {
            return pos + off >= src.size() ? '\0' : src[pos + off];
         }

         void advance()
         {
            char c = src[pos++];
            if (c == '\n')
            {
               ++line;
               column = 1;
            }
            else
            {
               ++column;
            }
         }

         void skip_ws_and_comments()
         {
            while (!eof())
            {
               char c = peek();
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
               {
                  advance();
               }
               else if (c == '#')
               {
                  while (!eof() && peek() != '\n')
                     advance();
               }
               else
               {
                  break;
               }
            }
         }

         // Read an identifier (letter / underscore start, alphanumeric tail).
         std::string read_ident()
         {
            skip_ws_and_comments();
            std::string out;
            if (eof())
               return out;
            char c = peek();
            if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_'))
               return out;
            while (!eof())
            {
               c = peek();
               if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
               {
                  out.push_back(c);
                  advance();
               }
               else
                  break;
            }
            return out;
         }

         // Read a digit string; returns the slice as text.
         std::string read_digits()
         {
            skip_ws_and_comments();
            std::string out;
            while (!eof() &&
                   std::isdigit(static_cast<unsigned char>(peek())))
            {
               out.push_back(peek());
               advance();
            }
            return out;
         }

         // Read `@<digits>` or `@0x<hex>` form. Returns the digit body
         // (no `@` / `0x` prefix); used for field ordinals and the
         // file id.  caller distinguishes hex vs. decimal from the
         // returned content.
         std::string read_ordinal_token()
         {
            skip_ws_and_comments();
            if (peek() != '@')
               return {};
            advance();
            std::string out = "@";
            if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X'))
            {
               out += peek();
               advance();
               out += peek();
               advance();
               while (!eof() &&
                      std::isxdigit(static_cast<unsigned char>(peek())))
               {
                  out += peek();
                  advance();
               }
            }
            else
            {
               while (!eof() &&
                      std::isdigit(static_cast<unsigned char>(peek())))
               {
                  out += peek();
                  advance();
               }
            }
            return out;
         }

         void expect(char c)
         {
            skip_ws_and_comments();
            if (peek() != c)
               throw capnp_parse_error(
                  std::string{"expected '"} + c + "'", line, column);
            advance();
         }

         void expect_keyword(std::string_view kw)
         {
            auto saved_line = line;
            auto saved_col  = column;
            auto id         = read_ident();
            if (id != kw)
               throw capnp_parse_error("expected '" + std::string{kw} +
                                          "', got '" + id + "'",
                                       saved_line, saved_col);
         }

         bool peek_keyword(std::string_view kw)
         {
            skip_ws_and_comments();
            if (pos + kw.size() > src.size())
               return false;
            for (std::size_t i = 0; i < kw.size(); ++i)
               if (src[pos + i] != kw[i])
                  return false;
            // Word boundary check.
            char after = pos + kw.size() < src.size()
                            ? src[pos + kw.size()]
                            : '\0';
            return !(std::isalnum(static_cast<unsigned char>(after)) ||
                     after == '_');
         }
      };

      // Snake-case from a Pascal/camel-case identifier so the IR
      // names round-trip cleanly with the WIT side (which kebab-cases
      // snake_case).  capnp identifiers come in Pascal/camel; the IR
      // canonicalises on snake.
      inline std::string to_snake(std::string_view name)
      {
         std::string out;
         out.reserve(name.size() + 4);
         for (std::size_t i = 0; i < name.size(); ++i)
         {
            char c = name[i];
            if (std::isupper(static_cast<unsigned char>(c)))
            {
               if (i > 0 && !out.empty() && out.back() != '_')
                  out.push_back('_');
               out.push_back(static_cast<char>(
                  std::tolower(static_cast<unsigned char>(c))));
            }
            else
            {
               out.push_back(c);
            }
         }
         return out;
      }

      class Parser
      {
       public:
         explicit Parser(std::string_view src) : _lex{Lexer{src}} {}

         Schema run()
         {
            parse_file_header();
            while (!_lex.eof())
            {
               _lex.skip_ws_and_comments();
               if (_lex.eof())
                  break;
               if (_lex.peek_keyword("struct"))
                  parse_struct_decl();
               else if (_lex.peek_keyword("enum"))
                  parse_enum_decl();
               else if (_lex.peek_keyword("interface"))
                  parse_interface_decl();
               else if (_lex.peek_keyword("using"))
                  skip_using();
               else if (_lex.peek_keyword("annotation"))
                  skip_balanced_until_semi();
               else if (_lex.peek_keyword("const"))
                  skip_balanced_until_semi();
               else
                  throw capnp_parse_error(
                     "unexpected top-level token", _lex.line, _lex.column);
            }
            return std::move(_s);
         }

       private:
         Lexer  _lex;
         Schema _s;

         void parse_file_header()
         {
            _lex.skip_ws_and_comments();
            if (_lex.peek() == '@')
            {
               auto tok = _lex.read_ordinal_token();
               _s.package.attributes.push_back(
                  Attribute{.name = "capnp_id", .value = tok});
               _lex.expect(';');
            }
         }

         void skip_using()
         {
            _lex.expect_keyword("using");
            skip_balanced_until_semi();
         }

         void skip_balanced_until_semi()
         {
            int depth = 0;
            while (!_lex.eof())
            {
               char c = _lex.peek();
               if (c == '{' || c == '(')
                  ++depth;
               else if (c == '}' || c == ')')
                  --depth;
               else if (c == ';' && depth == 0)
               {
                  _lex.advance();
                  return;
               }
               _lex.advance();
            }
         }

         // ── struct decl ──────────────────────────────────────────
         //
         // capnp struct body may contain:
         //   - field declarations (ident @N :Type;)
         //   - `union { … }` (anonymous; lifted to IR Variant)
         //   - nested struct / enum (recursively parsed)
         void parse_struct_decl()
         {
            _lex.expect_keyword("struct");
            auto name_pascal = _lex.read_ident();
            std::string name = to_snake(name_pascal);
            _lex.expect('{');

            std::vector<Member>                  fields;
            std::optional<std::vector<Member>>   union_cases;

            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               if (_lex.peek_keyword("union"))
               {
                  _lex.expect_keyword("union");
                  _lex.expect('{');
                  union_cases = parse_union_body();
                  _lex.expect('}');
                  continue;
               }
               if (_lex.peek_keyword("struct"))
               {
                  parse_struct_decl();
                  continue;
               }
               if (_lex.peek_keyword("enum"))
               {
                  parse_enum_decl();
                  continue;
               }
               if (_lex.peek_keyword("interface"))
               {
                  parse_interface_decl();
                  continue;
               }
               // field: ident @N :Type;
               fields.push_back(parse_field());
            }

            if (union_cases && fields.empty())
            {
               // Pure variant: the struct exists to wrap an anonymous
               // union with no surrounding fields.  Emit as Variant.
               _s.insert(name,
                         AnyType{Variant{.members    = *union_cases,
                                         .attributes = {}}});
            }
            else
            {
               // Plain object (or hybrid object+union — we surface
               // the fields and drop the union for now; round-tripping
               // a hybrid needs a richer IR shape).
               _s.insert(name, AnyType{Object{.members    = std::move(fields),
                                              .attributes = {}}});
            }
         }

         std::vector<Member> parse_union_body()
         {
            std::vector<Member> cases;
            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
                  break;
               cases.push_back(parse_field());
            }
            return cases;
         }

         Member parse_field()
         {
            auto ident_pascal = _lex.read_ident();
            std::string name  = to_snake(ident_pascal);
            auto ord_tok      = _lex.read_ordinal_token();
            std::vector<Attribute> attrs;
            if (!ord_tok.empty())
            {
               // strip leading '@'
               attrs.push_back(Attribute{
                  .name = "ordinal", .value = ord_tok.substr(1)});
            }
            _lex.expect(':');
            auto t = parse_type();
            _lex.expect(';');
            return Member{
               .name = name, .type = Box<AnyType>{std::move(t)}, .attributes = attrs};
         }

         // ── enum decl ────────────────────────────────────────────
         void parse_enum_decl()
         {
            _lex.expect_keyword("enum");
            auto name_pascal = _lex.read_ident();
            std::string name = to_snake(name_pascal);
            _lex.expect('{');
            std::vector<Member> cases;
            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               auto case_name = to_snake(_lex.read_ident());
               auto ord_tok   = _lex.read_ordinal_token();
               _lex.expect(';');
               std::vector<Attribute> attrs;
               if (!ord_tok.empty())
                  attrs.push_back(Attribute{
                     .name = "ordinal", .value = ord_tok.substr(1)});
               cases.push_back(Member{.name = case_name,
                                      .type = Box<AnyType>{Tuple{}},
                                      .attributes = std::move(attrs)});
            }
            _s.insert(name, AnyType{Variant{.members    = std::move(cases),
                                            .attributes = {}}});
         }

         // ── interface decl ───────────────────────────────────────
         void parse_interface_decl()
         {
            _lex.expect_keyword("interface");
            auto name_pascal = _lex.read_ident();
            std::string name = to_snake(name_pascal);
            // Optional `extends (…)` clause — skip if present.
            _lex.skip_ws_and_comments();
            if (_lex.peek_keyword("extends"))
            {
               _lex.expect_keyword("extends");
               _lex.expect('(');
               while (!_lex.eof() && _lex.peek() != ')')
                  _lex.advance();
               _lex.expect(')');
            }
            _lex.expect('{');
            std::vector<Func> methods;
            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               methods.push_back(parse_method());
            }
            Resource r;
            r.name    = name;
            r.methods = std::move(methods);
            _s.insert(name, AnyType{r});
         }

         Func parse_method()
         {
            auto ident_pascal = _lex.read_ident();
            std::string mname = to_snake(ident_pascal);
            auto ord_tok      = _lex.read_ordinal_token();
            std::vector<Attribute> attrs;
            if (!ord_tok.empty())
               attrs.push_back(Attribute{
                  .name = "ordinal", .value = ord_tok.substr(1)});

            _lex.expect('(');
            std::vector<Member> params;
            _lex.skip_ws_and_comments();
            if (_lex.peek() != ')')
            {
               while (true)
               {
                  auto pname = to_snake(_lex.read_ident());
                  _lex.expect(':');
                  auto ptype = parse_type();
                  params.push_back(
                     Member{.name = pname,
                            .type = Box<AnyType>{std::move(ptype)},
                            .attributes = {}});
                  _lex.skip_ws_and_comments();
                  if (_lex.peek() == ',')
                  {
                     _lex.advance();
                     continue;
                  }
                  break;
               }
            }
            _lex.expect(')');

            std::optional<Box<AnyType>> result;
            _lex.skip_ws_and_comments();
            if (_lex.peek() == '-')
            {
               _lex.advance();  // -
               _lex.expect('>');
               _lex.expect('(');
               // capnp returns are named-result records — we extract
               // a single `result :T` if present, otherwise build a
               // synthetic record-of-results.  Today only the single
               // form is generated by emit_capnp.
               auto first_ident = _lex.read_ident();
               (void)first_ident;
               _lex.expect(':');
               auto rtype = parse_type();
               result     = Box<AnyType>{std::move(rtype)};
               while (!_lex.eof() && _lex.peek() != ')')
                  _lex.advance();
               _lex.expect(')');
            }
            _lex.expect(';');

            return Func{.name       = mname,
                        .params     = std::move(params),
                        .result     = std::move(result),
                        .attributes = std::move(attrs)};
         }

         // ── type expression ──────────────────────────────────────
         AnyType parse_type()
         {
            auto ident = _lex.read_ident();
            if (ident == "Bool")
               return AnyType{
                  Custom{Box<AnyType>{Int{1, false}}, "bool"}};
            if (ident == "Int8")
               return AnyType{Int{8, true}};
            if (ident == "Int16")
               return AnyType{Int{16, true}};
            if (ident == "Int32")
               return AnyType{Int{32, true}};
            if (ident == "Int64")
               return AnyType{Int{64, true}};
            if (ident == "UInt8")
               return AnyType{Int{8, false}};
            if (ident == "UInt16")
               return AnyType{Int{16, false}};
            if (ident == "UInt32")
               return AnyType{Int{32, false}};
            if (ident == "UInt64")
               return AnyType{Int{64, false}};
            if (ident == "Float32")
               return AnyType{Float{8, 23}};
            if (ident == "Float64")
               return AnyType{Float{11, 52}};
            if (ident == "Text")
               return AnyType{Custom{
                  Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                  "string"}};
            if (ident == "Data")
               return AnyType{Custom{
                  Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                  "hex"}};
            if (ident == "Void")
               return AnyType{Tuple{}};
            if (ident == "List")
            {
               _lex.expect('(');
               auto inner = parse_type();
               _lex.expect(')');
               return AnyType{List{Box<AnyType>{std::move(inner)}}};
            }
            // Fallback: user-defined type → Type-by-name reference.
            return AnyType{Type{to_snake(ident)}};
         }
      };

   }  // namespace capnp_parse_detail

   /// Parse Cap'n Proto schema text into a Schema IR.
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
