//
// psio/pjson_number_model.hpp — what a pjson number is, and the one
// canonical encoding of each (docs/pjson-spec.md §15.2; gofractally/psio#17).
//
// Part of <psio/pjson.hpp>, which includes it at the right place; including
// this header alone includes that one.
//
// A number is a decimal value (an integer or mantissa * 10^scale) or one of
// -0, NaN, +Inf, -Inf. Its declared width or signedness is not part of it:
// 5 is 5 whether a u16, a u64 or JSON text held it.
//
// What a binary float STANDS FOR is the one decision isolated here
// (`float_model`): everything that turns a float into a number, or a number
// into a float — encoders, readers, views, the JSON emitter, the
// canonicalizer — goes through `float_reads_as`, `carried_by_float`,
// `carried_by_double`, `to_double` and `to_float` below, and nothing else in
// the library knows the model.

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

      // ── What a float stands for: THE switch point ──────────────────────
      //
      // DEFAULT CHOSEN, AWAITING OWNER (gofractally/psio#17, comment
      // 7208961356).
      //
      //   declared_width  a float stands for the shortest decimal that
      //                   reads back to it at its own width: f32 0.1f,
      //                   f64 0.1 and JSON 0.1 are one value. A binary32
      //                   frame read as a double goes through that decimal,
      //                   not a widening cast.
      //   binary64        a float is widened to binary64 first: f32 0.1f is
      //                   0.10000000149011612, and a binary32 frame widens.
      enum class float_reading { declared_width, binary64 };
      inline constexpr float_reading float_model = float_reading::declared_width;

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
         if (float_model == float_reading::binary64)
            return {number_kind::finite, shortest_decimal(static_cast<double>(f))};
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
      inline float nearest_float(const decimal_value& d) noexcept
      {
         if (float_model == float_reading::binary64)
            return static_cast<float>(nearest_double(d));
         char  text[64];
         auto  n = decimal_text(d, text);
         float x = 0;
         const auto r = std::from_chars(text, text + n, x);
         if (r.ec == std::errc::result_out_of_range)
            return d.scale > 0 ? (d.neg ? -HUGE_VALF : HUGE_VALF) : (d.neg ? -0.0f : 0.0f);
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
      inline bool carried_by_float(const number_value& v, float& out)
      {
         switch (v.kind)
         {
            case number_kind::minus_zero: out = -0.0f; return true;
            case number_kind::nan:        out = std::numeric_limits<float>::quiet_NaN(); return true;
            case number_kind::plus_inf:   out = HUGE_VALF; return true;
            case number_kind::minus_inf:  out = -HUGE_VALF; return true;
            case number_kind::finite:     break;
         }
         if (v.d.mag == 0) { out = 0.0f; return true; }
         if (float_model == float_reading::binary64)
         {
            double wide;
            if (!carried_by_double(v, wide)) return false;
            out = static_cast<float>(wide);
            return static_cast<double>(out) == wide;
         }
         if (decimal_digits(v.d.mag) > 9) return false;   // a float's shortest digits
         out = nearest_float(v.d);
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
      inline float to_float(const number_value& v) noexcept
      {
         switch (v.kind)
         {
            case number_kind::minus_zero: return -0.0f;
            case number_kind::nan:        return std::numeric_limits<float>::quiet_NaN();
            case number_kind::plus_inf:   return HUGE_VALF;
            case number_kind::minus_inf:  return -HUGE_VALF;
            case number_kind::finite:     break;
         }
         return v.d.mag == 0 ? 0.0f : nearest_float(v.d);
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

      inline double binary32_as_double(float f) { return to_double(float_reads_as(f)); }

      inline std::size_t canonical_number_bytes(const number_value& v, std::uint8_t* buf);
      // The canonical frame of a native double (the dynamic encoder's
      // double, declared in the core of pjson.hpp).
      inline std::size_t canonical_double_bytes(double d, std::uint8_t* buf)
      {
         return canonical_number_bytes(native_number(d), buf);
      }

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
      inline std::size_t canonical_number_bytes(const number_value& v, std::uint8_t* buf)
      {
         const auto half = [&](std::uint16_t bits) -> std::size_t {
            buf[0] = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f16);
            buf[1] = static_cast<std::uint8_t>(bits);
            buf[2] = static_cast<std::uint8_t>(bits >> 8);
            return 3;
         };
         switch (v.kind)
         {
            case number_kind::minus_zero: return half(0x8000);
            case number_kind::nan:        return half(0x7E00);
            case number_kind::plus_inf:   return half(0x7C00);
            case number_kind::minus_inf:  return half(0xFC00);
            case number_kind::finite:     break;
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
         // The smallest of decimal, binary32, binary64; a tie goes to the
         // float, the narrower first.
         float  f;
         double d;
         if (decimal_size >= 5 && carried_by_float(v, f))
         {
            buf[0] = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f32);
            std::memcpy(buf + 1, &f, 4);
            return 5;
         }
         if (decimal_size >= 9 && carried_by_double(v, d))
         {
            buf[0] = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f64);
            std::memcpy(buf + 1, &d, 8);
            return 9;
         }
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

      // The canonical array of `n` numbers (n >= 1), or false when they are
      // not all numbers a typed array holds (the caller writes it generic).
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
         // Floats: binary32 if it carries every element, else binary64.
         bool  narrow = true;
         float f;
         for (std::size_t i = 0; i < n && narrow; ++i) narrow = carried_by_float(at(i), f);
         if (narrow)
         {
            out.push_back(static_cast<std::uint8_t>((t_array << 4) | typed_array_low_from_code(tac_f32)));
            auto pos = out.size();
            out.resize(pos + n * 4);
            for (std::size_t i = 0; i < n; ++i, pos += 4)
            {
               carried_by_float(at(i), f);
               std::memcpy(out.data() + pos, &f, 4);
            }
            append_count(out, n);
            return true;
         }
         bool   wide = true;
         double d;
         for (std::size_t i = 0; i < n && wide; ++i) wide = carried_by_double(at(i), d);
         if (!wide) return false;
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

      // ── A list of numbers of one native type ────────────────────────────
      //
      // The canonical form of the list is a function of its values (the
      // rule above), so a typed encoder looks at them: the largest and
      // smallest of an integer list; for floats, whether every element is
      // an integer and whether binary32 carries every element (both stop at
      // the first element that says no).
      //
      // The code is tac_invalid for the empty list (the generic empty
      // array) and for a list no typed array holds.
      template <typename E>
      std::uint8_t typed_list_code(std::span<const E> v)
      {
         if (v.empty()) return tac_invalid;
         if constexpr (std::is_integral_v<E>)
         {
            if constexpr (std::is_unsigned_v<E>)
            {
               E top = 0;
               for (const E x : v) top = x > top ? x : top;
               return top <= 0xFF ? tac_u8 : top <= 0xFFFF ? tac_u16 : top <= 0xFFFFFFFFull ? tac_u32 : tac_u64;
            }
            else
            {
               E low = 0, top = 0;
               for (const E x : v)
               {
                  low = x < low ? x : low;
                  top = x > top ? x : top;
               }
               const auto t = static_cast<std::uint64_t>(top);
               if (low == 0) return t <= 0xFF ? tac_u8 : t <= 0xFFFF ? tac_u16 : t <= 0xFFFFFFFFull ? tac_u32 : tac_u64;
               const auto l = static_cast<std::int64_t>(low);
               if (l >= -128 && t <= 127) return tac_i8;
               if (l >= -32768 && t <= 32767) return tac_i16;
               if (l >= -2147483648ll && t <= 2147483647ull) return tac_i32;
               return tac_i64;
            }
         }
         else
         {
            number_run run;
            for (std::size_t i = 0; i < v.size() && run.integers; ++i) run.add(native_number(v[i]));
            if (const auto code = run.integer_code(); code != tac_invalid) return code;
            if constexpr (std::is_same_v<E, float>)
               return tac_f32;   // a binary32 carries the number it stands for
            else
            {
               float f;
               for (const E x : v)
                  if (!carried_by_float(native_number(x), f)) return tac_f64;
               return tac_f32;
            }
         }
      }
      inline std::size_t typed_list_size(std::uint8_t code, std::size_t n) noexcept
      {
         return code == tac_invalid ? 4 : 1 + n * typed_array_elem_size(code) + 2;
      }
      // Writes the list in the form `code` (from typed_list_code) names.
      template <typename E>
      std::size_t encode_typed_list_at(std::uint8_t* dst, std::size_t pos, std::span<const E> v, std::uint8_t code)
      {
         const std::size_t n = v.size();
         if (code == tac_invalid)
         {
            dst[pos] = static_cast<std::uint8_t>(t_array << 4);
            dst[pos + 1] = dst[pos + 2] = dst[pos + 3] = 0;
            return 4;
         }
         const std::size_t w = typed_array_elem_size(code);
         dst[pos] = static_cast<std::uint8_t>((t_array << 4) | typed_array_low_from_code(code));
         std::uint8_t* p = dst + pos + 1;
         if constexpr (std::is_integral_v<E>)
         {
            if (code == tac_for<E>())
               std::memcpy(p, v.data(), n * w);
            else
               for (std::size_t i = 0; i < n; ++i, p += w)
               {
                  // Two's complement of the value, truncated to the width
                  // that holds it.
                  const std::uint64_t u = std::is_signed_v<E>
                                             ? static_cast<std::uint64_t>(static_cast<std::int64_t>(v[i]))
                                             : static_cast<std::uint64_t>(v[i]);
                  std::memcpy(p, &u, w);
               }
         }
         else if (code == tac_for<E>())
         {
            std::memcpy(p, v.data(), n * w);
            // One NaN: the quiet one.
            const E quiet = std::numeric_limits<E>::quiet_NaN();
            for (std::size_t i = 0; i < n; ++i)
               if (v[i] != v[i]) std::memcpy(p + i * w, &quiet, w);
         }
         else if (code == tac_f32)
         {
            for (std::size_t i = 0; i < n; ++i, p += 4)
            {
               float f;
               carried_by_float(native_number(v[i]), f);
               std::memcpy(p, &f, 4);
            }
         }
         else
         {
            for (std::size_t i = 0; i < n; ++i, p += w)
            {
               const number_value x = native_number(v[i]);
               unsigned __int128  whole = 0;
               as_integer(x.d, whole);
               const std::uint64_t u = x.d.neg ? 0 - static_cast<std::uint64_t>(whole) : static_cast<std::uint64_t>(whole);
               std::memcpy(p, &u, w);
            }
         }
         dst[pos + 1 + n * w] = static_cast<std::uint8_t>(n & 0xFF);
         dst[pos + 2 + n * w] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
         return 1 + n * w + 2;
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
