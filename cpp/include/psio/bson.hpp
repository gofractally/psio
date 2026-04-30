#pragma once
//
// psio/bson.hpp — BSON (MongoDB Binary JSON) format tag.
//
// Wire (MongoDB BSON spec, subset):
//   document  = int32 totalSize + e_list + \x00
//   element   = u8 type + cstring fieldName + value
//
// Type codes implemented:
//   0x01  double                 — 8 LE bytes
//   0x02  utf-8 string           — int32 lenWithNull + bytes + \x00
//   0x03  embedded document      — recursive document
//   0x04  array                  — document with stringified-int keys
//   0x08  bool                   — 1 byte (0/1)
//   0x10  int32 LE
//   0x12  int64 LE
//
// Coverage matches the bench shape library: arithmetic / bool /
// std::string / std::vector<T> / std::optional<T> / Reflected records.
// Top-level encoded type must be Reflected (BSON's spec only defines
// documents at the top level).

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio {

   struct bson;

   namespace detail::bson_impl {

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename T>
      struct is_std_vector<std::vector<T>> : std::true_type {};
      template <typename T>
      inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type {};
      template <typename T>
      inline constexpr bool is_std_optional_v = is_std_optional<T>::value;

      // ── MongoDB-idiomatic vector-of-primitive encoding ──────────────
      //
      // BSON has two relevant binary subtypes for packed vectors:
      //   0x00  (generic binary)   — used by mongo for vector<u8> bytes.
      //   0x09  (Vector subtype)   — adds dtype + padding header.
      //                              Officially supports three dtypes:
      //                                0x03  INT8
      //                                0x10  FLOAT32
      //                                0x27  PACKED_BIT
      //                              See the MongoDB BSON binary-vector
      //                              spec.
      //
      // For everything else (u16/u32/i32/u64/i64/f64 elements), mongo
      // falls back to a plain BSON array — no compact subtype exists.
      // Vector<Reflected> always encodes as a real array of embedded
      // documents (deliberately not blob-encoded; preserves
      // self-description of each element).

      template <typename E>
      consteval bool is_bson_generic_binary_elem()
      {
         return std::is_same_v<E, std::uint8_t>;
      }

      template <typename E>
      consteval std::uint8_t bson_vector_dtype()
      {
         using F = std::remove_cvref_t<E>;
         if constexpr (std::is_same_v<F, std::int8_t>)
            return 0x03;       // INT8
         else if constexpr (std::is_same_v<F, float>)
            return 0x10;       // FLOAT32
         else
            return 0xFF;       // sentinel: not a Vector dtype
      }

      template <typename E>
      consteval bool is_bson_vector_elem()
      {
         return bson_vector_dtype<E>() != 0xFF;
      }

      // ── BSON type code dispatch ─────────────────────────────────────
      //
      // For `std::vector<E>` returns 0x05 (binary) when E matches a
      // mongo-idiomatic packed encoding (u8 → generic binary; i8/f32 →
      // Vector subtype); otherwise 0x04 (array) so each element gets
      // its full type-tag + cstring-key treatment.
      template <typename T>
      consteval std::uint8_t bson_type_code()
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)              return 0x08;
         else if constexpr (std::is_same_v<U, std::string>)  return 0x02;
         else if constexpr (is_std_vector_v<U>) {
            using E = typename U::value_type;
            if constexpr (is_bson_generic_binary_elem<E>()
                          || is_bson_vector_elem<E>())
               return 0x05;                  // binary subtype path
            else
               return 0x04;                  // array path
         }
         else if constexpr (is_std_optional_v<U>)
            return bson_type_code<typename U::value_type>();
         else if constexpr (std::is_floating_point_v<U>)     return 0x01;
         else if constexpr (std::is_arithmetic_v<U>)
            return sizeof(U) <= 4 ? 0x10 : 0x12;
         else if constexpr (::psio::Reflected<U>)            return 0x03;
         else                                                 return 0x00;
      }

      // ── size walker ────────────────────────────────────────────────
      template <typename T>
      std::size_t value_size(const T& v);

      template <typename T>
      std::size_t document_size(const T& v)
      {
         using R = ::psio::reflect<T>;
         std::size_t size = 4 + 1;  // int32 totalSize + 0x00 terminator
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((size += [&]() -> std::size_t {
               using F = std::remove_cvref_t<
                  typename R::template member_type<Is>>;
               const F& fref = v.*(R::template member_pointer<Is>);
               constexpr std::string_view name =
                  R::template member_name<Is>;
               if constexpr (is_std_optional_v<F>) {
                  if (!fref.has_value()) return 0;  // omit
                  return 1 + name.size() + 1 + value_size(*fref);
               } else {
                  return 1 + name.size() + 1 + value_size(fref);
               }
            }()), ...);
         }(std::make_index_sequence<R::member_count>{});
         return size;
      }

      template <typename E>
      std::size_t array_size(const std::vector<E>& v)
      {
         std::size_t size = 4 + 1;  // header + terminator
         char keybuf[24];
         for (std::size_t i = 0; i < v.size(); ++i) {
            int klen = std::snprintf(keybuf, sizeof(keybuf), "%zu", i);
            size += 1 + std::size_t(klen) + 1 + value_size(v[i]);
         }
         return size;
      }

      template <typename T>
      std::size_t value_size(const T& v)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)              return 1;
         else if constexpr (std::is_same_v<U, std::string>)  return 4 + v.size() + 1;
         else if constexpr (is_std_vector_v<U>) {
            using E = typename U::value_type;
            if constexpr (is_bson_generic_binary_elem<E>()) {
               // generic binary: int32 length + 1 subtype + N bytes
               return 4 + 1 + v.size();
            } else if constexpr (is_bson_vector_elem<E>()) {
               // Vector subtype: int32 length + 1 subtype + 1 dtype +
               //                 1 padding + N*sizeof(E) bytes
               return 4 + 1 + 1 + 1 + v.size() * sizeof(E);
            } else {
               return array_size(v);
            }
         }
         else if constexpr (is_std_optional_v<U>) {
            return v.has_value() ? value_size(*v) : 0;
         }
         else if constexpr (std::is_floating_point_v<U>)     return 8;
         else if constexpr (std::is_arithmetic_v<U>)
            return sizeof(U) <= 4 ? 4 : 8;
         else if constexpr (::psio::Reflected<U>)            return document_size(v);
         else                                                 return 0;
      }

      template <typename T>
      std::size_t packed_size_of(const T& v)
      {
         static_assert(::psio::Reflected<T>,
                       "bson: top-level type must be Reflected");
         return document_size(v);
      }

      // ── encoder ────────────────────────────────────────────────────
      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s);

      template <typename Sink>
      [[gnu::always_inline]] void
      write_u32(Sink& s, std::uint32_t v)
      {
         s.write(reinterpret_cast<const char*>(&v), 4);
      }
      template <typename Sink>
      [[gnu::always_inline]] void
      write_u64(Sink& s, std::uint64_t v)
      {
         s.write(reinterpret_cast<const char*>(&v), 8);
      }
      template <typename Sink>
      [[gnu::always_inline]] void
      write_byte(Sink& s, std::uint8_t v)
      {
         s.write(reinterpret_cast<const char*>(&v), 1);
      }
      template <typename Sink>
      [[gnu::always_inline]] void
      write_cstring(Sink& s, std::string_view sv)
      {
         if (!sv.empty()) s.write(sv.data(), sv.size());
         char z = 0;
         s.write(&z, 1);
      }

      template <typename T, typename Sink>
      void write_document(const T& v, Sink& s)
      {
         using R = ::psio::reflect<T>;
         const std::size_t total = document_size(v);
         write_u32(s, static_cast<std::uint32_t>(total));
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (([&] {
               using F = std::remove_cvref_t<
                  typename R::template member_type<Is>>;
               const F& fref = v.*(R::template member_pointer<Is>);
               constexpr std::string_view name =
                  R::template member_name<Is>;
               if constexpr (is_std_optional_v<F>) {
                  if (!fref.has_value()) return;
                  write_byte(s,
                     bson_type_code<typename F::value_type>());
                  write_cstring(s, name);
                  write_value(*fref, s);
               } else {
                  write_byte(s, bson_type_code<F>());
                  write_cstring(s, name);
                  write_value(fref, s);
               }
            }()), ...);
         }(std::make_index_sequence<R::member_count>{});
         write_byte(s, 0);
      }

      template <typename E, typename Sink>
      void write_array(const std::vector<E>& v, Sink& s)
      {
         const std::size_t total = array_size(v);
         write_u32(s, static_cast<std::uint32_t>(total));
         char keybuf[24];
         for (std::size_t i = 0; i < v.size(); ++i) {
            int klen = std::snprintf(keybuf, sizeof(keybuf), "%zu", i);
            write_byte(s, bson_type_code<E>());
            write_cstring(s, std::string_view{keybuf,
                                              static_cast<std::size_t>(klen)});
            write_value(v[i], s);
         }
         write_byte(s, 0);
      }

      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>) {
            write_byte(s, v ? 1 : 0);
         }
         else if constexpr (std::is_same_v<U, std::string>) {
            write_u32(s, static_cast<std::uint32_t>(v.size() + 1));
            if (!v.empty()) s.write(v.data(), v.size());
            write_byte(s, 0);
         }
         else if constexpr (is_std_vector_v<U>) {
            using E = typename U::value_type;
            if constexpr (is_bson_generic_binary_elem<E>()) {
               // 0x05 binary, subtype 0x00 generic.  Length = N bytes.
               write_u32(s, static_cast<std::uint32_t>(v.size()));
               write_byte(s, 0x00);
               if (!v.empty())
                  s.write(reinterpret_cast<const char*>(v.data()),
                          v.size());
            } else if constexpr (is_bson_vector_elem<E>()) {
               // 0x05 binary, subtype 0x09 Vector.
               // Payload = 1 dtype + 1 padding + N*sizeof(E) bytes.
               const std::uint32_t payload =
                  2 + static_cast<std::uint32_t>(v.size() * sizeof(E));
               write_u32(s, payload);
               write_byte(s, 0x09);                  // subtype
               write_byte(s, bson_vector_dtype<E>()); // dtype
               write_byte(s, 0x00);                  // padding (0 — no
                                                      // partial bytes
                                                      // for INT8/FLOAT32)
               if (!v.empty())
                  s.write(reinterpret_cast<const char*>(v.data()),
                          v.size() * sizeof(E));
            } else {
               write_array(v, s);
            }
         }
         else if constexpr (is_std_optional_v<U>) {
            if (v.has_value()) write_value(*v, s);
         }
         else if constexpr (std::is_floating_point_v<U>) {
            double d = static_cast<double>(v);
            s.write(reinterpret_cast<const char*>(&d), 8);
         }
         else if constexpr (std::is_arithmetic_v<U>) {
            if constexpr (sizeof(U) <= 4) {
               std::uint32_t x = static_cast<std::uint32_t>(v);
               write_u32(s, x);
            } else {
               std::uint64_t x = static_cast<std::uint64_t>(v);
               write_u64(s, x);
            }
         }
         else if constexpr (::psio::Reflected<U>) {
            write_document(v, s);
         }
      }

      // ── decoder ────────────────────────────────────────────────────
      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      inline std::uint32_t read_u32(std::span<const char> src,
                                     std::size_t pos)
      {
         std::uint32_t v;
         std::memcpy(&v, src.data() + pos, 4);
         return v;
      }

      inline std::string_view read_cstring(std::span<const char> src,
                                            std::size_t&          pos)
      {
         const char* base = src.data() + pos;
         std::size_t len  = 0;
         while (pos + len < src.size() && base[len] != 0) ++len;
         std::string_view r{base, len};
         pos += len + 1;
         return r;
      }

      // Skip a value of unknown type — used when decoder hits a field
      // not in the receiver schema (forward compatibility).
      inline void skip_value(std::uint8_t code,
                              std::span<const char> src, std::size_t& pos)
      {
         switch (code) {
            case 0x01: pos += 8; break;                       // double
            case 0x02: {
               std::uint32_t len = read_u32(src, pos);
               pos += 4 + len;
               break;
            }
            case 0x03:                                        // doc
            case 0x04: {                                      // array
               std::uint32_t total = read_u32(src, pos);
               pos += total;
               break;
            }
            case 0x05: {                                      // binary
               std::uint32_t len = read_u32(src, pos);
               pos += 4 + 1 /*subtype*/ + len;
               break;
            }
            case 0x08: pos += 1; break;                       // bool
            case 0x10: pos += 4; break;                       // int32
            case 0x12: pos += 8; break;                       // int64
            default: break;
         }
      }

      template <typename T>
      T decode_document(std::span<const char> src, std::size_t& pos)
      {
         using R = ::psio::reflect<T>;
         T            out{};
         const std::size_t doc_start = pos;
         const std::uint32_t total   = read_u32(src, pos);
         pos += 4;
         while (pos < doc_start + total) {
            const std::uint8_t code =
               static_cast<std::uint8_t>(src[pos]);
            ++pos;
            if (code == 0) break;  // terminator
            std::string_view name = read_cstring(src, pos);
            bool found = false;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                  if (found) return;
                  constexpr std::string_view fname =
                     R::template member_name<Is>;
                  if (fname == name) {
                     using F = std::remove_cvref_t<
                        typename R::template member_type<Is>>;
                     auto& slot =
                        out.*(R::template member_pointer<Is>);
                     if constexpr (is_std_optional_v<F>) {
                        slot =
                           decode_value<typename F::value_type>(
                              src, pos);
                     } else {
                        slot = decode_value<F>(src, pos);
                     }
                     found = true;
                  }
               }()), ...);
            }(std::make_index_sequence<R::member_count>{});
            if (!found) skip_value(code, src, pos);
         }
         return out;
      }

      template <typename E>
      std::vector<E> decode_array(std::span<const char> src,
                                  std::size_t&          pos)
      {
         const std::size_t arr_start = pos;
         const std::uint32_t total   = read_u32(src, pos);
         pos += 4;
         std::vector<E> v;
         while (pos < arr_start + total) {
            const std::uint8_t code =
               static_cast<std::uint8_t>(src[pos]);
            ++pos;
            if (code == 0) break;
            (void)read_cstring(src, pos);  // ignore key for array
            v.push_back(decode_value<E>(src, pos));
         }
         return v;
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>) {
            bool b = src[pos] != 0;
            ++pos;
            return b;
         }
         else if constexpr (std::is_same_v<U, std::string>) {
            std::uint32_t len = read_u32(src, pos);
            pos += 4;
            std::string s(src.data() + pos, len - 1);
            pos += len;
            return s;
         }
         else if constexpr (is_std_vector_v<U>) {
            using E = typename U::value_type;
            if constexpr (is_bson_generic_binary_elem<E>()) {
               // 0x05 binary, subtype 0x00.  int32 length + 1 subtype +
               // length bytes.
               std::uint32_t len = read_u32(src, pos);
               pos += 4;
               // skip subtype byte
               ++pos;
               std::vector<std::uint8_t> v(len);
               if (len) std::memcpy(v.data(), src.data() + pos, len);
               pos += len;
               return v;
            } else if constexpr (is_bson_vector_elem<E>()) {
               // 0x05 binary, subtype 0x09 Vector.  int32 length + 1
               // subtype + 1 dtype + 1 padding + length-2 bytes data.
               std::uint32_t len = read_u32(src, pos);
               pos += 4;
               ++pos;  // subtype 0x09
               ++pos;  // dtype
               ++pos;  // padding
               const std::size_t data_bytes = len - 2;
               const std::size_t count = data_bytes / sizeof(E);
               std::vector<E> v(count);
               if (count)
                  std::memcpy(v.data(), src.data() + pos, data_bytes);
               pos += data_bytes;
               return v;
            } else {
               return decode_array<E>(src, pos);
            }
         }
         else if constexpr (std::is_floating_point_v<U>) {
            double d;
            std::memcpy(&d, src.data() + pos, 8);
            pos += 8;
            return static_cast<U>(d);
         }
         else if constexpr (std::is_arithmetic_v<U>) {
            if constexpr (sizeof(U) <= 4) {
               std::uint32_t x;
               std::memcpy(&x, src.data() + pos, 4);
               pos += 4;
               return static_cast<U>(x);
            } else {
               std::uint64_t x;
               std::memcpy(&x, src.data() + pos, 8);
               pos += 8;
               return static_cast<U>(x);
            }
         }
         else if constexpr (::psio::Reflected<U>) {
            return decode_document<U>(src, pos);
         }
         else {
            return U{};
         }
      }

   }  // namespace detail::bson_impl

   struct bson : format_tag_base<bson>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
         requires ::psio::Reflected<T>
      friend std::vector<char>
      tag_invoke(decltype(::psio::encode), bson, const T& v)
      {
         const std::size_t       n = detail::bson_impl::packed_size_of(v);
         std::vector<char>       out(n);
         ::psio::fast_buf_stream fbs{out.data(), out.size()};
         detail::bson_impl::write_value(v, fbs);
         return out;
      }

      template <typename T>
         requires ::psio::Reflected<T>
      friend void
      tag_invoke(decltype(::psio::encode), bson, const T& v,
                 std::vector<char>& sink)
      {
         const std::size_t n     = detail::bson_impl::packed_size_of(v);
         const std::size_t orig  = sink.size();
         sink.resize(orig + n);
         ::psio::fast_buf_stream fbs{sink.data() + orig, n};
         detail::bson_impl::write_value(v, fbs);
      }

      template <typename T>
         requires ::psio::Reflected<T>
      friend T tag_invoke(decltype(::psio::decode<T>), bson, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::bson_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
         requires ::psio::Reflected<T>
      friend std::size_t
      tag_invoke(decltype(::psio::size_of), bson, const T& v)
      {
         return detail::bson_impl::packed_size_of(v);
      }

      template <typename T>
         requires ::psio::Reflected<T>
      friend codec_status
      tag_invoke(decltype(::psio::validate<T>), bson, T*,
                 std::span<const char> bytes) noexcept
      {
         if (bytes.size() < 5)
            return codec_fail("bson: too short", 0, "bson");
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "bson");
             !st.ok())
            return st;
         std::uint32_t total;
         std::memcpy(&total, bytes.data(), 4);
         if (total != bytes.size())
            return codec_fail("bson: total mismatch", 0, "bson");
         if (bytes.back() != 0)
            return codec_fail("bson: missing terminator", 0, "bson");
         return codec_ok();
      }

      template <typename T>
         requires ::psio::Reflected<T>
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio::make_boxed<T>), bson, T*,
                 std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::bson_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio
