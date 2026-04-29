#pragma once
//
// psio/fbs_parser.hpp — FlatBuffers `.fbs` schema text → Schema IR.
//
// Recursive-descent parser for the FBS schema language; the inverse
// of psio/emit_fbs.hpp.  Coverage matches the emitter:
//
//   - `namespace ns.name;`           → Schema::package.name
//                                      (with `.` re-mapped to `:`)
//   - `table Name { … }`              → Object
//   - `struct Name { … }`             → Struct
//   - `enum Name : ubyte { a, b }`    → Variant of empty cases
//   - `union Name { A, B, C }`        → Variant; combined with the
//                                      sibling wrapper table when
//                                      emit_fbs's pair-form is
//                                      detected (Name + NameUnion).
//                                      Round-tripping a hand-written
//                                      union without the wrapper
//                                      is not yet supported.
//   - field id `(id: N)`              → Member.attributes["fbs_id"]
//   - `root_type Name;`               → ignored at the IR level today
//                                      (not on the round-trip path
//                                      since it points at an
//                                      already-parsed table)
//   - primitive types: byte/ubyte/short/ushort/int/uint/long/ulong,
//     float/double, bool, string, [T]
//   - bare identifier type → Type-by-name reference

#include <psio/schema_ir.hpp>

#include <cctype>
#include <cstdint>
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
               ++column;
         }

         void skip_ws_and_comments()
         {
            while (!eof())
            {
               char c = peek();
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                  advance();
               else if (c == '/' && peek(1) == '/')
               {
                  while (!eof() && peek() != '\n')
                     advance();
               }
               else if (c == '/' && peek(1) == '*')
               {
                  advance();
                  advance();
                  while (!eof() &&
                         !(peek() == '*' && peek(1) == '/'))
                     advance();
                  if (!eof())
                  {
                     advance();
                     advance();
                  }
               }
               else
                  break;
            }
         }

         std::string read_ident()
         {
            skip_ws_and_comments();
            std::string out;
            char        c = peek();
            if (eof() ||
                !(std::isalpha(static_cast<unsigned char>(c)) || c == '_'))
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

         std::string read_dotted_ident()
         {
            std::string out = read_ident();
            while (!eof())
            {
               skip_ws_and_comments();
               if (peek() != '.')
                  break;
               advance();
               out += '.';
               out += read_ident();
            }
            return out;
         }

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

         void expect(char c)
         {
            skip_ws_and_comments();
            if (peek() != c)
               throw fbs_parse_error(
                  std::string{"expected '"} + c + "'", line, column);
            advance();
         }

         bool peek_keyword(std::string_view kw)
         {
            skip_ws_and_comments();
            if (pos + kw.size() > src.size())
               return false;
            for (std::size_t i = 0; i < kw.size(); ++i)
               if (src[pos + i] != kw[i])
                  return false;
            char after = pos + kw.size() < src.size()
                            ? src[pos + kw.size()]
                            : '\0';
            return !(std::isalnum(static_cast<unsigned char>(after)) ||
                     after == '_');
         }

         void expect_keyword(std::string_view kw)
         {
            auto saved_line = line;
            auto saved_col  = column;
            auto id         = read_ident();
            if (id != kw)
               throw fbs_parse_error("expected '" + std::string{kw} +
                                        "', got '" + id + "'",
                                     saved_line, saved_col);
         }
      };

      // Pascal-case to snake-case so IR names round-trip with WIT etc.
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
               out.push_back(c);
         }
         return out;
      }

      // FBS namespace `wasi.io` → IR package name `wasi:io`.
      inline std::string fbs_to_pkg(std::string_view ns)
      {
         std::string out;
         out.reserve(ns.size());
         for (char c : ns)
            out += (c == '.' ? ':' : c);
         return out;
      }

      class Parser
      {
       public:
         explicit Parser(std::string_view src) : _lex{Lexer{src}} {}

         Schema run()
         {
            // Defer wrapper tables (the Object generated by emit_fbs
            // for a Variant-with-payloads) until after we've seen the
            // matching union; we collapse them into a single Variant
            // entry on the IR side.
            while (!_lex.eof())
            {
               _lex.skip_ws_and_comments();
               if (_lex.eof())
                  break;
               if (_lex.peek_keyword("namespace"))
                  parse_namespace();
               else if (_lex.peek_keyword("table"))
                  parse_table_decl();
               else if (_lex.peek_keyword("struct"))
                  parse_struct_decl();
               else if (_lex.peek_keyword("enum"))
                  parse_enum_decl();
               else if (_lex.peek_keyword("union"))
                  parse_union_decl();
               else if (_lex.peek_keyword("root_type"))
                  skip_until_semi();
               else if (_lex.peek_keyword("file_identifier") ||
                        _lex.peek_keyword("file_extension") ||
                        _lex.peek_keyword("attribute") ||
                        _lex.peek_keyword("include"))
                  skip_until_semi();
               else
                  throw fbs_parse_error(
                     "unexpected top-level token", _lex.line, _lex.column);
            }
            collapse_union_wrappers();
            return std::move(_s);
         }

       private:
         Lexer  _lex;
         Schema _s;

         // Collected union declarations awaiting their matching
         // wrapper table.  Keyed by union name (Pascal-case).
         struct PendingUnion
         {
            std::string         pascal_name;
            std::vector<Member> cases;
         };
         std::vector<PendingUnion> _pending_unions;

         void parse_namespace()
         {
            _lex.expect_keyword("namespace");
            auto ns = _lex.read_dotted_ident();
            _lex.expect(';');
            _s.package.name = fbs_to_pkg(ns);
         }

         void skip_until_semi()
         {
            while (!_lex.eof() && _lex.peek() != ';')
               _lex.advance();
            if (!_lex.eof())
               _lex.advance();
         }

         void parse_table_decl()
         {
            _lex.expect_keyword("table");
            auto pascal = _lex.read_ident();
            _lex.expect('{');
            std::vector<Member> members;
            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               members.push_back(parse_field());
            }
            _s.insert(to_snake(pascal),
                      AnyType{Object{.members    = std::move(members),
                                     .attributes = {}}});
         }

         void parse_struct_decl()
         {
            _lex.expect_keyword("struct");
            auto pascal = _lex.read_ident();
            _lex.expect('{');
            std::vector<Member> members;
            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               members.push_back(parse_field());
            }
            _s.insert(to_snake(pascal),
                      AnyType{Struct{.members    = std::move(members),
                                     .attributes = {}}});
         }

         void parse_enum_decl()
         {
            _lex.expect_keyword("enum");
            auto pascal = _lex.read_ident();
            // Optional `: type` underlying.
            _lex.skip_ws_and_comments();
            if (_lex.peek() == ':')
            {
               _lex.advance();
               (void)_lex.read_ident();  // discard
            }
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
               auto case_name = _lex.read_ident();
               // Optional `= N` value — ignore for now.
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '=')
               {
                  _lex.advance();
                  (void)_lex.read_digits();
               }
               cases.push_back(Member{.name = to_snake(case_name),
                                      .type = Box<AnyType>{Tuple{}},
                                      .attributes = {}});
               _lex.skip_ws_and_comments();
               if (_lex.peek() == ',')
                  _lex.advance();
            }
            _s.insert(to_snake(pascal),
                      AnyType{Variant{.members    = std::move(cases),
                                      .attributes = {}}});
         }

         void parse_union_decl()
         {
            _lex.expect_keyword("union");
            auto pascal = _lex.read_ident();
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
               auto case_pascal = _lex.read_ident();
               cases.push_back(
                  Member{.name = to_snake(case_pascal),
                         .type = Box<AnyType>{Type{to_snake(case_pascal)}},
                         .attributes = {}});
               _lex.skip_ws_and_comments();
               if (_lex.peek() == ',')
                  _lex.advance();
            }
            _pending_unions.push_back(
               PendingUnion{pascal, std::move(cases)});
         }

         Member parse_field()
         {
            auto name = _lex.read_ident();
            _lex.expect(':');
            auto t = parse_type();
            std::vector<Attribute> attrs;
            // Optional metadata `(id: N, …)`.
            _lex.skip_ws_and_comments();
            if (_lex.peek() == '(')
            {
               _lex.advance();
               while (true)
               {
                  _lex.skip_ws_and_comments();
                  auto key = _lex.read_ident();
                  _lex.skip_ws_and_comments();
                  if (_lex.peek() == ':')
                  {
                     _lex.advance();
                     _lex.skip_ws_and_comments();
                     auto val = _lex.read_digits();
                     if (val.empty())
                        val = _lex.read_ident();
                     if (key == "id")
                        attrs.push_back(
                           Attribute{.name = "fbs_id", .value = val});
                     else
                        attrs.push_back(
                           Attribute{.name = key, .value = val});
                  }
                  else
                  {
                     attrs.push_back(
                        Attribute{.name = key, .value = std::nullopt});
                  }
                  _lex.skip_ws_and_comments();
                  if (_lex.peek() == ',')
                  {
                     _lex.advance();
                     continue;
                  }
                  break;
               }
               _lex.expect(')');
            }
            // Optional default `= literal` — skip.
            _lex.skip_ws_and_comments();
            if (_lex.peek() == '=')
            {
               _lex.advance();
               while (!_lex.eof() && _lex.peek() != ';')
                  _lex.advance();
            }
            _lex.expect(';');
            return Member{
               .name       = name,
               .type       = Box<AnyType>{std::move(t)},
               .attributes = std::move(attrs)};
         }

         AnyType parse_type()
         {
            // `[T]` list form
            _lex.skip_ws_and_comments();
            if (_lex.peek() == '[')
            {
               _lex.advance();
               auto inner = parse_type();
               _lex.expect(']');
               return AnyType{List{Box<AnyType>{std::move(inner)}}};
            }
            auto ident = _lex.read_ident();
            if (ident == "bool")
               return AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}};
            if (ident == "byte")
               return AnyType{Int{8, true}};
            if (ident == "ubyte")
               return AnyType{Int{8, false}};
            if (ident == "short")
               return AnyType{Int{16, true}};
            if (ident == "ushort")
               return AnyType{Int{16, false}};
            if (ident == "int")
               return AnyType{Int{32, true}};
            if (ident == "uint")
               return AnyType{Int{32, false}};
            if (ident == "long")
               return AnyType{Int{64, true}};
            if (ident == "ulong")
               return AnyType{Int{64, false}};
            if (ident == "float")
               return AnyType{Float{8, 23}};
            if (ident == "double")
               return AnyType{Float{11, 52}};
            if (ident == "string")
               return AnyType{Custom{
                  Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                  "string"}};
            if (ident == "void")
               return AnyType{Tuple{}};
            return AnyType{Type{to_snake(ident)}};
         }

         // After top-level parsing, fold any "Name + NameUnion +
         // table NameUnion case-table" structure produced by
         // emit_fbs back into a single Variant entry under `Name`.
         void collapse_union_wrappers()
         {
            for (auto& pu : _pending_unions)
            {
               // emit_fbs names the union "<NamePascal>Union" and
               // wraps it in a table "<NamePascal>".  Look for both.
               if (pu.pascal_name.size() >= 5 &&
                   pu.pascal_name.compare(pu.pascal_name.size() - 5, 5,
                                          "Union") == 0)
               {
                  std::string outer_pascal =
                     pu.pascal_name.substr(0, pu.pascal_name.size() - 5);
                  std::string outer_snake = to_snake(outer_pascal);
                  // Replace the wrapper Object with a Variant.
                  for (auto& m : _s.types)
                     if (m.name == outer_snake)
                     {
                        *m.type = AnyType{
                           Variant{.members    = std::move(pu.cases),
                                   .attributes = {}}};
                     }
               }
               else
               {
                  // Standalone union — surface as a Variant under
                  // the union's own name.  The case-table empties /
                  // wrappers stay in the schema as separate entries.
                  _s.insert(to_snake(pu.pascal_name),
                            AnyType{Variant{.members    = std::move(pu.cases),
                                            .attributes = {}}});
               }
            }
         }
      };

   }  // namespace fbs_parse_detail

   /// Parse FlatBuffers schema text into a Schema IR.
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
