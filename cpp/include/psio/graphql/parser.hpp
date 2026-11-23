#pragma once
//
// psio/graphql/parser.hpp — phase 1: executable documents (GraphQL October
// 2021, section 2) into a compact syntax tree.
//
// The tree is a handful of flat vectors linked by 32-bit indices; names are
// views into the source text; every node carries its byte offset, turned
// into line/column only when an error needs it. Argument and default values
// are parsed straight into the input-value store (`in_store`) the plan keeps.
//
// Limits enforced here, before anything else looks at the document: size,
// token count, selection-set nesting, value nesting. The parser recurses only
// under those depth guards and stops at the first syntax error.

#include <psio/graphql/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace psio::graphql
{
   // Every limit is checked before execution starts, except the two response
   // limits, which stop a running request.
   struct limits
   {
      std::size_t max_document_bytes = 64 * 1024;
      std::size_t max_tokens         = 16 * 1024;
      // Nesting of selection sets, counted after fragments are expanded.
      std::size_t max_depth = 32;
      // Field selections in the whole document, counted after fragments are
      // expanded (a fragment spread N times counts N times).
      std::size_t max_fields = 8 * 1024;
      // Nesting of lists / input objects in argument and variable values.
      std::size_t max_input_depth     = 16;
      std::size_t max_variables_bytes = 64 * 1024;
      std::size_t max_errors          = 16;
      // Sum over selected fields of the field's cost (psio::graphql::cost,
      // default 1), where the selection below a list field is multiplied by
      // its page size: the `first` / `last` argument when the field has one
      // and it is given, `default_list_size` otherwise. A connection's page
      // size is the length of its `edges`, counted once; an Int argument
      // named `first` / `last` of a field that returns neither a list nor a
      // connection is not a page size. docs/graphql.md "Complexity".
      std::uint64_t max_complexity    = 100'000;
      std::uint32_t default_list_size = 10;
      // Fields and list elements actually written / bytes of the response;
      // exceeding either ends the request with an error and `"data": null`.
      std::uint64_t max_response_fields = 1'000'000;
      std::size_t   max_response_bytes  = 16 * 1024 * 1024;
   };

   struct error
   {
      std::string   message;
      std::uint32_t line   = 0;  // 1-based; 0: no location
      std::uint32_t column = 0;
      std::string   path{};  // the response path as JSON array text, or empty
   };

   // ── Input values ──────────────────────────────────────────────────────
   //
   // One node per value. Scalars keep their text (a number's characters, a
   // string's unescaped bytes, an enum value's name); lists and objects
   // chain their children through `child` / `next`. Validation resolves the
   // names: `slot` for a member of an input object, `index` for a variable
   // and for an enum value.
   struct in_node
   {
      value_kind    kind = value_kind::absent;
      bool          flag = false;  // boolean value
      std::uint16_t slot = 0;      // member index, inside an input object
      std::uint16_t index = 0;     // a variable: its index in the operation;
                                   // an enum value: its index in the enum
      std::uint32_t text = 0, text_len = 0;
      std::uint32_t name = 0, name_len = 0;  // member name, inside an object
      std::uint32_t child    = nil;
      std::uint32_t next     = nil;
      std::uint32_t pos      = 0;  // source offset of the value
      std::uint32_t name_pos = 0;  // ... and of the member name
   };
   struct in_store
   {
      std::vector<in_node> nodes;
      std::string          text;

      std::string_view text_of(const in_node& n) const noexcept
      {
         return std::string_view{text}.substr(n.text, n.text_len);
      }
      std::string_view name_of(const in_node& n) const noexcept
      {
         return std::string_view{text}.substr(n.name, n.name_len);
      }
      std::uint32_t add(value_kind kind, std::uint32_t pos)
      {
         in_node n;
         n.kind = kind;
         n.pos  = pos;
         nodes.push_back(n);
         return static_cast<std::uint32_t>(nodes.size() - 1);
      }
      void set_text(std::uint32_t node, std::string_view s)
      {
         nodes[node].text     = static_cast<std::uint32_t>(text.size());
         nodes[node].text_len = static_cast<std::uint32_t>(s.size());
         text.append(s);
      }
      void set_name(std::uint32_t node, std::string_view s)
      {
         nodes[node].name     = static_cast<std::uint32_t>(text.size());
         nodes[node].name_len = static_cast<std::uint32_t>(s.size());
         text.append(s);
      }
      void clear()
      {
         nodes.clear();
         text.clear();
      }
   };

   // ── Syntax tree ───────────────────────────────────────────────────────
   namespace ast
   {
      struct directive
      {
         std::string_view name;
         std::uint32_t    pos  = 0;
         std::uint32_t    args = nil;
         std::uint32_t    next = nil;
      };
      struct argument
      {
         std::string_view name;
         std::uint32_t    pos   = 0;
         std::uint32_t    value = nil;
         std::uint32_t    next  = nil;
      };
      enum class selection_kind : std::uint8_t
      {
         field,
         spread,
         inline_fragment,
      };
      struct selection
      {
         selection_kind   kind    = selection_kind::field;
         bool             has_set = false;
         std::string_view alias{};
         // Field name, fragment name, or type condition (empty: none).
         std::string_view name{};
         std::uint32_t    pos        = 0;
         std::uint32_t    args       = nil;
         std::uint32_t    directives = nil;
         std::uint32_t    set        = nil;  // first selection of the sub-selection
         std::uint32_t    next       = nil;
      };
      struct type_syntax
      {
         std::string_view name;
         std::uint8_t     depth   = 0;
         std::uint8_t     nonnull = 0;  // as type_ref::nonnull
         std::uint32_t    pos     = 0;
      };
      struct variable
      {
         std::string_view name;
         type_syntax      type;
         std::uint32_t    default_value = nil;
         std::uint32_t    directives    = nil;
         std::uint32_t    pos           = 0;
      };
      enum class operation_kind : std::uint8_t
      {
         query,
         mutation,
         subscription,
      };
      struct operation
      {
         operation_kind   kind = operation_kind::query;
         std::string_view name{};
         std::uint32_t    vars_first = 0, vars_count = 0;
         std::uint32_t    directives = nil;
         std::uint32_t    set        = nil;
         std::uint32_t    pos        = 0;
      };
      struct fragment
      {
         std::string_view name;
         std::string_view on;
         std::uint32_t    on_pos     = 0;
         std::uint32_t    directives = nil;
         std::uint32_t    set        = nil;
         std::uint32_t    pos        = 0;
      };
      struct document
      {
         std::string_view        source;
         std::vector<operation>  operations;
         std::vector<fragment>   fragments;
         std::vector<variable>   variables;
         std::vector<selection>  selections;
         std::vector<argument>   arguments;
         std::vector<directive>  directives;
         in_store                values;
      };
   }  // namespace ast

   namespace detail
   {
      // Line and column (1-based, in bytes) of a source offset.
      class line_index
      {
         std::string_view           src_;
         std::vector<std::uint32_t> starts_;

        public:
         explicit line_index(std::string_view src) : src_(src) {}
         void locate(std::uint32_t pos, std::uint32_t& line, std::uint32_t& column)
         {
            if (starts_.empty())
            {
               starts_.push_back(0);
               for (std::size_t i = 0; i < src_.size(); ++i)
                  if (src_[i] == '\n' || (src_[i] == '\r' && !(i + 1 < src_.size() && src_[i + 1] == '\n')))
                     starts_.push_back(static_cast<std::uint32_t>(i + 1));
            }
            std::size_t lo = 0, hi = starts_.size();
            while (hi - lo > 1)
            {
               const std::size_t mid = lo + (hi - lo) / 2;
               (starts_[mid] <= pos ? lo : hi) = mid;
            }
            line   = static_cast<std::uint32_t>(lo + 1);
            column = pos - starts_[lo] + 1;
         }
      };

      enum class tok : std::uint8_t
      {
         eof,
         punct,  // one of ! $ & ( ) : = @ [ ] { | }
         spread,
         name,
         integer,
         floating,
         string,
         block_string,
      };
      struct token
      {
         tok           kind  = tok::eof;
         char          punct = 0;
         std::uint32_t begin = 0, end = 0;
      };

      constexpr bool name_start(char c) noexcept
      {
         return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
      }
      constexpr bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
      constexpr bool name_char(char c) noexcept { return name_start(c) || digit(c); }

      class parser
      {
         std::string_view src_;
         const limits&    lim_;
         ast::document&   doc_;
         error&           err_;
         token            tok_{};
         std::uint32_t    pos_    = 0;
         std::size_t      tokens_ = 0;
         bool             failed_ = false;

         std::string_view text(const token& t) const noexcept
         {
            return src_.substr(t.begin, t.end - t.begin);
         }

         bool fail_at(std::uint32_t pos, std::string message)
         {
            if (!failed_)
            {
               failed_      = true;
               err_.message = std::move(message);
               line_index{src_}.locate(pos, err_.line, err_.column);
            }
            return false;
         }
         bool syntax(std::uint32_t pos, std::string_view what)
         {
            std::string m = "Syntax Error: ";
            m += what;
            return fail_at(pos, std::move(m));
         }

         std::string describe(const token& t) const
         {
            switch (t.kind)
            {
               case tok::eof: return "<EOF>";
               case tok::spread: return "\"...\"";
               case tok::string:
               case tok::block_string: return "String";
               default:
               {
                  std::string out = "\"";
                  out += text(t).substr(0, 40);
                  out += '"';
                  return out;
               }
            }
         }
         bool expected(std::string_view what)
         {
            std::string m = "Expected ";
            m += what;
            m += ", found ";
            m += describe(tok_);
            m += '.';
            return syntax(tok_.begin, m);
         }

         // ── Lexer ─────────────────────────────────────────────────────────
         bool advance()
         {
            if (failed_)
               return false;
            const char* const s = src_.data();
            const std::uint32_t n = static_cast<std::uint32_t>(src_.size());
            std::uint32_t       p = pos_;
            while (p < n)
            {
               const char c = s[p];
               if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',')
                  ++p;
               else if (c == '#')
               {
                  while (p < n && s[p] != '\n' && s[p] != '\r')
                  {
                     if (static_cast<unsigned char>(s[p]) < 0x20 && s[p] != '\t')
                        return syntax(p, "Invalid character in comment.");
                     ++p;
                  }
               }
               else if (c == '\xEF' && n - p >= 3 && s[p + 1] == '\xBB' && s[p + 2] == '\xBF')
                  p += 3;  // byte order mark
               else
                  break;
            }
            tok_.begin = p;
            if (p >= n)
            {
               tok_.kind = tok::eof;
               tok_.end  = p;
               pos_      = p;
               return true;
            }
            if (++tokens_ > lim_.max_tokens)
               return fail_at(p, "Document has more tokens than the limit allows.");
            const char c = s[p];
            if (name_start(c))
            {
               std::uint32_t e = p + 1;
               while (e < n && name_char(s[e]))
                  ++e;
               tok_.kind = tok::name;
               tok_.end  = e;
            }
            else if (c == '-' || digit(c))
            {
               std::uint32_t e = p;
               if (s[e] == '-')
                  ++e;
               if (e >= n || !digit(s[e]))
                  return syntax(p, "Invalid number, expected digit.");
               if (s[e] == '0')
               {
                  ++e;
                  if (e < n && digit(s[e]))
                     return syntax(p, "Invalid number, unexpected digit after 0.");
               }
               else
                  while (e < n && digit(s[e]))
                     ++e;
               tok_.kind = tok::integer;
               if (e < n && s[e] == '.')
               {
                  ++e;
                  if (e >= n || !digit(s[e]))
                     return syntax(p, "Invalid number, expected digit after \".\".");
                  while (e < n && digit(s[e]))
                     ++e;
                  tok_.kind = tok::floating;
               }
               if (e < n && (s[e] == 'e' || s[e] == 'E'))
               {
                  ++e;
                  if (e < n && (s[e] == '+' || s[e] == '-'))
                     ++e;
                  if (e >= n || !digit(s[e]))
                     return syntax(p, "Invalid number, expected digit in exponent.");
                  while (e < n && digit(s[e]))
                     ++e;
                  tok_.kind = tok::floating;
               }
               if (e < n && (name_start(s[e]) || s[e] == '.'))
                  return syntax(p, "Invalid number, unexpected character after it.");
               tok_.end = e;
            }
            else if (c == '"')
            {
               if (n - p >= 3 && s[p + 1] == '"' && s[p + 2] == '"')
               {
                  std::uint32_t e = p + 3;
                  while (true)
                  {
                     if (e >= n)
                        return syntax(p, "Unterminated string.");
                     if (s[e] == '"' && n - e >= 3 && s[e + 1] == '"' && s[e + 2] == '"')
                        break;
                     if (s[e] == '\\' && n - e >= 4 && s[e + 1] == '"' && s[e + 2] == '"' &&
                         s[e + 3] == '"')
                        e += 4;
                     else
                        ++e;
                  }
                  tok_.kind = tok::block_string;
                  tok_.end  = e + 3;
               }
               else
               {
                  std::uint32_t e = p + 1;
                  while (true)
                  {
                     if (e >= n || s[e] == '\n' || s[e] == '\r')
                        return syntax(p, "Unterminated string.");
                     if (s[e] == '"')
                        break;
                     if (s[e] == '\\')
                     {
                        ++e;
                        if (e >= n)
                           return syntax(p, "Unterminated string.");
                     }
                     ++e;
                  }
                  tok_.kind = tok::string;
                  tok_.end  = e + 1;
               }
            }
            else if (c == '.')
            {
               if (n - p < 3 || s[p + 1] != '.' || s[p + 2] != '.')
                  return syntax(p, "Unexpected character \".\".");
               tok_.kind = tok::spread;
               tok_.end  = p + 3;
            }
            else
            {
               switch (c)
               {
                  case '!': case '$': case '&': case '(': case ')': case ':': case '=':
                  case '@': case '[': case ']': case '{': case '|': case '}':
                     tok_.kind  = tok::punct;
                     tok_.punct = c;
                     tok_.end   = p + 1;
                     break;
                  default: return syntax(p, "Unexpected character.");
               }
            }
            pos_ = tok_.end;
            return true;
         }

         bool is_punct(char c) const noexcept { return tok_.kind == tok::punct && tok_.punct == c; }
         bool is_name(std::string_view n) const noexcept
         {
            return tok_.kind == tok::name && text(tok_) == n;
         }
         bool expect_punct(char c)
         {
            if (!is_punct(c))
            {
               const char what[4] = {'"', c, '"', 0};
               return expected(what);
            }
            return advance();
         }
         bool parse_name(std::string_view& out)
         {
            if (tok_.kind != tok::name)
               return expected("Name");
            out = text(tok_);
            return advance();
         }

         // ── Strings ───────────────────────────────────────────────────────
         static int hex(char c) noexcept
         {
            return c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                   : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                          : -1;
         }
         bool hex4(std::uint32_t at, std::uint32_t end, std::uint32_t& out) const noexcept
         {
            if (end - at < 4)
               return false;
            out = 0;
            for (int i = 0; i < 4; ++i)
            {
               const int d = hex(src_[at + i]);
               if (d < 0)
                  return false;
               out = (out << 4) | static_cast<std::uint32_t>(d);
            }
            return true;
         }
         // Source text must be UTF-8 without control characters.
         bool check_text(std::string_view s, std::uint32_t pos, bool lines)
         {
            const auto* p   = reinterpret_cast<const unsigned char*>(s.data());
            const auto* end = p + s.size();
            while (p < end)
            {
               if (*p < 0x80)
               {
                  if (*p < 0x20 && *p != '\t' && !(lines && (*p == '\n' || *p == '\r')))
                     return syntax(pos, "Invalid character within String.");
                  ++p;
               }
               else
               {
                  std::uint32_t  cp = 0;
                  const unsigned k  = ji::utf8_multibyte(p, end, cp);
                  if (k == 0)
                     return syntax(pos, "Invalid UTF-8 within String.");
                  p += k;
               }
            }
            return true;
         }
         bool unescape(const token& t, std::string& out)
         {
            std::uint32_t       p = t.begin + 1;
            const std::uint32_t e = t.end - 1;
            if (!check_text(src_.substr(p, e - p), t.begin, false))
               return false;
            while (p < e)
            {
               const char c = src_[p];
               if (c != '\\')
               {
                  out.push_back(c);
                  ++p;
                  continue;
               }
               ++p;
               switch (src_[p])
               {
                  case '"': out.push_back('"'); break;
                  case '\\': out.push_back('\\'); break;
                  case '/': out.push_back('/'); break;
                  case 'b': out.push_back('\b'); break;
                  case 'f': out.push_back('\f'); break;
                  case 'n': out.push_back('\n'); break;
                  case 'r': out.push_back('\r'); break;
                  case 't': out.push_back('\t'); break;
                  case 'u':
                  {
                     std::uint32_t cp = 0;
                     if (!hex4(p + 1, e, cp))
                        return syntax(p - 1, "Invalid Unicode escape sequence.");
                     p += 4;
                     if (cp >= 0xD800 && cp <= 0xDBFF)
                     {
                        std::uint32_t low = 0;
                        if (e - p < 7 || src_[p + 1] != '\\' || src_[p + 2] != 'u' ||
                            !hex4(p + 3, e, low) || low < 0xDC00 || low > 0xDFFF)
                           return syntax(p - 5, "Invalid Unicode escape sequence.");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                     }
                     else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        return syntax(p - 5, "Invalid Unicode escape sequence.");
                     ji::append_utf8(out, cp);
                     break;
                  }
                  default: return syntax(p - 1, "Invalid character escape sequence.");
               }
               ++p;
            }
            return true;
         }
         // BlockStringValue (spec 2.9.4): common indentation and blank first /
         // last lines removed.
         bool block_value(const token& t, std::string& out)
         {
            const std::string_view raw = src_.substr(t.begin + 3, t.end - t.begin - 6);
            if (!check_text(raw, t.begin, true))
               return false;
            std::string text;
            text.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
               if (raw[i] == '\\' && raw.size() - i >= 4 && raw.substr(i + 1, 3) == "\"\"\"")
               {
                  text += "\"\"\"";
                  i += 3;
               }
               else
                  text.push_back(raw[i]);
            }
            std::vector<std::string_view> lines;
            {
               std::size_t start = 0;
               for (std::size_t i = 0; i <= text.size(); ++i)
               {
                  if (i == text.size() || text[i] == '\n' || text[i] == '\r')
                  {
                     lines.push_back(std::string_view{text}.substr(start, i - start));
                     if (i < text.size() && text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                        ++i;
                     start = i + 1;
                  }
               }
            }
            auto indent_of = [](std::string_view l) {
               std::size_t k = 0;
               while (k < l.size() && (l[k] == ' ' || l[k] == '\t'))
                  ++k;
               return k;
            };
            std::size_t common = std::string_view::npos;
            for (std::size_t i = 1; i < lines.size(); ++i)
            {
               const std::size_t k = indent_of(lines[i]);
               if (k < lines[i].size() && k < common)
                  common = k;
            }
            if (common != std::string_view::npos)
               for (std::size_t i = 1; i < lines.size(); ++i)
                  lines[i].remove_prefix(std::min(common, lines[i].size()));
            std::size_t first = 0, last = lines.size();
            while (first < last && indent_of(lines[first]) == lines[first].size())
               ++first;
            while (last > first && indent_of(lines[last - 1]) == lines[last - 1].size())
               --last;
            for (std::size_t i = first; i < last; ++i)
            {
               if (i != first)
                  out.push_back('\n');
               out.append(lines[i]);
            }
            return true;
         }

         // ── Values ────────────────────────────────────────────────────────
         bool parse_value(bool constant, std::size_t depth, std::uint32_t& out)
         {
            in_store&           v   = doc_.values;
            const std::uint32_t pos = tok_.begin;
            switch (tok_.kind)
            {
               case tok::integer:
               case tok::floating:
                  out = v.add(tok_.kind == tok::integer ? value_kind::integer : value_kind::floating, pos);
                  // GraphQL's `-0` is the integer 0 (psio's integer text has no -0).
                  v.set_text(out, text(tok_) == "-0" ? std::string_view{"0"} : text(tok_));
                  return advance();
               case tok::string:
               case tok::block_string:
               {
                  std::string s;
                  if (!(tok_.kind == tok::string ? unescape(tok_, s) : block_value(tok_, s)))
                     return false;
                  out = v.add(value_kind::string, pos);
                  v.set_text(out, s);
                  return advance();
               }
               case tok::name:
               {
                  const std::string_view n = text(tok_);
                  if (n == "true" || n == "false")
                  {
                     out               = v.add(value_kind::boolean, pos);
                     v.nodes[out].flag = n == "true";
                  }
                  else if (n == "null")
                     out = v.add(value_kind::null, pos);
                  else
                  {
                     out = v.add(value_kind::enumeration, pos);
                     v.set_text(out, n);
                  }
                  return advance();
               }
               case tok::punct:
                  if (tok_.punct == '$')
                  {
                     if (constant)
                        return syntax(pos, "Unexpected variable in constant value.");
                     std::string_view name;
                     if (!advance() || !parse_name(name))
                        return false;
                     out = v.add(value_kind::variable, pos);
                     v.set_text(out, name);
                     return true;
                  }
                  if (tok_.punct == '[' || tok_.punct == '{')
                  {
                     if (depth >= lim_.max_input_depth)
                        return fail_at(pos, "Input value is nested deeper than the limit allows.");
                     const bool list = tok_.punct == '[';
                     out             = v.add(list ? value_kind::list : value_kind::object, pos);
                     if (!advance())
                        return false;
                     std::uint32_t last = nil;
                     while (!is_punct(list ? ']' : '}'))
                     {
                        std::string_view    name;
                        const std::uint32_t member_pos = tok_.begin;
                        if (!list && (!parse_name(name) || !expect_punct(':')))
                           return false;
                        std::uint32_t child = nil;
                        if (!parse_value(constant, depth + 1, child))
                           return false;
                        if (!list)
                        {
                           v.set_name(child, name);
                           v.nodes[child].name_pos = member_pos;
                        }
                        (last == nil ? v.nodes[out].child : v.nodes[last].next) = child;
                        last = child;
                     }
                     return advance();
                  }
                  [[fallthrough]];
               default: return expected("a value");
            }
         }

         bool parse_arguments(bool constant, std::uint32_t& first)
         {
            first = nil;
            if (!is_punct('('))
               return true;
            if (!advance())
               return false;
            std::uint32_t last = nil;
            do
            {
               ast::argument a;
               a.pos = tok_.begin;
               if (!parse_name(a.name) || !expect_punct(':') || !parse_value(constant, 0, a.value))
                  return false;
               doc_.arguments.push_back(a);
               const auto index = static_cast<std::uint32_t>(doc_.arguments.size() - 1);
               (last == nil ? first : doc_.arguments[last].next) = index;
               last = index;
            } while (!is_punct(')'));
            return advance();
         }

         bool parse_directives(bool constant, std::uint32_t& first)
         {
            first              = nil;
            std::uint32_t last = nil;
            while (is_punct('@'))
            {
               ast::directive d;
               d.pos = tok_.begin;
               if (!advance() || !parse_name(d.name) || !parse_arguments(constant, d.args))
                  return false;
               doc_.directives.push_back(d);
               const auto index = static_cast<std::uint32_t>(doc_.directives.size() - 1);
               (last == nil ? first : doc_.directives[last].next) = index;
               last = index;
            }
            return true;
         }

         bool parse_type(ast::type_syntax& out, unsigned depth)
         {
            out.pos = tok_.begin;
            if (is_punct('['))
            {
               if (depth >= max_list_depth)
                  return fail_at(tok_.begin, "List types nest at most 7 deep.");
               ast::type_syntax inner;
               if (!advance() || !parse_type(inner, depth + 1) || !expect_punct(']'))
                  return false;
               out.name    = inner.name;
               out.depth   = static_cast<std::uint8_t>(inner.depth + 1);
               out.nonnull = static_cast<std::uint8_t>(inner.nonnull << 1);
            }
            else if (!parse_name(out.name))
               return false;
            if (is_punct('!'))
            {
               out.nonnull |= 1;
               return advance();
            }
            return true;
         }

         bool parse_selection_set(std::size_t depth, std::uint32_t& first)
         {
            if (depth >= lim_.max_depth)
               return fail_at(tok_.begin, "Selection sets are nested deeper than the limit allows.");
            if (!expect_punct('{'))
               return false;
            first              = nil;
            std::uint32_t last = nil;
            do
            {
               ast::selection s;
               s.pos = tok_.begin;
               if (tok_.kind == tok::spread)
               {
                  if (!advance())
                     return false;
                  if (tok_.kind == tok::name && !is_name("on"))
                  {
                     s.kind = ast::selection_kind::spread;
                     if (!parse_name(s.name) || !parse_directives(false, s.directives))
                        return false;
                  }
                  else
                  {
                     s.kind    = ast::selection_kind::inline_fragment;
                     s.has_set = true;
                     if (is_name("on") && (!advance() || !parse_name(s.name)))
                        return false;
                     if (!parse_directives(false, s.directives) ||
                         !parse_selection_set(depth + 1, s.set))
                        return false;
                  }
               }
               else
               {
                  if (!parse_name(s.name))
                     return false;
                  if (is_punct(':'))
                  {
                     s.alias = s.name;
                     if (!advance() || !parse_name(s.name))
                        return false;
                  }
                  if (!parse_arguments(false, s.args) || !parse_directives(false, s.directives))
                     return false;
                  if (is_punct('{'))
                  {
                     s.has_set = true;
                     if (!parse_selection_set(depth + 1, s.set))
                        return false;
                  }
               }
               doc_.selections.push_back(s);
               const auto index = static_cast<std::uint32_t>(doc_.selections.size() - 1);
               (last == nil ? first : doc_.selections[last].next) = index;
               last = index;
            } while (!is_punct('}'));
            return advance();
         }

         bool parse_operation()
         {
            ast::operation op;
            op.pos        = tok_.begin;
            op.vars_first = static_cast<std::uint32_t>(doc_.variables.size());
            if (!is_punct('{'))
            {
               if (is_name("mutation"))
                  op.kind = ast::operation_kind::mutation;
               else if (is_name("subscription"))
                  op.kind = ast::operation_kind::subscription;
               if (!advance())
                  return false;
               if (tok_.kind == tok::name && !parse_name(op.name))
                  return false;
               if (is_punct('('))
               {
                  if (!advance())
                     return false;
                  do
                  {
                     ast::variable v;
                     v.pos = tok_.begin;
                     if (!expect_punct('$') || !parse_name(v.name) || !expect_punct(':') ||
                         !parse_type(v.type, 0))
                        return false;
                     if (is_punct('=') && (!advance() || !parse_value(true, 0, v.default_value)))
                        return false;
                     if (!parse_directives(true, v.directives))
                        return false;
                     doc_.variables.push_back(v);
                  } while (!is_punct(')'));
                  if (!advance())
                     return false;
               }
               if (!parse_directives(false, op.directives))
                  return false;
            }
            op.vars_count = static_cast<std::uint32_t>(doc_.variables.size()) - op.vars_first;
            if (!parse_selection_set(0, op.set))
               return false;
            doc_.operations.push_back(op);
            return true;
         }

         bool parse_fragment()
         {
            ast::fragment f;
            f.pos = tok_.begin;
            if (!advance())
               return false;
            if (is_name("on"))
               return expected("a fragment name");
            if (!parse_name(f.name))
               return false;
            if (!is_name("on"))
               return expected("\"on\"");
            if (!advance())
               return false;
            f.on_pos = tok_.begin;
            if (!parse_name(f.on) || !parse_directives(false, f.directives) ||
                !parse_selection_set(0, f.set))
               return false;
            doc_.fragments.push_back(f);
            return true;
         }

        public:
         parser(std::string_view src, const limits& lim, ast::document& doc, error& err)
             : src_(src), lim_(lim), doc_(doc), err_(err)
         {
         }

         bool run()
         {
            doc_.source = src_;
            if (src_.size() > lim_.max_document_bytes)
               return fail_at(0, "Document is larger than the limit allows.");
            if (!advance())
               return false;
            if (tok_.kind == tok::eof)
               return syntax(tok_.begin, "Unexpected <EOF>.");
            while (tok_.kind != tok::eof)
            {
               if (is_punct('{') || is_name("query") || is_name("mutation") || is_name("subscription"))
               {
                  if (!parse_operation())
                     return false;
               }
               else if (is_name("fragment"))
               {
                  if (!parse_fragment())
                     return false;
               }
               else if (tok_.kind == tok::name || tok_.kind == tok::string ||
                        tok_.kind == tok::block_string)
                  return fail_at(tok_.begin,
                                 "Only executable definitions (operations and fragments) are "
                                 "allowed in a request.");
               else
                  return expected("a definition");
            }
            return true;
         }
      };
   }  // namespace detail

   // Parses `source` into `out`. On failure returns false with one syntax
   // error (message, line, column) in `err`. `out` refers to `source`.
   inline bool parse(std::string_view source, const limits& lim, ast::document& out, error& err)
   {
      return detail::parser{source, lim, out, err}.run();
   }

}  // namespace psio::graphql
