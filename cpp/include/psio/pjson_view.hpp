#pragma once
//
// psio/pjson_view.hpp — zero-copy view over pjson bytes.
//
// A pjson value is (ptr, size). pjson_view holds exactly that pair.
// Every accessor is offset arithmetic + small decode of the tag byte.
// No allocation, no copies, no intermediate tree.
//
// Construct from a (ptr, size) pair (typically the buffer the caller
// owns), or from a span. Sub-views of nested children are produced by
// `find()` / `at()`, computing each child's `(ptr, size)` from the
// parent's slot table.

#include <psio/format.hpp>
#include <psio/pjson.hpp>

#include <psio/detail/find_byte.hpp>  // psio::detail::find_byte — SWAR 8-byte scan

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace psio {

   template <>
   class dynamic_view<pjson_format>
   {
     public:
      using format = pjson_format;
      enum class kind : std::uint8_t
      {
         null,
         boolean,
         integer,
         decimal,
         floating,
         string,
         bytes,
         array,
         object,
         invalid
      };

      dynamic_view() = default;
      dynamic_view(const std::uint8_t* data, std::size_t size) noexcept
          : data_(data), size_(size)
      {
      }
      explicit dynamic_view(std::span<const std::uint8_t> b) noexcept
          : data_(b.data()), size_(b.size())
      {
      }

      bool                          valid() const noexcept { return data_ != nullptr; }
      const std::uint8_t*           data() const noexcept { return data_; }
      std::size_t                   size() const noexcept { return size_; }
      std::span<const std::uint8_t> raw() const noexcept
      {
         return {data_, size_};
      }

      kind type() const noexcept
      {
         if (!data_) return kind::invalid;
         using namespace pjson_detail;
         // Synthetic forms — interpret based on form_/synth_param_.
         if (form_ == form_typed_array_element)
         {
            switch (synth_param_)
            {
               case tac_i8:  case tac_i16: case tac_i32: case tac_i64:
               case tac_u8:  case tac_u16: case tac_u32: case tac_u64:
                  return kind::integer;
               case tac_f32: case tac_f64:
                  return kind::floating;
               default:
                  return kind::invalid;
            }
         }
         if (form_ == form_row_array_record)
            return kind::object;

         // Normal form — read the tag byte.
         if (size_ == 0) return kind::invalid;
         std::uint8_t t = data_[0] >> 4;
         switch (t)
         {
            case t_null:        return kind::null;
            case t_bool:        return kind::boolean;
            case t_nint_inline:
            case t_uint_inline:
            case t_uint:
            case t_negint:      return kind::integer;
            case t_decimal:     return kind::decimal;
            case t_ieee_float:  return kind::floating;
            case t_string:      return kind::string;
            case t_bytes:       return kind::bytes;
            // t_array's low nibble selects layout: 0 = generic, 1..10
            // = typed homogeneous. Both surface as kind::array; callers
            // that care about the difference (JSON emit, fast bulk
            // reads) test is_typed_array().
            case t_array:       return kind::array;
            // t_object low nibble: 0 = single object → kind::object;
            // 1 = row_array (homogeneous-shape array of objects) →
            // kind::array (semantically a list, even though the wire
            // shares t_object's high nibble).
            case t_object:
               return ((data_[0] & 0x0F) == pjson_detail::object_form_row_array)
                          ? kind::array
                          : kind::object;
            default:            return kind::invalid;
         }
      }

      bool is_null() const noexcept { return type() == kind::null; }
      bool is_bool() const noexcept { return type() == kind::boolean; }
      bool is_integer() const noexcept { return type() == kind::integer; }
      bool is_decimal() const noexcept { return type() == kind::decimal; }
      bool is_floating() const noexcept { return type() == kind::floating; }
      bool is_number() const noexcept
      {
         auto k = type();
         return k == kind::integer || k == kind::decimal ||
                k == kind::floating;
      }
      bool is_string() const noexcept { return type() == kind::string; }
      bool is_bytes() const noexcept { return type() == kind::bytes; }
      bool is_array() const noexcept { return type() == kind::array; }
      bool is_object() const noexcept { return type() == kind::object; }

      // Distinguishes the typed homogeneous-primitive array form
      // (tag t_array, low nibble 1..10, raw little-endian element
      // bytes, no slot table) from the generic array (tag t_array,
      // low nibble = 0, per-element tags + slot table). Reserved
      // low-nibble values (11..15) return false — those buffers are
      // malformed and a validator should reject them.
      bool is_typed_array() const noexcept
      {
         if (!data_ || size_ == 0) return false;
         if ((data_[0] >> 4) != pjson_detail::t_array) return false;
         return pjson_detail::typed_array_code_from_low(
                    data_[0] & 0x0F) != pjson_detail::tac_invalid;
      }
      // Distinguishes the row_array form of t_object from the single-
      // object form. Both report kind::object/kind::array via type();
      // is_row_array() answers the wire-form question directly.
      bool is_row_array() const noexcept
      {
         if (!data_ || size_ == 0) return false;
         if ((data_[0] >> 4) != pjson_detail::t_object) return false;
         return (data_[0] & 0x0F) == pjson_detail::object_form_row_array;
      }
      // Element type code (tac_i8 .. tac_f64). Undefined unless
      // is_typed_array().
      std::uint8_t typed_array_elem_code() const noexcept
      {
         return pjson_detail::typed_array_code_from_low(
             data_[0] & 0x0F);
      }
      // Element byte size. Undefined unless is_typed_array().
      std::size_t typed_array_elem_size() const noexcept
      {
         return pjson_detail::typed_array_elem_size(
             typed_array_elem_code());
      }

      // ── scalar accessors ────────────────────────────────────────────

      bool as_bool() const
      {
         require_(kind::boolean);
         // low nibble carries the boolean value
         return (data_[0] & 0x0F) != 0;
      }

      // Cross-tier: integer / decimal-with-scale-0 / integer-valued double.
      std::int64_t as_int64() const
      {
         using namespace pjson_detail;
         if (form_ == form_typed_array_element)
         {
            switch (synth_param_)
            {
               case tac_i8:
               { std::int8_t v;  std::memcpy(&v, data_, 1); return v; }
               case tac_i16:
               { std::int16_t v; std::memcpy(&v, data_, 2); return v; }
               case tac_i32:
               { std::int32_t v; std::memcpy(&v, data_, 4); return v; }
               case tac_i64:
               { std::int64_t v; std::memcpy(&v, data_, 8); return v; }
               case tac_u8:  return data_[0];
               case tac_u16:
               { std::uint16_t v; std::memcpy(&v, data_, 2); return v; }
               case tac_u32:
               { std::uint32_t v; std::memcpy(&v, data_, 4); return v; }
               case tac_u64:
               {
                  std::uint64_t v; std::memcpy(&v, data_, 8);
                  if (v > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()))
                     throw std::out_of_range(
                        "pjson_view::as_int64: u64 exceeds i64");
                  return static_cast<std::int64_t>(v);
               }
               default:
                  throw std::runtime_error(
                     "pjson_view::as_int64: float element");
            }
         }
         scalar_ok_();
         std::uint8_t t = data_[0] >> 4;
         if (t == t_nint_inline) {
            if (size_ != 1 || !(data_[0] & 15)) throw std::runtime_error("pjson_view: invalid inline negative integer");
            return -static_cast<std::int64_t>(data_[0] & 15);
         }
         if (t == t_uint_inline) return static_cast<std::int64_t>(data_[0] & 0x0F);
         if (t == t_uint)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            if (bc > 8)
               throw std::out_of_range(
                   "pjson_view::as_int64: exceeds int64");
            std::uint64_t mag = 0;
            std::memcpy(&mag, data_ + 1, bc);
            if (mag > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
               throw std::out_of_range(
                   "pjson_view::as_int64: exceeds int64");
            return static_cast<std::int64_t>(mag);
         }
         if (t == t_negint)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            if (bc > 8)
               throw std::out_of_range(
                   "pjson_view::as_int64: exceeds int64");
            std::uint64_t mag = 0;
            std::memcpy(&mag, data_ + 1, bc);
            if (mag == 0)
               throw std::runtime_error(
                   "pjson_view::as_int64: negint zero is reserved");
            // mag fits u64; -mag fits i64 iff mag ≤ 2^63.
            constexpr std::uint64_t imin_mag =
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) + 1u;
            if (mag > imin_mag)
               throw std::out_of_range(
                   "pjson_view::as_int64: exceeds int64");
            if (mag == imin_mag)
               return std::numeric_limits<std::int64_t>::min();
            return -static_cast<std::int64_t>(mag);
         }
         pjson_number n   = as_number();
         auto         opt = n.to_int64();
         if (opt) return *opt;
         throw std::out_of_range(
             "pjson_view::as_int64: not exactly representable");
      }

      __int128 as_int128() const
      {
         using namespace pjson_detail;
         // An element of a typed array is its raw bytes, not a tagged value.
         if (form_ == form_typed_array_element)
         {
            if (synth_param_ == tac_u64)
            {
               std::uint64_t v;
               std::memcpy(&v, data_, 8);
               return static_cast<__int128>(v);
            }
            return as_int64();  // (throws for a float element)
         }
         scalar_ok_();
         std::uint8_t t = data_[0] >> 4;
         if (t == t_nint_inline) return as_int64();
         if (t == t_uint_inline)
            return static_cast<__int128>(data_[0] & 0x0F);
         if (t == t_uint)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            __uint128_t mag = 0;
            std::memcpy(&mag, data_ + 1, bc);
            if (mag >> 127)
               throw std::out_of_range(
                   "pjson_view::as_int128: exceeds int128 (as_uint128 reads a uint of 128 bits)");
            return static_cast<__int128>(mag);
         }
         if (t == t_negint)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            __uint128_t mag = 0;
            std::memcpy(&mag, data_ + 1, bc);
            if (mag > (static_cast<__uint128_t>(1) << 127))
               throw std::out_of_range("pjson_view::as_int128: exceeds int128");
            return static_cast<__int128>(__uint128_t{0} - mag);
         }
         // Not an integer tag: a decimal's mantissa (best effort), a
         // float's integer part. (Never back through as_number() for an
         // integer: that is this function.)
         if (t == t_decimal || t == t_ieee_float)
            return as_number().mantissa;
         throw std::runtime_error("pjson_view::as_int128: not a number");
      }

      // The magnitude of an integer (`uint` and the inline forms: the
      // value; `negint`: its absolute value), all 128 bits of it.
      unsigned __int128 as_uint128() const
      {
         using namespace pjson_detail;
         if (form_ == form_typed_array_element)
         {
            const __int128 v = as_int128();
            if (v < 0) throw std::out_of_range("pjson_view::as_uint128: negative");
            return static_cast<unsigned __int128>(v);
         }
         scalar_ok_();
         const std::uint8_t t = data_[0] >> 4;
         if (t == t_uint)
         {
            __uint128_t mag = 0;
            std::memcpy(&mag, data_ + 1, (data_[0] & 0x0F) + 1u);
            return mag;
         }
         const __int128 v = as_int128();
         if (v < 0) throw std::out_of_range("pjson_view::as_uint128: negative");
         return static_cast<unsigned __int128>(v);
      }

      double as_double() const
      {
         using namespace pjson_detail;
         if (form_ == form_typed_array_element)
         {
            switch (synth_param_)
            {
               case tac_f32:
               { float f;  std::memcpy(&f, data_, 4); return f; }
               case tac_f64:
               { double d; std::memcpy(&d, data_, 8); return d; }
               default:
                  return static_cast<double>(as_int64());
            }
         }
         scalar_ok_();
         std::uint8_t t = data_[0] >> 4;
         if (t == t_ieee_float)
         {
            // §4.6 width selector: bits 2..0 of the low nibble.
            std::uint8_t width_bits = data_[0] & ieee_width_mask;
            if (width_bits == ieee_width_f64)
            {
               double d;
               std::memcpy(&d, data_ + 1, 8);
               return d;
            }
            if (width_bits == ieee_width_f32)
            {
               float f;
               std::memcpy(&f, data_ + 1, 4);
               return static_cast<double>(f);
            }
            if (width_bits == ieee_width_f16)
            {
               // Software-widen binary16 → double.
               std::uint16_t bits;
               std::memcpy(&bits, data_ + 1, 2);
               return f16_bits_to_double(bits);
            }
            throw std::runtime_error(
                "pjson_view::as_double: ieee_float width not supported");
         }
         return as_number().to_double();
      }

      pjson_number as_number() const
      {
         using namespace pjson_detail;
         auto k = type();
         if (k == kind::integer) return pjson_number{as_int128(), 0};
         if (k == kind::decimal)
         {
            scalar_ok_();
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            __uint128_t zz = 0;
            std::memcpy(&zz, data_ + 1, bc);
            std::int32_t scale;
            std::size_t  n =
                read_varint62(data_ + 1 + bc, size_ - 1 - bc, scale);
            if (n == 0) throw std::runtime_error("pjson_view: bad decimal");
            return pjson_number{zz128_decode(zz), scale};
         }
         if (k == kind::floating)
            return pjson_number::from_double(as_double());
         throw std::runtime_error("pjson_view::as_number: not a number");
      }

      std::string_view as_string() const
      {
         require_(kind::string);
         return std::string_view(
             reinterpret_cast<const char*>(data_ + 1), size_ - 1);
      }
      std::span<const std::uint8_t> as_bytes() const
      {
         require_(kind::bytes);
         return {data_ + 1, size_ - 1};
      }

      // String encoding flag (low nibble of the t_string tag).
      //   0 = raw_text       — bytes are unescaped text; emit must
      //                        run JSON escape detection
      //   1 = escape_form    — bytes are JSON-escape form already;
      //                        emit verbatim with surrounding quotes
      // Binary blobs use a separate t_bytes tag (kind::bytes), not a
      // string flag. Caller must check is_string() first; calling
      // this on a non-string value is undefined.
      std::uint8_t string_flag() const noexcept
      {
         return size_ ? data_[0] & 0x0F : 0;
      }

      // ── container access ────────────────────────────────────────────

      std::size_t count() const
      {
         using namespace pjson_detail;
         if (form_ == form_row_array_record)
         {
            // Field count K lives at parent's varuint after the width
            // byte; same value for every record in the row_array.
            return read_row_meta_(parent_, parent_size_).K;
         }
         if (form_ == form_typed_array_element)
            throw std::runtime_error(
               "pjson_view::count: not a container");
         if (!data_ || size_ < 3)
            throw std::runtime_error("pjson_view::count: not a container");
         std::uint8_t t = data_[0] >> 4;
         if (t != t_array && t != t_object)
            throw std::runtime_error("pjson_view::count: not a container");
         // The count is compared with the bytes before anything is sized
         // or walked by it (each layout has its own arithmetic).
         if (is_typed_array()) return typed_count_();
         if (is_row_array()) return read_row_meta_(data_, size_).N;
         return layout_(t == t_object).N;
      }

      // Element access: index into an array. Three cases:
      //   * typed_array — return a view in form_typed_array_element
      //     mode pointing at the raw element bytes; the synth_param_
      //     remembers the element type so as_int64/as_double work.
      //   * row_array — return a view in form_row_array_record mode
      //     pointing at the record body; parent_ keeps a pointer to
      //     the row_array buffer so find/for_each_field can read the
      //     shared schema directly.
      //   * generic array — return a normal view directly into the
      //     buffer (zero copy, form_normal).
      //
      // No allocation, no scratch buffers — the synthetic views alias
      // bytes already in the source buffer and carry just enough extra
      // state to interpret them correctly.
      dynamic_view at(std::size_t i) const
      {
         using namespace pjson_detail;
         if (is_typed_array())
         {
            std::size_t N = typed_count_();
            if (i >= N)
               throw std::out_of_range("pjson_view::at: typed_array index");
            std::uint8_t code = typed_array_elem_code();
            std::size_t  es   = pjson_detail::typed_array_elem_size(code);
            dynamic_view v;
            v.data_        = data_ + 1 + i * es;
            v.size_        = es;
            v.form_        = form_typed_array_element;
            v.synth_param_ = code;
            return v;
         }
         if (is_row_array())
         {
            return row_array_record_view_(i);
         }
         require_(kind::array);
         const layout_t L = layout_(false);
         if (i >= L.N)
            throw std::out_of_range("pjson_view::at: array index");
         const auto [off_i, off_next] = child_range_(L, i);
         return dynamic_view{data_ + 2 + off_i,
                             static_cast<std::size_t>(off_next - off_i)};
      }

      // Typed-array element accessors. Caller must check
      // is_typed_array() first; type code determines which accessor is
      // appropriate (signed / unsigned / float). Out-of-range index
      // throws.
      std::int64_t typed_int64_at(std::size_t i) const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_int64_at: not a typed array");
         std::size_t N  = typed_count_();
         if (i >= N)
            throw std::out_of_range("pjson_view::typed_int64_at");
         std::uint8_t       code = typed_array_elem_code();
         std::size_t        es   = pjson_detail::typed_array_elem_size(code);
         const std::uint8_t* eb  = data_ + 1 + i * es;
         switch (code)
         {
            case pjson_detail::tac_i8:
            { std::int8_t v;  std::memcpy(&v, eb, 1); return v; }
            case pjson_detail::tac_i16:
            { std::int16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_i32:
            { std::int32_t v; std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_i64:
            { std::int64_t v; std::memcpy(&v, eb, 8); return v; }
            case pjson_detail::tac_u8:
               return static_cast<std::int64_t>(eb[0]);
            case pjson_detail::tac_u16:
            { std::uint16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_u32:
            { std::uint32_t v; std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_u64:
            {
               std::uint64_t v;
               std::memcpy(&v, eb, 8);
               if (v > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()))
                  throw std::out_of_range(
                      "pjson_view::typed_int64_at: u64 exceeds i64");
               return static_cast<std::int64_t>(v);
            }
            default:
               throw std::runtime_error(
                   "pjson_view::typed_int64_at: float element");
         }
      }

      std::uint64_t typed_uint64_at(std::size_t i) const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_uint64_at: not a typed array");
         std::size_t N  = typed_count_();
         if (i >= N)
            throw std::out_of_range("pjson_view::typed_uint64_at");
         std::uint8_t       code = typed_array_elem_code();
         std::size_t        es   = pjson_detail::typed_array_elem_size(code);
         const std::uint8_t* eb  = data_ + 1 + i * es;
         switch (code)
         {
            case pjson_detail::tac_u8:  return eb[0];
            case pjson_detail::tac_u16:
            { std::uint16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_u32:
            { std::uint32_t v; std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_u64:
            { std::uint64_t v; std::memcpy(&v, eb, 8); return v; }
            // Signed → unsigned: reject negatives.
            case pjson_detail::tac_i8:
            {
               std::int8_t v; std::memcpy(&v, eb, 1);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            case pjson_detail::tac_i16:
            {
               std::int16_t v; std::memcpy(&v, eb, 2);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            case pjson_detail::tac_i32:
            {
               std::int32_t v; std::memcpy(&v, eb, 4);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            case pjson_detail::tac_i64:
            {
               std::int64_t v; std::memcpy(&v, eb, 8);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            default:
               throw std::runtime_error(
                   "pjson_view::typed_uint64_at: float element");
         }
      }

      double typed_double_at(std::size_t i) const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_double_at: not a typed array");
         std::size_t N  = typed_count_();
         if (i >= N)
            throw std::out_of_range("pjson_view::typed_double_at");
         std::uint8_t       code = typed_array_elem_code();
         std::size_t        es   = pjson_detail::typed_array_elem_size(code);
         const std::uint8_t* eb  = data_ + 1 + i * es;
         switch (code)
         {
            case pjson_detail::tac_f32:
            { float v;  std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_f64:
            { double v; std::memcpy(&v, eb, 8); return v; }
            // Integer fall-throughs: explicit conversion to double.
            case pjson_detail::tac_i8:
            { std::int8_t v;  std::memcpy(&v, eb, 1); return v; }
            case pjson_detail::tac_i16:
            { std::int16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_i32:
            { std::int32_t v; std::memcpy(&v, eb, 4); return static_cast<double>(v); }
            case pjson_detail::tac_i64:
            { std::int64_t v; std::memcpy(&v, eb, 8); return static_cast<double>(v); }
            case pjson_detail::tac_u8:  return static_cast<double>(eb[0]);
            case pjson_detail::tac_u16:
            { std::uint16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_u32:
            { std::uint32_t v; std::memcpy(&v, eb, 4); return static_cast<double>(v); }
            case pjson_detail::tac_u64:
            { std::uint64_t v; std::memcpy(&v, eb, 8); return static_cast<double>(v); }
            default:
               throw std::runtime_error(
                   "pjson_view::typed_double_at: invalid element code");
         }
      }

      // Raw typed-element span. Caller asserts the requested type
      // matches this view's element code; otherwise the bytes are
      // mis-interpreted. Useful for memcpy-style consumption when the
      // element type is known to match.
      template <typename T>
      std::span<const T> typed_array_span() const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_array_span: not a typed array");
         if (typed_array_elem_code() != pjson_detail::tac_for<T>())
            throw std::runtime_error(
                "pjson_view::typed_array_span: element-type mismatch");
         std::size_t N = typed_count_();
         // A span of T is read as T: the element bytes must be aligned for
         // it, which a wire buffer does not promise. (typed_int64_at /
         // typed_uint64_at / typed_double_at read byte-wise and always work.)
         if (reinterpret_cast<std::uintptr_t>(data_ + 1) % alignof(T) != 0)
            throw std::runtime_error(
                "pjson_view::typed_array_span: the elements are not aligned for the type");
         return std::span<const T>{
             reinterpret_cast<const T*>(data_ + 1), N};
      }

      // Object key lookup: hash prefilter + key verify.
      //
      // Prefilter uses `psio::detail::find_byte` (psitri's SWAR 8-byte-at-a-
      // time scan). Multi-trial bench (5M iterations × 5 trials):
      //
      //   psio::detail::find_byte    15.0-15.8 ns/access   (winner)
      //   libc memchr       16.1-16.3 ns/access
      //   scalar loop       17.9-18.9 ns/access
      //
      // For our typical N=5-30 hash bytes (all in one cache line,
      // already aligned), find_byte's tight SWAR loop has less setup
      // than libc memchr's general-purpose alignment + fallback
      // machinery. The tradeoff would flip around N=64+ where
      // memchr's larger-size optimizations pay off.
      // The elements of a typed array as values, copied byte-wise: works
      // wherever the bytes lie (typed_array_span needs them aligned).
      template <typename T>
      std::vector<T> typed_array_values() const
      {
         if (!is_typed_array())
            throw std::runtime_error("pjson_view::typed_array_values: not a typed array");
         if (typed_array_elem_code() != pjson_detail::tac_for<T>())
            throw std::runtime_error("pjson_view::typed_array_values: element-type mismatch");
         std::vector<T> out(typed_count_());
         if (!out.empty())
            std::memcpy(out.data(), data_ + 1, out.size() * sizeof(T));
         return out;
      }

      std::optional<dynamic_view> find(std::string_view key) const
      {
         using namespace pjson_detail;
         if (form_ == form_row_array_record)
            return row_record_find_(key);
         if (type() != kind::object) return std::nullopt;
         const layout_t L = layout_(true);
         const std::size_t  N = L.N;
         const std::uint8_t slot_w_code = L.slot_w_code;
         const std::size_t  slot_w      = L.slot_w;
         const std::size_t  slot_stride = L.stride;
         const std::size_t  slot_table_pos   = L.slot_table_pos;
         const std::size_t  hash_table_pos   = L.hash_table_pos;
         const std::size_t  value_data_start = 2;
         std::uint8_t want = key_hash8(key);

         const std::uint8_t* hashes    = data_ + hash_table_pos;
         std::size_t         remaining = N;
         std::size_t         base      = 0;
         while (remaining > 0)
         {
            int hit = psio::detail::find_byte(hashes + base, remaining, want);
            if (hit >= static_cast<int>(remaining)) return std::nullopt;
            std::size_t i = base + static_cast<std::size_t>(hit);
            const std::uint8_t* slot =
                data_ + slot_table_pos + i * slot_stride;
            std::uint8_t  ks    = slot[slot_w];
            const auto [off_i, off_next] = child_range_(L, i);
            const std::uint8_t* entry = data_ + value_data_start + off_i;
            std::size_t         entry_size = off_next - off_i;
            const auto [klen, klen_bytes] = entry_key_(entry, entry_size, ks);
            (void)slot_w_code;
            if (klen == key.size() &&
                std::memcmp(entry + klen_bytes, key.data(), klen) == 0)
            {
               std::size_t value_off = klen_bytes + klen;
               return dynamic_view{entry + value_off,
                                   entry_size - value_off};
            }
            // Hash collision; advance past and re-scan.
            base      = i + 1;
            remaining = (i + 1 <= N) ? (N - (i + 1)) : 0;
         }
         return std::nullopt;
      }

      dynamic_view operator[](std::string_view key) const
      {
         auto v = find(key);
         if (!v) throw std::out_of_range("pjson_view: key not found");
         return *v;
      }
      dynamic_view operator[](std::size_t i) const { return at(i); }

      // Iterators — visit each child in encounter order.
      template <typename Fn>
      void for_each_field(Fn&& fn) const
      {
         using namespace pjson_detail;
         if (form_ == form_row_array_record)
         {
            row_record_for_each_field_(std::forward<Fn>(fn));
            return;
         }
         require_(kind::object);
         const layout_t L = layout_(true);
         const std::size_t N = L.N;
         const std::size_t value_data_start = 2;
         for (std::size_t i = 0; i < N; ++i)
         {
            const std::uint8_t* slot =
                data_ + L.slot_table_pos + i * L.stride;
            std::uint8_t  ks    = slot[L.slot_w];
            const auto [off_i, off_next] = child_range_(L, i);
            const std::uint8_t* entry = data_ + value_data_start + off_i;
            std::size_t         entry_size = off_next - off_i;
            const auto [klen, klen_bytes] = entry_key_(entry, entry_size, ks);
            std::string_view k(
                reinterpret_cast<const char*>(entry + klen_bytes), klen);
            std::size_t value_off = klen_bytes + klen;
            fn(k, dynamic_view{entry + value_off, entry_size - value_off});
         }
      }

      template <typename Fn>
      void for_each_element(Fn&& fn) const
      {
         using namespace pjson_detail;
         require_(kind::array);
         if (is_typed_array() || is_row_array())
         {
            // Synthetic-element forms: defer to at(i), which builds a
            // form_typed_array_element / form_row_array_record view
            // pointing into the source bytes (no allocation).
            std::size_t N = count();
            for (std::size_t i = 0; i < N; ++i)
               fn(at(i));
            return;
         }
         const layout_t L = layout_(false);
         for (std::size_t i = 0; i < L.N; ++i)
         {
            const auto [off_i, off_next] = child_range_(L, i);
            fn(dynamic_view{data_ + 2 + off_i,
                            static_cast<std::size_t>(off_next - off_i)});
         }
      }

     private:
      void require_(kind k) const
      {
         if (type() != k)
            throw std::runtime_error("pjson_view: type mismatch");
      }

      // ── row_array helpers ──────────────────────────────────────────────
      //
      // Schema descriptor for a row_array, computed once per accessor
      // call from the parent buffer (data_ in array view, parent_ in
      // record view). All offsets are relative to the row_array buffer
      // start.
      // ── Placing what is read (gofractally/psio#15, #17) ────────────────
      //
      // A view may be handed any bytes. Every accessor places what it
      // reads inside them before reading it, and a member that cannot be
      // placed is a failure (std::runtime_error, as the pjson decoder
      // gives), never a read outside the bytes. A count is compared with
      // the bytes before anything is sized or walked by it.
      [[noreturn]] static void bad_(const char* what)
      {
         throw std::runtime_error(what);
      }
      // A number's tag says how many bytes follow it.
      void scalar_ok_() const
      {
         using namespace pjson_detail;
         if (!data_ || size_ == 0)
            bad_("pjson_view: not a pjson value: no bytes");
         const std::uint8_t t   = data_[0] >> 4;
         const std::uint8_t low = data_[0] & 0x0F;
         if (t == t_uint || t == t_negint)
         {
            if (size_ != 2u + low)
               bad_("pjson_view: not a pjson value: an integer is cut short");
         }
         else if (t == t_decimal)
         {
            if (size_ < 3u + low)
               bad_("pjson_view: not a pjson value: a decimal is cut short");
         }
         else if (t == t_ieee_float)
         {
            const std::uint8_t w = low & ieee_width_mask;
            const std::size_t  n = w == ieee_width_f64   ? 8
                                   : w == ieee_width_f32 ? 4
                                   : w == ieee_width_f16 ? 2
                                                         : 0;
            if (n == 0 || size_ != 1 + n)
               bad_("pjson_view: not a pjson value: a float is cut short");
         }
      }
      // The index of a generic array or a single object, at its tail.
      struct layout_t
      {
         std::size_t  N;
         std::uint8_t slot_w_code;
         std::size_t  slot_w;
         std::size_t  stride;           // bytes per slot (object: + key size)
         std::size_t  slot_table_pos;
         std::size_t  hash_table_pos;   // object only; == slot_table_pos for an array
         std::size_t  value_data_size;
      };
      layout_t layout_(bool object) const
      {
         using namespace pjson_detail;
         if (!data_ || size_ < 4)
            bad_("pjson_view: not a pjson value: a container is cut short");
         if (data_[1] & 0xFCu)
            bad_("pjson_view: not a pjson value: reserved bits in a container's width byte");
         layout_t L;
         L.N = static_cast<std::size_t>(data_[size_ - 2]) |
               (static_cast<std::size_t>(data_[size_ - 1]) << 8);
         L.slot_w_code = data_[1] & 0x03;
         L.slot_w      = width_bytes(L.slot_w_code);
         L.stride      = L.slot_w + (object ? 1 : 0);
         const std::size_t per = L.stride + (object ? 1 : 0);  // + hash byte
         if (L.N > (size_ - 4) / per)
            bad_("pjson_view: not a pjson value: a container's count exceeds its bytes");
         L.slot_table_pos  = size_ - 2 - L.stride * L.N;
         L.hash_table_pos  = L.slot_table_pos - (object ? L.N : 0);
         L.value_data_size = L.hash_table_pos - 2;
         return L;
      }
      // Where child i lies in the value data: [first, second).
      std::pair<std::uint32_t, std::uint32_t> child_range_(const layout_t& L,
                                                           std::size_t     i) const
      {
         using namespace pjson_detail;
         const std::uint32_t off_i =
            read_width(data_ + L.slot_table_pos + i * L.stride, L.slot_w_code);
         const std::uint32_t off_next =
            i + 1 < L.N
               ? read_width(data_ + L.slot_table_pos + (i + 1) * L.stride, L.slot_w_code)
               : static_cast<std::uint32_t>(L.value_data_size);
         if (off_i > off_next || off_next > L.value_data_size)
            bad_("pjson_view: not a pjson value: a child's offsets are out of order or out of range");
         return {off_i, off_next};
      }
      // The key of an object entry: {length, bytes of the long-key prefix}.
      static std::pair<std::size_t, std::size_t> entry_key_(const std::uint8_t* entry,
                                                            std::size_t         entry_size,
                                                            std::uint8_t        ks)
      {
         using namespace pjson_detail;
         std::size_t klen = ks, klen_bytes = 0;
         if (ks == 0xFF)
         {
            std::uint64_t excess;
            klen_bytes = read_varuint62(entry, entry_size, excess);
            if (klen_bytes == 0 || excess > entry_size)
               bad_("pjson_view: not a pjson value: a long key's length is cut short");
            klen = 0xFFu + static_cast<std::size_t>(excess);
         }
         if (klen_bytes + klen > entry_size)
            bad_("pjson_view: not a pjson value: a key lies outside its entry");
         return {klen, klen_bytes};
      }
      // The count of a typed array, which its span must hold exactly.
      std::size_t typed_count_() const
      {
         if (size_ < 3)
            bad_("pjson_view: not a pjson value: a typed array is cut short");
         const std::size_t N = static_cast<std::size_t>(data_[size_ - 2]) |
                               (static_cast<std::size_t>(data_[size_ - 1]) << 8);
         const std::size_t es = pjson_detail::typed_array_elem_size(typed_array_elem_code());
         if (es == 0 || size_ != 3 + N * es)
            bad_("pjson_view: not a pjson value: a typed array's count does not match its bytes");
         return N;
      }

      struct row_array_meta_
      {
         std::size_t   K;
         std::uint8_t  slot_w_code;
         std::uint8_t  recoff_w_code;
         std::size_t   slot_w;
         std::size_t   recoff_w;
         std::size_t   key_slots_pos;
         std::size_t   hash_pos;
         std::size_t   keys_pos;
         std::size_t   records_body_start;
         std::size_t   record_offsets_pos;
         std::size_t   keys_area_size;
         std::size_t   N;
         const std::uint8_t* base;
         std::size_t         base_size;
      };

      static row_array_meta_ read_row_meta_(const std::uint8_t* base,
                                            std::size_t         bsize)
      {
         using namespace pjson_detail;
         row_array_meta_ m;
         m.base          = base;
         m.base_size     = bsize;
         if (!base || bsize < 4)
            bad_("pjson_view: not a pjson value: a row array is cut short");
         std::uint8_t wb = base[1];
         if (wb & 0xF0u)
            bad_("pjson_view: not a pjson value: reserved bits in a row array's width byte");
         m.slot_w_code   = wb & 0x03;
         m.recoff_w_code = (wb >> 2) & 0x03;
         m.slot_w        = width_bytes(m.slot_w_code);
         m.recoff_w      = width_bytes(m.recoff_w_code);
         std::uint64_t K_u64;
         std::size_t   nb = read_varuint62(base + 2, bsize - 2, K_u64);
         if (nb == 0)
            throw std::runtime_error("pjson_view: bad row_array K");
         m.key_slots_pos  = 2 + nb;
         // K keys take at least five bytes each (slot + hash) in the header.
         if (K_u64 == 0 || m.key_slots_pos > bsize - 2 ||
             K_u64 > (bsize - 2 - m.key_slots_pos) / 5)
            bad_("pjson_view: not a pjson value: a row array's key count exceeds its bytes");
         m.K              = static_cast<std::size_t>(K_u64);
         m.hash_pos       = m.key_slots_pos + 4 * m.K;
         m.keys_pos       = m.hash_pos + m.K;
         std::uint32_t last_slot = read_u32_le(
             base + m.key_slots_pos + (m.K - 1) * 4);
         std::size_t keys_area_size =
             static_cast<std::size_t>(slot_offset(last_slot)) + slot_key_size(last_slot);
         if (keys_area_size > bsize - 2 - m.keys_pos)
            bad_("pjson_view: not a pjson value: a row array's keys lie outside it");
         m.keys_area_size     = keys_area_size;
         m.records_body_start = m.keys_pos + keys_area_size;
         m.N = static_cast<std::size_t>(base[bsize - 2]) |
               (static_cast<std::size_t>(base[bsize - 1]) << 8);
         if (m.N > (bsize - 2 - m.records_body_start) / m.recoff_w)
            bad_("pjson_view: not a pjson value: a row array's count exceeds its bytes");
         m.record_offsets_pos = bsize - 2 - m.N * m.recoff_w;
         return m;
      }

      // Construct a form_row_array_record view for record i. Caller is
      // a row_array array view (form_normal, t_object low_nibble = 1).
      dynamic_view row_array_record_view_(std::size_t i) const
      {
         using namespace pjson_detail;
         auto m = read_row_meta_(data_, size_);
         if (i >= m.N)
            throw std::out_of_range(
               "pjson_view::at: row_array record index");
         std::uint32_t roff_i = read_width(
             data_ + m.record_offsets_pos + i * m.recoff_w,
             m.recoff_w_code);
         std::uint32_t roff_next =
             i + 1 < m.N
                 ? read_width(
                       data_ + m.record_offsets_pos +
                           (i + 1) * m.recoff_w,
                       m.recoff_w_code)
                 : static_cast<std::uint32_t>(
                       m.record_offsets_pos - m.records_body_start);
         if (roff_i > roff_next ||
             roff_next > m.record_offsets_pos - m.records_body_start ||
             roff_next - roff_i < m.K * m.slot_w)
            bad_("pjson_view: not a pjson value: a row's offsets are out of order or out of range");
         dynamic_view v;
         v.data_        = data_ + m.records_body_start + roff_i;
         v.size_        = roff_next - roff_i;
         v.parent_      = data_;
         v.parent_size_ = size_;
         v.form_        = form_row_array_record;
         return v;
      }

      // Find a key inside a row_array record. Hash-prefilter scan over
      // the shared hash[K] from the parent buffer; on a hit, byte-equal
      // compare against the shared key bytes; on match, return a view
      // over the value bytes inside the record body.
      std::optional<dynamic_view>
      row_record_find_(std::string_view key) const
      {
         using namespace pjson_detail;
         auto m = read_row_meta_(parent_, parent_size_);
         if (size_ < m.K * m.slot_w)
            bad_("pjson_view: not a pjson value: a row is shorter than its slots");
         std::size_t  rec_slot_pos = size_ - m.K * m.slot_w;
         std::uint8_t want         = key_hash8(key);
         const std::uint8_t* hashes = parent_ + m.hash_pos;
         std::size_t         remaining = m.K;
         std::size_t         base      = 0;
         while (remaining > 0)
         {
            int hit = psio::detail::find_byte(hashes + base, remaining, want);
            if (hit >= static_cast<int>(remaining)) return std::nullopt;
            std::size_t j = base + static_cast<std::size_t>(hit);
            std::uint32_t kslot = read_u32_le(
                parent_ + m.key_slots_pos + j * 4);
            std::uint32_t koff  = slot_offset(kslot);
            std::uint8_t  ksize = slot_key_size(kslot);
            if (static_cast<std::size_t>(koff) + ksize > m.keys_area_size)
               bad_("pjson_view: not a pjson value: a row array key lies outside the keys");
            const std::uint8_t* kp = parent_ + m.keys_pos + koff;
            if (ksize == key.size() &&
                std::memcmp(kp, key.data(), ksize) == 0)
            {
               std::uint32_t voff = read_width(
                   data_ + rec_slot_pos + j * m.slot_w, m.slot_w_code);
               std::uint32_t vend =
                   j + 1 < m.K
                       ? read_width(
                             data_ + rec_slot_pos + (j + 1) * m.slot_w,
                             m.slot_w_code)
                       : static_cast<std::uint32_t>(rec_slot_pos);
               if (voff > vend || vend > rec_slot_pos)
                  bad_("pjson_view: not a pjson value: a row field's offsets are out of order or out of range");
               return dynamic_view{data_ + voff, vend - voff};
            }
            base      = j + 1;
            remaining = (j + 1 <= m.K) ? (m.K - (j + 1)) : 0;
         }
         return std::nullopt;
      }

      template <typename Fn>
      void row_record_for_each_field_(Fn&& fn) const
      {
         using namespace pjson_detail;
         auto m = read_row_meta_(parent_, parent_size_);
         if (size_ < m.K * m.slot_w)
            bad_("pjson_view: not a pjson value: a row is shorter than its slots");
         std::size_t rec_slot_pos = size_ - m.K * m.slot_w;
         for (std::size_t j = 0; j < m.K; ++j)
         {
            std::uint32_t kslot = read_u32_le(
                parent_ + m.key_slots_pos + j * 4);
            std::uint32_t koff  = slot_offset(kslot);
            std::uint8_t  ksize = slot_key_size(kslot);
            if (static_cast<std::size_t>(koff) + ksize > m.keys_area_size)
               bad_("pjson_view: not a pjson value: a row array key lies outside the keys");
            std::string_view k(
                reinterpret_cast<const char*>(parent_ + m.keys_pos + koff),
                ksize);
            std::uint32_t voff = read_width(
                data_ + rec_slot_pos + j * m.slot_w, m.slot_w_code);
            std::uint32_t vend =
                j + 1 < m.K
                    ? read_width(
                          data_ + rec_slot_pos + (j + 1) * m.slot_w,
                          m.slot_w_code)
                    : static_cast<std::uint32_t>(rec_slot_pos);
            if (voff > vend || vend > rec_slot_pos)
               bad_("pjson_view: not a pjson value: a row field's offsets are out of order or out of range");
            fn(k, dynamic_view{data_ + voff, vend - voff});
         }
      }

      // ── synth state ────────────────────────────────────────────────────
      //
      // The view augments its (data_, size_) pair with a small synth
      // descriptor that lets the read accessors handle wire forms whose
      // bytes aren't a self-contained pjson value. No scratch buffers,
      // no allocation — the bytes live in the parent buffer; the view
      // remembers how to interpret them.
      //
      //   form_normal              data_[0] is the real tag byte.
      //   form_typed_array_element data_ points at raw element bytes;
      //                            synth_param_ is the tac_* code that
      //                            tells type()/as_int64()/as_double()
      //                            how to interpret them.
      //   form_row_array_record    data_ points at a record body within
      //                            a row_array; parent_ points at the
      //                            row_array buffer (so find/for_each
      //                            can read the shared schema directly).

      enum form_t : std::uint8_t
      {
         form_normal              = 0,
         form_typed_array_element = 1,
         form_row_array_record    = 2,
      };

      const std::uint8_t* data_   = nullptr;
      std::size_t         size_   = 0;
      const std::uint8_t* parent_ = nullptr;  // row_array buffer (form 2 only)
      std::size_t         parent_size_ = 0;   // row_array buffer size (form 2)
      std::uint8_t        form_   = form_normal;
      std::uint8_t        synth_param_ = 0;   // form 1: tac_* element code
   };

   // Backward-compat alias. New code should prefer
   // dynamic_view<pjson_format>; pjson_view stays for ergonomics in
   // pjson-only code.
   using pjson_view = dynamic_view<pjson_format>;

}  // namespace psio
