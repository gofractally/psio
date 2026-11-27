//
// psio/pjson_canonical.hpp — the canonical form of a pjson value.
//
// Part of <psio/pjson.hpp>, which includes it at the right place; including
// this header alone includes that one.
//
// A pjson value has exactly one canonical encoding (docs/pjson-spec.md §15;
// gofractally/psio#17). It is a function of the VALUE alone:
//
//   number    an integer below 2^128 in magnitude: uint_inline / nint_inline
//             / uint / negint with the fewest bytes. Any other finite
//             number: the smallest of decimal (no trailing zeros), binary32
//             and binary64 that carries it; a tie goes to the float, the
//             narrower first. -0, NaN, +Inf, -Inf: binary16.
//   string    raw_text (flag 0), valid UTF-8.
//   bytes     tag A0.
//   array     empty: the generic empty array. Every element an integer of
//             at most 64 bits: a typed array of the narrowest code
//             (unsigned if none is negative). Else every element a number
//             binary32 carries: typed f32; else every element a number
//             binary64 carries: typed f64. Else every element an object
//             with the same names in the same order (at least one name,
//             each under 255 bytes): a row array. Else generic.
//   object    generic; members in the order given; repeated names kept;
//             names are raw text.
//   widths    the smallest that fit.
//
// Numbers: psio/pjson_number_model.hpp.

#include <psio/pjson.hpp>

#ifndef PSIO_PJSON_CANONICAL_HPP
#define PSIO_PJSON_CANONICAL_HPP

#include <cstdint>
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
                  return append_canonical_number(out, frame_number(raw.data(), raw.size()));
               case kind::string:
               {
                  out.push_back(static_cast<std::uint8_t>(t_string << 4));
                  if ((raw[0] & 0x0F) == string_flag_escape_form)
                  {
                     if (!append_unescaped(out, raw.data() + 1, raw.size() - 1))
                        no_form("pjson: a string is not text (its escapes do not name UTF-8): it has no canonical form");
                  }
                  else
                  {
                     if (!is_utf8(raw.data() + 1, raw.size() - 1))
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

         void typed_array(const view& v)
         {
            const auto        raw  = v.raw();
            const auto        code = v.typed_array_elem_code();
            const std::size_t w    = typed_array_elem_size(code);
            const std::size_t n    = (raw.size() - 3) / w;
            if (n == 0) return append_empty_array(out);
            // Always a typed array again: a typed element is a number a
            // typed array holds.
            if (!append_number_array(out, n, [&](std::size_t i) { return typed_element_number(code, raw.data() + 1 + i * w); }))
               no_form("pjson: internal: a typed array element is not a number a typed array holds");
         }

         void object(const view& v)
         {
            out.push_back(static_cast<std::uint8_t>(t_object << 4));
            const std::size_t width_at = out.size();
            out.push_back(0);
            const std::size_t data = out.size(), base = offsets.size();
            std::size_t       n    = 0;
            v.for_each_field([&](std::string_view key, view field) {
               if (!is_utf8(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()))
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
                  if (!is_utf8(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()))
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
         canonicalizer c{out, {}, {}, {}};
         c.value(dynamic_view<pjson_format>{bytes.data(), bytes.size()});
      }

   }  // namespace pjson_detail
}  // namespace psio

#endif  // PSIO_PJSON_CANONICAL_HPP
