#pragma once
//
// psio/msgpack.hpp — Schema-driven MsgPack codec.
//
// MsgPack is a self-describing binary format (every value carries a
// 1-byte type tag, sometimes followed by a length prefix).  The wire
// is schema-less by design — any byte stream decodes to a tagged
// value tree without a receiver-side schema.
//
// psio's codec is *schema-driven*: at every position we know the C++
// type to expect, so we never build the tagged-value-tree intermediate
// and never branch through a tag-dispatch table.  Encode pre-sizes
// the buffer via a size_of pass and writes once; decode verifies the
// tag matches the expected shape and extracts directly.  Output
// stays valid MsgPack — generic decoders read it normally — but
// internally the typed-encode / typed-decode paths run several
// times faster than reflective libraries.
//
// Type mapping (default — record-as-array):
//
//   bool                            → 0xc2 / 0xc3
//   {u,s}int{8,16,32,64}            → smallest msgpack int form that
//                                     fits the value at encode time;
//                                     any wider-or-equal form
//                                     accepted on decode
//   float / double                  → 0xca f32-be / 0xcb f64-be
//   std::string / std::string_view  → fixstr / str{8,16,32}
//   std::vector<T>                  → fixarray / array{16,32}
//   std::array<T, N>                → fixarray / array{16,32}
//   std::optional<T>                → nil OR T
//   PSIO_REFLECT'd struct           → array of fields in declaration
//                                     order  (positional record;
//                                     map-of-string-keys mode lands
//                                     under an annotation later)
//   std::variant<Ts…>               → 2-element array
//                                     [discriminator-int, value]
//
// Endianness: MsgPack lengths and multi-byte numerics are big-endian.
// The codec byteswaps on little-endian platforms.
//
// Out of scope for the first cut (extension points already left in
// the wire dispatch — add when the carrier types land):
//   - bin vs str distinction (currently always str for std::string;
//     a `Custom{vector<u8>, "hex"}` annotation will route to bin)
//   - ext types and the standard timestamp ext
//   - record-as-map encoding (annotation-driven)
//   - canonicalised float / NaN handling

#include <psio/adapter.hpp>
#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>

namespace psio
{
   //  Per-type opt-in for record-as-fixmap encoding.  Default is
   //  fixarray (positional, schema-required, fastest, smallest); set
   //  ::as_map = true on a specialisation to switch a type to fixmap
   //  (string-keyed, self-describing, interop-friendly with generic
   //  msgpack libraries).
   //
   //      template <>
   //      struct psio::msgpack_record_form<MyType> {
   //         static constexpr bool as_map = true;
   //      };
   //
   //  The trait is consulted recursively: each PSIO_REFLECT'd struct
   //  hit during walk picks its own form.  Decode accepts either form
   //  regardless of the type's trait so a producer that flipped to
   //  fixmap doesn't strand consumers built against the previous wire.
   template <typename T>
   struct msgpack_record_form
   {
      static constexpr bool as_map = false;
   };
}  // namespace psio

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio
{

   namespace detail::msgpack_impl
   {
      // ── Wire-format constants ─────────────────────────────────────
      namespace tag
      {
         constexpr std::uint8_t nil       = 0xc0;
         constexpr std::uint8_t false_    = 0xc2;
         constexpr std::uint8_t true_     = 0xc3;
         constexpr std::uint8_t u8        = 0xcc;
         constexpr std::uint8_t u16       = 0xcd;
         constexpr std::uint8_t u32       = 0xce;
         constexpr std::uint8_t u64       = 0xcf;
         constexpr std::uint8_t i8        = 0xd0;
         constexpr std::uint8_t i16       = 0xd1;
         constexpr std::uint8_t i32       = 0xd2;
         constexpr std::uint8_t i64       = 0xd3;
         constexpr std::uint8_t f32       = 0xca;
         constexpr std::uint8_t f64       = 0xcb;
         constexpr std::uint8_t str8      = 0xd9;
         constexpr std::uint8_t str16     = 0xda;
         constexpr std::uint8_t str32     = 0xdb;
         constexpr std::uint8_t bin8      = 0xc4;
         constexpr std::uint8_t bin16     = 0xc5;
         constexpr std::uint8_t bin32     = 0xc6;
         constexpr std::uint8_t array16   = 0xdc;
         constexpr std::uint8_t array32   = 0xdd;
         constexpr std::uint8_t map16     = 0xde;
         constexpr std::uint8_t map32     = 0xdf;
         // Ranges (low bits carry payload):
         //   pos fixint   0x00..0x7f   value 0..127
         //   neg fixint   0xe0..0xff   value -32..-1
         //   fixstr       0xa0..0xbf   length 0..31
         //   fixarray     0x90..0x9f   count 0..15
         //   fixmap       0x80..0x8f   count 0..15
      }

      // ── Endian helpers — MsgPack is big-endian on the wire ────────
      template <typename T>
         requires std::is_trivially_copyable_v<T>
      inline T byteswap_into(T v) noexcept
      {
         if constexpr (sizeof(T) == 1)
            return v;
         else if constexpr (std::endian::native == std::endian::big)
            return v;
         else
         {
            unsigned char buf[sizeof(T)];
            std::memcpy(buf, &v, sizeof(T));
            for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
               std::swap(buf[i], buf[sizeof(T) - 1 - i]);
            T out;
            std::memcpy(&out, buf, sizeof(T));
            return out;
         }
      }

      // ── size_of pass ──────────────────────────────────────────────
      template <typename T>
      std::size_t packed_size_of(const T& v) noexcept;

      template <typename T>
      std::size_t packed_size_of_int(T v) noexcept
      {
         using U = std::make_unsigned_t<T>;
         if constexpr (std::is_signed_v<T>)
         {
            if (v >= 0 && v <= 127)
               return 1;
            if (v >= -32 && v < 0)
               return 1;
            if (v >= std::numeric_limits<std::int8_t>::min() &&
                v <= std::numeric_limits<std::int8_t>::max())
               return 2;
            if (v >= std::numeric_limits<std::int16_t>::min() &&
                v <= std::numeric_limits<std::int16_t>::max())
               return 3;
            if (v >= std::numeric_limits<std::int32_t>::min() &&
                v <= std::numeric_limits<std::int32_t>::max())
               return 5;
            return 9;
         }
         else
         {
            U u = static_cast<U>(v);
            if (u <= 127)
               return 1;
            if (u <= std::numeric_limits<std::uint8_t>::max())
               return 2;
            if (u <= std::numeric_limits<std::uint16_t>::max())
               return 3;
            if (u <= std::numeric_limits<std::uint32_t>::max())
               return 5;
            return 9;
         }
      }

      inline std::size_t packed_size_of_str(std::size_t n) noexcept
      {
         if (n <= 31)
            return 1 + n;
         if (n <= 0xff)
            return 2 + n;
         if (n <= 0xffff)
            return 3 + n;
         return 5 + n;
      }

      inline std::size_t packed_size_of_array(std::size_t n) noexcept
      {
         if (n <= 15)
            return 1;
         if (n <= 0xffff)
            return 3;
         return 5;
      }

      // bin8/16/32 length-prefix size (no fixbin — bin always carries
      // at least a 1-byte length, even for tiny payloads).
      inline std::size_t packed_size_of_bin(std::size_t n) noexcept
      {
         if (n <= 0xff)
            return 2;
         if (n <= 0xffff)
            return 3;
         return 5;
      }

      // Route raw-byte vectors to bin: vector<uint8_t> and
      // vector<std::byte>.  vector<int8_t> stays as array because its
      // semantics are "list of small signed ints", not "blob".
      template <typename T>
      constexpr bool is_byte_vector_v =
         std::is_same_v<T, std::vector<std::uint8_t>> ||
         std::is_same_v<T, std::vector<std::byte>>;

      // Reflect-based record size: msgpack-array of fields in
      // declaration order.
      template <typename T>
      std::size_t packed_size_of_record(const T& v) noexcept;

      // std::vector / std::array detectors
      template <typename T>
      struct mp_is_vector : std::false_type {};
      template <typename E, typename A>
      struct mp_is_vector<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename T>
      struct mp_is_array : std::false_type {};
      template <typename E, std::size_t N>
      struct mp_is_array<std::array<E, N>> : std::true_type
      {
         using elem                       = E;
         static constexpr std::size_t len = N;
      };

      template <typename T>
      struct mp_is_optional : std::false_type {};
      template <typename T>
      struct mp_is_optional<std::optional<T>> : std::true_type
      {
         using elem = T;
      };

      template <typename T>
      struct mp_is_variant : std::false_type {};
      template <typename... Ts>
      struct mp_is_variant<std::variant<Ts...>> : std::true_type {};

      template <typename T>
      std::size_t packed_size_of(const T& v) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
            return 1;
         else if constexpr (std::is_integral_v<U>)
            return packed_size_of_int(v);
         else if constexpr (std::is_same_v<U, float>)
            return 5;
         else if constexpr (std::is_same_v<U, double>)
            return 9;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return packed_size_of_str(v.size());
         else if constexpr (mp_is_optional<U>::value)
         {
            if (!v.has_value())
               return 1;  // nil
            return packed_size_of(*v);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            return packed_size_of_bin(v.size()) + v.size();
         }
         else if constexpr (mp_is_vector<U>::value ||
                            mp_is_array<U>::value)
         {
            std::size_t total = packed_size_of_array(v.size());
            for (const auto& x : v)
               total += packed_size_of(x);
            return total;
         }
         else if constexpr (mp_is_variant<U>::value)
         {
            // [discriminator, value] = fixarray of 2
            std::size_t total = 1;
            // discriminator: positive fixint up to 127 covers all
            // reasonable variant arities; assume it fits.
            total += 1;
            std::visit([&](const auto& alt) { total += packed_size_of(alt); },
                       v);
            return total;
         }
         else if constexpr (Reflected<U>)
         {
            return packed_size_of_record(v);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::msgpack: unsupported type in size_of");
         }
      }

      // Map-header size: fixmap (≤15) is 1 byte; map16 is 3; map32 is 5.
      inline std::size_t packed_size_of_map(std::size_t n) noexcept
      {
         if (n <= 15)
            return 1;
         if (n <= 0xffff)
            return 3;
         return 5;
      }

      template <typename T>
      std::size_t packed_size_of_record(const T& v) noexcept
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         if constexpr (::psio::msgpack_record_form<T>::as_map)
         {
            //  fixmap: header + sum_i (fixstr-of-name) + (value-size).
            std::size_t total = packed_size_of_map(N);
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((total +=
                    packed_size_of_str(
                       std::string_view{R::template member_name<Is>}.size()) +
                    packed_size_of(v.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});
            return total;
         }
         else
         {
            //  fixarray: header + sum_i (value-size).
            std::size_t total = packed_size_of_array(N);
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((total +=
                    packed_size_of(v.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});
            return total;
         }
      }

      // ── encode pass ───────────────────────────────────────────────

      template <typename Sink>
      inline void put_byte(Sink& s, std::uint8_t b)
      {
         s.write(&b, 1);
      }

      template <typename T, typename Sink>
         requires std::is_trivially_copyable_v<T>
      inline void put_be(Sink& s, T v)
      {
         T be = byteswap_into(v);
         s.write(&be, sizeof(T));
      }

      template <typename Sink>
      void emit_uint(std::uint64_t v, Sink& s)
      {
         if (v <= 127)
         {
            put_byte(s, static_cast<std::uint8_t>(v));
            return;
         }
         if (v <= 0xff)
         {
            put_byte(s, tag::u8);
            put_byte(s, static_cast<std::uint8_t>(v));
            return;
         }
         if (v <= 0xffff)
         {
            put_byte(s, tag::u16);
            put_be<std::uint16_t>(s, static_cast<std::uint16_t>(v));
            return;
         }
         if (v <= 0xffffffffu)
         {
            put_byte(s, tag::u32);
            put_be<std::uint32_t>(s, static_cast<std::uint32_t>(v));
            return;
         }
         put_byte(s, tag::u64);
         put_be<std::uint64_t>(s, v);
      }

      template <typename Sink>
      void emit_int(std::int64_t v, Sink& s)
      {
         if (v >= 0)
         {
            emit_uint(static_cast<std::uint64_t>(v), s);
            return;
         }
         if (v >= -32)
         {
            put_byte(s, static_cast<std::uint8_t>(0xe0 | (v & 0x1f)));
            return;
         }
         if (v >= std::numeric_limits<std::int8_t>::min())
         {
            put_byte(s, tag::i8);
            put_byte(s, static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
            return;
         }
         if (v >= std::numeric_limits<std::int16_t>::min())
         {
            put_byte(s, tag::i16);
            put_be<std::int16_t>(s, static_cast<std::int16_t>(v));
            return;
         }
         if (v >= std::numeric_limits<std::int32_t>::min())
         {
            put_byte(s, tag::i32);
            put_be<std::int32_t>(s, static_cast<std::int32_t>(v));
            return;
         }
         put_byte(s, tag::i64);
         put_be<std::int64_t>(s, v);
      }

      template <typename Sink>
      void emit_str_header(std::size_t n, Sink& s)
      {
         if (n <= 31)
            put_byte(s, static_cast<std::uint8_t>(0xa0 | n));
         else if (n <= 0xff)
         {
            put_byte(s, tag::str8);
            put_byte(s, static_cast<std::uint8_t>(n));
         }
         else if (n <= 0xffff)
         {
            put_byte(s, tag::str16);
            put_be<std::uint16_t>(s, static_cast<std::uint16_t>(n));
         }
         else
         {
            put_byte(s, tag::str32);
            put_be<std::uint32_t>(s, static_cast<std::uint32_t>(n));
         }
      }

      template <typename Sink>
      void emit_bin_header(std::size_t n, Sink& s)
      {
         if (n <= 0xff)
         {
            put_byte(s, tag::bin8);
            put_byte(s, static_cast<std::uint8_t>(n));
         }
         else if (n <= 0xffff)
         {
            put_byte(s, tag::bin16);
            put_be<std::uint16_t>(s, static_cast<std::uint16_t>(n));
         }
         else
         {
            put_byte(s, tag::bin32);
            put_be<std::uint32_t>(s, static_cast<std::uint32_t>(n));
         }
      }

      template <typename Sink>
      void emit_array_header(std::size_t n, Sink& s)
      {
         if (n <= 15)
            put_byte(s, static_cast<std::uint8_t>(0x90 | n));
         else if (n <= 0xffff)
         {
            put_byte(s, tag::array16);
            put_be<std::uint16_t>(s, static_cast<std::uint16_t>(n));
         }
         else
         {
            put_byte(s, tag::array32);
            put_be<std::uint32_t>(s, static_cast<std::uint32_t>(n));
         }
      }

      template <typename Sink>
      void emit_map_header(std::size_t n, Sink& s)
      {
         if (n <= 15)
            put_byte(s, static_cast<std::uint8_t>(0x80 | n));
         else if (n <= 0xffff)
         {
            put_byte(s, tag::map16);
            put_be<std::uint16_t>(s, static_cast<std::uint16_t>(n));
         }
         else
         {
            put_byte(s, tag::map32);
            put_be<std::uint32_t>(s, static_cast<std::uint32_t>(n));
         }
      }

      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s);

      template <typename T, typename Sink>
      void write_record(const T& v, Sink& s)
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         if constexpr (::psio::msgpack_record_form<T>::as_map)
         {
            emit_map_header(N, s);
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((
                  [&]
                  {
                     constexpr auto name = R::template member_name<Is>;
                     std::string_view sv{name};
                     emit_str_header(sv.size(), s);
                     if (!sv.empty())
                        s.write(sv.data(), sv.size());
                     write_value(v.*(R::template member_pointer<Is>), s);
                  }()),
                ...);
            }(std::make_index_sequence<N>{});
         }
         else
         {
            emit_array_header(N, s);
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((write_value(v.*(R::template member_pointer<Is>), s)), ...);
            }(std::make_index_sequence<N>{});
         }
      }

      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            put_byte(s, v ? tag::true_ : tag::false_);
         }
         else if constexpr (std::is_signed_v<U> && std::is_integral_v<U>)
         {
            emit_int(static_cast<std::int64_t>(v), s);
         }
         else if constexpr (std::is_unsigned_v<U> && std::is_integral_v<U>)
         {
            emit_uint(static_cast<std::uint64_t>(v), s);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            put_byte(s, tag::f32);
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            put_be<std::uint32_t>(s, bits);
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            put_byte(s, tag::f64);
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            put_be<std::uint64_t>(s, bits);
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            emit_str_header(v.size(), s);
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (mp_is_optional<U>::value)
         {
            if (!v.has_value())
               put_byte(s, tag::nil);
            else
               write_value(*v, s);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            //  Single memcpy of the whole payload — the win over the
            //  array-of-fixints path is biggest for arbitrary bytes,
            //  any of which ≥ 0x80 would otherwise inflate to a
            //  2-byte uint8 form.
            emit_bin_header(v.size(), s);
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (mp_is_vector<U>::value || mp_is_array<U>::value)
         {
            emit_array_header(v.size(), s);
            for (const auto& x : v)
               write_value(x, s);
         }
         else if constexpr (mp_is_variant<U>::value)
         {
            emit_array_header(2, s);
            emit_uint(static_cast<std::uint64_t>(v.index()), s);
            std::visit([&](const auto& alt) { write_value(alt, s); }, v);
         }
         else if constexpr (Reflected<U>)
         {
            write_record(v, s);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::msgpack: unsupported type in write_value");
         }
      }

      // ── decode pass ───────────────────────────────────────────────

      struct DecodeError : std::runtime_error
      {
         std::size_t pos;
         DecodeError(const std::string& m, std::size_t p)
            : std::runtime_error(m), pos{p}
         {
         }
      };

      [[noreturn]] inline void fail(const std::string& msg, std::size_t pos)
      {
         throw DecodeError(msg, pos);
      }

      inline std::uint8_t read_u8(std::span<const char> s, std::size_t& pos)
      {
         if (pos >= s.size())
            fail("msgpack: unexpected end of input", pos);
         return static_cast<std::uint8_t>(s[pos++]);
      }

      template <typename T>
         requires std::is_trivially_copyable_v<T>
      inline T read_be(std::span<const char> s, std::size_t& pos)
      {
         if (pos + sizeof(T) > s.size())
            fail("msgpack: unexpected end of input", pos);
         T v;
         std::memcpy(&v, s.data() + pos, sizeof(T));
         pos += sizeof(T);
         return byteswap_into(v);
      }

      // Decode a msgpack integer (any width) into a target signed or
      // unsigned C++ integer T, with range-check.
      template <typename T>
      T decode_int(std::span<const char> s, std::size_t& pos)
      {
         std::uint8_t t = read_u8(s, pos);
         std::int64_t signed_val   = 0;
         std::uint64_t unsigned_val = 0;
         bool          have_signed  = false;
         if (t <= 0x7f)
         {
            unsigned_val = t;
         }
         else if (t >= 0xe0)
         {
            // negative fixint: sign-extend the low 5 bits via an i8 cast.
            signed_val  = static_cast<std::int8_t>(t);
            have_signed = true;
         }
         else
         {
            switch (t)
            {
               case tag::u8:  unsigned_val = read_u8(s, pos); break;
               case tag::u16: unsigned_val = read_be<std::uint16_t>(s, pos); break;
               case tag::u32: unsigned_val = read_be<std::uint32_t>(s, pos); break;
               case tag::u64: unsigned_val = read_be<std::uint64_t>(s, pos); break;
               case tag::i8:
                  signed_val  = static_cast<std::int8_t>(read_u8(s, pos));
                  have_signed = true;
                  break;
               case tag::i16:
                  signed_val  = read_be<std::int16_t>(s, pos);
                  have_signed = true;
                  break;
               case tag::i32:
                  signed_val  = read_be<std::int32_t>(s, pos);
                  have_signed = true;
                  break;
               case tag::i64:
                  signed_val  = read_be<std::int64_t>(s, pos);
                  have_signed = true;
                  break;
               default:
                  fail("msgpack: expected int, got tag 0x" +
                          std::to_string(t),
                       pos - 1);
            }
         }
         if constexpr (std::is_signed_v<T>)
         {
            std::int64_t v = have_signed
                                ? signed_val
                                : static_cast<std::int64_t>(unsigned_val);
            if (v < std::numeric_limits<T>::min() ||
                v > std::numeric_limits<T>::max())
               fail("msgpack: int out of target range", pos);
            return static_cast<T>(v);
         }
         else
         {
            if (have_signed && signed_val < 0)
               fail("msgpack: negative int into unsigned target", pos);
            std::uint64_t u = have_signed
                                 ? static_cast<std::uint64_t>(signed_val)
                                 : unsigned_val;
            if (u > std::numeric_limits<T>::max())
               fail("msgpack: uint out of target range", pos);
            return static_cast<T>(u);
         }
      }

      inline std::size_t decode_str_header(std::span<const char> s,
                                           std::size_t&          pos)
      {
         std::uint8_t t = read_u8(s, pos);
         if ((t & 0xe0) == 0xa0)
            return t & 0x1f;
         if (t == tag::str8)
            return read_u8(s, pos);
         if (t == tag::str16)
            return read_be<std::uint16_t>(s, pos);
         if (t == tag::str32)
            return read_be<std::uint32_t>(s, pos);
         fail("msgpack: expected str, got tag 0x" + std::to_string(t),
              pos - 1);
      }

      inline std::size_t decode_array_header(std::span<const char> s,
                                             std::size_t&          pos)
      {
         std::uint8_t t = read_u8(s, pos);
         if ((t & 0xf0) == 0x90)
            return t & 0x0f;
         if (t == tag::array16)
            return read_be<std::uint16_t>(s, pos);
         if (t == tag::array32)
            return read_be<std::uint32_t>(s, pos);
         fail("msgpack: expected array, got tag 0x" + std::to_string(t),
              pos - 1);
      }

      // Decode a bin{8,16,32} header.  Returns the payload byte count;
      // the caller reads `count` raw bytes starting at `pos`.
      inline std::size_t decode_bin_header(std::span<const char> s,
                                           std::size_t&          pos)
      {
         std::uint8_t t = read_u8(s, pos);
         if (t == tag::bin8)
            return read_u8(s, pos);
         if (t == tag::bin16)
            return read_be<std::uint16_t>(s, pos);
         if (t == tag::bin32)
            return read_be<std::uint32_t>(s, pos);
         fail("msgpack: expected bin, got tag 0x" + std::to_string(t),
              pos - 1);
      }

      template <typename T>
      T decode_value(std::span<const char> s, std::size_t& pos);

      template <typename V, std::size_t I = 0>
      V decode_variant_alt(std::size_t           idx,
                           std::span<const char> s,
                           std::size_t&          pos);

      //  Map header reader — fixmap (0x80..0x8f), map16 (0xde),
      //  map32 (0xdf).  Returns the pair count.
      inline std::size_t decode_map_header(std::span<const char> s,
                                           std::size_t&          pos)
      {
         std::uint8_t t = read_u8(s, pos);
         if ((t & 0xf0) == 0x80)
            return t & 0x0f;
         if (t == tag::map16)
            return read_be<std::uint16_t>(s, pos);
         if (t == tag::map32)
            return read_be<std::uint32_t>(s, pos);
         fail("msgpack: expected map, got tag 0x" + std::to_string(t),
              pos - 1);
      }

      //  Skip one msgpack value at `pos`.  Used by the map decoder
      //  when an unknown key shows up so the rest of the stream stays
      //  aligned.
      inline void skip_value(std::span<const char> s, std::size_t& pos);

      template <typename T>
      T decode_record(std::span<const char> s, std::size_t& pos)
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         //  Branch on the actual wire tag, not the type's own
         //  msgpack_record_form trait — that lets a producer that
         //  flipped to fixmap not strand consumers built against
         //  the previous wire (and vice versa).
         std::uint8_t t = pos < s.size()
                             ? static_cast<std::uint8_t>(s[pos])
                             : 0;
         bool is_map = (t & 0xf0) == 0x80 || t == tag::map16 ||
                       t == tag::map32;
         if (is_map)
         {
            std::size_t n = decode_map_header(s, pos);
            T out{};
            //  Fast path: keys appear in declaration order — the
            //  common case for typed encoders.  We try a paired-walk
            //  through the key+value pairs and the reflected fields;
            //  if a key mismatch shows up we fall back to a per-pair
            //  string-match scan.
            bool ordered = true;
            //  Track which fields we've decoded so we can default-
            //  initialise the rest.
            bool seen[N == 0 ? 1 : N] = {};
            std::size_t walked = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((
                  [&]
                  {
                     if (!ordered || walked >= n)
                        return;
                     std::size_t key_len = decode_str_header(s, pos);
                     std::string_view key{s.data() + pos, key_len};
                     pos += key_len;
                     std::string_view want{R::template member_name<Is>};
                     if (key == want)
                     {
                        out.*(R::template member_pointer<Is>) =
                           decode_value<std::remove_cvref_t<
                              typename R::template member_type<Is>>>(s, pos);
                        seen[Is] = true;
                        ++walked;
                     }
                     else
                     {
                        //  Pos already advanced past the key; restore by
                        //  rewinding and dropping into the fallback
                        //  scan that handles arbitrary key order.
                        pos -= key_len;
                        //  Unread the str header too.  fixstr is 1
                        //  byte; str8/16/32 are 2/3/5.  Easier: just
                        //  bail out of the ordered walk and let the
                        //  fallback scan from `walked` re-read the
                        //  remaining (n - walked) pairs.
                        if (key_len <= 31)
                           --pos;
                        else if (key_len <= 0xff)
                           pos -= 2;
                        else if (key_len <= 0xffff)
                           pos -= 3;
                        else
                           pos -= 5;
                        ordered = false;
                     }
                  }()),
                ...);
            }(std::make_index_sequence<N>{});
            //  Fallback scan: O(remaining-pairs × N) string matches.
            //  Defaults stay for fields not present in the wire.
            while (walked < n)
            {
               std::size_t key_len = decode_str_header(s, pos);
               std::string_view key{s.data() + pos, key_len};
               pos += key_len;
               bool matched = false;
               [&]<std::size_t... Is>(std::index_sequence<Is...>)
               {
                  ((
                     [&]
                     {
                        if (matched || seen[Is])
                           return;
                        std::string_view want{R::template member_name<Is>};
                        if (key == want)
                        {
                           out.*(R::template member_pointer<Is>) =
                              decode_value<std::remove_cvref_t<
                                 typename R::template member_type<Is>>>(s,
                                                                        pos);
                           seen[Is] = true;
                           matched = true;
                        }
                     }()),
                   ...);
               }(std::make_index_sequence<N>{});
               if (!matched)
                  skip_value(s, pos);
               ++walked;
            }
            return out;
         }
         //  fixarray form — positional, the production fast path.
         std::size_t n = decode_array_header(s, pos);
         if (n != N)
            fail("msgpack: record array arity mismatch", pos);
         T out{};
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((out.*(R::template member_pointer<Is>) = decode_value<
                 std::remove_cvref_t<typename R::template member_type<Is>>>(
                 s, pos)),
             ...);
         }(std::make_index_sequence<N>{});
         return out;
      }

      //  skip_value implementation — read one value of any type and
      //  advance `pos` past it.  Used by the map decoder's unknown-key
      //  branch.  Doesn't validate beyond what's required to step
      //  cleanly to the next position.
      inline void skip_value(std::span<const char> s, std::size_t& pos)
      {
         std::uint8_t t = read_u8(s, pos);
         if (t <= 0x7f) return;                       // pos fixint
         if (t >= 0xe0) return;                       // neg fixint
         if ((t & 0xe0) == 0xa0)                      // fixstr
         {
            pos += t & 0x1f;
            return;
         }
         if ((t & 0xf0) == 0x90)                      // fixarray
         {
            std::size_t n = t & 0x0f;
            for (std::size_t i = 0; i < n; ++i)
               skip_value(s, pos);
            return;
         }
         if ((t & 0xf0) == 0x80)                      // fixmap
         {
            std::size_t n = t & 0x0f;
            for (std::size_t i = 0; i < n; ++i)
            {
               skip_value(s, pos);
               skip_value(s, pos);
            }
            return;
         }
         switch (t)
         {
            case tag::nil:
            case tag::false_:
            case tag::true_:    return;
            case tag::u8:
            case tag::i8:       pos += 1; return;
            case tag::u16:
            case tag::i16:      pos += 2; return;
            case tag::u32:
            case tag::i32:
            case tag::f32:      pos += 4; return;
            case tag::u64:
            case tag::i64:
            case tag::f64:      pos += 8; return;
            case tag::str8:
            case tag::bin8:
            {
               std::size_t n = read_u8(s, pos);
               pos += n;
               return;
            }
            case tag::str16:
            case tag::bin16:
            {
               std::size_t n = read_be<std::uint16_t>(s, pos);
               pos += n;
               return;
            }
            case tag::str32:
            case tag::bin32:
            {
               std::size_t n = read_be<std::uint32_t>(s, pos);
               pos += n;
               return;
            }
            case tag::array16:
            {
               std::size_t n = read_be<std::uint16_t>(s, pos);
               for (std::size_t i = 0; i < n; ++i) skip_value(s, pos);
               return;
            }
            case tag::array32:
            {
               std::size_t n = read_be<std::uint32_t>(s, pos);
               for (std::size_t i = 0; i < n; ++i) skip_value(s, pos);
               return;
            }
            case tag::map16:
            {
               std::size_t n = read_be<std::uint16_t>(s, pos);
               for (std::size_t i = 0; i < n; ++i)
               {
                  skip_value(s, pos);
                  skip_value(s, pos);
               }
               return;
            }
            case tag::map32:
            {
               std::size_t n = read_be<std::uint32_t>(s, pos);
               for (std::size_t i = 0; i < n; ++i)
               {
                  skip_value(s, pos);
                  skip_value(s, pos);
               }
               return;
            }
            default:
               fail("msgpack: skip_value: unknown tag 0x" +
                       std::to_string(t),
                    pos - 1);
         }
      }

      template <typename T>
      T decode_value(std::span<const char> s, std::size_t& pos)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            std::uint8_t t = read_u8(s, pos);
            if (t == tag::true_)
               return true;
            if (t == tag::false_)
               return false;
            fail("msgpack: expected bool", pos - 1);
         }
         else if constexpr (std::is_integral_v<U>)
         {
            return decode_int<U>(s, pos);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            std::uint8_t t = read_u8(s, pos);
            if (t == tag::f32)
            {
               auto bits = read_be<std::uint32_t>(s, pos);
               float f;
               std::memcpy(&f, &bits, sizeof(f));
               return f;
            }
            if (t == tag::f64)
            {
               auto   bits = read_be<std::uint64_t>(s, pos);
               double d;
               std::memcpy(&d, &bits, sizeof(d));
               return static_cast<float>(d);
            }
            fail("msgpack: expected float", pos - 1);
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            std::uint8_t t = read_u8(s, pos);
            if (t == tag::f64)
            {
               auto   bits = read_be<std::uint64_t>(s, pos);
               double d;
               std::memcpy(&d, &bits, sizeof(d));
               return d;
            }
            if (t == tag::f32)
            {
               auto  bits = read_be<std::uint32_t>(s, pos);
               float f;
               std::memcpy(&f, &bits, sizeof(f));
               return static_cast<double>(f);
            }
            fail("msgpack: expected float", pos - 1);
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            std::size_t n = decode_str_header(s, pos);
            if (pos + n > s.size())
               fail("msgpack: str overruns buffer", pos);
            std::string out(s.data() + pos, n);
            pos += n;
            return out;
         }
         else if constexpr (mp_is_optional<U>::value)
         {
            if (pos < s.size() &&
                static_cast<std::uint8_t>(s[pos]) == tag::nil)
            {
               ++pos;
               return U{};
            }
            return U{decode_value<typename mp_is_optional<U>::elem>(s, pos)};
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            // Accept either bin (canonical) or array (back-compat for
            // data emitted before this codec started routing byte
            // vectors to bin).
            std::uint8_t t = pos < s.size()
                                ? static_cast<std::uint8_t>(s[pos])
                                : 0;
            if (t == tag::bin8 || t == tag::bin16 || t == tag::bin32)
            {
               std::size_t n = decode_bin_header(s, pos);
               if (pos + n > s.size())
                  fail("msgpack: bin overruns buffer", pos);
               U out;
               out.resize(n);
               if (n)
                  std::memcpy(out.data(), s.data() + pos, n);
               pos += n;
               return out;
            }
            std::size_t n = decode_array_header(s, pos);
            U           out;
            out.reserve(n);
            using E = typename mp_is_vector<U>::elem;
            for (std::size_t i = 0; i < n; ++i)
               out.push_back(decode_value<E>(s, pos));
            return out;
         }
         else if constexpr (mp_is_vector<U>::value)
         {
            using E       = typename mp_is_vector<U>::elem;
            std::size_t n = decode_array_header(s, pos);
            U out;
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
               out.push_back(decode_value<E>(s, pos));
            return out;
         }
         else if constexpr (mp_is_array<U>::value)
         {
            using E       = typename mp_is_array<U>::elem;
            std::size_t n = decode_array_header(s, pos);
            if (n != mp_is_array<U>::len)
               fail("msgpack: array arity mismatch", pos);
            U out;
            for (std::size_t i = 0; i < n; ++i)
               out[i] = decode_value<E>(s, pos);
            return out;
         }
         else if constexpr (mp_is_variant<U>::value)
         {
            std::size_t n = decode_array_header(s, pos);
            if (n != 2)
               fail("msgpack: variant must be 2-element array", pos);
            std::size_t idx = decode_int<std::size_t>(s, pos);
            return decode_variant_alt<U>(idx, s, pos);
         }
         else if constexpr (Reflected<U>)
         {
            return decode_record<U>(s, pos);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::msgpack: unsupported type in decode_value");
         }
      }

      template <typename V, std::size_t I>
      V decode_variant_alt(std::size_t           idx,
                           std::span<const char> s,
                           std::size_t&          pos)
      {
         constexpr std::size_t N = std::variant_size_v<V>;
         if constexpr (I < N)
         {
            if (idx == I)
               return V{
                  std::in_place_index<I>,
                  decode_value<std::variant_alternative_t<I, V>>(s, pos)};
            return decode_variant_alt<V, I + 1>(idx, s, pos);
         }
         else
         {
            fail("msgpack: variant discriminator out of range", pos);
         }
      }

   }  // namespace detail::msgpack_impl

   struct msgpack : format_tag_base<msgpack>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), msgpack, const T& v,
                             std::vector<char>& sink)
      {
         //  Pre-size + fast_buf_stream beats vector_stream's grow-as-
         //  you-write on nested data (one resize beats N reallocations).
         //  Mirrors the pattern used by fracpack / pssz / ssz / bin /
         //  borsh / bincode (see commit 6f1cef9).
         const std::size_t       n    = detail::msgpack_impl::packed_size_of(v);
         const std::size_t       orig = sink.size();
         sink.resize(orig + n);
         ::psio::fast_buf_stream fbs{sink.data() + orig, n};
         detail::msgpack_impl::write_value(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), msgpack,
                                          const T& v)
      {
         const std::size_t       n = detail::msgpack_impl::packed_size_of(v);
         std::vector<char>       out(n);
         ::psio::fast_buf_stream fbs{out.data(), out.size()};
         detail::msgpack_impl::write_value(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), msgpack, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::msgpack_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), msgpack,
                                    const T& v)
      {
         return detail::msgpack_impl::packed_size_of(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), msgpack,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(),
                                                         "msgpack");
             !st.ok())
            return st;
         try
         {
            std::size_t pos = 0;
            (void)detail::msgpack_impl::decode_value<T>(bytes, pos);
            return codec_ok();
         }
         catch (const detail::msgpack_impl::DecodeError& e)
         {
            return codec_fail(e.what(), e.pos, "msgpack");
         }
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     msgpack, T*,
                                     std::span<const char> bytes) noexcept
      {
         try
         {
            std::size_t pos = 0;
            (void)detail::msgpack_impl::decode_value<T>(bytes, pos);
            if (pos != bytes.size())
               return codec_fail("msgpack: trailing bytes", pos, "msgpack");
            return codec_ok();
         }
         catch (const detail::msgpack_impl::DecodeError& e)
         {
            return codec_fail(e.what(), e.pos, "msgpack");
         }
      }
   };

}  // namespace psio
