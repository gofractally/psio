//
// psio/pjson_number_model.hpp — what a pjson number is, and how a number
// with no declared type is encoded (docs/pjson-spec.md §15.2;
// gofractally/psio#17).
//
// Part of <psio/pjson.hpp>, which includes it at the right place; including
// this header alone includes that one.
//
// A TYPED number is stored as its type says: an integer in the integer
// forms with the fewest bytes, a float as its IEEE bits at its declared
// width (binary32 for float, binary64 for double). Nothing here is on that
// path.
//
// A number with NO type — a JSON number, an element of a generic array — is
// a decimal value (an integer or mantissa * 10^scale, or -0): the anonymous
// canonical pass chooses its form from the value (`canonical_number_bytes`,
// `append_number_array`). A float frame met on that path, and a float
// written as JSON text, is the shortest decimal that reads back to it at its
// own width (`float_reads_as`): a binary32 holding 0.1f is 0.1.

#include <psio/pjson.hpp>

#ifndef PSIO_PJSON_NUMBER_MODEL_HPP
#define PSIO_PJSON_NUMBER_MODEL_HPP

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace psio {
   namespace pjson_detail {

      // A finite number: (-1)^neg * mag * 10^scale. Normal form: mag is not
      // a multiple of 10, or the number is zero (mag 0, scale 0, not neg).
      struct decimal_value
      {
         bool              neg   = false;
         unsigned __int128 mag   = 0;
         std::int32_t      scale = 0;
         friend bool operator==(const decimal_value&, const decimal_value&) = default;
      };
      enum class number_kind : std::uint8_t { finite, minus_zero, nan, plus_inf, minus_inf };
      struct number_value
      {
         number_kind   kind = number_kind::finite;
         decimal_value d;
         friend bool operator==(const number_value&, const number_value&) = default;
      };

      inline decimal_value normalized(decimal_value d) noexcept
      {
         if (d.mag == 0) return {};
         while (d.mag % 10 == 0)
         {
            d.mag /= 10;
            ++d.scale;
         }
         return d;
      }
      inline unsigned decimal_digits(unsigned __int128 mag) noexcept
      {
         unsigned n = 0;
         do { ++n; mag /= 10; } while (mag);
         return n;
      }
      // mag * 10^scale as an integer below 2^128, when it is one.
      inline bool as_integer(const decimal_value& d, unsigned __int128& out) noexcept
      {
         if (d.scale < 0) return false;   // normal form: a negative scale is a fraction
         if (d.scale > 38) return d.mag == 0 ? (out = 0, true) : false;
         unsigned __int128 v = d.mag;
         constexpr unsigned __int128 limit = ~static_cast<unsigned __int128>(0) / 10;
         for (std::int32_t i = 0; i < d.scale; ++i)
         {
            if (v > limit) return false;
            v *= 10;
         }
         out = v;
         return true;
      }

      // "<digits>e<scale>" for from_chars. buf: at least 64 chars.
      inline std::size_t decimal_text(const decimal_value& d, char* buf) noexcept
      {
         char  digits[40];
         int   n   = 0;
         auto  mag = d.mag;
         do { digits[n++] = static_cast<char>('0' + static_cast<int>(mag % 10)); mag /= 10; } while (mag);
         std::size_t at = 0;
         if (d.neg) buf[at++] = '-';
         while (n) buf[at++] = digits[--n];
         buf[at++] = 'e';
         const auto r = std::to_chars(buf + at, buf + at + 12, d.scale);
         return static_cast<std::size_t>(r.ptr - buf);
      }

      // The shortest decimal that reads back to a finite, non-zero float:
      // the fewest significant digits that do, times a power of ten. (The
      // scientific form is asked for: std::to_chars' default writes a large
      // integer out in full — 2^60 as 1152921504606846976 — and those are
      // not its shortest digits, 1152921504606847e3.)
      template <typename F>
      decimal_value shortest_decimal(F f)
      {
         char       text[48];
         const auto r = std::to_chars(text, text + sizeof(text), f, std::chars_format::scientific);
         const auto n = parse_json_number(std::string_view{text, static_cast<std::size_t>(r.ptr - text)});
         return normalized(decimal_value{n.neg, n.mag, n.scale});
      }

      template <typename F>
      number_value specials_or(F f, bool& special) noexcept
      {
         special = true;
         if (std::isnan(f)) return {number_kind::nan, {}};
         if (std::isinf(f)) return {f > 0 ? number_kind::plus_inf : number_kind::minus_inf, {}};
         if (f == 0) return {std::signbit(f) ? number_kind::minus_zero : number_kind::finite, {}};
         special = false;
         return {};
      }

      // The number a float stands for.
      inline number_value float_reads_as(double f)
      {
         bool       special;
         const auto s = specials_or(f, special);
         return special ? s : number_value{number_kind::finite, shortest_decimal(f)};
      }
      inline number_value float_reads_as(float f)
      {
         bool       special;
         const auto s = specials_or(f, special);
         if (special) return s;
         return {number_kind::finite, shortest_decimal(f)};
      }

      // The float nearest a finite number, at each width.
      inline double nearest_double(const decimal_value& d) noexcept
      {
         char   text[64];
         auto   n = decimal_text(d, text);
         double x = 0;
         const auto r = std::from_chars(text, text + n, x);
         if (r.ec == std::errc::result_out_of_range)
            return d.scale > 0 ? (d.neg ? -HUGE_VAL : HUGE_VAL) : (d.neg ? -0.0 : 0.0);
         return x;
      }
      // Does a float frame of the width carry the number: does it read back
      // as exactly this number?
      inline bool carried_by_double(const number_value& v, double& out)
      {
         switch (v.kind)
         {
            case number_kind::minus_zero: out = -0.0; return true;
            case number_kind::nan:        out = std::numeric_limits<double>::quiet_NaN(); return true;
            case number_kind::plus_inf:   out = HUGE_VAL; return true;
            case number_kind::minus_inf:  out = -HUGE_VAL; return true;
            case number_kind::finite:     break;
         }
         if (v.d.mag == 0) { out = 0.0; return true; }
         if (decimal_digits(v.d.mag) > 17) return false;   // a double's shortest digits
         out = nearest_double(v.d);
         return std::isfinite(out) && out != 0 && shortest_decimal(out) == v.d;
      }
      // A number as a native float: the nearest one.
      inline double to_double(const number_value& v) noexcept
      {
         switch (v.kind)
         {
            case number_kind::minus_zero: return -0.0;
            case number_kind::nan:        return std::numeric_limits<double>::quiet_NaN();
            case number_kind::plus_inf:   return HUGE_VAL;
            case number_kind::minus_inf:  return -HUGE_VAL;
            case number_kind::finite:     break;
         }
         return v.d.mag == 0 ? 0.0 : nearest_double(v.d);
      }
      // The number of a native float, with the common case — an integer
      // small enough to be exact and to print as itself — taken without a
      // decimal conversion.
      template <typename F>
      number_value native_number(F f)
      {
         constexpr F exact = std::is_same_v<F, float> ? F(16777216) : F(9007199254740992.0);
         if (f > -exact && f < exact && f == std::trunc(f) && !(f == 0 && std::signbit(f)))
         {
            number_value v;
            v.d = normalized({f < 0, static_cast<std::uint64_t>(f < 0 ? -f : f), 0});
            return v;
         }
         return float_reads_as(f);
      }

      // A float as a scalar: its bits at its own width, a NaN as the quiet
      // NaN of that width (§15.2.1; a list is copied whole, bits and all).
      inline std::size_t float_frame_bytes(float f, std::uint8_t* buf) noexcept
      {
         buf[0] = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f32);
         std::uint32_t bits;
         std::memcpy(&bits, &f, 4);
         if (f != f) bits = canonical_nan_f32;
         std::memcpy(buf + 1, &bits, 4);
         return 5;
      }
      inline std::size_t float_frame_bytes(double d, std::uint8_t* buf) noexcept
      {
         buf[0] = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f64);
         std::uint64_t bits;
         std::memcpy(&bits, &d, 8);
         if (d != d) bits = canonical_nan_f64;
         std::memcpy(buf + 1, &bits, 8);
         return 9;
      }
      // (The dynamic encoder's double; declared in the core of pjson.hpp.)
      inline std::size_t canonical_double_bytes(double d, std::uint8_t* buf) { return float_frame_bytes(d, buf); }
      // A binary32 frame read as a double is the float, widened.
      inline double binary32_as_double(float f) { return static_cast<double>(f); }

      // ── Numbers on the wire ─────────────────────────────────────────────

      // The number of a float frame's bits (width_log2: 1, 2 or 3).
      inline number_value float_frame_number(std::uint8_t width_log2, const std::uint8_t* bits)
      {
         if (width_log2 == ieee_width_f64)
         {
            double d;
            std::memcpy(&d, bits, 8);
            return float_reads_as(d);
         }
         if (width_log2 == ieee_width_f32)
         {
            float f;
            std::memcpy(&f, bits, 4);
            return float_reads_as(f);
         }
         // binary16 holds exactly what a binary32 holds: it reads as one.
         std::uint16_t half;
         std::memcpy(&half, bits, 2);
         return float_reads_as(static_cast<float>(f16_bits_to_double(half)));
      }

      // The number of a scalar numeric frame (a validated one).
      inline number_value frame_number(const std::uint8_t* p, std::size_t size)
      {
         const std::uint8_t tag = p[0] >> 4, low = p[0] & 0x0F;
         number_value       out;
         switch (tag)
         {
            case t_uint_inline: out.d = normalized({false, low, 0}); return out;
            case t_nint_inline: out.d = normalized({true, low, 0}); return out;
            case t_uint:
            case t_negint:
            {
               unsigned __int128 mag = 0;
               std::memcpy(&mag, p + 1, size - 1);
               out.d = normalized({tag == t_negint && mag != 0, mag, 0});
               return out;
            }
            case t_decimal:
            {
               const std::size_t bc = static_cast<std::size_t>(low) + 1;
               __uint128_t       zz = 0;
               std::memcpy(&zz, p + 1, bc);
               const __int128 m = zz128_decode(zz);
               std::int32_t   scale = 0;
               read_varint62(p + 1 + bc, size - 1 - bc, scale);
               const bool neg = m < 0;
               out.d = normalized({neg, neg ? static_cast<unsigned __int128>(0) - static_cast<unsigned __int128>(m)
                                            : static_cast<unsigned __int128>(m),
                                   scale});
               return out;
            }
            default:   // t_ieee_float
               return float_frame_number(low & ieee_width_mask, p + 1);
         }
      }

      // The one canonical encoding of a number, into `buf` (at most 17
      // bytes). Returns its size.
      inline constexpr std::size_t max_number_size = 17;

      inline std::size_t canonical_integer_bytes(bool neg, unsigned __int128 mag, std::uint8_t* buf) noexcept
      {
         if (mag <= 15)
         {
            buf[0] = static_cast<std::uint8_t>(((neg && mag) ? t_nint_inline : t_uint_inline) << 4 |
                                               static_cast<std::uint8_t>(mag));
            return 1;
         }
         const std::uint8_t bc = u128_byte_count(mag);
         buf[0] = static_cast<std::uint8_t>((neg ? t_negint : t_uint) << 4 | (bc - 1));
         std::memcpy(buf + 1, &mag, bc);
         return 1u + bc;
      }
      // The encoding of a number with no declared type: an integer below
      // 2^128 in the integer forms; any other finite number as the smaller of
      // decimal (no trailing zeros) and binary64, where binary64 carries it
      // (a tie goes to the float); -0 as the binary64 it is. (NaN and the
      // infinities have no decimal and never arrive without a type; they
      // are binary64 too.)
      inline std::size_t canonical_number_bytes(const number_value& v, std::uint8_t* buf)
      {
         const auto wide = [&](double d) -> std::size_t {
            buf[0] = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f64);
            std::memcpy(buf + 1, &d, 8);
            return 9;
         };
         double d;
         if (v.kind != number_kind::finite)
         {
            carried_by_double(v, d);
            return wide(d);
         }
         unsigned __int128 whole;
         if (as_integer(v.d, whole)) return canonical_integer_bytes(v.d.neg, whole, buf);

         // decimal: 1 + mantissa bytes + scale bytes (a mantissa of 2^127 or
         // more has no decimal form).
         std::size_t decimal_size = ~std::size_t{0};
         __int128    mantissa     = 0;
         if (!(v.d.mag >> 127))
         {
            mantissa = v.d.neg ? -static_cast<__int128>(v.d.mag) : static_cast<__int128>(v.d.mag);
            decimal_size = 1 + mantissa_byte_count(zz128_encode(mantissa)) + varint62_byte_count(v.d.scale);
         }
         if (decimal_size >= 9 && carried_by_double(v, d)) return wide(d);
         if (decimal_size == ~std::size_t{0})
            throw std::runtime_error("pjson: a number has no canonical form");
         return encode_number_at(buf, 0, pjson_number{mantissa, v.d.scale});
      }
      inline void append_canonical_number(std::vector<std::uint8_t>& out, const number_value& v)
      {
         std::uint8_t buf[max_number_size + 8];
         const auto   n = canonical_number_bytes(v, buf);
         out.insert(out.end(), buf, buf + n);
      }

      // ── Arrays of numbers ───────────────────────────────────────────────

      // What the elements of an array are, as far as its form goes.
      struct number_run
      {
         bool          integers = true;   // every element an integer of at most 64 bits
         bool          any_negative = false;
         std::uint64_t max_positive = 0;  // largest non-negative element
         std::uint64_t max_negative = 0;  // largest magnitude of a negative element
         void add_integer(bool neg, unsigned __int128 mag) noexcept
         {
            if (mag >> 64 || (neg && mag > (static_cast<unsigned __int128>(1) << 63)))
            {
               integers = false;
               return;
            }
            const auto m = static_cast<std::uint64_t>(mag);
            if (neg && m)
            {
               any_negative = true;
               if (m > max_negative) max_negative = m;
            }
            else if (m > max_positive)
               max_positive = m;
         }
         void add(const number_value& v) noexcept
         {
            unsigned __int128 whole;
            if (v.kind == number_kind::finite && as_integer(v.d, whole))
               add_integer(v.d.neg, whole);
            else
               integers = false;
         }
         // The typed-array code of an all-integer array, or tac_invalid.
         std::uint8_t integer_code() const noexcept
         {
            if (!integers) return tac_invalid;
            if (!any_negative)
               return max_positive <= 0xFF ? tac_u8 : max_positive <= 0xFFFF ? tac_u16
                    : max_positive <= 0xFFFFFFFFull ? tac_u32 : tac_u64;
            const auto fits = [&](unsigned bits) {
               return max_positive <= (1ull << (bits - 1)) - 1 && max_negative <= (1ull << (bits - 1));
            };
            if (fits(8)) return tac_i8;
            if (fits(16)) return tac_i16;
            if (fits(32)) return tac_i32;
            return max_positive <= 0x7FFFFFFFFFFFFFFFull ? tac_i64 : tac_invalid;
         }
      };

      inline void append_count(std::vector<std::uint8_t>& out, std::size_t n)
      {
         out.push_back(static_cast<std::uint8_t>(n));
         out.push_back(static_cast<std::uint8_t>(n >> 8));
      }
      inline void append_width(std::vector<std::uint8_t>& out, std::uint8_t code, std::uint32_t v)
      {
         for (std::size_t i = 0; i <= code; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
      }
      inline void append_empty_array(std::vector<std::uint8_t>& out)
      {
         out.insert(out.end(), {static_cast<std::uint8_t>(t_array << 4), 0, 0, 0});
      }

      // The array of `n` numbers (n >= 1) that have no declared element type:
      // every element an integer of at most 64 bits -> a typed array of the
      // narrowest code; else binary64 as below; else false (the caller
      // writes a generic array).
      // `at(i)` gives element i.
      template <typename At>
      bool append_number_array(std::vector<std::uint8_t>& out, std::size_t n, At&& at)
      {
         number_run run;
         for (std::size_t i = 0; i < n && run.integers; ++i) run.add(at(i));
         if (const auto code = run.integer_code(); code != tac_invalid)
         {
            const std::size_t w = typed_array_elem_size(code);
            out.push_back(static_cast<std::uint8_t>((t_array << 4) | typed_array_low_from_code(code)));
            auto pos = out.size();
            out.resize(pos + n * w);
            for (std::size_t i = 0; i < n; ++i, pos += w)
            {
               const number_value v = at(i);
               unsigned __int128  whole = 0;
               as_integer(v.d, whole);
               const std::uint64_t x = v.d.neg ? 0 - static_cast<std::uint64_t>(whole) : static_cast<std::uint64_t>(whole);
               std::memcpy(out.data() + pos, &x, w);
            }
            append_count(out, n);
            return true;
         }
         // Else binary64, when it carries every element and the typed
         // array is not larger than the generic one would be.
         bool        wide    = true;
         std::size_t scalars = 0;
         double      d;
         for (std::size_t i = 0; i < n && wide; ++i)
         {
            const number_value v = at(i);
            std::uint8_t       scratch[max_number_size + 8];
            wide    = carried_by_double(v, d);
            scalars += canonical_number_bytes(v, scratch);
         }
         if (!wide) return false;
         const std::size_t generic = 2 + scalars + n * width_bytes(width_code_for(scalars)) + 2;
         if (1 + n * 8 + 2 > generic) return false;
         out.push_back(static_cast<std::uint8_t>((t_array << 4) | typed_array_low_from_code(tac_f64)));
         auto pos = out.size();
         out.resize(pos + n * 8);
         for (std::size_t i = 0; i < n; ++i, pos += 8)
         {
            carried_by_double(at(i), d);
            std::memcpy(out.data() + pos, &d, 8);
         }
         append_count(out, n);
         return true;
      }

      // The number of element i of a typed array's element bytes.
      inline number_value typed_element_number(std::uint8_t code, const std::uint8_t* p)
      {
         number_value v;
         switch (code)
         {
            case tac_f32: { float f;  std::memcpy(&f, p, 4); return float_reads_as(f); }
            case tac_f64: { double d; std::memcpy(&d, p, 8); return float_reads_as(d); }
            default: break;
         }
         const std::size_t w = typed_array_elem_size(code);
         std::uint64_t     raw = 0;
         std::memcpy(&raw, p, w);
         if (typed_array_is_signed_int(code))
         {
            const unsigned     shift = static_cast<unsigned>(64 - 8 * w);
            const std::int64_t x     = static_cast<std::int64_t>(raw << shift) >> shift;
            v.d = normalized({x < 0, x < 0 ? 0 - static_cast<std::uint64_t>(x) : static_cast<std::uint64_t>(x), 0});
         }
         else
            v.d = normalized({false, raw, 0});
         return v;
      }

      // ── Text ────────────────────────────────────────────────────────────

      inline bool is_utf8(const std::uint8_t* p, std::size_t n) noexcept
      {
         std::size_t i = 0;
         while (i < n)
         {
            // Eight ASCII bytes at a time.
            while (i + 8 <= n)
            {
               std::uint64_t word;
               std::memcpy(&word, p + i, 8);
               if (word & 0x8080808080808080ull) break;
               i += 8;
            }
            if (i >= n) break;
            const std::uint8_t c = p[i];
            if (c < 0x80) { ++i; continue; }
            std::size_t   len;
            std::uint32_t cp;
            if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
            else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
            else return false;
            if (i + len > n) return false;
            for (std::size_t k = 1; k < len; ++k)
            {
               if ((p[i + k] & 0xC0) != 0x80) return false;
               cp = (cp << 6) | (p[i + k] & 0x3F);
            }
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
               return false;
            i += len;
         }
         return true;
      }

      // JSON escape form -> the text. False when the escapes are malformed
      // or do not name text (a lone surrogate).
      inline bool append_unescaped(std::vector<std::uint8_t>& out, const std::uint8_t* p, std::size_t n)
      {
         const auto hex4 = [&](std::size_t at, std::uint32_t& v) {
            if (at + 4 > n) return false;
            v = 0;
            for (std::size_t k = 0; k < 4; ++k)
            {
               const std::uint8_t c = p[at + k];
               v <<= 4;
               if (c >= '0' && c <= '9') v |= c - '0';
               else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
               else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
               else return false;
            }
            return true;
         };
         const std::size_t start = out.size();
         for (std::size_t i = 0; i < n;)
         {
            // What may not stand unescaped in a JSON string is not escape form.
            if (p[i] == '"' || p[i] < 0x20) return false;
            if (p[i] != '\\') { out.push_back(p[i++]); continue; }
            if (++i >= n) return false;
            switch (p[i++])
            {
               case '"':  out.push_back('"'); break;
               case '\\': out.push_back('\\'); break;
               case '/':  out.push_back('/'); break;
               case 'b':  out.push_back('\b'); break;
               case 'f':  out.push_back('\f'); break;
               case 'n':  out.push_back('\n'); break;
               case 'r':  out.push_back('\r'); break;
               case 't':  out.push_back('\t'); break;
               case 'u':
               {
                  std::uint32_t cp;
                  if (!hex4(i, cp)) return false;
                  i += 4;
                  if (cp >= 0xD800 && cp <= 0xDBFF)
                  {
                     std::uint32_t low;
                     if (i + 6 > n || p[i] != '\\' || p[i + 1] != 'u' || !hex4(i + 2, low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                     i += 6;
                     cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                  }
                  else if (cp >= 0xDC00 && cp <= 0xDFFF)
                     return false;
                  if (cp < 0x80) out.push_back(static_cast<std::uint8_t>(cp));
                  else if (cp < 0x800)
                  {
                     out.push_back(static_cast<std::uint8_t>(0xC0 | (cp >> 6)));
                     out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
                  }
                  else if (cp < 0x10000)
                  {
                     out.push_back(static_cast<std::uint8_t>(0xE0 | (cp >> 12)));
                     out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
                     out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
                  }
                  else
                  {
                     out.push_back(static_cast<std::uint8_t>(0xF0 | (cp >> 18)));
                     out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
                     out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
                     out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
                  }
                  break;
               }
               default: return false;
            }
         }
         return is_utf8(out.data() + start, out.size() - start);
      }

   }  // namespace pjson_detail
}  // namespace psio

#endif  // PSIO_PJSON_NUMBER_MODEL_HPP
