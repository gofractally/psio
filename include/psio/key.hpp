#pragma once
//
// psio/key.hpp — `key` format tag: memcmp-sortable byte sequences.
//
// Wire (MVP scope):
//   bool:               0x00 or 0x01
//   unsigned integral:  big-endian
//   signed integral:    sign bit flipped, big-endian
//   float:              IEEE-754 sign-transform + big-endian
//   double:             ditto, 8 bytes
//   std::string:        content with \0→\0\1 escape + \0\0 terminator
//   std::optional<T>:   \0 (none) or \1 + value
//   std::vector<octet>: same as string (escape + terminator)
//   std::vector<other>: \1 + value per element, \0 terminator
//   record:             fields concatenated in reflected order
//
// `octet` means uint8_t / int8_t / char.

#include <psio/cpo.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

   struct key;

   namespace detail::key_impl {

      template <typename T>
      concept Record = ::psio::Reflected<T>;

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type {};
      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type {};

      using ::psio::detail::is_std_variant;

      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio::bitvector<N>> : std::true_type {};

      template <typename T>
      inline constexpr bool is_octet_v =
         std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::int8_t> ||
         std::is_same_v<T, char>;

      using sink_t = std::vector<char>;

      template <typename T>
      T bswap(T v)
      {
         if constexpr (sizeof(T) == 1)
            return v;
         else if constexpr (sizeof(T) == 2)
            return static_cast<T>(
               __builtin_bswap16(static_cast<std::uint16_t>(v)));
         else if constexpr (sizeof(T) == 4)
            return static_cast<T>(
               __builtin_bswap32(static_cast<std::uint32_t>(v)));
         else if constexpr (sizeof(T) == 8)
            return static_cast<T>(
               __builtin_bswap64(static_cast<std::uint64_t>(v)));
      }

      template <typename T>
      T to_big_endian(T v)
      {
         if constexpr (std::endian::native == std::endian::big)
            return v;
         else
            return bswap(v);
      }

      template <typename T>
      void append_be(sink_t& s, T v)
      {
         v = to_big_endian(v);
         s.insert(s.end(), reinterpret_cast<const char*>(&v),
                  reinterpret_cast<const char*>(&v) + sizeof(T));
      }

      inline void append_escaped(sink_t& s, const char* data, std::size_t len)
      {
         const char* p   = data;
         const char* end = data + len;
         while (p < end)
         {
            const char* null_pos =
               static_cast<const char*>(std::memchr(p, '\0', end - p));
            if (!null_pos)
            {
               s.insert(s.end(), p, end);
               break;
            }
            if (null_pos > p)
               s.insert(s.end(), p, null_pos);
            s.push_back('\0');
            s.push_back('\1');
            p = null_pos + 1;
         }
         s.push_back('\0');
         s.push_back('\0');
      }

      template <typename T>
      void encode_scalar(const T& v, sink_t& s)
      {
         if constexpr (std::is_same_v<T, bool>)
            s.push_back(v ? '\x01' : '\x00');
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            // Big-endian across all 32 bytes (MSB limb first, byte-
            // swap each limb). Gives memcmp-sortable order.
            for (int i = 3; i >= 0; --i)
               append_be(s, v.limb[i]);
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128>)
         {
            auto hi = static_cast<std::uint64_t>(v >> 64);
            auto lo = static_cast<std::uint64_t>(v);
            append_be(s, hi);
            append_be(s, lo);
         }
         else if constexpr (std::is_same_v<T, ::psio::int128>)
         {
            // Sign-flip the top bit, then big-endian.
            auto uv = static_cast<::psio::uint128>(v);
            uv ^= (::psio::uint128{1} << 127);
            auto hi = static_cast<std::uint64_t>(uv >> 64);
            auto lo = static_cast<std::uint64_t>(uv);
            append_be(s, hi);
            append_be(s, lo);
         }
         else if constexpr (std::is_same_v<T, float>)
         {
            std::uint32_t bits;
            std::memcpy(&bits, &v, 4);
            if (bits == 0x80000000u)
               bits = 0;
            if (bits & 0x80000000u)
               bits = ~bits;
            else
               bits ^= 0x80000000u;
            append_be(s, bits);
         }
         else if constexpr (std::is_same_v<T, double>)
         {
            std::uint64_t bits;
            std::memcpy(&bits, &v, 8);
            if (bits == 0x8000000000000000ull)
               bits = 0;
            if (bits & 0x8000000000000000ull)
               bits = ~bits;
            else
               bits ^= 0x8000000000000000ull;
            append_be(s, bits);
         }
         else if constexpr (std::is_enum_v<T>)
         {
            encode_scalar(static_cast<std::underlying_type_t<T>>(v), s);
         }
         else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>)
         {
            append_be(s, v);
         }
         else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>)
         {
            using U = std::make_unsigned_t<T>;
            U uv =
               static_cast<U>(v) ^ (static_cast<U>(1) << (sizeof(T) * 8 - 1));
            append_be(s, uv);
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::key: not a scalar in encode_scalar");
         }
      }

      template <typename T>
      void encode_value(const T& v, sink_t& s)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            append_escaped(s, v.data(), v.size());
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
            {
               s.push_back('\x01');
               encode_value(*v, s);
            }
            else
            {
               s.push_back('\x00');
            }
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (is_octet_v<E>)
            {
               // Per-element key bytes + escape + \0\0 terminator.
               for (const auto& elem : v)
               {
                  sink_t tmp;
                  encode_scalar(elem, tmp);
                  // Single byte; escape if null.
                  s.push_back(tmp[0]);
                  if (tmp[0] == '\0')
                     s.push_back('\1');
               }
               s.push_back('\0');
               s.push_back('\0');
            }
            else
            {
               for (const auto& elem : v)
               {
                  s.push_back('\x01');
                  encode_value(elem, s);
               }
               s.push_back('\x00');
            }
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // v1 key: 1-byte index + value.
            s.push_back(static_cast<char>(v.index()));
            std::visit([&](const auto& alt) { encode_value(alt, s); }, v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            // Fixed-size — sortable directly as raw bytes.
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.insert(s.end(),
                        reinterpret_cast<const char*>(v.data()),
                        reinterpret_cast<const char*>(v.data()) + nbytes);
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (encode_value(v.*(R::template member_pointer<Is>), s), ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            encode_scalar(v, s);
         }
      }

      // ── Decoder ───────────────────────────────────────────────────────

      template <typename T>
      T read_be(std::span<const char> src, std::size_t& pos)
      {
         T v{};
         std::memcpy(&v, src.data() + pos, sizeof(T));
         pos += sizeof(T);
         return to_big_endian(v);
      }

      inline std::string read_escaped(std::span<const char> src,
                                      std::size_t&          pos)
      {
         std::string out;
         while (pos < src.size())
         {
            const char* p = src.data() + pos;
            const char* e = src.data() + src.size();
            const char* null_pos =
               static_cast<const char*>(std::memchr(p, '\0', e - p));
            if (!null_pos)
               return out;  // malformed; caller should have validated
            if (null_pos > p)
               out.append(p, null_pos - p);
            pos = static_cast<std::size_t>(null_pos + 1 - src.data());
            if (pos >= src.size())
               return out;
            char next = src[pos++];
            if (next == '\0')
               return out;
            out.push_back('\0');
         }
         return out;
      }

      template <typename T>
      T decode_scalar(std::span<const char> src, std::size_t& pos)
      {
         if constexpr (std::is_same_v<T, bool>)
            return static_cast<unsigned char>(src[pos++]) != 0;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            T out{};
            for (int i = 3; i >= 0; --i)
               out.limb[i] = read_be<std::uint64_t>(src, pos);
            return out;
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128>)
         {
            auto hi = read_be<std::uint64_t>(src, pos);
            auto lo = read_be<std::uint64_t>(src, pos);
            return (static_cast<::psio::uint128>(hi) << 64) | lo;
         }
         else if constexpr (std::is_same_v<T, ::psio::int128>)
         {
            auto hi = read_be<std::uint64_t>(src, pos);
            auto lo = read_be<std::uint64_t>(src, pos);
            auto uv = (static_cast<::psio::uint128>(hi) << 64) | lo;
            uv ^= (::psio::uint128{1} << 127);
            return static_cast<::psio::int128>(uv);
         }
         else if constexpr (std::is_same_v<T, float>)
         {
            std::uint32_t bits = read_be<std::uint32_t>(src, pos);
            if (bits & 0x80000000u)
               bits ^= 0x80000000u;
            else
               bits = ~bits;
            float out;
            std::memcpy(&out, &bits, 4);
            return out;
         }
         else if constexpr (std::is_same_v<T, double>)
         {
            std::uint64_t bits = read_be<std::uint64_t>(src, pos);
            if (bits & 0x8000000000000000ull)
               bits ^= 0x8000000000000000ull;
            else
               bits = ~bits;
            double out;
            std::memcpy(&out, &bits, 8);
            return out;
         }
         else if constexpr (std::is_enum_v<T>)
         {
            return static_cast<T>(
               decode_scalar<std::underlying_type_t<T>>(src, pos));
         }
         else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>)
         {
            return read_be<T>(src, pos);
         }
         else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>)
         {
            using U = std::make_unsigned_t<T>;
            U uv =
               read_be<U>(src, pos) ^ (static_cast<U>(1) << (sizeof(T) * 8 - 1));
            return static_cast<T>(uv);
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::key: not a scalar in decode_scalar");
         }
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            return read_escaped(src, pos);
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V        = typename T::value_type;
            const char tag = src[pos++];
            if (tag == '\0')
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(src, pos)};
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            std::vector<E> out;
            if constexpr (is_octet_v<E>)
            {
               while (pos < src.size())
               {
                  char c = src[pos++];
                  if (c == '\0')
                  {
                     if (pos < src.size() && src[pos] == '\0')
                     {
                        ++pos;
                        return out;
                     }
                     if (pos < src.size() && src[pos] == '\1')
                        ++pos;  // escaped null
                  }
                  // Decode single key byte back to element value.
                  std::size_t tmp_pos = 0;
                  char        buf[1]  = {c};
                  std::span<const char> s{buf, 1};
                  out.push_back(decode_scalar<E>(s, tmp_pos));
               }
               return out;
            }
            else
            {
               while (pos < src.size())
               {
                  char tag = src[pos++];
                  if (tag == '\0')
                     return out;
                  out.push_back(decode_value<E>(src, pos));
               }
               return out;
            }
         }
         else if constexpr (is_std_variant<T>::value)
         {
            const auto idx = static_cast<std::size_t>(
               static_cast<unsigned char>(src[pos++]));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  std::variant_alternative_t<Is, T>>(src,
                                                                       pos)},
                       true)
                    : false) ||
                  ...);
               (void)found;
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return out;
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            T                     out{};
            if constexpr (nbytes > 0)
            {
               std::memcpy(out.data(), src.data() + pos, nbytes);
               pos += nbytes;
            }
            return out;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            T       out{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (((out.*(R::template member_pointer<Is>)) =
                    decode_value<typename R::template member_type<Is>>(src,
                                                                         pos)),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }
         else
         {
            return decode_scalar<T>(src, pos);
         }
      }

   }  // namespace detail::key_impl

   struct key : format_tag_base<key>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), key, const T& v,
                             std::vector<char>& sink)
      {
         detail::key_impl::encode_value(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), key,
                                          const T& v)
      {
         std::vector<char> out;
         detail::key_impl::encode_value(v, out);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), key, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::key_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), key,
                                    const T& v)
      {
         std::vector<char> tmp;
         detail::key_impl::encode_value(v, tmp);
         return tmp.size();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), key, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("key: empty buffer", 0, "key");
         return codec_ok();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>), key,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("key: empty buffer", 0, "key");
         return codec_ok();
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>), key,
                                           T*,
                                           std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::key_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio
