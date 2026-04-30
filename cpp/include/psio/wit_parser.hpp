#pragma once

// Recursive-descent WIT parser.
//
//   auto world = psio::wit_parse("package my:pkg@1.0.0;\n"
//                                "world greeter {\n"
//                                "  export greet: func(name: string) -> string;\n"
//                                "}\n");
//
// Parses the Component Model WIT text format into wit_world structs.
// Supports: package, interface, world, record, variant, enum, flags,
// type aliases, function signatures, list<T>, option<T>, result<T,E>, tuple<T...>.

#include <psio/wit_types.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psio {

   // =========================================================================
   // Parser error
   // =========================================================================

   struct wit_parse_error : std::runtime_error {
      uint32_t line;
      uint32_t column;
      wit_parse_error(const std::string& msg, uint32_t line, uint32_t col)
         : std::runtime_error("wit:" + std::to_string(line) + ":" +
                              std::to_string(col) + ": " + msg),
           line(line), column(col) {}
   };

   // =========================================================================
   // Lexer
   // =========================================================================

   namespace detail {

      enum class wit_tok : uint8_t {
         eof, ident, number,
         // punctuation
         lbrace, rbrace, lparen, rparen, langle, rangle,
         comma, colon, semicolon, equal, arrow, slash, at, dot, star,
         // keywords
         kw_package, kw_interface, kw_world,
         kw_import, kw_export, kw_use, kw_include, kw_as, kw_with,
         kw_func, kw_type,
         kw_record, kw_variant, kw_enum, kw_flags,
         kw_resource, kw_own, kw_borrow,
         // primitive type keywords
         kw_bool, kw_char, kw_string,
         kw_u8, kw_s8, kw_u16, kw_s16, kw_u32, kw_s32, kw_u64, kw_s64,
         kw_f32, kw_f64,
         // compound type keywords
         kw_list, kw_option, kw_result, kw_tuple,
      };

      struct wit_token {
         wit_tok          kind = wit_tok::eof;
         std::string_view text;
         uint32_t         line = 1;
         uint32_t         col  = 1;
      };

      class wit_lexer {
       public:
         explicit wit_lexer(std::string_view src) : src_(src), pos_(0), line_(1), col_(1) {}

         wit_token next() {
            skip_ws_and_comments();
            if (pos_ >= src_.size())
               return {wit_tok::eof, {}, line_, col_};

            uint32_t start_line = line_;
            uint32_t start_col  = col_;
            char c = src_[pos_];

            // Single-char punctuation
            auto single = [&](wit_tok k) -> wit_token {
               auto sv = src_.substr(pos_, 1);
               advance(1);
               return {k, sv, start_line, start_col};
            };

            switch (c) {
               case '{': return single(wit_tok::lbrace);
               case '}': return single(wit_tok::rbrace);
               case '(': return single(wit_tok::lparen);
               case ')': return single(wit_tok::rparen);
               case '<': return single(wit_tok::langle);
               case '>': return single(wit_tok::rangle);
               case ',': return single(wit_tok::comma);
               case ':': return single(wit_tok::colon);
               case ';': return single(wit_tok::semicolon);
               case '=': return single(wit_tok::equal);
               case '/': return single(wit_tok::slash);
               case '@': return single(wit_tok::at);
               case '.': return single(wit_tok::dot);
               case '*': return single(wit_tok::star);
               case '-':
                  if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '>') {
                     auto sv = src_.substr(pos_, 2);
                     advance(2);
                     return {wit_tok::arrow, sv, start_line, start_col};
                  }
                  // '-' can be part of kebab-case identifiers, fall through to ident
                  break;
               default:
                  break;
            }

            // Numbers
            if (std::isdigit(static_cast<unsigned char>(c))) {
               size_t start = pos_;
               while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
                  advance(1);
               return {wit_tok::number, src_.substr(start, pos_ - start), start_line, start_col};
            }

            // Identifiers and keywords (kebab-case: [a-zA-Z_][a-zA-Z0-9_-]*)
            if (is_ident_start(c) || c == '%') {
               size_t start = pos_;
               if (c == '%') advance(1); // escaped keyword
               advance(1);
               while (pos_ < src_.size() && is_ident_cont(src_[pos_]))
                  advance(1);
               auto text = src_.substr(start, pos_ - start);
               auto kind = classify_keyword(text);
               return {kind, text, start_line, start_col};
            }

            throw wit_parse_error(std::string("unexpected character '") + c + "'", line_, col_);
         }

         wit_token peek() {
            auto saved_pos  = pos_;
            auto saved_line = line_;
            auto saved_col  = col_;
            auto tok = next();
            pos_  = saved_pos;
            line_ = saved_line;
            col_  = saved_col;
            return tok;
         }

       private:
         std::string_view src_;
         size_t           pos_;
         uint32_t         line_;
         uint32_t         col_;

         void advance(size_t n) {
            for (size_t i = 0; i < n && pos_ < src_.size(); i++, pos_++) {
               if (src_[pos_] == '\n') { line_++; col_ = 1; }
               else { col_++; }
            }
         }

         void skip_ws_and_comments() {
            while (pos_ < src_.size()) {
               char c = src_[pos_];
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                  advance(1);
                  continue;
               }
               if (c == '/' && pos_ + 1 < src_.size()) {
                  if (src_[pos_ + 1] == '/') {
                     // Line comment (including /// doc comments)
                     while (pos_ < src_.size() && src_[pos_] != '\n')
                        advance(1);
                     continue;
                  }
                  if (src_[pos_ + 1] == '*') {
                     // Block comment
                     advance(2);
                     while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/'))
                        advance(1);
                     if (pos_ + 1 < src_.size()) advance(2);
                     continue;
                  }
               }
               break;
            }
         }

         static bool is_ident_start(char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
         }

         static bool is_ident_cont(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
         }

         static wit_tok classify_keyword(std::string_view text) {
            // Strip leading % for escaped keywords
            auto key = text;
            if (!key.empty() && key[0] == '%')
               key.remove_prefix(1);

            // Keywords
            if (key == "package")   return wit_tok::kw_package;
            if (key == "interface") return wit_tok::kw_interface;
            if (key == "world")    return wit_tok::kw_world;
            if (key == "import")   return wit_tok::kw_import;
            if (key == "export")   return wit_tok::kw_export;
            if (key == "use")      return wit_tok::kw_use;
            if (key == "include")  return wit_tok::kw_include;
            if (key == "as")       return wit_tok::kw_as;
            if (key == "with")     return wit_tok::kw_with;
            if (key == "func")     return wit_tok::kw_func;
            if (key == "type")     return wit_tok::kw_type;
            if (key == "record")   return wit_tok::kw_record;
            if (key == "variant")  return wit_tok::kw_variant;
            if (key == "enum")     return wit_tok::kw_enum;
            if (key == "flags")    return wit_tok::kw_flags;
            if (key == "resource") return wit_tok::kw_resource;
            if (key == "own")      return wit_tok::kw_own;
            if (key == "borrow")   return wit_tok::kw_borrow;
            // Primitives
            if (key == "bool")     return wit_tok::kw_bool;
            if (key == "char")     return wit_tok::kw_char;
            if (key == "string")   return wit_tok::kw_string;
            if (key == "u8")       return wit_tok::kw_u8;
            if (key == "s8")       return wit_tok::kw_s8;
            if (key == "u16")      return wit_tok::kw_u16;
            if (key == "s16")      return wit_tok::kw_s16;
            if (key == "u32")      return wit_tok::kw_u32;
            if (key == "s32")      return wit_tok::kw_s32;
            if (key == "u64")      return wit_tok::kw_u64;
            if (key == "s64")      return wit_tok::kw_s64;
            if (key == "f32")      return wit_tok::kw_f32;
            if (key == "f64")      return wit_tok::kw_f64;
            // Compound types
            if (key == "list")     return wit_tok::kw_list;
            if (key == "option")   return wit_tok::kw_option;
            if (key == "result")   return wit_tok::kw_result;
            if (key == "tuple")    return wit_tok::kw_tuple;
            return wit_tok::ident;
         }
      };

   } // namespace detail

   // =========================================================================
   // Parser
   // =========================================================================

   class wit_parser {
    public:
      explicit wit_parser(std::string_view source) : lex_(source) {}

      /// Parse the complete .wit source into a pzam_wit_world.
      pzam_wit_world parse() {
         pzam_wit_world world;
         world.wit_source = std::string(lex_.peek().text.data() - (lex_.peek().col - 1),
                                         lex_.peek().text.data());
         // Store full source — re-capture from the original string_view
         // (The lexer was constructed from the full source, so we can recover it)

         // Parse optional package declaration
         if (peek_is(detail::wit_tok::kw_package)) {
            parse_package(world);
         }

         // Parse top-level items
         while (!peek_is(detail::wit_tok::eof)) {
            auto attrs = parse_attributes();
            auto tok = lex_.peek();
            switch (tok.kind) {
               case detail::wit_tok::kw_interface:
                  parse_interface_decl(world, std::move(attrs));
                  break;
               case detail::wit_tok::kw_world:
                  parse_world_decl(world, std::move(attrs));
                  break;
               case detail::wit_tok::kw_use:
                  skip_use_stmt(); // skip top-level use for now
                  break;
               default:
                  error(tok, "expected 'interface', 'world', or 'use'");
            }
         }

         return world;
      }

      /// Convenience: parse and set the source text on the result.
      static pzam_wit_world parse_wit(std::string_view source) {
         wit_parser p(source);
         auto world = p.parse();
         world.wit_source = std::string(source);
         return world;
      }

    private:
      detail::wit_lexer lex_;
      // Map from type name → index into world.types for name resolution
      std::unordered_map<std::string, uint32_t> type_name_map_;

      // ----- Token helpers -----

      using tok = detail::wit_tok;

      bool peek_is(tok k) { return lex_.peek().kind == k; }

      detail::wit_token expect(tok k) {
         auto t = lex_.next();
         if (t.kind != k)
            error(t, "expected " + tok_name(k) + ", got " + tok_name(t.kind));
         return t;
      }

      detail::wit_token expect_ident() {
         auto t = lex_.next();
         if (t.kind != tok::ident && !is_keyword_usable_as_ident(t.kind))
            error(t, "expected identifier, got " + tok_name(t.kind));
         return t;
      }

      [[noreturn]] void error(const detail::wit_token& t, const std::string& msg) {
         throw wit_parse_error(msg, t.line, t.col);
      }

      static bool is_keyword_usable_as_ident(tok k) {
         // In some positions, keywords can appear as identifiers
         // (e.g., field names). We allow all non-structural keywords.
         switch (k) {
            case tok::kw_bool: case tok::kw_char: case tok::kw_string:
            case tok::kw_u8: case tok::kw_s8: case tok::kw_u16: case tok::kw_s16:
            case tok::kw_u32: case tok::kw_s32: case tok::kw_u64: case tok::kw_s64:
            case tok::kw_f32: case tok::kw_f64:
            case tok::kw_list: case tok::kw_option: case tok::kw_result: case tok::kw_tuple:
            case tok::kw_own: case tok::kw_borrow:
               return true;
            default:
               return false;
         }
      }

      static std::string tok_name(tok k) {
         switch (k) {
            case tok::eof:       return "EOF";
            case tok::ident:     return "identifier";
            case tok::number:    return "number";
            case tok::lbrace:    return "'{'";
            case tok::rbrace:    return "'}'";
            case tok::lparen:    return "'('";
            case tok::rparen:    return "')'";
            case tok::langle:    return "'<'";
            case tok::rangle:    return "'>'";
            case tok::comma:     return "','";
            case tok::colon:     return "':'";
            case tok::semicolon: return "';'";
            case tok::equal:     return "'='";
            case tok::arrow:     return "'->'";
            case tok::slash:     return "'/'";
            case tok::at:        return "'@'";
            case tok::dot:       return "'.'";
            case tok::star:      return "'*'";
            default:             return "keyword";
         }
      }

      // ----- Package -----

      void parse_package(pzam_wit_world& world) {
         expect(tok::kw_package);
         // package namespace:name@version;
         // namespace = ident ('.' ident)*
         // We just collect everything up to ';' as the package name
         std::string pkg;
         auto t = expect_ident();
         pkg = std::string(t.text);

         // Could have dots in namespace: my.pkg
         while (peek_is(tok::dot)) {
            lex_.next();
            auto part = expect_ident();
            pkg += ".";
            pkg += part.text;
         }

         // `:` and the second name are technically required by
         // canonical WIT but psio's emit_wit historically renders the
         // package id without the namespace prefix when the input
         // schema didn't carry one (the colon-less form is what v1
         // produces too).  Tolerate both shapes so emit→parse
         // round-trips on those schemas.
         if (peek_is(tok::colon))
         {
            lex_.next();
            auto name = expect_ident();
            pkg += ":";
            pkg += name.text;
         }

         // Optional @version
         if (peek_is(tok::at)) {
            lex_.next();
            pkg += "@";
            // version is num.num.num[-prerelease]
            auto v1 = expect(tok::number);
            pkg += v1.text;
            expect(tok::dot);
            pkg += ".";
            auto v2 = expect(tok::number);
            pkg += v2.text;
            expect(tok::dot);
            pkg += ".";
            auto v3 = expect(tok::number);
            pkg += v3.text;
            // Skip prerelease for now
         }

         expect(tok::semicolon);

         // Store the package id in the dedicated field, not the world
         // name.  v1 confused the two, which broke any consumer that
         // relied on `world.package` being the canonical "ns:pkg@ver"
         // string (e.g. wit_encode for the qualified-interface name).
         if (world.package.empty())
            world.package = pkg;
      }

      // ----- Attributes -----

      std::vector<wit_attribute> parse_attributes() {
         std::vector<wit_attribute> attrs;
         while (peek_is(tok::at)) {
            lex_.next(); // consume '@'
            auto name_tok = expect_ident();
            wit_attribute attr;
            attr.name = std::string(name_tok.text);
            if (peek_is(tok::lparen)) {
               lex_.next(); // '('
               if (!peek_is(tok::rparen)) {
                  auto key_tok = expect_ident();
                  attr.arg_key = std::string(key_tok.text);
                  expect(tok::equal);
                  attr.arg_value = parse_attr_value();
               }
               expect(tok::rparen);
            }
            attrs.push_back(std::move(attr));
         }
         return attrs;
      }

      // Attribute values are either an identifier (feature = foo) or a
      // semver literal (version = 0.2.0).
      std::string parse_attr_value() {
         auto t = lex_.next();
         std::string v(t.text);
         if (t.kind == tok::number) {
            // semver: num.num.num
            while (peek_is(tok::dot)) {
               lex_.next();
               auto n = lex_.next();
               v += '.';
               v += n.text;
            }
         }
         return v;
      }

      // ----- Interface -----

      void parse_interface_decl(pzam_wit_world& world,
                                std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_interface);
         auto name = expect_ident();

         wit_interface iface;
         iface.name = std::string(name.text);
         iface.attributes = std::move(attrs);

         expect(tok::lbrace);
         parse_interface_body(world, iface);
         expect(tok::rbrace);

         // Interfaces at top-level are exports by default
         world.exports.push_back(std::move(iface));
      }

      void parse_interface_body(pzam_wit_world& world, wit_interface& iface) {
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof)) {
            auto item_attrs = parse_attributes();
            auto t = lex_.peek();
            switch (t.kind) {
               case tok::kw_record:
                  parse_record(world, &iface, std::move(item_attrs));
                  break;
               case tok::kw_variant:
                  parse_variant(world, &iface, std::move(item_attrs));
                  break;
               case tok::kw_enum:
                  parse_enum(world, &iface, std::move(item_attrs));
                  break;
               case tok::kw_flags:
                  parse_flags(world, &iface, std::move(item_attrs));
                  break;
               case tok::kw_type:
                  parse_type_alias(world, &iface, std::move(item_attrs));
                  break;
               case tok::kw_resource:
                  skip_resource();
                  break;
               case tok::kw_use:
                  skip_use_stmt();
                  break;
               case tok::ident:
                  // Could be: name: func(...) -> ...;
                  parse_named_func(world, &iface, std::move(item_attrs));
                  break;
               default:
                  error(t, "unexpected token in interface body");
            }
         }
      }

      // ----- World -----

      void parse_world_decl(pzam_wit_world& world,
                            std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_world);
         auto name = expect_ident();

         if (world.name.empty())
            world.name = std::string(name.text);
         if (world.attributes.empty())
            world.attributes = std::move(attrs);

         expect(tok::lbrace);

         while (!peek_is(tok::rbrace) && !peek_is(tok::eof)) {
            auto item_attrs = parse_attributes();
            auto t = lex_.peek();
            switch (t.kind) {
               case tok::kw_import:
                  parse_world_import(world);
                  break;
               case tok::kw_export:
                  parse_world_export(world);
                  break;
               case tok::kw_record:
                  parse_record(world, nullptr, std::move(item_attrs));
                  break;
               case tok::kw_variant:
                  parse_variant(world, nullptr, std::move(item_attrs));
                  break;
               case tok::kw_enum:
                  parse_enum(world, nullptr, std::move(item_attrs));
                  break;
               case tok::kw_flags:
                  parse_flags(world, nullptr, std::move(item_attrs));
                  break;
               case tok::kw_type:
                  parse_type_alias(world, nullptr, std::move(item_attrs));
                  break;
               case tok::kw_use:
                  skip_use_stmt();
                  break;
               case tok::kw_include:
                  skip_include_stmt();
                  break;
               default:
                  error(t, "unexpected token in world body");
            }
         }

         expect(tok::rbrace);
      }

      void parse_world_import(pzam_wit_world& world) {
         expect(tok::kw_import);
         auto name_tok = expect_ident();
         std::string name(name_tok.text);

         // `import <name>;` — bare reference, mirrors parse_world_export.
         if (peek_is(tok::semicolon)) {
            lex_.next();
            world.imports.push_back(wit_interface{name, {}, {}});
            return;
         }

         expect(tok::colon);

         if (peek_is(tok::kw_func)) {
            // import name: func(...) -> ...;
            auto func = parse_func_sig(world, name);
            world.funcs.push_back(std::move(func));
            uint32_t func_idx = static_cast<uint32_t>(world.funcs.size() - 1);

            // Add to or create an import interface
            if (world.imports.empty() || world.imports.back().name != "")
               world.imports.push_back(wit_interface{"", {}, {}});
            world.imports.back().func_idxs.push_back(func_idx);

            expect(tok::semicolon);
         } else if (peek_is(tok::kw_interface)) {
            // import name: interface { ... }
            lex_.next(); // consume 'interface'
            wit_interface iface;
            iface.name = name;
            expect(tok::lbrace);
            parse_interface_body(world, iface);
            expect(tok::rbrace);
            world.imports.push_back(std::move(iface));
         } else if (peek_is(tok::ident)) {
            // import name: qualified-id;
            skip_qualified_id();
            expect(tok::semicolon);
            // For now, just record an import interface with the given name
            world.imports.push_back(wit_interface{name, {}, {}});
         } else {
            error(lex_.peek(), "expected 'func', 'interface', or qualified-id after ':'");
         }
      }

      void parse_world_export(pzam_wit_world& world) {
         expect(tok::kw_export);
         auto name_tok = expect_ident();
         std::string name(name_tok.text);

         // `export <name>;` — bare re-export by name. wit_gen emits this
         // form for every export interface; canonical WIT 0.2.x accepts
         // it when <name> already names an interface in scope.
         if (peek_is(tok::semicolon)) {
            lex_.next();
            world.exports.push_back(wit_interface{name, {}, {}});
            return;
         }

         expect(tok::colon);

         if (peek_is(tok::kw_func)) {
            // export name: func(...) -> ...;
            auto func = parse_func_sig(world, name);
            world.funcs.push_back(std::move(func));
            uint32_t func_idx = static_cast<uint32_t>(world.funcs.size() - 1);

            // Add to or create an export interface
            if (world.exports.empty() || world.exports.back().name != "")
               world.exports.push_back(wit_interface{"", {}, {}});
            world.exports.back().func_idxs.push_back(func_idx);

            expect(tok::semicolon);
         } else if (peek_is(tok::kw_interface)) {
            // export name: interface { ... }
            lex_.next();
            wit_interface iface;
            iface.name = name;
            expect(tok::lbrace);
            parse_interface_body(world, iface);
            expect(tok::rbrace);
            world.exports.push_back(std::move(iface));
         } else if (peek_is(tok::ident)) {
            // export name: qualified-id;
            skip_qualified_id();
            expect(tok::semicolon);
            world.exports.push_back(wit_interface{name, {}, {}});
         } else {
            error(lex_.peek(), "expected 'func', 'interface', or qualified-id after ':'");
         }
      }

      // ----- Type definitions -----

      void parse_record(pzam_wit_world& world, wit_interface* iface,
                        std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_record);
         auto name = expect_ident();

         wit_type_def td;
         td.name = std::string(name.text);
         td.kind = static_cast<uint8_t>(wit_type_kind::record_);
         td.attributes = std::move(attrs);

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof)) {
            auto field_attrs = parse_attributes();
            auto field_name = expect_ident();
            expect(tok::colon);
            int32_t type_idx = parse_type_ref(world);

            td.fields.push_back({std::string(field_name.text), type_idx,
                                 std::move(field_attrs)});

            // Optional trailing comma
            if (peek_is(tok::comma)) lex_.next();
         }
         expect(tok::rbrace);

         uint32_t idx = add_type(world, std::move(td));
         if (iface) iface->type_idxs.push_back(idx);
      }

      void parse_variant(pzam_wit_world& world, wit_interface* iface,
                         std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_variant);
         auto name = expect_ident();

         wit_type_def td;
         td.name = std::string(name.text);
         td.kind = static_cast<uint8_t>(wit_type_kind::variant_);
         td.attributes = std::move(attrs);

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof)) {
            auto case_attrs = parse_attributes();
            auto case_name = expect_ident();
            int32_t type_idx = 0;
            // Optional payload type
            if (peek_is(tok::lparen)) {
               lex_.next();
               type_idx = parse_type_ref(world);
               expect(tok::rparen);
            }
            td.fields.push_back({std::string(case_name.text), type_idx,
                                 std::move(case_attrs)});
            if (peek_is(tok::comma)) lex_.next();
         }
         expect(tok::rbrace);

         uint32_t idx = add_type(world, std::move(td));
         if (iface) iface->type_idxs.push_back(idx);
      }

      void parse_enum(pzam_wit_world& world, wit_interface* iface,
                      std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_enum);
         auto name = expect_ident();

         wit_type_def td;
         td.name = std::string(name.text);
         td.kind = static_cast<uint8_t>(wit_type_kind::enum_);
         td.attributes = std::move(attrs);

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof)) {
            auto label_attrs = parse_attributes();
            auto label = expect_ident();
            td.fields.push_back({std::string(label.text), 0,
                                 std::move(label_attrs)});
            if (peek_is(tok::comma)) lex_.next();
         }
         expect(tok::rbrace);

         uint32_t idx = add_type(world, std::move(td));
         if (iface) iface->type_idxs.push_back(idx);
      }

      void parse_flags(pzam_wit_world& world, wit_interface* iface,
                       std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_flags);
         auto name = expect_ident();

         wit_type_def td;
         td.name = std::string(name.text);
         td.kind = static_cast<uint8_t>(wit_type_kind::flags_);
         td.attributes = std::move(attrs);

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof)) {
            auto label_attrs = parse_attributes();
            auto label = expect_ident();
            td.fields.push_back({std::string(label.text), 0,
                                 std::move(label_attrs)});
            if (peek_is(tok::comma)) lex_.next();
         }
         expect(tok::rbrace);

         uint32_t idx = add_type(world, std::move(td));
         if (iface) iface->type_idxs.push_back(idx);
      }

      void parse_type_alias(pzam_wit_world& world, wit_interface* iface,
                            std::vector<wit_attribute> attrs = {}) {
         expect(tok::kw_type);
         auto name = expect_ident();
         expect(tok::equal);
         int32_t type_idx = parse_type_ref(world);
         expect(tok::semicolon);

         // A type alias is stored as a record with 0 fields and element_type_idx
         // pointing to the aliased type. The name is the alias name.
         wit_type_def td;
         td.name = std::string(name.text);
         td.kind = static_cast<uint8_t>(wit_type_kind::record_); // alias
         td.element_type_idx = type_idx;
         td.attributes = std::move(attrs);

         uint32_t idx = add_type(world, std::move(td));
         if (iface) iface->type_idxs.push_back(idx);
      }

      // ----- Functions -----

      void parse_named_func(pzam_wit_world& world, wit_interface* iface,
                            std::vector<wit_attribute> attrs = {}) {
         auto name = expect_ident();
         expect(tok::colon);
         auto func = parse_func_sig(world, std::string(name.text));
         func.attributes = std::move(attrs);
         expect(tok::semicolon);

         world.funcs.push_back(std::move(func));
         uint32_t func_idx = static_cast<uint32_t>(world.funcs.size() - 1);
         if (iface) iface->func_idxs.push_back(func_idx);
      }

      wit_func parse_func_sig(pzam_wit_world& world, std::string name) {
         expect(tok::kw_func);

         wit_func func;
         func.name = std::move(name);

         // Parameters
         expect(tok::lparen);
         while (!peek_is(tok::rparen) && !peek_is(tok::eof)) {
            auto pname = expect_ident();
            expect(tok::colon);
            int32_t type_idx = parse_type_ref(world);
            func.params.push_back({std::string(pname.text), type_idx});
            if (peek_is(tok::comma)) lex_.next();
         }
         expect(tok::rparen);

         // Optional return type
         if (peek_is(tok::arrow)) {
            lex_.next();
            if (peek_is(tok::lparen)) {
               // Multi-return: -> (name: type, ...)
               lex_.next();
               while (!peek_is(tok::rparen) && !peek_is(tok::eof)) {
                  auto rname = expect_ident();
                  expect(tok::colon);
                  int32_t type_idx = parse_type_ref(world);
                  func.results.push_back({std::string(rname.text), type_idx});
                  if (peek_is(tok::comma)) lex_.next();
               }
               expect(tok::rparen);
            } else {
               // Single return type
               int32_t type_idx = parse_type_ref(world);
               func.results.push_back({"", type_idx});
            }
         }

         return func;
      }

      // ----- Type references -----

      int32_t parse_type_ref(pzam_wit_world& world) {
         auto t = lex_.peek();

         // Primitive types
         switch (t.kind) {
            case tok::kw_bool:   lex_.next(); return wit_prim_idx(wit_prim::bool_);
            case tok::kw_u8:     lex_.next(); return wit_prim_idx(wit_prim::u8);
            case tok::kw_s8:     lex_.next(); return wit_prim_idx(wit_prim::s8);
            case tok::kw_u16:    lex_.next(); return wit_prim_idx(wit_prim::u16);
            case tok::kw_s16:    lex_.next(); return wit_prim_idx(wit_prim::s16);
            case tok::kw_u32:    lex_.next(); return wit_prim_idx(wit_prim::u32);
            case tok::kw_s32:    lex_.next(); return wit_prim_idx(wit_prim::s32);
            case tok::kw_u64:    lex_.next(); return wit_prim_idx(wit_prim::u64);
            case tok::kw_s64:    lex_.next(); return wit_prim_idx(wit_prim::s64);
            case tok::kw_f32:    lex_.next(); return wit_prim_idx(wit_prim::f32);
            case tok::kw_f64:    lex_.next(); return wit_prim_idx(wit_prim::f64);
            case tok::kw_char:   lex_.next(); return wit_prim_idx(wit_prim::char_);
            case tok::kw_string: lex_.next(); return wit_prim_idx(wit_prim::string_);
            default: break;
         }

         // Compound types
         if (t.kind == tok::kw_list) {
            lex_.next();
            expect(tok::langle);
            int32_t elem = parse_type_ref(world);
            expect(tok::rangle);

            wit_type_def td;
            td.name = "";
            td.kind = static_cast<uint8_t>(wit_type_kind::list_);
            td.element_type_idx = elem;
            return static_cast<int32_t>(add_type(world, std::move(td)));
         }

         if (t.kind == tok::kw_option) {
            lex_.next();
            expect(tok::langle);
            int32_t elem = parse_type_ref(world);
            expect(tok::rangle);

            wit_type_def td;
            td.name = "";
            td.kind = static_cast<uint8_t>(wit_type_kind::option_);
            td.element_type_idx = elem;
            return static_cast<int32_t>(add_type(world, std::move(td)));
         }

         if (t.kind == tok::kw_result) {
            lex_.next();
            int32_t ok_type = 0;
            int32_t err_type = 0;
            if (peek_is(tok::langle)) {
               lex_.next();
               ok_type = parse_type_ref(world);
               if (peek_is(tok::comma)) {
                  lex_.next();
                  err_type = parse_type_ref(world);
               }
               expect(tok::rangle);
            }

            wit_type_def td;
            td.name = "";
            td.kind = static_cast<uint8_t>(wit_type_kind::result_);
            td.element_type_idx = ok_type;
            td.error_type_idx = err_type;
            return static_cast<int32_t>(add_type(world, std::move(td)));
         }

         if (t.kind == tok::kw_tuple) {
            lex_.next();
            expect(tok::langle);

            wit_type_def td;
            td.name = "";
            td.kind = static_cast<uint8_t>(wit_type_kind::tuple_);

            uint32_t elem_num = 0;
            while (!peek_is(tok::rangle) && !peek_is(tok::eof)) {
               int32_t elem = parse_type_ref(world);
               td.fields.push_back({std::to_string(elem_num++), elem});
               if (peek_is(tok::comma)) lex_.next();
            }
            expect(tok::rangle);
            return static_cast<int32_t>(add_type(world, std::move(td)));
         }

         // Named type reference
         if (t.kind == tok::ident) {
            auto name = lex_.next();
            std::string type_name(name.text);

            auto it = type_name_map_.find(type_name);
            if (it != type_name_map_.end())
               return static_cast<int32_t>(it->second);

            // Forward reference — create placeholder
            wit_type_def td;
            td.name = type_name;
            td.kind = static_cast<uint8_t>(wit_type_kind::record_); // placeholder
            return static_cast<int32_t>(add_type(world, std::move(td)));
         }

         error(t, "expected type");
      }

      // ----- Helpers -----

      uint32_t add_type(pzam_wit_world& world, wit_type_def&& td) {
         uint32_t idx = static_cast<uint32_t>(world.types.size());
         if (!td.name.empty())
            type_name_map_[td.name] = idx;
         world.types.push_back(std::move(td));
         return idx;
      }

      void skip_use_stmt() {
         expect(tok::kw_use);
         // Skip everything until semicolon
         while (!peek_is(tok::semicolon) && !peek_is(tok::eof))
            lex_.next();
         if (peek_is(tok::semicolon)) lex_.next();
      }

      void skip_include_stmt() {
         expect(tok::kw_include);
         while (!peek_is(tok::semicolon) && !peek_is(tok::eof))
            lex_.next();
         if (peek_is(tok::semicolon)) lex_.next();
      }

      void skip_resource() {
         expect(tok::kw_resource);
         expect_ident();
         if (peek_is(tok::lbrace)) {
            lex_.next();
            int depth = 1;
            while (depth > 0 && !peek_is(tok::eof)) {
               auto t = lex_.next();
               if (t.kind == tok::lbrace) depth++;
               else if (t.kind == tok::rbrace) depth--;
            }
         } else {
            expect(tok::semicolon);
         }
      }

      void skip_qualified_id() {
         // namespace:name/path@version
         expect_ident();
         while (peek_is(tok::colon) || peek_is(tok::slash) || peek_is(tok::dot)) {
            lex_.next();
            if (peek_is(tok::ident) || peek_is(tok::number))
               lex_.next();
         }
         if (peek_is(tok::at)) {
            lex_.next();
            // version: num.num.num
            if (peek_is(tok::number)) lex_.next();
            if (peek_is(tok::dot)) { lex_.next(); if (peek_is(tok::number)) lex_.next(); }
            if (peek_is(tok::dot)) { lex_.next(); if (peek_is(tok::number)) lex_.next(); }
         }
      }
   };

   // =========================================================================
   // Convenience free function
   // =========================================================================

   /// Parse a WIT source string into a pzam_wit_world.
   inline pzam_wit_world wit_parse(std::string_view source) {
      return wit_parser::parse_wit(source);
   }

} // namespace psio
