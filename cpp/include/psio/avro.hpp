#pragma once
//
// psio/avro.hpp — `avro` format tag (Apache Avro binary).
//
// Wire (MVP scope):
//   bool:              1 byte (0/1)
//   signed/unsigned integrals: zig-zag varint (Avro long)
//   float:             raw 4-byte LE IEEE-754
//   double:            raw 8-byte LE IEEE-754
//   std::string:       varint(length) + utf-8 bytes
//   std::vector<T>:    block encoding — varint(count) + items, then 0-count
//   std::array<byte,N>: raw N bytes (Avro "fixed" when element is byte-sized)
//   std::array<other,N>: block encoding identical to vector
//   std::optional<T>:  union{null,T}: varint(0) or varint(1)+value
//   record:            fields concatenated in reflected order

#include <psio/cpo.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/varint/leb128.hpp>
#include <psio/wrappers.hpp>

#include <array>
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

   struct avro;

   namespace detail::avro_impl {

      template <typename T>
      concept Record = ::psio::Reflected<T>;

      template <typename T>
      struct is_std_array : std::false_type {};
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type {};
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
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio::bitlist<N>> : std::true_type {};

      // Avro `long` is zig-zag varint over int64 (max 10 wire bytes).
      // Wire encoding lives in psio/varint/leb128.hpp; the wrappers
      // here bridge to the codec's `Sink::write(const void*, n)` and
      // `std::span<const char>` calling conventions.
      template <typename Sink>
      inline void write_long(Sink& s, std::int64_t v)
      {
         std::uint8_t buf[::psio::varint::leb128::max_bytes_i64];
         const auto   n =
            ::psio::varint::leb128::encode_zigzag64(buf, v);
         s.write(reinterpret_cast<const void*>(buf), n);
      }

      inline std::int64_t read_long(std::span<const char> src,
                                    std::size_t&          pos)
      {
         const auto avail = src.size() - pos;
         const auto r     = ::psio::varint::leb128::decode_zigzag64(
            reinterpret_cast<const std::uint8_t*>(src.data() + pos), avail);
         if (!r.ok) return 0;
         pos += r.len;
         return r.value;
      }

      template <typename T, typename Sink>
      void encode_value(const T& v, Sink& s)
      {
         if constexpr (std::is_same_v<T, bool>)
            s.write(v ? '\x01' : '\x00');
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            // Treat as Avro "fixed" 32-byte field — raw LE bytes,
            // no varint. Matches v1 ext-int behavior in multiformat
            // tests.
            s.write(v.limb, 32);
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
         {
            s.write(&v, 16);
         }
         else if constexpr (std::is_same_v<T, float>)
         {
            s.write(&v, 4);
         }
         else if constexpr (std::is_same_v<T, double>)
         {
            s.write(&v, 8);
         }
         else if constexpr (std::is_enum_v<T>)
         {
            write_long(s,
                       static_cast<std::int64_t>(
                          static_cast<std::underlying_type_t<T>>(v)));
         }
         else if constexpr (std::is_integral_v<T>)
         {
            write_long(s, static_cast<std::int64_t>(v));
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (sizeof(E) == 1 && std::is_arithmetic_v<E>)
            {
               s.write(v.data(), v.size());
            }
            else
            {
               if (v.size() > 0)
               {
                  write_long(s, static_cast<std::int64_t>(v.size()));
                  for (const auto& x : v)
                     encode_value(x, s);
               }
               write_long(s, 0);
            }
         }
         else if constexpr (is_std_vector<T>::value)
         {
            if (!v.empty())
            {
               write_long(s, static_cast<std::int64_t>(v.size()));
               for (const auto& x : v)
                  encode_value(x, s);
            }
            write_long(s, 0);
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            write_long(s, static_cast<std::int64_t>(v.size()));
            s.write(v.data(), v.size());
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
            {
               write_long(s, 1);
               encode_value(*v, s);
            }
            else
            {
               write_long(s, 0);
            }
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // Avro union: long(branch index) + value.
            write_long(s, static_cast<std::int64_t>(v.index()));
            std::visit([&](const auto& alt) { encode_value(alt, s); }, v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            // Avro fixed: raw N bytes (no length prefix).
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(v.data(), nbytes);
         }
         else if constexpr (is_bitlist<T>::value)
         {
            // Avro long(bit_count) + packed bytes.
            write_long(s, static_cast<std::int64_t>(v.size()));
            auto bytes = v.bytes();
            if (!bytes.empty())
               s.write(bytes.data(), bytes.size());
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
            static_assert(sizeof(T) == 0,
                          "psio::avro: unsupported type in encode_value");
         }
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      // In-place decode for std::string. Avro encodes string as
      // varint(length) + bytes; assigning straight into the
      // destination saves the temp + move-assign cost.
      template <typename T>
      void decode_into(std::span<const char> src, std::size_t& pos,
                       T& out)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            const std::int64_t n = read_long(src, pos);
            const std::size_t  sn = static_cast<std::size_t>(n);
            out.assign(src.data() + pos, src.data() + pos + sn);
            pos += sn;
         }
         else
         {
            out = decode_value<T>(src, pos);
         }
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos)
      {
         if constexpr (std::is_same_v<T, bool>)
            return static_cast<unsigned char>(src[pos++]) != 0;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            T out{};
            std::memcpy(out.limb, src.data() + pos, 32);
            pos += 32;
            return out;
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, 16);
            pos += 16;
            return out;
         }
         else if constexpr (std::is_same_v<T, float>)
         {
            float out;
            std::memcpy(&out, src.data() + pos, 4);
            pos += 4;
            return out;
         }
         else if constexpr (std::is_same_v<T, double>)
         {
            double out;
            std::memcpy(&out, src.data() + pos, 8);
            pos += 8;
            return out;
         }
         else if constexpr (std::is_enum_v<T>)
         {
            return static_cast<T>(
               static_cast<std::underlying_type_t<T>>(read_long(src, pos)));
         }
         else if constexpr (std::is_integral_v<T>)
         {
            return static_cast<T>(read_long(src, pos));
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            T out{};
            if constexpr (sizeof(E) == 1 && std::is_arithmetic_v<E>)
            {
               std::memcpy(out.data(), src.data() + pos, out.size());
               pos += out.size();
            }
            else
            {
               std::size_t i = 0;
               while (true)
               {
                  std::int64_t count = read_long(src, pos);
                  if (count < 0)
                  {
                     // Avro optional size-prefixed block variant — treat as abs
                     count = -count;
                     // skip size-in-bytes long
                     (void)read_long(src, pos);
                  }
                  if (count == 0)
                     break;
                  for (std::int64_t j = 0; j < count; ++j, ++i)
                  {
                     if (i < out.size())
                        out[i] = decode_value<E>(src, pos);
                  }
               }
            }
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            std::vector<E> out;
            while (true)
            {
               std::int64_t count = read_long(src, pos);
               if (count < 0)
               {
                  count = -count;
                  (void)read_long(src, pos);  // skip byte size
               }
               if (count == 0)
                  break;
               out.reserve(out.size() + static_cast<std::size_t>(count));
               for (std::int64_t j = 0; j < count; ++j)
                  out.push_back(decode_value<E>(src, pos));
            }
            return out;
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            std::int64_t n = read_long(src, pos);
            std::string  out(src.data() + pos,
                            src.data() + pos + static_cast<std::size_t>(n));
            pos += static_cast<std::size_t>(n);
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V      = typename T::value_type;
            std::int64_t branch = read_long(src, pos);
            if (branch == 0)
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(src, pos)};
         }
         else if constexpr (is_std_variant<T>::value)
         {
            const auto idx =
               static_cast<std::size_t>(read_long(src, pos));
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
         else if constexpr (is_bitlist<T>::value)
         {
            const std::size_t bit_count =
               static_cast<std::size_t>(read_long(src, pos));
            T                 out;
            const std::size_t byte_count = (bit_count + 7) / 8;
            auto&             bits       = out.storage();
            bits.resize(bit_count);
            for (std::size_t i = 0; i < bit_count; ++i)
            {
               const unsigned char b =
                  static_cast<unsigned char>(src[pos + (i >> 3)]);
               bits[i] = (b >> (i & 7)) & 1;
            }
            pos += byte_count;
            return out;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            T       out{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (decode_into<typename R::template member_type<Is>>(
                    src, pos, out.*(R::template member_pointer<Is>)),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::avro: unsupported type in decode_value");
         }
      }

   }  // namespace detail::avro_impl

   struct avro : format_tag_base<avro>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), avro, const T& v,
                             std::vector<char>& sink)
      {
         // Two-pass: size_stream first, then fast_buf_stream into a
         // pre-sized buffer — mirrors v1's convert_to_avro algorithm.
         ::psio::size_stream ss;
         detail::avro_impl::encode_value(v, ss);
         const std::size_t orig = sink.size();
         sink.resize(orig + ss.size);
         ::psio::fast_buf_stream fbs(sink.data() + orig, ss.size);
         detail::avro_impl::encode_value(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), avro,
                                          const T& v)
      {
         ::psio::size_stream ss;
         detail::avro_impl::encode_value(v, ss);
         std::vector<char> out(ss.size);
         ::psio::fast_buf_stream fbs(out.data(), ss.size);
         detail::avro_impl::encode_value(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), avro, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::avro_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), avro,
                                    const T& v)
      {
         ::psio::size_stream ss;
         detail::avro_impl::encode_value(v, ss);
         return ss.size;
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), avro, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("avro: empty buffer", 0, "avro");
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "avro");
             !st.ok())
            return st;
         return codec_ok();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     avro, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("avro: empty buffer", 0, "avro");
         return codec_ok();
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           avro, T*,
                                           std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::avro_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio
