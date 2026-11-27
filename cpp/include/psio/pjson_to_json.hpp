#pragma once
//
// psio/pjson_to_json.hpp — direct pjson bytes → JSON text writer.
//
// This is the fast-path equivalent of `view_to_json(pjson_view{...})`.
// Skips the view abstraction entirely:
//
//   * No per-child `dynamic_view{ptr, size}` construction.
//   * No `view.type()` then switch (we read the tag byte once and
//     dispatch in-place).
//   * No accessor methods that re-discriminate the type internally.
//   * Container walks read the slot table directly via raw pointer
//     arithmetic; no for_each_* lambda indirection.
//
// Use this when you have pjson bytes and want JSON text out — the
// generic `view_to_json` is fine for most cases (~146 ns on a 4-field
// doc) but this path shaves the schemaless dispatch overhead.
//
// Output format:
//   * Compact JSON (no whitespace).
//   * Strings emitted verbatim — pjson stores them in JSON-escape
//     form, so no per-character escape pass.
//   * Numbers emit via std::to_chars (shortest round-trippable form
//     for doubles, exact digit string for integers).
//   * `bytes` emit as quoted hex (pending base64 wiring).

#include <psio/pjson.hpp>
#include <psio/pjson_view.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace psio {

   namespace pjson_detail {

      // mantissa × 10^scale as JSON number text, exactly. Plain notation
      // while that stays short; an exponent otherwise (`1e400`).
      inline void emit_decimal(std::string& out, __int128 m, std::int32_t scale)
      {
         if (m == 0)
         {
            out.push_back('0');
            return;
         }
         char              digits[40];
         std::size_t       nd  = 0;
         unsigned __int128 mag = m < 0 ? static_cast<unsigned __int128>(-(m + 1)) + 1
                                       : static_cast<unsigned __int128>(m);
         while (mag != 0)
         {
            digits[nd++] = static_cast<char>('0' + static_cast<int>(mag % 10));
            mag /= 10;
         }
         std::reverse(digits, digits + nd);
         if (m < 0) out.push_back('-');
         const std::int64_t point = static_cast<std::int64_t>(nd) + scale;  // digits before the point
         if (scale >= 0 && scale <= 20)
         {
            out.append(digits, nd);
            out.append(static_cast<std::size_t>(scale), '0');
         }
         else if (scale < 0 && point > 0)
         {
            out.append(digits, static_cast<std::size_t>(point));
            out.push_back('.');
            out.append(digits + point, nd - static_cast<std::size_t>(point));
         }
         else if (scale < 0 && point > -7)
         {
            out.append("0.");
            out.append(static_cast<std::size_t>(-point), '0');
            out.append(digits, nd);
         }
         else
         {
            // d.ddde±x
            out.push_back(digits[0]);
            if (nd > 1)
            {
               out.push_back('.');
               out.append(digits + 1, nd - 1);
            }
            out.push_back('e');
            char       tmp[16];
            const auto r = std::to_chars(tmp, tmp + sizeof(tmp), point - 1);
            out.append(tmp, r.ptr);
         }
      }

      inline void emit_u128(std::string& out, unsigned __int128 mag)
      {
         char        digits[40];
         std::size_t nd = 0;
         do
         {
            digits[nd++] = static_cast<char>('0' + static_cast<int>(mag % 10));
            mag /= 10;
         } while (mag != 0);
         std::reverse(digits, digits + nd);
         out.append(digits, nd);
      }

      inline void emit_base64(std::string& out, const std::uint8_t* p, std::size_t n)
      {
         static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
         std::size_t i = 0;
         for (; i + 2 < n; i += 3)
         {
            const std::uint32_t v = (std::uint32_t{p[i]} << 16) | (std::uint32_t{p[i + 1]} << 8) | p[i + 2];
            out.push_back(alphabet[v >> 18]);
            out.push_back(alphabet[(v >> 12) & 63]);
            out.push_back(alphabet[(v >> 6) & 63]);
            out.push_back(alphabet[v & 63]);
         }
         if (i + 1 == n)
         {
            const std::uint32_t v = std::uint32_t{p[i]} << 16;
            out.push_back(alphabet[v >> 18]);
            out.push_back(alphabet[(v >> 12) & 63]);
            out.append("==");
         }
         else if (i + 2 == n)
         {
            const std::uint32_t v = (std::uint32_t{p[i]} << 16) | (std::uint32_t{p[i + 1]} << 8);
            out.push_back(alphabet[v >> 18]);
            out.push_back(alphabet[(v >> 12) & 63]);
            out.push_back(alphabet[(v >> 6) & 63]);
            out.push_back('=');
         }
      }

      // Cold path for raw_text: per-character JSON escape pass. Out
      // of line so the hot path (escape_form / binary memcpy) stays
      // tight.
      [[gnu::noinline]] inline void emit_string_escaping(
          std::string& out, std::string_view s)
      {
         for (char c : s)
         {
            switch (c)
            {
               case '"':  out.append("\\\"", 2); break;
               case '\\': out.append("\\\\", 2); break;
               case '\b': out.append("\\b", 2); break;
               case '\f': out.append("\\f", 2); break;
               case '\n': out.append("\\n", 2); break;
               case '\r': out.append("\\r", 2); break;
               case '\t': out.append("\\t", 2); break;
               default:
                  if (static_cast<unsigned char>(c) < 0x20)
                  {
                     char buf[8];
                     auto n = std::snprintf(buf, sizeof(buf),
                                            "\\u%04x", c);
                     out.append(buf, n);
                  }
                  else
                     out.push_back(c);
                  break;
            }
         }
      }

      // Render an i128 mantissa as base-10 digits to a small buffer.
      // Returns chars written.
      inline std::size_t i128_to_chars(char* buf, std::size_t cap,
                                       __int128 v) noexcept
      {
         char* end = buf + cap;
         char* p   = end;
         bool  neg = v < 0;
         __uint128_t u = neg ? static_cast<__uint128_t>(-v)
                             : static_cast<__uint128_t>(v);
         if (u == 0) { *--p = '0'; }
         else
            while (u > 0)
            {
               *--p = static_cast<char>('0' +
                                        static_cast<int>(u % 10));
               u /= 10;
            }
         if (neg) *--p = '-';
         std::memmove(buf, p, end - p);
         return static_cast<std::size_t>(end - p);
      }

      // Direct walker. Recursive over (ptr, size) pairs.
      inline void direct_pjson_to_json(std::string&        out,
                                       const std::uint8_t* p,
                                       std::size_t         size)
      {
         std::uint8_t tag  = p[0];
         std::uint8_t type = tag >> 4;
         std::uint8_t low  = tag & 0x0F;

         switch (type)
         {
            case t_null:
               out.append("null", 4);
               return;
            case t_bool:
               out.append((low & 1) ? "true" : "false",
                          (low & 1) ? 4 : 5);
               return;
            case t_nint_inline:
               if (size != 1 || low == 0) throw std::runtime_error("pjson_to_json: invalid inline negative integer");
               out.push_back('-');
               [[fallthrough]];
            case t_uint_inline:
               // Value 0..15.
               if (low < 10)
                  out.push_back(static_cast<char>('0' + low));
               else
               {
                  out.push_back('1');
                  out.push_back(static_cast<char>('0' + (low - 10)));
               }
               return;
            case t_uint:
            {
               // §4.4 uint: payload = magnitude as raw LE bytes.
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               if (bc <= 8)
               {
                  std::uint64_t mag = 0;
                  std::memcpy(&mag, p + 1, bc);
                  char tmp[24];
                  auto r = std::to_chars(tmp, tmp + sizeof(tmp), mag);
                  out.append(tmp, r.ptr);
               }
               else
               {
                  // 128-bit path: mag fits u128, emit as decimal text
                  // via the i128 helper (cast through __int128).
                  __uint128_t mag = 0;
                  std::memcpy(&mag, p + 1, bc);
                  emit_u128(out, mag);
               }
               return;
            }
            case t_negint:
            {
               // §4.4 negint: payload = |value| as raw LE bytes.
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               if (bc <= 8)
               {
                  std::uint64_t mag = 0;
                  std::memcpy(&mag, p + 1, bc);
                  // Render as -mag. INT64_MIN special case: emit literal.
                  out.push_back('-');
                  char tmp[24];
                  auto r = std::to_chars(tmp, tmp + sizeof(tmp), mag);
                  out.append(tmp, r.ptr);
               }
               else
               {
                  __uint128_t mag = 0;
                  std::memcpy(&mag, p + 1, bc);
                  // (The magnitude is unsigned: 128 bits of it.)
                  out.push_back('-');
                  emit_u128(out, mag);
               }
               return;
            }
            case t_decimal:
            {
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               __uint128_t zz = 0;
               std::memcpy(&zz, p + 1, bc);
               __int128     m = zz128_decode(zz);
               std::int32_t scale;
               std::size_t  n =
                   read_varint62(p + 1 + bc, size - 1 - bc, scale);
               (void)n;
               // The exact decimal: a decimal holds what a double cannot
               // (more than 17 digits, exponents beyond ±308).
               emit_decimal(out, m, scale);
               return;
            }
            case t_ieee_float:
            {
               // §4.6 width selector. JSON forbids NaN/Inf, so any
               // non-finite IEEE bit pattern raises (callers control
               // their input domain).
               std::uint8_t width_bits = low & ieee_width_mask;
               double d;
               if (width_bits == ieee_width_f64)
               {
                  std::memcpy(&d, p + 1, 8);
               }
               else if (width_bits == ieee_width_f32)
               {
                  float f;
                  std::memcpy(&f, p + 1, 4);
                  d = static_cast<double>(f);
               }
               else if (width_bits == ieee_width_f16)
               {
                  std::uint16_t bits;
                  std::memcpy(&bits, p + 1, 2);
                  d = f16_bits_to_double(bits);
               }
               else
               {
                  throw std::runtime_error(
                      "pjson_to_json: ieee_float width not supported");
               }
               // §7.3: JSON has no NaN or infinity. An error, never `inf`
               // or `null` in their place.
               if (!std::isfinite(d))
                  throw std::runtime_error("pjson_to_json: NaN and infinity have no JSON form");
               char tmp[32];
               auto r = std::to_chars(tmp, tmp + sizeof(tmp), d);
               out.append(tmp, r.ptr);
               return;
            }
            case t_string:
            {
               std::string_view s(
                   reinterpret_cast<const char*>(p + 1), size - 1);
               out.push_back('"');
               // Hot path: escape_form (JSON-coming-from-JSON pipeline)
               // — verbatim memcpy. Cold path: raw_text — escape per
               // char (out-of-line).
               if (low == string_flag_escape_form) [[likely]]
                  out.append(s);
               else
                  emit_string_escaping(out, s);
               out.push_back('"');
               return;
            }
            case t_bytes:
            {
               // §4.10 hint 0: standard base64, padded. (Raw octets are
               // not JSON text.)
               out.push_back('"');
               emit_base64(out, p + 1, size - 1);
               out.push_back('"');
               return;
            }
            case t_array:
            {
               // Generic array (low_nibble = 0) vs typed homogeneous
               // (low_nibble = 1..10 → element type code = low - 1).
               if (low == 0)
               {
                  std::uint8_t  slot_w_code = p[1] & 0x03;
                  std::size_t   slot_w      = width_bytes(slot_w_code);
                  std::uint16_t N =
                      static_cast<std::uint16_t>(p[size - 2]) |
                      (static_cast<std::uint16_t>(p[size - 1]) << 8);
                  std::size_t slot_table_pos  = size - 2 - slot_w * N;
                  std::size_t value_data_start = 2;
                  std::size_t value_data_size  =
                     slot_table_pos - value_data_start;
                  out.push_back('[');
                  for (std::uint16_t i = 0; i < N; ++i)
                  {
                     if (i) out.push_back(',');
                     std::uint32_t off = read_width(
                        p + slot_table_pos + i * slot_w, slot_w_code);
                     std::uint32_t off_next =
                         i + 1 < N
                             ? read_width(
                                  p + slot_table_pos + (i + 1) * slot_w,
                                  slot_w_code)
                             : static_cast<std::uint32_t>(value_data_size);
                     direct_pjson_to_json(out,
                                          p + value_data_start + off,
                                          off_next - off);
                  }
                  out.push_back(']');
                  return;
               }
               // Typed-array path.
               std::uint8_t code = typed_array_code_from_low(low);
               if (code == tac_invalid)
                  throw std::runtime_error(
                      "pjson_to_json: reserved t_array low_nibble");
               std::size_t es = typed_array_elem_size(code);
               std::uint16_t N =
                   static_cast<std::uint16_t>(p[size - 2]) |
                   (static_cast<std::uint16_t>(p[size - 1]) << 8);
               const std::uint8_t* base = p + 1;
               out.push_back('[');
               for (std::uint16_t i = 0; i < N; ++i)
               {
                  if (i) out.push_back(',');
                  const std::uint8_t* eb = base + i * es;
                  switch (code)
                  {
                     case tac_i8:
                     {
                        std::int8_t v;  std::memcpy(&v, eb, 1);
                        char tmp[8];
                        auto r = std::to_chars(tmp, tmp + sizeof(tmp),
                                               static_cast<int>(v));
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_i16:
                     {
                        std::int16_t v; std::memcpy(&v, eb, 2);
                        char tmp[8];
                        auto r = std::to_chars(tmp, tmp + sizeof(tmp),
                                               static_cast<int>(v));
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_i32:
                     {
                        std::int32_t v; std::memcpy(&v, eb, 4);
                        char tmp[16];
                        auto r =
                            std::to_chars(tmp, tmp + sizeof(tmp), v);
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_i64:
                     {
                        std::int64_t v; std::memcpy(&v, eb, 8);
                        char tmp[24];
                        auto r =
                            std::to_chars(tmp, tmp + sizeof(tmp), v);
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_u8:
                     {
                        char tmp[8];
                        auto r = std::to_chars(
                            tmp, tmp + sizeof(tmp),
                            static_cast<unsigned>(eb[0]));
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_u16:
                     {
                        std::uint16_t v; std::memcpy(&v, eb, 2);
                        char tmp[8];
                        auto r = std::to_chars(
                            tmp, tmp + sizeof(tmp),
                            static_cast<unsigned>(v));
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_u32:
                     {
                        std::uint32_t v; std::memcpy(&v, eb, 4);
                        char tmp[16];
                        auto r =
                            std::to_chars(tmp, tmp + sizeof(tmp), v);
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_u64:
                     {
                        std::uint64_t v; std::memcpy(&v, eb, 8);
                        char tmp[24];
                        auto r =
                            std::to_chars(tmp, tmp + sizeof(tmp), v);
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_f32:
                     {
                        float v; std::memcpy(&v, eb, 4);
                        if (!std::isfinite(v))
                           throw std::runtime_error("pjson_to_json: NaN and infinity have no JSON form");
                        char tmp[32];
                        auto r =
                            std::to_chars(tmp, tmp + sizeof(tmp), v);
                        out.append(tmp, r.ptr);
                        break;
                     }
                     case tac_f64:
                     {
                        double v; std::memcpy(&v, eb, 8);
                        if (!std::isfinite(v))
                           throw std::runtime_error("pjson_to_json: NaN and infinity have no JSON form");
                        char tmp[32];
                        auto r =
                            std::to_chars(tmp, tmp + sizeof(tmp), v);
                        out.append(tmp, r.ptr);
                        break;
                     }
                     default:
                        throw std::runtime_error(
                            "pjson_to_json: bad typed_array code");
                  }
               }
               out.push_back(']');
               return;
            }
            case t_object:
            {
               // low_nibble = 0: single object (handled below)
               // low_nibble = 1: row_array — not handled by direct walker yet,
               // fall through to error.
               if (low == object_form_row_array)
               {
                  // §5.2.1: an array of objects that share their keys. Read
                  // through the view, which places every record and field.
                  const pjson_view rows{p, size};
                  const std::size_t n = rows.count();
                  out.push_back('[');
                  for (std::size_t i = 0; i < n; ++i)
                  {
                     if (i) out.push_back(',');
                     out.push_back('{');
                     bool first = true;
                     rows.at(i).for_each_field([&](std::string_view k, pjson_view field) {
                        if (!first) out.push_back(',');
                        first = false;
                        out.push_back('"');
                        out.append(k);
                        out.push_back('"');
                        out.push_back(':');
                        direct_pjson_to_json(out, field.data(), field.size());
                     });
                     out.push_back('}');
                  }
                  out.push_back(']');
                  return;
               }
               if (low != 0)
                  throw std::runtime_error("pjson_to_json: invalid object form");
               std::uint8_t  slot_w_code = p[1] & 0x03;
               std::size_t   slot_w      = width_bytes(slot_w_code);
               std::size_t   slot_stride = slot_w + 1;
               std::uint16_t N =
                   static_cast<std::uint16_t>(p[size - 2]) |
                   (static_cast<std::uint16_t>(p[size - 1]) << 8);
               std::size_t slot_table_pos  = size - 2 - slot_stride * N;
               std::size_t hash_table_pos  = slot_table_pos - N;
               std::size_t value_data_start = 2;
               std::size_t value_data_size  =
                  hash_table_pos - value_data_start;
               (void)hash_table_pos;  // not needed for emission
               out.push_back('{');
               for (std::uint16_t i = 0; i < N; ++i)
               {
                  if (i) out.push_back(',');
                  const std::uint8_t* slot =
                     p + slot_table_pos + i * slot_stride;
                  std::uint32_t off = read_width(slot, slot_w_code);
                  std::uint8_t  ks  = slot[slot_w];
                  std::uint32_t off_next =
                      i + 1 < N
                          ? read_width(p + slot_table_pos +
                                          (i + 1) * slot_stride,
                                       slot_w_code)
                          : static_cast<std::uint32_t>(value_data_size);
                  const std::uint8_t* entry = p + value_data_start + off;
                  std::size_t         entry_size = off_next - off;
                  std::size_t         klen, klen_bytes;
                  if (ks != 0xFF)
                  {
                     klen       = ks;
                     klen_bytes = 0;
                  }
                  else
                  {
                     std::uint64_t excess;
                     klen_bytes =
                         read_varuint62(entry, entry_size, excess);
                     klen = 0xFFu + static_cast<std::size_t>(excess);
                  }
                  // Emit key (escape-form already in pjson bytes).
                  out.push_back('"');
                  out.append(reinterpret_cast<const char*>(
                                 entry + klen_bytes),
                             klen);
                  out.push_back('"');
                  out.push_back(':');
                  std::size_t value_off = klen_bytes + klen;
                  direct_pjson_to_json(out, entry + value_off,
                                       entry_size - value_off);
               }
               out.push_back('}');
               return;
            }
            default:
               throw std::runtime_error(
                   "pjson_to_json: invalid tag");
         }
      }

   }  // namespace pjson_detail

   // Convert pjson bytes directly to JSON text. Skips the view
   // abstraction; ~10-15 ns faster than view_to_json on small docs,
   // similar for large.
   inline std::string pjson_to_json(std::span<const std::uint8_t> bytes)
   {
      // The walker reads a frame without comparing anything: the bytes are
      // validated first, so it is never handed anything else. What the
      // validator accepts, this converts (gofractally/psio#17).
      if (!pjson::validate(bytes))
         throw std::runtime_error("pjson_to_json: the bytes are not a pjson value");
      std::string out;
      out.reserve(bytes.size());
      pjson_detail::direct_pjson_to_json(out, bytes.data(), bytes.size());
      return out;
   }

   // In-place form: caller-owned output buffer.
   inline void pjson_to_json(std::span<const std::uint8_t> bytes,
                             std::string&                  out)
   {
      if (!pjson::validate(bytes))
         throw std::runtime_error("pjson_to_json: the bytes are not a pjson value");
      out.clear();
      pjson_detail::direct_pjson_to_json(out, bytes.data(), bytes.size());
   }

}  // namespace psio
