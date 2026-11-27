//
// psio/pjson_canonical.hpp — the canonical form of a pjson value.
//
// Part of <psio/pjson.hpp>, which includes it at the right place; including
// this header alone includes that one.
//
// A pjson value has exactly one canonical encoding (docs/pjson-spec.md §15;
// gofractally/psio#17). It is a function of the VALUE alone:
//
// Canonical is ONE ENCODING PER VALUE OF A GIVEN TYPE. A typed encoder
// writes what its type says, with no look at the data: an integer in the
// integer forms with the fewest bytes, a float as its bits at its declared
// width, a list of numbers as a typed array of its declared element code, a
// list of records as a row array, a string as raw UTF-8 text.
//
// What has no type is decided from the data, by the canonical pass
// (`anonymous`): a number is the integer forms, or the smaller of decimal
// and binary64; a generic array whose elements are all integers of at most
// 64 bits is a typed array of the narrowest code, one whose elements are all
// numbers binary64 carries is typed f64 when that is not larger, one whose
// elements are all objects with the same names is a row array.
//
// The walk below writes a well-formed frame again:
//
//   typed mode      every construct in the SAME type, in its one encoding
//                   (a frame is canonical exactly when this gives it back);
//   anonymous mode  as a document with no types: numbers and typed arrays
//                   are chosen again from their values.
//
// In both modes: the empty array is the generic empty array, a generic
// array is typed / a row array where the rules above say, strings are raw
// text, widths are the smallest that fit.
//
// Numbers: psio/pjson_number_model.hpp.

#include <psio/pjson.hpp>

#ifndef PSIO_PJSON_CANONICAL_HPP
#define PSIO_PJSON_CANONICAL_HPP

#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace psio {
   namespace pjson_detail {

      // ── A valid frame -> its canonical frame ────────────────────────────

      struct canonicalizer
      {
         using view = dynamic_view<pjson_format>;
         using kind = view::kind;
         std::vector<std::uint8_t>&    out;
         bool                          anonymous; // choose numbers and typed arrays from the data
         // Hold strings and member names to UTF-8. A validating read and
         // the pass over an outside document do; a WRITE of one's own value
         // does not (the fast write: gofractally/psio#14, #17) — text that
         // is not UTF-8 is then written as it is, and the validator refuses
         // the frame.
         bool                          check_text;
         std::vector<std::uint32_t>    offsets;   // one stack for every container
         std::vector<std::string_view> names;
         std::vector<number_value>     numbers;

         [[noreturn]] static void no_form(const char* what) { throw std::runtime_error(what); }

         static bool is_number(const view& v) noexcept
         {
            const auto k = v.type();
            return k == kind::integer || k == kind::decimal || k == kind::floating;
         }

         void value(const view& v)
         {
            const auto raw = v.raw();
            switch (v.type())
            {
               case kind::integer:
               case kind::decimal:
               case kind::floating:
                  return number(raw);
               case kind::string:
               {
                  const std::size_t start = out.size();
                  out.push_back(static_cast<std::uint8_t>(t_string << 4));
                  if ((raw[0] & 0x0F) == string_flag_escape_form)
                  {
                     if (!append_unescaped(out, raw.data() + 1, raw.size() - 1))
                     {
                        if (check_text)
                           no_form("pjson: a string is not text (its escapes do not name UTF-8): it has no canonical form");
                        out.resize(start);   // written as given
                        out.insert(out.end(), raw.begin(), raw.end());
                     }
                  }
                  else
                  {
                     if (check_text && !is_utf8(raw.data() + 1, raw.size() - 1))
                        no_form("pjson: a string is not UTF-8: it has no canonical form");
                     out.insert(out.end(), raw.begin() + 1, raw.end());
                  }
                  return;
               }
               case kind::array:
                  if (v.is_typed_array()) return typed_array(v);
                  if (v.is_row_array()) return rows(v, v.count());
                  return array(v);
               case kind::object: return object(v);
               default:   // null, bool, bytes: one form
                  out.insert(out.end(), raw.begin(), raw.end());
            }
         }

         // A scalar number. Typed mode keeps its type: an integer in the
         // fewest bytes, a decimal without trailing zeros (an integer when
         // it is one), a float as its bits (the notation hint cleared).
         void number(std::span<const std::uint8_t> raw)
         {
            const std::uint8_t tag = raw[0] >> 4;
            if (anonymous) return append_canonical_number(out, frame_number(raw.data(), raw.size()));
            if (tag == t_ieee_float)
            {
               out.push_back(static_cast<std::uint8_t>(raw[0] & ~ieee_sci_bit));
               const std::uint8_t width = raw[0] & ieee_width_mask;
               std::uint64_t      bits  = 0;
               std::memcpy(&bits, raw.data() + 1, raw.size() - 1);
               // A scalar NaN is the quiet NaN of its width.
               if (width == ieee_width_f64 && (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (bits & 0xFFFFFFFFFFFFFull))
                  bits = canonical_nan_f64;
               else if (width == ieee_width_f32 && (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu))
                  bits = canonical_nan_f32;
               else if (width == ieee_width_f16 && (bits & 0x7C00u) == 0x7C00u && (bits & 0x3FFu))
                  bits = 0x7E00u;
               const auto at = out.size();
               out.resize(at + raw.size() - 1);
               std::memcpy(out.data() + at, &bits, raw.size() - 1);
               return;
            }
            const number_value v = frame_number(raw.data(), raw.size());
            std::uint8_t       buf[max_number_size + 8];
            unsigned __int128  whole;
            std::size_t        n;
            if (as_integer(v.d, whole))
               n = canonical_integer_bytes(v.d.neg, whole, buf);
            else
            {
               const __int128 m = static_cast<__int128>(v.d.mag);   // a decimal frame's mantissa is below 2^127
               n = encode_number_at(buf, 0, pjson_number{v.d.neg ? -m : m, v.d.scale});
            }
            out.insert(out.end(), buf, buf + n);
         }

         // A generic array of numbers, each with no type.
         void generic_numbers(std::size_t n, const std::function<number_value(std::size_t)>& at)
         {
            out.push_back(static_cast<std::uint8_t>(t_array << 4));
            const std::size_t width_at = out.size();
            out.push_back(0);
            const std::size_t data = out.size(), base = offsets.size();
            for (std::size_t i = 0; i < n; ++i)
            {
               offsets.push_back(static_cast<std::uint32_t>(out.size() - data));
               append_canonical_number(out, at(i));
            }
            const std::uint8_t code = width_code_for(out.size() - data);
            out[width_at] = code;
            for (std::size_t i = 0; i < n; ++i) append_width(out, code, offsets[base + i]);
            offsets.resize(base);
            append_count(out, n);
         }

         void typed_array(const view& v)
         {
            const auto        raw  = v.raw();
            const auto        code = v.typed_array_elem_code();
            const std::size_t w    = typed_array_elem_size(code);
            const std::size_t n    = (raw.size() - 3) / w;
            if (n == 0) return append_empty_array(out);
            // A typed array has its element type: typed mode keeps it.
            if (!anonymous) return (void)out.insert(out.end(), raw.begin(), raw.end());
            // Without a type the form is the values': the narrowest integer
            // code, binary64, or a generic array of numbers.
            const auto at = [&](std::size_t i) { return typed_element_number(code, raw.data() + 1 + i * w); };
            if (!append_number_array(out, n, at)) generic_numbers(n, at);
         }

         void object(const view& v)
         {
            out.push_back(static_cast<std::uint8_t>(t_object << 4));
            const std::size_t width_at = out.size();
            out.push_back(0);
            const std::size_t data = out.size(), base = offsets.size();
            std::size_t       n    = 0;
            v.for_each_field([&](std::string_view key, view field) {
               if (check_text && !is_utf8(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()))
                  no_form("pjson: a member name is not UTF-8: it has no canonical form");
               offsets.push_back(static_cast<std::uint32_t>(out.size() - data));
               offsets.push_back(static_cast<std::uint32_t>(key.size() < 0xFF ? key.size() : 0xFF) |
                                 (static_cast<std::uint32_t>(key_hash8(key)) << 8));
               if (key.size() >= 0xFF)
               {
                  std::uint8_t excess[4];
                  out.insert(out.end(), excess, excess + write_varuint62(excess, 0, key.size() - 0xFF));
               }
               out.insert(out.end(), key.begin(), key.end());
               value(field);
               ++n;
            });
            const std::uint8_t code = width_code_for(out.size() - data);
            out[width_at] = code;
            for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<std::uint8_t>(offsets[base + 2 * i + 1] >> 8));
            for (std::size_t i = 0; i < n; ++i)
            {
               append_width(out, code, offsets[base + 2 * i]);
               out.push_back(static_cast<std::uint8_t>(offsets[base + 2 * i + 1]));
            }
            offsets.resize(base);
            append_count(out, n);
         }

         // A row array of `n` records whose names are those of record 0.
         // `v` is a generic array of objects or a row array.
         void rows(const view& v, std::size_t n)
         {
            if (n == 0) return append_empty_array(out);
            const std::size_t name_base = names.size();
            v.at(0).for_each_field([&](std::string_view key, view) { names.push_back(key); });
            const std::size_t k = names.size() - name_base;
            for (std::uint8_t slot_code = 0;; ++slot_code)
            {
               const std::size_t start = out.size();
               out.push_back(static_cast<std::uint8_t>((t_object << 4) | object_form_row_array));
               out.push_back(0);
               std::uint8_t count[4];
               out.insert(out.end(), count, count + write_varuint62(count, 0, k));
               std::uint32_t key_at = 0;
               for (std::size_t j = 0; j < k; ++j)
               {
                  const auto key = names[name_base + j];
                  append_width(out, 3, pack_slot(key_at, static_cast<std::uint8_t>(key.size())));
                  key_at += static_cast<std::uint32_t>(key.size());
               }
               for (std::size_t j = 0; j < k; ++j) out.push_back(key_hash8(names[name_base + j]));
               for (std::size_t j = 0; j < k; ++j)
               {
                  const auto key = names[name_base + j];
                  if (check_text && !is_utf8(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()))
                     no_form("pjson: a member name is not UTF-8: it has no canonical form");
                  out.insert(out.end(), key.begin(), key.end());
               }
               const std::size_t body = out.size(), base = offsets.size();
               bool              refit = false;
               for (std::size_t i = 0; i < n && !refit; ++i)
               {
                  offsets.push_back(static_cast<std::uint32_t>(out.size() - body));
                  const std::size_t record = out.size(), fields = offsets.size();
                  v.at(i).for_each_field([&](std::string_view, view field) {
                     offsets.push_back(static_cast<std::uint32_t>(out.size() - record));
                     value(field);
                  });
                  if (width_code_for(out.size() - record) > slot_code)
                     refit = true;
                  else
                     for (std::size_t j = 0; j < k; ++j) append_width(out, slot_code, offsets[fields + j]);
                  offsets.resize(fields);
               }
               if (refit)
               {
                  out.resize(start);
                  offsets.resize(base);
                  continue;
               }
               const std::uint8_t record_code = width_code_for(out.size() - body);
               out[start + 1] = static_cast<std::uint8_t>(slot_code | (record_code << 2));
               for (std::size_t i = 0; i < n; ++i) append_width(out, record_code, offsets[base + i]);
               offsets.resize(base);
               append_count(out, n);
               break;
            }
            names.resize(name_base);
         }

         void array(const view& v)
         {
            const std::size_t n = v.count();
            if (n == 0) return append_empty_array(out);
            // One look at the elements: all numbers? all objects of one shape?
            bool              all_numbers = true, same_shape = true;
            const std::size_t name_base = names.size(), number_base = numbers.size();
            std::size_t       index = 0;
            v.for_each_element([&](view e) {
               if (all_numbers)
               {
                  if (is_number(e))
                  {
                     const auto raw = e.raw();
                     numbers.push_back(frame_number(raw.data(), raw.size()));
                  }
                  else
                     all_numbers = false;
               }
               if (same_shape)
               {
                  if (e.type() != kind::object || e.is_row_array())
                     same_shape = false;
                  else if (index == 0)
                  {
                     e.for_each_field([&](std::string_view key, view) {
                        names.push_back(key);
                        if (key.size() >= 0xFF) same_shape = false;
                     });
                     if (names.size() == name_base) same_shape = false;
                  }
                  else
                  {
                     std::size_t j = name_base;
                     e.for_each_field([&](std::string_view key, view) {
                        if (j >= names.size() || names[j] != key) same_shape = false;
                        ++j;
                     });
                     if (j != names.size()) same_shape = false;
                  }
               }
               ++index;
            });
            names.resize(name_base);
            if (all_numbers)
            {
               const bool typed = append_number_array(out, n, [&](std::size_t i) { return numbers[number_base + i]; });
               numbers.resize(number_base);
               if (typed) return;
            }
            numbers.resize(number_base);
            if (same_shape) return rows(v, n);

            out.push_back(static_cast<std::uint8_t>(t_array << 4));
            const std::size_t width_at = out.size();
            out.push_back(0);
            const std::size_t data = out.size(), base = offsets.size();
            v.for_each_element([&](view e) {
               offsets.push_back(static_cast<std::uint32_t>(out.size() - data));
               value(e);
            });
            const std::uint8_t code = width_code_for(out.size() - data);
            out[width_at] = code;
            for (std::size_t i = 0; i < n; ++i) append_width(out, code, offsets[base + i]);
            offsets.resize(base);
            append_count(out, n);
         }
      };

      // `bytes` must be a valid frame (pjson_lazy::validate).
      inline void append_canonical(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes)
      {
         canonicalizer c{out, true, true, {}, {}, {}};
         c.value(dynamic_view<pjson_format>{bytes.data(), bytes.size()});
      }
      // `bytes` in the same types, each in its one encoding: the frame
      // itself exactly when it is canonical.
      inline void append_normalized(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes,
                                    bool check_text)
      {
         canonicalizer c{out, false, check_text, {}, {}, {}};
         c.value(dynamic_view<pjson_format>{bytes.data(), bytes.size()});
      }

   }  // namespace pjson_detail
}  // namespace psio

#endif  // PSIO_PJSON_CANONICAL_HPP
