#pragma once
//
// psio/borsh.hpp — `borsh` format tag.
//
// Borsh is NEAR/Solana's canonical binary format. Wire (MVP scope):
//   * primitives: raw LE; bool: 1 byte (0/1)
//   * std::string:      u32 length + utf-8 bytes
//   * std::vector<T>:   u32 length + elements
//   * std::array<T, N>: elements concatenated (length is part of type)
//   * std::optional<T>: u8 tag (0=None, 1=Some) + value
//   * record:           fields concatenated in reflected order

#include <psio/cpo.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
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

   struct borsh;

   namespace detail::borsh_impl {

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

      inline std::uint32_t read_u32(std::span<const char> src, std::size_t pos)
      {
         std::uint32_t v{};
         std::memcpy(&v, src.data() + pos, 4);
         return v;
      }

      // ── Fixed / variable packsize split ─────────────────────────────────
      //
      // Mirrors bin.hpp. Wire differences from bin: variants use a u8
      // tag (not u32); bitlist uses a u32 bit_count; bitvector is raw.

      template <typename T>
      consteval std::size_t fixed_contrib();

      template <typename T>
      std::size_t variable_contrib(const T& v);

      template <typename T>
      consteval bool fully_fixed()
      {
         if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                       std::is_same_v<T, ::psio::uint128> ||
                       std::is_same_v<T, ::psio::int128> ||
                       std::is_same_v<T, ::psio::uint256>)
            return true;
         else if constexpr (is_std_array<T>::value)
            return fully_fixed<typename T::value_type>();
         else if constexpr (is_bitvector<T>::value)
            return true;
         else if constexpr (std::is_same_v<T, std::string> ||
                            is_std_vector<T>::value ||
                            is_std_optional<T>::value ||
                            is_std_variant<T>::value ||
                            is_bitlist<T>::value)
            return false;
         else if constexpr (Record<T>)
         {
            using R  = ::psio::reflect<T>;
            bool all = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((all = all && fully_fixed<
                                  typename R::template member_type<Is>>()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return all;
         }
         else
            return false;
      }

      template <typename T>
      consteval std::size_t fixed_contrib()
      {
         if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            return 16;
         else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            return sizeof(T);
         else if constexpr (is_std_array<T>::value)
            return std::tuple_size<T>::value *
                   fixed_contrib<typename T::value_type>();
         else if constexpr (is_bitvector<T>::value)
            return (T::size_value + 7) / 8;
         else if constexpr (std::is_same_v<T, std::string>)
            return 4;   // u32 byte count
         else if constexpr (is_std_vector<T>::value)
            return 4;   // u32 element count
         else if constexpr (is_std_optional<T>::value)
            return 1;   // discriminant byte
         else if constexpr (is_std_variant<T>::value)
            return 1;   // borsh variant tag is u8
         else if constexpr (is_bitlist<T>::value)
            return 4;   // u32 bit count
         else if constexpr (Record<T>)
         {
            std::size_t total = 0;
            using R           = ::psio::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((total += fixed_contrib<
                             typename R::template member_type<Is>>()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return total;
         }
         else
            return 0;
      }

      template <typename T>
      std::size_t variable_contrib(const T& v)
      {
         if constexpr (fully_fixed<T>())
            return 0;
         else if constexpr (std::is_same_v<T, std::string>)
            return v.size();
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (fully_fixed<E>())
               return v.size() * fixed_contrib<E>();
            else
            {
               std::size_t total = 0;
               for (const auto& x : v)
                  total += fixed_contrib<E>() + variable_contrib(x);
               return total;
            }
         }
         else if constexpr (is_std_array<T>::value)
         {
            std::size_t total = 0;
            for (const auto& x : v)
               total += variable_contrib(x);
            return total;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (!v.has_value())
               return 0;
            using E = typename T::value_type;
            return fixed_contrib<E>() + variable_contrib(*v);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            std::size_t total = 0;
            std::visit(
               [&](const auto& alt)
               {
                  using A = std::remove_cvref_t<decltype(alt)>;
                  total   = fixed_contrib<A>() + variable_contrib(alt);
               },
               v);
            return total;
         }
         else if constexpr (is_bitlist<T>::value)
            return v.bytes().size();
         else if constexpr (Record<T>)
         {
            using R           = ::psio::reflect<T>;
            std::size_t total = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      if constexpr (!fully_fixed<F>())
                         total += variable_contrib(
                            v.*(R::template member_pointer<Is>));
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return total;
         }
         else
            return 0;
      }

      template <typename T>
      std::size_t packed_size_of(const T& v)
      {
         return fixed_contrib<T>() + variable_contrib(v);
      }

      // Unified walker — same template as bin.hpp, different wire rules.
      // Works with any `Sink` that exposes `.put<T>(T)`, `.write(ptr,n)`,
      // `.write(ch)`. That lets `size_stream` compute packsize and
      // `fast_buf_stream` / `vector_stream` produce bytes from the one
      // template body.
      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s)
      {
         if constexpr (std::is_same_v<T, bool>)
         {
            std::uint8_t b = v ? 1 : 0;
            s.write(reinterpret_cast<const char*>(&b), 1);
         }
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            s.write(v.limb, 32);
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            s.write(&v, 16);
         else if constexpr (std::is_arithmetic_v<T>)
            s.put(v);
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (std::is_arithmetic_v<E> &&
                          !std::is_same_v<E, bool>)
               s.write(v.data(), sizeof(E) * v.size());
            else
               for (const auto& x : v)
                  write_value(x, s);
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            s.put(static_cast<std::uint32_t>(v.size()));
            // Bulk-memcpy fast path covers arithmetic primitives AND
            // DWNC packed records whose memory layout matches the wire.
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && ::psio::is_dwnc_v<E> && fully_fixed<E>() &&
               std::is_trivially_copyable_v<E> &&
               fixed_contrib<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               if (!v.empty())
                  s.write(v.data(), sizeof(E) * v.size());
            }
            else
            {
               for (const auto& x : v)
                  write_value(x, s);
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            s.put(static_cast<std::uint32_t>(v.size()));
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (is_std_optional<T>::value)
         {
            std::uint8_t tag = v.has_value() ? 1 : 0;
            s.write(reinterpret_cast<const char*>(&tag), 1);
            if (v.has_value())
               write_value(*v, s);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            static_assert(std::variant_size_v<T> <= 256,
                          "borsh variant tag is u8 (max 256 alternatives)");
            std::uint8_t idx = static_cast<std::uint8_t>(v.index());
            s.write(reinterpret_cast<const char*>(&idx), 1);
            std::visit([&](const auto& alt) { write_value(alt, s); }, v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(v.data(), nbytes);
         }
         else if constexpr (is_bitlist<T>::value)
         {
            s.put(static_cast<std::uint32_t>(v.size()));
            auto bytes = v.bytes();
            if (!bytes.empty())
               s.write(bytes.data(), bytes.size());
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path for DWNC packed records — same trick
            // bin/ssz/pssz/frac use. Single struct-wide write instead
            // of N per-field writes.
            if constexpr (::psio::is_dwnc_v<T> && fully_fixed<T>() &&
                          std::is_trivially_copyable_v<T> &&
                          fixed_contrib<T>() == sizeof(T))
            {
               s.write(reinterpret_cast<const char*>(&v), sizeof(T));
               return;
            }
            using R = ::psio::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (write_value(v.*(R::template member_pointer<Is>), s), ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::borsh: unsupported type in write_value");
         }
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      // In-place decode helper. v1's `from_borsh(T&, stream)` reads
      // straight into existing storage; v3's `decode_value<T>` returns
      // by value and the caller move-assigns. The move + temp's
      // destructor add ~1 ns per std::string field and compound on
      // records with several variable-length fields. decode_into
      // matches v1's in-place algorithm: for std::string and bulk-
      // memcpy std::vector it allocates directly into `out`. Other
      // types fall back to assigning from decode_value (the move is
      // cheap for arithmetic / fixed types).
      template <typename T>
      void decode_into(std::span<const char> src, std::size_t& pos,
                       T& out)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            out.assign(src.data() + pos, src.data() + pos + n);
            pos += n;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E               = typename T::value_type;
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && ::psio::is_dwnc_v<E> && fully_fixed<E>() &&
               std::is_trivially_copyable_v<E> &&
               fixed_contrib<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               const E* first =
                  reinterpret_cast<const E*>(src.data() + pos);
               out.assign(first, first + n);
               pos += sizeof(E) * n;
            }
            else
            {
               out.clear();
               out.reserve(n);
               for (std::uint32_t i = 0; i < n; ++i)
                  out.push_back(decode_value<E>(src, pos));
            }
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
         else if constexpr (std::is_arithmetic_v<T>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, sizeof(T));
            pos += sizeof(T);
            return out;
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            T out{};
            if constexpr (std::is_arithmetic_v<E> &&
                          !std::is_same_v<E, bool>)
            {
               std::memcpy(out.data(), src.data() + pos,
                           sizeof(E) * out.size());
               pos += sizeof(E) * out.size();
            }
            else
            {
               for (std::size_t i = 0; i < std::tuple_size<T>::value; ++i)
                  out[i] = decode_value<E>(src, pos);
            }
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E             = typename T::value_type;
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            std::vector<E> out;
            // Bulk-memcpy fast path for arithmetic OR memcpy-layout
            // Records (DWNC packed). assign(p, p+n) lowers to one
            // memcpy without resize's value-init pass.
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && ::psio::is_dwnc_v<E> && fully_fixed<E>() &&
               std::is_trivially_copyable_v<E> &&
               fixed_contrib<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               const E* first =
                  reinterpret_cast<const E*>(src.data() + pos);
               out.assign(first, first + n);
               pos += sizeof(E) * n;
            }
            else
            {
               out.reserve(n);
               for (std::uint32_t i = 0; i < n; ++i)
                  out.push_back(decode_value<E>(src, pos));
            }
            return out;
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            std::string out(src.data() + pos, src.data() + pos + n);
            pos += n;
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V            = typename T::value_type;
            const bool present = static_cast<unsigned char>(src[pos++]) != 0;
            if (!present)
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(src, pos)};
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
         else if constexpr (is_bitlist<T>::value)
         {
            std::uint32_t bit_count;
            std::memcpy(&bit_count, src.data() + pos, 4);
            pos += 4;
            T                 out;
            const std::size_t byte_count = (bit_count + 7) / 8;
            auto&             bits       = out.storage();
            bits.resize(bit_count);
            for (std::uint32_t i = 0; i < bit_count; ++i)
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
            // Memcpy fast path for DWNC packed records.
            if constexpr (::psio::is_dwnc_v<T> && fully_fixed<T>() &&
                          std::is_trivially_copyable_v<T> &&
                          fixed_contrib<T>() == sizeof(T))
            {
               T out;
               std::memcpy(&out, src.data() + pos, sizeof(T));
               pos += sizeof(T);
               return out;
            }
            using R = ::psio::reflect<T>;
            T       out{};
            // In-place decode each field — avoids the move-assign +
            // temp-destructor cost on std::string / std::vector
            // fields. Mirrors v1's `from_borsh(T&, stream)` walker.
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
                          "psio::borsh: unsupported type in decode_value");
         }
      }

   }  // namespace detail::borsh_impl

   struct borsh : format_tag_base<borsh>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), borsh, const T& v,
                             std::vector<char>& sink)
      {
         //  Pre-size + fast_buf_stream beats vector_stream's grow-as-
         //  you-write on nested data: one resize beats N reallocations.
         //  Same pattern fracpack/pssz/ssz already use.
         const std::size_t       n     = detail::borsh_impl::packed_size_of(v);
         const std::size_t       orig  = sink.size();
         sink.resize(orig + n);
         ::psio::fast_buf_stream fbs{sink.data() + orig, n};
         detail::borsh_impl::write_value(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), borsh,
                                          const T& v)
      {
         const std::size_t        n = detail::borsh_impl::packed_size_of(v);
         std::vector<char>        out(n);
         ::psio::fast_buf_stream fbs{out.data(), out.size()};
         detail::borsh_impl::write_value(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), borsh, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::borsh_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), borsh,
                                    const T& v)
      {
         return detail::borsh_impl::packed_size_of(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), borsh, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("borsh: empty buffer", 0, "borsh");
         if (auto st =
                ::psio::check_max_dynamic_cap<T>(bytes.size(), "borsh");
             !st.ok())
            return st;
         return codec_ok();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     borsh, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("borsh: empty buffer", 0, "borsh");
         return codec_ok();
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           borsh, T*,
                                           std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::borsh_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio
