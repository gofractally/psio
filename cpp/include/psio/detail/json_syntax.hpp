#pragma once
//
// psio/detail/json_syntax.hpp — the hand-written JSON lexer: UTF-8 checks,
// string unescaping, the RFC 8259 number grammar, skipping a value.
//
// This is the text half of the JSON readers and has no knowledge of types.
// `json_text_cursor` (json_cursor.hpp) wraps it as a JsonCursor. It never
// throws or aborts, needs no padding after the input and allocates only for
// strings that carry escapes, so it builds for a WASI guest with exceptions
// and RTTI disabled.

#include <psio/detail/validate_depth.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace psio::detail::json_impl
{
   // ── UTF-8 ─────────────────────────────────────────────────────────────

   // Length (2..4) of the well-formed multi-byte sequence at p, or 0.
   // Rejects overlong forms, surrogates and values above U+10FFFF.
   inline unsigned utf8_multibyte(const unsigned char* p,
                                  const unsigned char* end,
                                  std::uint32_t&       cp) noexcept
   {
      const unsigned char b = *p;
      unsigned            n;
      std::uint32_t       min;
      if ((b & 0xE0) == 0xC0)
      {
         n   = 2;
         cp  = b & 0x1F;
         min = 0x80;
      }
      else if ((b & 0xF0) == 0xE0)
      {
         n   = 3;
         cp  = b & 0x0F;
         min = 0x800;
      }
      else if ((b & 0xF8) == 0xF0)
      {
         n   = 4;
         cp  = b & 0x07;
         min = 0x10000;
      }
      else
         return 0;
      if (static_cast<std::size_t>(end - p) < n)
         return 0;
      for (unsigned k = 1; k < n; ++k)
      {
         if ((p[k] & 0xC0) != 0x80)
            return 0;
         cp = (cp << 6) | (p[k] & 0x3F);
      }
      if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
         return 0;
      return n;
   }

   constexpr bool is_noncharacter(std::uint32_t cp) noexcept
   {
      return (cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE;
   }

   inline void append_utf8(std::string& out, std::uint32_t cp)
   {
      if (cp < 0x80)
         out.push_back(static_cast<char>(cp));
      else if (cp < 0x800)
      {
         out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
         out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else if (cp < 0x10000)
      {
         out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
         out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
         out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else
      {
         out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
         out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
         out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
         out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
   }

   // Whether any of the eight bytes of `word` needs attention inside a JSON
   // string: a quote, a backslash, a control character or a non-ASCII byte.
   constexpr bool string_special(std::uint64_t word) noexcept
   {
      constexpr std::uint64_t ones  = 0x0101010101010101ull;
      constexpr std::uint64_t highs = 0x8080808080808080ull;
      const std::uint64_t     quote = word ^ (ones * '"');
      const std::uint64_t     slash = word ^ (ones * '\\');
      return (((word - ones * 0x20) | (quote - ones) | (slash - ones) | word) & highs) != 0;
   }

   struct reader
   {
      const char*      begin;
      const char*      p;
      const char*      end;
      std::string_view err{};
      const char*      err_at = nullptr;

      bool fail(std::string_view what) noexcept
      {
         if (err.empty())
         {
            err    = what;
            err_at = p;
         }
         return false;
      }

      void ws() noexcept
      {
         while (p < end &&
                (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
      }

      // Skips whitespace, then consumes c if it is next.
      bool take(char c) noexcept
      {
         ws();
         if (p < end && *p == c)
         {
            ++p;
            return true;
         }
         return false;
      }

      bool keyword(std::string_view kw) noexcept
      {
         if (static_cast<std::size_t>(end - p) >= kw.size() &&
             std::memcmp(p, kw.data(), kw.size()) == 0)
         {
            p += kw.size();
            return true;
         }
         return false;
      }
   };

   constexpr int hex_digit(char c) noexcept
   {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
   }

   inline bool read_hex4(reader& r, std::uint32_t& v) noexcept
   {
      if (r.end - r.p < 4)
         return r.fail("json: truncated \\uXXXX escape");
      v = 0;
      for (int k = 0; k < 4; ++k)
      {
         const int d = hex_digit(r.p[k]);
         if (d < 0)
            return r.fail("json: bad hex digit in \\uXXXX");
         v = (v << 4) | static_cast<std::uint32_t>(d);
      }
      r.p += 4;
      return true;
   }

   // Reads a JSON string; r.p must be at its opening quote. Appends the
   // unescaped UTF-8 to *out when given. `escaped` reports whether the
   // text between the quotes differs from the value.
   inline bool read_string(reader& r, std::string* out, bool nonchar,
                           bool* escaped = nullptr)
   {
      if (r.p >= r.end || *r.p != '"')
         return r.fail("json: expected a string");
      ++r.p;
      const char* run = r.p;
      auto flush = [&] {
         if (out)
            out->append(run, static_cast<std::size_t>(r.p - run));
      };
      if (escaped)
         *escaped = false;
      while (true)
      {
         // Plain ASCII (no quote, backslash, control or high-bit byte),
         // eight bytes at a time.
         while (r.end - r.p >= 8)
         {
            std::uint64_t word;
            std::memcpy(&word, r.p, 8);
            if (string_special(word))
               break;
            r.p += 8;
         }
         if (r.p >= r.end)
            return r.fail("json: unterminated string");
         const unsigned char c = static_cast<unsigned char>(*r.p);
         if (c == '"')
         {
            flush();
            ++r.p;
            return true;
         }
         if (c == '\\')
         {
            flush();
            if (escaped)
               *escaped = true;
            if (r.end - r.p < 2)
               return r.fail("json: trailing backslash in string");
            const char e = r.p[1];
            r.p += 2;
            std::uint32_t cp;
            switch (e)
            {
               case '"':  cp = '"'; break;
               case '\\': cp = '\\'; break;
               case '/':  cp = '/'; break;
               case 'b':  cp = '\b'; break;
               case 'f':  cp = '\f'; break;
               case 'n':  cp = '\n'; break;
               case 'r':  cp = '\r'; break;
               case 't':  cp = '\t'; break;
               case 'u':
               {
                  if (!read_hex4(r, cp))
                     return false;
                  if (cp >= 0xDC00 && cp <= 0xDFFF)
                     return r.fail("json: unpaired surrogate escape");
                  if (cp >= 0xD800 && cp <= 0xDBFF)
                  {
                     std::uint32_t lo = 0;
                     if (r.end - r.p < 2 || r.p[0] != '\\' ||
                         r.p[1] != 'u')
                        return r.fail("json: unpaired surrogate escape");
                     r.p += 2;
                     if (!read_hex4(r, lo))
                        return false;
                     if (lo < 0xDC00 || lo > 0xDFFF)
                        return r.fail("json: unpaired surrogate escape");
                     cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                  }
                  if (nonchar && is_noncharacter(cp))
                     return r.fail("ijson: Unicode noncharacter in string");
                  break;
               }
               default:
                  r.p -= 1;
                  return r.fail("json: invalid escape character in string");
            }
            if (out)
               append_utf8(*out, cp);
            run = r.p;
            continue;
         }
         if (c < 0x20)
            return r.fail("json: unescaped control character in string");
         if (c < 0x80)
         {
            ++r.p;
            continue;
         }
         std::uint32_t  cp = 0;
         const unsigned n  = utf8_multibyte(
            reinterpret_cast<const unsigned char*>(r.p),
            reinterpret_cast<const unsigned char*>(r.end), cp);
         if (n == 0)
            return r.fail("json: string is not valid UTF-8");
         if (nonchar && is_noncharacter(cp))
            return r.fail("ijson: Unicode noncharacter in string");
         r.p += n;
      }
   }

   // Reads an object key. `key` views the input when the key has no
   // escapes, else `scratch`.
   inline bool read_key(reader& r, std::string& scratch,
                        std::string_view& key, bool nonchar)
   {
      r.ws();
      const char* start   = r.p;
      bool        escaped = false;
      if (!read_string(r, nullptr, nonchar, &escaped))
         return false;
      if (!escaped)
      {
         key = std::string_view{start + 1,
                                static_cast<std::size_t>(r.p - start - 2)};
         return true;
      }
      r.p = start;
      scratch.clear();
      if (!read_string(r, &scratch, nonchar))
         return false;
      key = scratch;
      return true;
   }

   // RFC 8259 number grammar; r.p at its first character.
   inline bool skip_number(reader& r) noexcept
   {
      const char* p = r.p;
      if (p < r.end && *p == '-')
         ++p;
      if (p >= r.end)
         return r.fail("json: truncated number");
      if (*p == '0')
         ++p;
      else if (*p >= '1' && *p <= '9')
      {
         ++p;
         while (p < r.end && *p >= '0' && *p <= '9')
            ++p;
      }
      else
         return r.fail("json: number missing integer part");
      if (p < r.end && *p == '.')
      {
         ++p;
         const char* frac = p;
         while (p < r.end && *p >= '0' && *p <= '9')
            ++p;
         if (p == frac)
            return r.fail("json: number missing fractional digits");
      }
      if (p < r.end && (*p == 'e' || *p == 'E'))
      {
         ++p;
         if (p < r.end && (*p == '+' || *p == '-'))
            ++p;
         const char* exp = p;
         while (p < r.end && *p >= '0' && *p <= '9')
            ++p;
         if (p == exp)
            return r.fail("json: number missing exponent digits");
      }
      r.p = p;
      return true;
   }

   // Skips one well-formed JSON value of any shape (the value of an
   // unknown key). `unique_keys` applies the I-JSON string rule
   // (no noncharacters) inside it; repeated keys are not an error.
   inline bool skip_value(reader& r, std::size_t depth, bool unique_keys)
   {
      r.ws();
      if (r.p >= r.end)
         return r.fail("json: unexpected end of input");
      switch (*r.p)
      {
         case '{':
         {
            if (depth >= kMaxValidationDepth)
               return r.fail("json: max depth exceeded");
            ++r.p;
            if (r.take('}'))
               return true;
            std::string scratch;
            while (true)
            {
               std::string_view key;
               if (!read_key(r, scratch, key, unique_keys))
                  return false;
               if (!r.take(':'))
                  return r.fail("json: expected ':' after object key");
               if (!skip_value(r, depth + 1, unique_keys))
                  return false;
               if (r.take('}'))
                  break;
               if (!r.take(','))
                  return r.fail("json: expected ',' or '}' in object");
            }
            return true;
         }
         case '[':
         {
            if (depth >= kMaxValidationDepth)
               return r.fail("json: max depth exceeded");
            ++r.p;
            if (r.take(']'))
               return true;
            while (true)
            {
               if (!skip_value(r, depth + 1, unique_keys))
                  return false;
               if (r.take(']'))
                  return true;
               if (!r.take(','))
                  return r.fail("json: expected ',' or ']' in array");
            }
         }
         case '"':
            return read_string(r, nullptr, unique_keys);
         case 't':
            return r.keyword("true") || r.fail("json: malformed keyword");
         case 'f':
            return r.keyword("false") || r.fail("json: malformed keyword");
         case 'n':
            return r.keyword("null") || r.fail("json: malformed keyword");
         default:
            if (*r.p == '-' || (*r.p >= '0' && *r.p <= '9'))
               return skip_number(r);
            return r.fail("json: unexpected character at value start");
      }
   }

}  // namespace psio::detail::json_impl
