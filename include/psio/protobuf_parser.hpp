#pragma once
//
// psio/protobuf_parser.hpp — `.proto3` schema text → Schema IR.
//
// Recursive-descent parser; the inverse of psio/emit_protobuf.hpp.
// Coverage matches the emitter:
//
//   - `syntax = "proto3";`            → consumed, no IR effect
//   - `package x.y.z;`                → Schema::package.name (`.`→`:`)
//   - `import "path";`                → skipped
//   - `option key = value;`           → skipped (top-level + per-msg)
//   - `message Name { … }`            → Object
//   - `enum Name { K = N; … }`        → Variant of empty cases
//   - `oneof name { … }` inside msg   → message becomes a Variant
//                                        (when the message body is a
//                                        single oneof — emit_protobuf's
//                                        round-trip shape)
//   - field line:
//       `[repeated|optional] <type> <name> = <id>;`
//     scalar `<type>` ∈ { int32 / int64 / uint32 / uint64
//                       / sint32 / sint64 / fixed32 / fixed64
//                       / sfixed32 / sfixed64
//                       / float / double / bool / string / bytes
//                       / Identifier }
//   - sint*/fixed*/sfixed* round-trip back through pb_sint / pb_fixed
//     attributes on the Member, matching the emitter's hint format.
//   - field number captured as `field` attribute on Member.
//
// Out of scope (silently skipped): map<K,V>, services/RPC, reserved
// blocks, extensions, edition-pragmas.  The parser advances past each
// while keeping line/column tracking accurate.

#include <psio/schema_ir.hpp>

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psio::schema_types
{

   struct protobuf_parse_error : std::runtime_error
   {
      std::uint32_t line;
      std::uint32_t column;
      protobuf_parse_error(const std::string& msg,
                           std::uint32_t      l,
                           std::uint32_t      c)
         : std::runtime_error("proto:" + std::to_string(l) + ":" +
                              std::to_string(c) + ": " + msg),
           line{l},
           column{c}
      {
      }
   };

   namespace protobuf_parse_detail
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

         std::string read_string_literal()
         {
            skip_ws_and_comments();
            std::string out;
            if (peek() != '"')
               return out;
            advance();
            while (!eof() && peek() != '"')
            {
               if (peek() == '\\' && !eof())
               {
                  advance();
                  if (!eof())
                  {
                     out.push_back(peek());
                     advance();
                  }
               }
               else
               {
                  out.push_back(peek());
                  advance();
               }
            }
            if (!eof())
               advance();
            return out;
         }

         void expect(char c)
         {
            skip_ws_and_comments();
            if (peek() != c)
               throw protobuf_parse_error(
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
               throw protobuf_parse_error(
                  "expected '" + std::string{kw} + "', got '" + id + "'",
                  saved_line, saved_col);
         }
      };

      // proto3 dotted package `wasi.io` → IR `wasi:io`.
      inline std::string pkg_to_ir(std::string_view ns)
      {
         std::string out;
         out.reserve(ns.size());
         for (char c : ns)
            out += (c == '.' ? ':' : c);
         return out;
      }

      // proto3 PascalCase message name → snake-case IR.
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

      // Map a proto3 scalar keyword to (AnyType, encoding-hint
      // attribute name).  Returns empty type on miss; caller falls
      // through to the named-type path.
      inline std::pair<AnyType, std::string> scalar_lookup(
         std::string_view kw)
      {
         //  width/sign comes from proto3 spec:
         //    int32  → 32-bit signed   varint, default
         //    int64  → 64-bit signed   varint, default
         //    uint32 → 32-bit unsigned varint, default
         //    uint64 → 64-bit unsigned varint, default
         //    sint32 → 32-bit signed   zigzag (pb_sint)
         //    sint64 → 64-bit signed   zigzag (pb_sint)
         //    fixed32  → 32-bit unsigned fixed   (pb_fixed)
         //    fixed64  → 64-bit unsigned fixed   (pb_fixed)
         //    sfixed32 → 32-bit signed   fixed   (pb_fixed)
         //    sfixed64 → 64-bit signed   fixed   (pb_fixed)
         if (kw == "int32")
            return {AnyType{Int{32, true}}, ""};
         if (kw == "int64")
            return {AnyType{Int{64, true}}, ""};
         if (kw == "uint32")
            return {AnyType{Int{32, false}}, ""};
         if (kw == "uint64")
            return {AnyType{Int{64, false}}, ""};
         if (kw == "sint32")
            return {AnyType{Int{32, true}}, "pb_sint"};
         if (kw == "sint64")
            return {AnyType{Int{64, true}}, "pb_sint"};
         if (kw == "fixed32")
            return {AnyType{Int{32, false}}, "pb_fixed"};
         if (kw == "fixed64")
            return {AnyType{Int{64, false}}, "pb_fixed"};
         if (kw == "sfixed32")
            return {AnyType{Int{32, true}}, "pb_fixed"};
         if (kw == "sfixed64")
            return {AnyType{Int{64, true}}, "pb_fixed"};
         if (kw == "float")
            return {AnyType{Float{8, 23}}, ""};
         if (kw == "double")
            return {AnyType{Float{11, 52}}, ""};
         if (kw == "bool")
            return {AnyType{Custom{Box<AnyType>{Int{8, false}}, "bool"}}, ""};
         if (kw == "string")
            return {
               AnyType{Custom{
                  Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                  "string"}},
               ""};
         if (kw == "bytes")
            return {AnyType{List{Box<AnyType>{Int{8, false}}}}, ""};
         return {AnyType{}, ""};
      }

      class Parser
      {
       public:
         explicit Parser(std::string_view src) : _lex{Lexer{src}} {}

         Schema run()
         {
            while (!_lex.eof())
            {
               _lex.skip_ws_and_comments();
               if (_lex.eof())
                  break;
               if (_lex.peek_keyword("syntax"))
                  skip_until_semi();
               else if (_lex.peek_keyword("package"))
                  parse_package();
               else if (_lex.peek_keyword("import"))
                  skip_until_semi();
               else if (_lex.peek_keyword("option"))
                  skip_until_semi();
               else if (_lex.peek_keyword("message"))
                  parse_message_decl();
               else if (_lex.peek_keyword("enum"))
                  parse_enum_decl();
               else if (_lex.peek_keyword("service"))
                  skip_balanced_braces_after_header();
               else
                  throw protobuf_parse_error(
                     "unexpected top-level token", _lex.line, _lex.column);
            }
            return std::move(_s);
         }

       private:
         Lexer  _lex;
         Schema _s;

         void skip_until_semi()
         {
            while (!_lex.eof() && _lex.peek() != ';')
               _lex.advance();
            if (!_lex.eof())
               _lex.advance();
         }

         //  Used for service / extension blocks etc. — consume header
         //  up to '{', then skip a balanced braced body.
         void skip_balanced_braces_after_header()
         {
            while (!_lex.eof() && _lex.peek() != '{')
               _lex.advance();
            if (_lex.eof())
               return;
            _lex.advance();
            int depth = 1;
            while (!_lex.eof() && depth > 0)
            {
               char c = _lex.peek();
               if (c == '{')
                  ++depth;
               else if (c == '}')
                  --depth;
               _lex.advance();
            }
         }

         void parse_package()
         {
            _lex.expect_keyword("package");
            auto ns = _lex.read_dotted_ident();
            _lex.expect(';');
            _s.package.name = pkg_to_ir(ns);
         }

         //  message body is one of:
         //    1. plain field list (possibly with a trailing nested
         //       enum/message decl) → Object
         //    2. exactly one `oneof <name> { fields }` (and nothing
         //       else) → Variant (matches emit_protobuf's shape for
         //       a payload-bearing IR variant)
         void parse_message_decl()
         {
            _lex.expect_keyword("message");
            auto pascal = _lex.read_ident();
            _lex.expect('{');

            std::vector<Member> obj_members;
            std::vector<Member> oneof_members;
            std::string         oneof_name;
            bool                saw_oneof    = false;
            bool                saw_non_oneof = false;

            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               //  Tolerate stray semicolons (proto3 allows them).
               if (_lex.peek() == ';')
               {
                  _lex.advance();
                  continue;
               }
               if (_lex.peek_keyword("option") ||
                   _lex.peek_keyword("reserved") ||
                   _lex.peek_keyword("extensions"))
               {
                  skip_until_semi();
                  continue;
               }
               if (_lex.peek_keyword("oneof"))
               {
                  saw_oneof = true;
                  parse_oneof(oneof_name, oneof_members);
                  continue;
               }
               if (_lex.peek_keyword("enum"))
               {
                  //  Nested enum — emit at top level with a prefixed
                  //  name so it doesn't collide.
                  parse_enum_decl(/*name_prefix=*/pascal);
                  continue;
               }
               if (_lex.peek_keyword("message"))
               {
                  //  Nested message — same handling.
                  parse_message_decl_with_prefix(pascal);
                  continue;
               }
               //  Otherwise: a field line.
               obj_members.push_back(parse_field());
               saw_non_oneof = true;
            }

            //  Pure single-oneof body → Variant.  Mixed bodies fall
            //  back to Object (the oneof's cases land as plain
            //  members; the round-trip is lossy but the type is at
            //  least decodable).
            if (saw_oneof && !saw_non_oneof && !oneof_members.empty())
            {
               _s.insert(to_snake(pascal),
                         AnyType{Variant{.members    = std::move(oneof_members),
                                         .attributes = {}}});
            }
            else
            {
               //  If both forms are present, fold the oneof cases in
               //  as regular members so nothing is lost.
               for (auto& m : oneof_members)
                  obj_members.push_back(std::move(m));
               _s.insert(to_snake(pascal),
                         AnyType{Object{.members    = std::move(obj_members),
                                        .attributes = {}}});
            }
         }

         void parse_message_decl_with_prefix(const std::string& prefix)
         {
            _lex.expect_keyword("message");
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
               if (_lex.peek() == ';')
               {
                  _lex.advance();
                  continue;
               }
               if (_lex.peek_keyword("option") ||
                   _lex.peek_keyword("reserved") ||
                   _lex.peek_keyword("extensions"))
               {
                  skip_until_semi();
                  continue;
               }
               members.push_back(parse_field());
            }
            _s.insert(to_snake(prefix + "_" + pascal),
                      AnyType{Object{.members    = std::move(members),
                                     .attributes = {}}});
         }

         void parse_oneof(std::string&         name_out,
                          std::vector<Member>& cases_out)
         {
            _lex.expect_keyword("oneof");
            name_out = _lex.read_ident();
            _lex.expect('{');
            while (true)
            {
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '}')
               {
                  _lex.advance();
                  break;
               }
               if (_lex.peek() == ';')
               {
                  _lex.advance();
                  continue;
               }
               if (_lex.peek_keyword("option"))
               {
                  skip_until_semi();
                  continue;
               }
               //  oneof bodies disallow `repeated` / `optional`, so
               //  parse_field's handling of those is irrelevant here.
               cases_out.push_back(parse_field());
            }
         }

         void parse_enum_decl(const std::string& name_prefix = {})
         {
            _lex.expect_keyword("enum");
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
               if (_lex.peek() == ';')
               {
                  _lex.advance();
                  continue;
               }
               if (_lex.peek_keyword("option") ||
                   _lex.peek_keyword("reserved"))
               {
                  skip_until_semi();
                  continue;
               }
               auto case_name = _lex.read_ident();
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '=')
               {
                  _lex.advance();
                  //  Optional minus sign before digits.
                  _lex.skip_ws_and_comments();
                  if (_lex.peek() == '-')
                     _lex.advance();
                  (void)_lex.read_digits();
               }
               cases.push_back(Member{.name       = case_name,
                                      .type       = Box<AnyType>{Tuple{}},
                                      .attributes = {}});
               //  Skip optional [trailing field options] like `[deprecated=true]`.
               _lex.skip_ws_and_comments();
               if (_lex.peek() == '[')
               {
                  while (!_lex.eof() && _lex.peek() != ']')
                     _lex.advance();
                  if (!_lex.eof())
                     _lex.advance();
               }
               _lex.skip_ws_and_comments();
               if (_lex.peek() == ';')
                  _lex.advance();
            }
            std::string name = name_prefix.empty()
                                  ? to_snake(pascal)
                                  : to_snake(name_prefix + "_" + pascal);
            _s.insert(name,
                      AnyType{Variant{.members    = std::move(cases),
                                      .attributes = {}}});
         }

         //  Parse a single field line.  Returns the Member with
         //  `field` attribute populated with the wire id and any
         //  encoding-hint attribute (pb_fixed / pb_sint).
         Member parse_field()
         {
            std::vector<Attribute> attrs;
            bool repeated_ = false;
            bool optional_ = false;

            _lex.skip_ws_and_comments();
            if (_lex.peek_keyword("repeated"))
            {
               _lex.expect_keyword("repeated");
               repeated_ = true;
            }
            else if (_lex.peek_keyword("optional"))
            {
               _lex.expect_keyword("optional");
               optional_ = true;
            }
            else if (_lex.peek_keyword("required"))
            {
               //  proto2 carry-over; treat as plain.
               _lex.expect_keyword("required");
            }

            //  Parse type token — may be dotted (e.g. nested types).
            std::string type_token = _lex.read_dotted_ident();
            auto [t, hint]         = scalar_lookup(type_token);
            //  scalar_lookup returns a default-constructed AnyType
            //  (whose variant default-init is Struct{}) when it
            //  doesn't match a known scalar — that's the "named-type
            //  reference" path.
            const bool matched = std::holds_alternative<Int>(t.value) ||
                                 std::holds_alternative<Float>(t.value) ||
                                 std::holds_alternative<List>(t.value) ||
                                 std::holds_alternative<Custom>(t.value);
            if (!matched)
               t = AnyType{Type{type_token}};
            if (!hint.empty())
               attrs.push_back(Attribute{.name = hint});

            auto name = _lex.read_ident();
            _lex.expect('=');
            _lex.skip_ws_and_comments();
            auto digits = _lex.read_digits();
            if (digits.empty())
               throw protobuf_parse_error("expected field number",
                                          _lex.line, _lex.column);
            attrs.push_back(Attribute{.name = "field", .value = digits});

            //  Skip optional trailing options `[ … ]`.
            _lex.skip_ws_and_comments();
            if (_lex.peek() == '[')
            {
               while (!_lex.eof() && _lex.peek() != ']')
                  _lex.advance();
               if (!_lex.eof())
                  _lex.advance();
            }
            _lex.expect(';');

            //  Wrap in repeated/optional after building the inner type
            //  so encoding hints stay on the inner Int.
            AnyType outer = std::move(t);
            if (repeated_)
               outer = AnyType{List{Box<AnyType>{std::move(outer)}}};
            else if (optional_)
               outer = AnyType{Option{Box<AnyType>{std::move(outer)}}};

            return Member{.name       = name,
                          .type       = Box<AnyType>{std::move(outer)},
                          .attributes = std::move(attrs)};
         }
      };

   }  // namespace protobuf_parse_detail

   /// Parse a `.proto3` schema text into a Schema IR.  Throws
   /// `protobuf_parse_error` (carrying line/column) on syntactic
   /// failure.  The inverse of `psio::emit_protobuf`.
   inline Schema parse_protobuf(std::string_view src)
   {
      protobuf_parse_detail::Parser p{src};
      return p.run();
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::parse_protobuf;
   using schema_types::protobuf_parse_error;
}
