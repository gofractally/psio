#pragma once
//
// psio/key.hpp — `key` format tag: memcmp-sortable byte sequences
// (docs/key-spec.md).
//
// The encoding of every type is ORDER-PRESERVING (bytewise comparison of
// two keys equals comparison of the two values) and PREFIX-FREE (no key is
// a proper prefix of another key of the same type). Those two properties
// are what make concatenation work: a tuple or record is its elements
// concatenated, the key of tuple<A, B> is a byte-prefix of the key of
// tuple<A, B, C> for equal A and B, and tuple keys order lexicographically
// by element.
//
//   bool                  0x00 or 0x01
//   unsigned integer      big-endian
//   signed integer        sign bit flipped, big-endian
//   char, std::byte       the byte (unsigned order, like a string's bytes)
//   enum                  its underlying integer
//   uint128 / uint256     big-endian; int128 sign-flipped big-endian
//   float / double        -0 becomes +0; then negative: every bit flipped,
//                         otherwise: sign bit flipped; big-endian
//   duration, time_point  the tick count's integer form (time.hpp)
//   std::string,
//   utf8_string<N>        bytes with 00 → 00 01, then the terminator 00 00
//   std::vector<octet>,
//   bounded<octet, N>     each element's key byte, escaped and terminated
//                         like a string (octet: uint8_t, int8_t, char,
//                         std::byte)
//   std::vector<T>,
//   bounded<T, N>         01 + element, per element, then 00
//   std::optional<T>      00 (none) or 01 + value
//   std::array<T, N>      the N elements concatenated
//   byte_array<N>         the N bytes
//   bitvector<N>          its (N+7)/8 bytes (equality order only)
//   std::variant          1-byte alternative index + the alternative
//   std::tuple, std::pair,
//   record                elements / reflected fields concatenated in
//                         order; tuple<> and an empty record are zero bytes
//   map, set              the key of the list of its entries / elements in
//                         canonical (ascending key) order: std::map,
//                         std::set, std::unordered_map, std::unordered_set
//
// bitlist has no key form, and the multi containers are not PSIO values;
// both are compile errors.
//
// Custom types: a type with a `sortable_binary_category` adapter
// (PSIO_ADAPTER) delegates encode/decode/validate to the adapter, and
// `psio::key` adds no framing around its bytes. The adapter owns both
// properties above: its bytes must sort like the value and be prefix-free
// (fixed width, or self-terminating). This is a separate slot from
// `binary_category`: wire and sort encodings usually differ.
//
// Reading is checked. `validate` accepts exactly the canonical encodings:
// the whole buffer is one value, booleans and presence bytes are 00 / 01,
// escapes and terminators are well formed, a float is not the image of -0,
// bounds hold, utf8_string bytes are UTF-8. `decode` runs the same reader
// and fails (codec_exception, or abort without exceptions) on anything
// `validate` rejects.
//
// Composite-key helpers for a database layer, at the bottom of this file:
//
//   psio::key_prefix<Key>(a, b)            key of the first K elements of
//                                          Key, each converted to and
//                                          encoded as Key's element type
//   psio::key_prefix_upper_bound(prefix)   exclusive end of the range of
//                                          keys that start with `prefix`
//   psio::decode<Key>(psio::key{}, bytes)  a full key back to its tuple

#include <psio/cpo.hpp>
#include <psio/detail/assoc_order.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/time.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   struct key;

   namespace detail::key_impl {

      template <typename T>
      concept Record = ::psio::Reflected<T> && !std::is_enum_v<T>;

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type {};
      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type {};
      template <typename T>
      struct is_std_array : std::false_type {};
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type {};
      template <typename T>
      struct is_std_tuple : std::false_type {};
      template <typename... Ts>
      struct is_std_tuple<std::tuple<Ts...>> : std::true_type {};
      // A std::pair is the 2-tuple of its members.
      template <typename A, typename B>
      struct is_std_tuple<std::pair<A, B>> : std::true_type {};
      template <typename T>
      struct is_bounded : std::false_type {};
      template <typename T, std::size_t N>
      struct is_bounded<::psio::bounded<T, N>> : std::true_type {};
      template <typename T>
      struct is_utf8_string : std::false_type {};
      template <std::size_t N>
      struct is_utf8_string<::psio::utf8_string<N>> : std::true_type {};
      template <typename T>
      struct is_byte_array : std::false_type {};
      template <std::size_t N>
      struct is_byte_array<::psio::byte_array<N>> : std::true_type {};
      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio::bitlist<N>> : std::true_type {};

      using ::psio::detail::is_std_variant;

      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio::bitvector<N>> : std::true_type {};

      // Elements that share the string framing (one key byte each).
      template <typename T>
      inline constexpr bool is_octet_v =
         std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::int8_t> ||
         std::is_same_v<T, char> || std::is_same_v<T, std::byte>;

      // A raw byte: its key byte is itself. `char` is one of these whatever
      // its signedness on the platform, so that key bytes do not depend on
      // the target and a vector<char> orders like the std::string it holds.
      template <typename T>
      inline constexpr bool is_raw_byte_v =
         std::is_same_v<T, std::uint8_t> || std::is_same_v<T, char> ||
         std::is_same_v<T, std::byte>;

      // No key form at all: a clear error instead of a deep one.
      template <typename T>
      inline constexpr bool unsupported_v =
         is_bitlist<T>::value || ::psio::detail::is_multi_assoc_v<T>;

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

      // The encode walker is templated on the sink, exactly like every other psio
      // format (bin/borsh): it talks to `s.write(ch)` / `s.write(ptr, n)` only, so the
      // SAME body drives `size_stream` (the packsize pass), `fast_buf_stream` (write
      // straight into a caller buffer), and `vector_stream` (grow a std::vector). That is
      // what lets a consumer size-then-encode into a stack/SBO buffer with no heap traffic.
      template <typename S, typename T>
      void append_be(S& s, T v)
      {
         v = to_big_endian(v);
         s.write(reinterpret_cast<const char*>(&v), sizeof(T));
      }

      template <typename S>
      void append_escaped(S& s, const char* data, std::size_t len)
      {
         const char* p   = data;
         const char* end = data + len;
         while (p < end)
         {
            const char* null_pos =
               static_cast<const char*>(std::memchr(p, '\0', end - p));
            if (!null_pos)
            {
               s.write(p, static_cast<std::size_t>(end - p));
               break;
            }
            if (null_pos > p)
               s.write(p, static_cast<std::size_t>(null_pos - p));
            s.write('\0');
            s.write('\1');
            p = null_pos + 1;
         }
         s.write('\0');
         s.write('\0');
      }

      template <typename T, typename S>
      void encode_scalar(const T& v, S& s)
      {
         if constexpr (std::is_same_v<T, bool>)
            s.write(static_cast<char>(v ? '\x01' : '\x00'));
         else if constexpr (is_raw_byte_v<T>)
            s.write(static_cast<char>(v));
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
         else if constexpr (::psio::TimeValue<T>)
         {
            encode_scalar(::psio::time_ticks(v), s);
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
                          "psio::key: this type has no key encoding");
         }
      }

      // The single key byte of an octet element.
      template <typename E>
      char octet_key_byte(E v) noexcept
      {
         if constexpr (std::is_same_v<E, std::int8_t>)
            return static_cast<char>(static_cast<std::uint8_t>(v) ^ 0x80u);
         else
            return static_cast<char>(v);
      }
      template <typename E>
      E octet_from_key_byte(char c) noexcept
      {
         if constexpr (std::is_same_v<E, std::int8_t>)
            return static_cast<E>(static_cast<std::uint8_t>(c) ^ 0x80u);
         else
            return static_cast<E>(static_cast<unsigned char>(c));
      }

      template <typename T, typename S>
      void encode_value(const T& v, S& s);

      // 01 + element per element, then 00; octets share the string framing.
      template <typename V, typename S>
      void encode_list(const V& v, S& s)
      {
         using E = typename V::value_type;
         if constexpr (is_raw_byte_v<E>)
            append_escaped(s, reinterpret_cast<const char*>(v.data()), v.size());
         else if constexpr (is_octet_v<E>)
         {
            for (const auto& elem : v)
            {
               const char b = octet_key_byte(elem);
               s.write(b);
               if (b == '\0')
                  s.write('\1');
            }
            s.write('\0');
            s.write('\0');
         }
         else
         {
            for (std::size_t i = 0; i < v.size(); ++i)
            {
               s.write('\x01');
               // By value: a std::vector<bool> element is a proxy.
               if constexpr (std::is_same_v<E, bool>)
                  encode_value<bool>(static_cast<bool>(v[i]), s);
               else
                  encode_value<E>(v[i], s);
            }
            s.write('\x00');
         }
      }

      // ── Which types have a key encoding ──────────────────────────────
      //
      //   none      no key encoding (bitlist, a multi container, a type
      //             encode_scalar rejects), here or in a part of the type
      //   adapted   encoded, with a sortable_binary_category adapter
      //             somewhere in the type: its bytes are the adapter's
      //   plain     encoded by the rules of docs/key-spec.md alone
      //
      // The key type of a map and the element type of a set must be
      // `plain`: their entries are ordered by key bytes, and that order
      // has to be the same for every reader, including one that knows the
      // type only through its schema.
      enum class key_form : std::uint8_t
      {
         none,
         adapted,
         plain,
      };
      constexpr key_form weakest(key_form a, key_form b) noexcept { return a < b ? a : b; }

      template <typename T>
      consteval key_form key_form_of() noexcept
      {
         using U = std::remove_cv_t<T>;
         if constexpr (unsupported_v<U>)
            return key_form::none;
         else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::key, U>)
            return key_form::adapted;
         else if constexpr (::psio::detail::is_map_v<U>)
            return key_form_of<::psio::detail::assoc_key_t<U>>() != key_form::plain
                      ? key_form::none
                      : weakest(key_form::plain, key_form_of<typename U::mapped_type>());
         else if constexpr (::psio::detail::is_set_v<U>)
            return key_form_of<::psio::detail::assoc_key_t<U>>() != key_form::plain
                      ? key_form::none
                      : key_form::plain;
         else if constexpr (std::is_same_v<U, std::string> || is_utf8_string<U>::value ||
                            is_byte_array<U>::value || is_bitvector<U>::value)
            return key_form::plain;
         else if constexpr (is_std_optional<U>::value || is_std_vector<U>::value ||
                            is_bounded<U>::value || is_std_array<U>::value)
            return key_form_of<typename U::value_type>();
         else if constexpr (is_std_variant<U>::value)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               key_form form = std::variant_size_v<U> <= 256 ? key_form::plain : key_form::none;
               ((form = weakest(form, key_form_of<std::variant_alternative_t<Is, U>>())), ...);
               return form;
            }(std::make_index_sequence<std::variant_size_v<U>>{});
         else if constexpr (is_std_tuple<U>::value)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               key_form form = key_form::plain;
               ((form = weakest(form, key_form_of<std::tuple_element_t<Is, U>>())), ...);
               return form;
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         else if constexpr (Record<U>)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               key_form form = key_form::plain;
               ((form = weakest(form, key_form_of<typename ::psio::reflect<
                                         U>::template member_type<Is>>())),
                ...);
               return form;
            }(std::make_index_sequence<::psio::reflect<U>::member_count>{});
         else
            return std::is_same_v<U, bool> || is_raw_byte_v<U> ||
                         std::is_same_v<U, ::psio::uint256> ||
                         std::is_same_v<U, ::psio::uint128> ||
                         std::is_same_v<U, ::psio::int128> || std::is_same_v<U, float> ||
                         std::is_same_v<U, double> || std::is_enum_v<U> ||
                         ::psio::TimeValue<U> || std::is_integral_v<U>
                      ? key_form::plain
                      : key_form::none;
      }

      // Whether K can be the key type of a map / the element type of a set.
      template <typename K>
      inline constexpr bool orders_entries_v = key_form_of<K>() == key_form::plain;

// What every codec asserts about the key type of a map / set C.
#define PSIO_REQUIRE_ENTRY_ORDER(Format, C)                                                   \
   static_assert(                                                                             \
      ::psio::detail::key_impl::orders_entries_v<::psio::detail::assoc_key_t<C>>,             \
      Format ": the key type of a map (the element type of a set) must have a key encoding " \
             "(docs/key-spec.md) and no sortable_binary_category adapter: entries are "      \
             "ordered by the key bytes of their keys")

      // A map / set is the list of its entries / elements in canonical
      // order: the octet framing for a set of octets, `01 entry ... 00`
      // otherwise. Two entries with equal key bytes have no encoding.
      template <typename C, typename S>
      void encode_assoc(const C& v, S& s)
      {
         PSIO_REQUIRE_ENTRY_ORDER("psio::key", C);
         using K = ::psio::detail::assoc_key_t<C>;
         const ::psio::detail::canonical_order<C> order(v);
         if (!order.unique())
            ::psio::codec_abort(::psio::codec_error{
               ::psio::detail::is_map_v<C> ? "key: map has duplicate keys"
                                           : "key: set has duplicate elements",
               0, "key"});
         if constexpr (::psio::detail::is_set_v<C> && is_octet_v<K>)
         {
            order.for_each([&](const K& elem) {
               const char b = octet_key_byte(elem);
               s.write(b);
               if (b == '\0')
                  s.write('\1');
            });
            s.write('\0');
            s.write('\0');
         }
         else
         {
            order.for_each([&](const auto& item) {
               s.write('\x01');
               if constexpr (::psio::detail::is_map_v<C>)
               {
                  encode_value<K>(item.first, s);
                  encode_value(item.second, s);
               }
               else
                  encode_value<K>(item, s);
            });
            s.write('\x00');
         }
      }

      template <typename T, typename S>
      void encode_value(const T& v, S& s)
      {
         static_assert(!is_bitlist<T>::value, "psio::key: bitlist has no key encoding");
         PSIO_REJECT_MULTI_ASSOC("psio::key", T);
         // Adapter dispatch: a type with a sortable_binary_category
         // adapter delegates encoding entirely to the adapter. The
         // adapter is responsible for emitting memcmp-sortable,
         // prefix-free bytes — no length prefix or framing is added on
         // this side, since framing would break key ordering.
         //
         // The branch is the FIRST `if constexpr` in the chain so the
         // tail `else { encode_scalar<T> }` is discarded for adapter
         // types.
         if constexpr (unsupported_v<T>)
         {
         }
         else if constexpr (::psio::format_should_dispatch_adapter_v<
                           ::psio::key, T>)
         {
            using A = ::psio::adapter<std::remove_cvref_t<T>,
                                      ::psio::sortable_binary_category>;
            // Adapters emit their bytes through a vector sink (the psio
            // adapter convention, same as bin.hpp). Bridge to the generic
            // stream: count on the size pass, copy the bytes through on a
            // real write.
            if constexpr (std::is_same_v<S, ::psio::size_stream>)
               s.write(nullptr, A::packsize(v));
            else
            {
               std::vector<char> tmp;
               A::encode(v, tmp);
               s.write(tmp.data(), tmp.size());
            }
         }
         else if constexpr (::psio::detail::is_assoc_v<T>)
            encode_assoc(v, s);
         else if constexpr (std::is_same_v<T, std::string>)
            append_escaped(s, v.data(), v.size());
         else if constexpr (is_utf8_string<T>::value)
            append_escaped(s, v.data(), v.size());
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
            {
               s.write('\x01');
               encode_value(*v, s);
            }
            else
            {
               s.write('\x00');
            }
         }
         else if constexpr (is_std_vector<T>::value)
            encode_list(v, s);
         else if constexpr (is_bounded<T>::value)
            encode_list(v.storage(), s);
         else if constexpr (is_byte_array<T>::value)
         {
            if constexpr (T::size_value > 0)
               s.write(reinterpret_cast<const char*>(v.data()), T::size_value);
         }
         else if constexpr (is_std_array<T>::value)
         {
            for (const auto& elem : v)
               encode_value(elem, s);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            static_assert(std::variant_size_v<T> <= 256,
                          "psio::key: a variant has at most 256 alternatives");
            s.write(static_cast<char>(v.index()));
            std::visit([&](const auto& alt) { encode_value(alt, s); }, v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            // Fixed-size raw bytes.
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(reinterpret_cast<const char*>(v.data()), nbytes);
         }
         else if constexpr (is_std_tuple<T>::value)
         {
            std::apply(
               [&](const auto&... elem) {
                  (encode_value<std::remove_cvref_t<decltype(elem)>>(elem, s), ...);
               },
               v);
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

      // Compile-time EXACT encoded size of a key, or nullopt when it depends on the value
      // (strings, vectors, optionals, adapter types). A consumer can give a fixed-width key
      // — double, integer, enum, time point, fixed tuple or reflected struct — a stack
      // buffer of the exact size and encode straight into it: no size pass, no allocation.
      template <typename T>
      consteval std::optional<std::size_t> key_fixed_size()
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (::psio::format_should_dispatch_adapter_v<::psio::key, U>)
            return std::nullopt;  // adapter bytes aren't known until encode
         else if constexpr (std::is_same_v<U, bool>)
            return std::size_t{1};
         else if constexpr (std::is_same_v<U, ::psio::uint256>)
            return std::size_t{32};
         else if constexpr (std::is_same_v<U, ::psio::uint128> ||
                            std::is_same_v<U, ::psio::int128>)
            return std::size_t{16};
         else if constexpr (std::is_same_v<U, float>)
            return std::size_t{4};
         else if constexpr (std::is_same_v<U, double>)
            return std::size_t{8};
         else if constexpr (std::is_enum_v<U>)
            return sizeof(std::underlying_type_t<U>);
         else if constexpr (::psio::TimeValue<U>)
            return sizeof(::psio::time_rep_t<U>);
         else if constexpr (std::is_integral_v<U>)
            return sizeof(U);
         else if constexpr (is_bitvector<U>::value)
            return std::size_t{(U::size_value + 7) / 8};
         else if constexpr (is_byte_array<U>::value)
            return std::size_t{U::size_value};
         else if constexpr (is_std_array<U>::value)
         {
            constexpr auto f = key_fixed_size<typename U::value_type>();
            if (f.has_value())
               return *f * std::tuple_size_v<U>;
            return std::nullopt;
         }
         else if constexpr (is_std_tuple<U>::value)
         {
            std::size_t total    = 0;
            bool        allFixed = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (([&]
                 {
                    constexpr auto f = key_fixed_size<std::tuple_element_t<Is, U>>();
                    if constexpr (f.has_value())
                       total += *f;
                    else
                       allFixed = false;
                 }()),
                ...);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
            if (allFixed)
               return total;
            return std::nullopt;
         }
         else if constexpr (Record<U>)
         {
            using R              = ::psio::reflect<U>;
            std::size_t total    = 0;
            bool        allFixed = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (([&]
                 {
                    constexpr auto f =
                       key_fixed_size<typename R::template member_type<Is>>();
                    if constexpr (f.has_value())
                       total += *f;
                    else
                       allFixed = false;
                 }()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            if (allFixed)
               return total;
            return std::nullopt;
         }
         else
            return std::nullopt;  // string, vector, optional — variable length
      }

      // ── Reader ────────────────────────────────────────────────────────
      //
      // One checked walk serves validate (out == nullptr) and decode. It
      // accepts exactly what encode_value produces.

      struct reader
      {
         const char* begin = nullptr;
         const char* p     = nullptr;
         const char* end   = nullptr;
         const char* error = nullptr;

         std::size_t left() const noexcept { return static_cast<std::size_t>(end - p); }
         bool        fail(const char* what) noexcept
         {
            if (!error)
               error = what;
            return false;
         }
         bool need(std::size_t n) noexcept
         {
            return left() >= n || fail("key: truncated");
         }
      };

      template <typename T>
      T read_be(reader& r) noexcept
      {
         T v{};
         std::memcpy(&v, r.p, sizeof(T));
         r.p += sizeof(T);
         return to_big_endian(v);
      }

      // Escaped bytes up to the 00 00 terminator, which is consumed. `sink`
      // is called with each unescaped run.
      template <typename Sink>
      bool read_escaped(reader& r, Sink&& sink)
      {
         for (;;)
         {
            const char* zero =
               static_cast<const char*>(std::memchr(r.p, '\0', r.left()));
            if (!zero || zero + 1 >= r.end)
               return r.fail("key: unterminated string");
            const char next = zero[1];
            if (next == '\0')
            {
               sink(r.p, static_cast<std::size_t>(zero - r.p));
               r.p = zero + 2;
               return true;
            }
            if (next != '\1')
               return r.fail("key: invalid escape");
            sink(r.p, static_cast<std::size_t>(zero + 1 - r.p));  // run + the 00
            r.p = zero + 2;
         }
      }

      template <typename T>
      bool read_scalar(reader& r, T* out) noexcept
      {
         if constexpr (std::is_same_v<T, bool>)
         {
            if (!r.need(1))
               return false;
            const auto b = static_cast<unsigned char>(*r.p++);
            if (b > 1)
               return r.fail("key: invalid bool");
            if (out)
               *out = b != 0;
            return true;
         }
         else if constexpr (is_raw_byte_v<T>)
         {
            if (!r.need(1))
               return false;
            const char c = *r.p++;
            if (out)
               *out = octet_from_key_byte<T>(c);
            return true;
         }
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            if (!r.need(32))
               return false;
            T v{};
            for (int i = 3; i >= 0; --i)
               v.limb[i] = read_be<std::uint64_t>(r);
            if (out)
               *out = v;
            return true;
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
         {
            if (!r.need(16))
               return false;
            const auto hi = read_be<std::uint64_t>(r);
            const auto lo = read_be<std::uint64_t>(r);
            auto       uv = (static_cast<::psio::uint128>(hi) << 64) | lo;
            if constexpr (std::is_same_v<T, ::psio::int128>)
               uv ^= (::psio::uint128{1} << 127);
            if (out)
               *out = static_cast<T>(uv);
            return true;
         }
         else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
         {
            using U = std::conditional_t<std::is_same_v<T, float>, std::uint32_t, std::uint64_t>;
            constexpr U sign = static_cast<U>(U{1} << (sizeof(U) * 8 - 1));
            if (!r.need(sizeof(U)))
               return false;
            U bits = read_be<U>(r);
            if (bits & sign)
               bits ^= sign;
            else
               bits = static_cast<U>(~bits);
            if (bits == sign)
               return r.fail("key: negative zero is not canonical");
            if (out)
               std::memcpy(out, &bits, sizeof(U));
            return true;
         }
         else if constexpr (std::is_enum_v<T>)
         {
            std::underlying_type_t<T> raw{};
            if (!read_scalar(r, &raw))
               return false;
            if (out)
               *out = static_cast<T>(raw);
            return true;
         }
         else if constexpr (::psio::TimeValue<T>)
         {
            ::psio::time_rep_t<T> raw{};
            if (!read_scalar(r, &raw))
               return false;
            if (out)
               *out = ::psio::time_from_ticks<T>(raw);
            return true;
         }
         else if constexpr (std::is_integral_v<T>)
         {
            using U = std::make_unsigned_t<T>;
            if (!r.need(sizeof(T)))
               return false;
            U uv = read_be<U>(r);
            if constexpr (std::is_signed_v<T>)
               uv ^= static_cast<U>(U{1} << (sizeof(T) * 8 - 1));
            if (out)
               *out = static_cast<T>(uv);
            return true;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::key: this type has no key encoding");
            return false;
         }
      }

      template <typename T>
      bool read_value(reader& r, T* out);

      // `limit` is the element bound of a psio::bounded (SIZE_MAX: none).
      template <typename E, typename A>
      bool read_list(reader& r, std::vector<E, A>* out, std::size_t limit)
      {
         std::size_t count = 0;
         if constexpr (is_octet_v<E>)
         {
            auto run = [&](const char* data, std::size_t n) {
               count += n;
               if (out && count <= limit)
                  for (std::size_t i = 0; i < n; ++i)
                     out->push_back(octet_from_key_byte<E>(data[i]));
            };
            if (!read_escaped(r, run))
               return false;
         }
         else
         {
            for (;;)
            {
               if (!r.need(1))
                  return false;
               const auto tag = static_cast<unsigned char>(*r.p++);
               if (tag == 0)
                  break;
               if (tag != 1)
                  return r.fail("key: invalid list element marker");
               if (++count > limit)
                  break;
               E elem{};
               if (!read_value<E>(r, out ? &elem : nullptr))
                  return false;
               if (out)
                  out->push_back(std::move(elem));
            }
         }
         if (count > limit)
            return r.fail("key: list exceeds its bound");
         return true;
      }

      // A map / set: the list framing of its entries / elements, whose keys
      // must be strictly ascending by key bytes. The key bytes are the
      // bytes just read, so the order is checked on the buffer itself.
      template <typename C>
      bool read_assoc(reader& r, C* out)
      {
         PSIO_REQUIRE_ENTRY_ORDER("psio::key", C);
         using K = ::psio::detail::assoc_key_t<C>;
         constexpr bool    is_map = ::psio::detail::is_map_v<C>;
         constexpr const char* unordered =
            is_map ? "key: map entries are not in strictly ascending key order"
                   : "key: set elements are not in strictly ascending order";
         constexpr const char* merged =
            is_map ? "key: map container merged distinct keys"
                   : "key: set container merged distinct elements";
         if (out)
            out->clear();
         std::size_t count = 0;
         if constexpr (::psio::detail::is_set_v<C> && is_octet_v<K>)
         {
            std::vector<K> elems;
            if (!read_list(r, &elems, std::numeric_limits<std::size_t>::max()))
               return false;
            for (std::size_t i = 1; i < elems.size(); ++i)
               if (static_cast<unsigned char>(octet_key_byte(elems[i - 1])) >=
                   static_cast<unsigned char>(octet_key_byte(elems[i])))
                  return r.fail(unordered);
            count = elems.size();
            if (out)
               out->insert(elems.begin(), elems.end());
         }
         else
         {
            const char* prev_begin = nullptr;
            const char* prev_end   = nullptr;
            for (;;)
            {
               if (!r.need(1))
                  return false;
               const auto tag = static_cast<unsigned char>(*r.p++);
               if (tag == 0)
                  break;
               if (tag != 1)
                  return r.fail("key: invalid list element marker");
               const char* const key_begin = r.p;
               K                 k{};
               if (!read_value<K>(r, out ? &k : nullptr))
                  return false;
               const char* const key_end = r.p;
               if (prev_begin &&
                   ::psio::detail::compare_key_bytes(
                      prev_begin, static_cast<std::size_t>(prev_end - prev_begin), key_begin,
                      static_cast<std::size_t>(key_end - key_begin)) >= 0)
               {
                  r.p = key_begin;
                  return r.fail(unordered);
               }
               prev_begin = key_begin;
               prev_end   = key_end;
               ++count;
               if constexpr (is_map)
               {
                  using V = typename C::mapped_type;
                  if (!out)
                  {
                     if (!read_value<V>(r, nullptr))
                        return false;
                  }
                  else
                  {
                     V value{};
                     if (!read_value<V>(r, &value))
                        return false;
                     out->emplace_hint(out->end(), std::move(k), std::move(value));
                  }
               }
               else if (out)
                  out->emplace_hint(out->end(), std::move(k));
            }
         }
         if (out && out->size() != count)
            return r.fail(merged);
         return true;
      }

      template <typename T>
      bool read_value(reader& r, T* out)
      {
         static_assert(!is_bitlist<T>::value, "psio::key: bitlist has no key encoding");
         PSIO_REJECT_MULTI_ASSOC("psio::key", T);
         if constexpr (unsupported_v<T>)
            return false;
         else if constexpr (::psio::format_should_dispatch_adapter_v<
                           ::psio::key, T>)
         {
            // The adapter sees the rest of the buffer and reports what it
            // consumed through packsize of the value it decoded.
            using A = ::psio::adapter<std::remove_cvref_t<T>,
                                      ::psio::sortable_binary_category>;
            const std::span<const char> rest{r.p, r.left()};
            if (!A::validate(rest).ok())
               return r.fail("key: adapter rejected its bytes");
            T                 v = A::decode(rest);
            const std::size_t n = A::packsize(v);
            if (n > rest.size())
               return r.fail("key: truncated");
            r.p += n;
            if (out)
               *out = std::move(v);
            return true;
         }
         else if constexpr (::psio::detail::is_assoc_v<T>)
            return read_assoc(r, out);
         else if constexpr (std::is_same_v<T, std::string>)
         {
            return read_escaped(r, [&](const char* data, std::size_t n) {
               if (out)
                  out->append(data, n);
            });
         }
         else if constexpr (is_utf8_string<T>::value)
         {
            // Unescaped length is at most the bytes left, so one scratch
            // string serves both the bound and the UTF-8 check.
            std::string text;
            if (!read_escaped(r, [&](const char* data, std::size_t n) { text.append(data, n); }))
               return false;
            if (text.size() > T::max_size_value)
               return r.fail("key: utf8_string exceeds its bound");
            if (!::psio::utf8_spec::validate(std::span<const char>(text.data(), text.size())).ok())
               return r.fail("key: utf8_string is not valid UTF-8");
            if (out)
               *out = T{std::move(text)};
            return true;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V = typename T::value_type;
            if (!r.need(1))
               return false;
            const auto tag = static_cast<unsigned char>(*r.p++);
            if (tag == 0)
            {
               if (out)
                  out->reset();
               return true;
            }
            if (tag != 1)
               return r.fail("key: invalid optional marker");
            if (!out)
               return read_value<V>(r, nullptr);
            V v{};
            if (!read_value<V>(r, &v))
               return false;
            *out = std::move(v);
            return true;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            if (out)
               out->clear();
            return read_list(r, out, std::numeric_limits<std::size_t>::max());
         }
         else if constexpr (is_bounded<T>::value)
         {
            if (out)
               out->storage().clear();
            return read_list(r, out ? &out->storage() : nullptr, T::max_size_value);
         }
         else if constexpr (is_byte_array<T>::value)
         {
            if (!r.need(T::size_value))
               return false;
            if constexpr (T::size_value > 0)
               if (out)
                  std::memcpy(out->data(), r.p, T::size_value);
            r.p += T::size_value;
            return true;
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            for (std::size_t i = 0; i < std::tuple_size_v<T>; ++i)
               if (!read_value<E>(r, out ? &(*out)[i] : nullptr))
                  return false;
            return true;
         }
         else if constexpr (is_std_variant<T>::value)
         {
            if (!r.need(1))
               return false;
            const auto idx = static_cast<std::size_t>(static_cast<unsigned char>(*r.p++));
            if (idx >= std::variant_size_v<T>)
               return r.fail("key: variant index out of range");
            bool ok = false;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               auto alt = [&]<std::size_t I>(std::in_place_index_t<I>) {
                  using Alt = std::variant_alternative_t<I, T>;
                  if (!out)
                     return read_value<Alt>(r, nullptr);
                  Alt v{};
                  if (!read_value<Alt>(r, &v))
                     return false;
                  out->template emplace<I>(std::move(v));
                  return true;
               };
               (void)((idx == Is ? (ok = alt(std::in_place_index<Is>), true) : false) || ...);
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return ok;
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if (!r.need(nbytes))
               return false;
            if constexpr (T::size_value % 8 != 0)
               if (static_cast<unsigned char>(r.p[nbytes - 1]) >> (T::size_value % 8))
                  return r.fail("key: bitvector padding bits are set");
            if constexpr (nbytes > 0)
               if (out)
                  std::memcpy(out->data(), r.p, nbytes);
            r.p += nbytes;
            return true;
         }
         else if constexpr (is_std_tuple<T>::value)
         {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               return (read_value<std::tuple_element_t<Is, T>>(
                          r, out ? &std::get<Is>(*out) : nullptr) &&
                       ...);
            }(std::make_index_sequence<std::tuple_size_v<T>>{});
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            // A member's declared length_bound is part of its type, as in
            // pssz: a key holding a member outside it is rejected.
            auto member = [&]<std::size_t I>() -> bool
            {
               using F   = typename R::template member_type<I>;
               using Ann = ::psio::effective_annotations_for<T, F, R::template member_pointer<I>>;
               using eff = std::remove_cvref_t<decltype(Ann::value)>;
               if constexpr (::psio::has_spec_v<::psio::length_bound, eff> &&
                             !::psio::has_as_override_v<eff>)
               {
                  F        scratch{};
                  F* const dst = out ? &(out->*(R::template member_pointer<I>)) : &scratch;
                  if (!read_value<F>(r, dst))
                     return false;
                  if (!::psio::detail::length_bound_ok(
                         *dst, std::get<::psio::length_bound>(Ann::value)))
                     return r.fail("key: member length_bound violated");
                  return true;
               }
               else
                  return read_value<F>(
                     r, out ? &(out->*(R::template member_pointer<I>)) : nullptr);
            };
            return [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               return (member.template operator()<Is>() && ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return read_scalar<T>(r, out);
         }
      }

      // Reads exactly one T spanning the whole buffer.
      template <typename T>
      codec_status read_document(std::span<const char> bytes, T* out)
      {
         reader r{bytes.data(), bytes.data(), bytes.data() + bytes.size()};
         if (!read_value<T>(r, out))
            return codec_fail(r.error ? r.error : "key: malformed",
                              static_cast<std::uint32_t>(r.p - r.begin), "key");
         if (r.p != r.end)
            return codec_fail("key: trailing bytes",
                              static_cast<std::uint32_t>(r.p - r.begin), "key");
         return codec_ok();
      }

      // ── Composite keys ────────────────────────────────────────────────

      // The ordered element types of a composite key type.
      template <typename K>
      struct key_elements;
      template <typename... Ts>
      struct key_elements<std::tuple<Ts...>>
      {
         static constexpr std::size_t count = sizeof...(Ts);
         template <std::size_t I>
         using type = std::remove_cvref_t<std::tuple_element_t<I, std::tuple<Ts...>>>;
      };
      template <typename A, typename B>
      struct key_elements<std::pair<A, B>>
      {
         static constexpr std::size_t count = 2;
         template <std::size_t I>
         using type = std::remove_cvref_t<std::tuple_element_t<I, std::pair<A, B>>>;
      };
      template <Record K>
      struct key_elements<K>
      {
         static constexpr std::size_t count = ::psio::reflect<K>::member_count;
         template <std::size_t I>
         using type = std::remove_cvref_t<typename ::psio::reflect<K>::template member_type<I>>;
      };

      template <typename K>
      concept Composite = requires { key_elements<K>::count; };

      // An argument that can stand for element type E: anything implicitly
      // convertible to it, plus a string_view for a std::string element.
      template <typename E, typename Arg>
      inline constexpr bool prefix_element_ok_v =
         std::is_convertible_v<const Arg&, E> ||
         (std::is_same_v<E, std::string> && std::is_convertible_v<const Arg&, std::string_view>);

      // Encodes `arg` as element type E.
      template <typename E, typename Arg, typename S>
      void encode_prefix_element(const Arg& arg, S& s)
      {
         if constexpr (std::is_same_v<E, Arg>)
            encode_value<E>(arg, s);
         else if constexpr (std::is_same_v<E, std::string> &&
                            std::is_convertible_v<const Arg&, std::string_view>)
         {
            // A literal or string_view: no temporary std::string.
            const std::string_view text = arg;
            append_escaped(s, text.data(), text.size());
         }
         else
         {
            const E converted = arg;
            encode_value<E>(converted, s);
         }
      }

   }  // namespace detail::key_impl

   struct key : format_tag_base<key>
   {
      // Note: `sortable_binary_category`, NOT `binary_category`. A
      // value's wire encoding (bin/pssz) and its sort encoding can
      // differ — signed ints, floats, and structs whose canonical
      // sort is bespoke all need a separate adapter slot. Types
      // whose wire and sort happen to coincide (fixed-width digests,
      // opaque blobs) register identical adapter impls in both slots.
      using preferred_presentation_category =
          ::psio::sortable_binary_category;

      // Generic sink: drives size_stream (packsize), fast_buf_stream (write into a
      // caller buffer), vector_stream, etc. — the path consumers use to encode straight
      // into a stack/SBO buffer. The std::vector<char>& overload below is more specialized
      // and wins for that sink (vectors have no .write()), bridging through vector_stream.
      template <typename T, typename Sink>
      friend void tag_invoke(decltype(::psio::encode), key, const T& v, Sink& sink)
      {
         detail::key_impl::encode_value(v, sink);
      }

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), key, const T& v,
                             std::vector<char>& sink)
      {
         ::psio::vector_stream s{sink};  // appends, matching the other formats
         detail::key_impl::encode_value(v, s);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), key,
                                          const T& v)
      {
         std::vector<char>    out;
         ::psio::vector_stream s{out};
         detail::key_impl::encode_value(v, s);
         return out;
      }

      // Checked: fails on anything `validate` rejects.
      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), key, T*,
                          std::span<const char> bytes)
      {
         T out{};
         if (auto st = detail::key_impl::read_document<T>(bytes, &out); !st.ok())
            ::psio::codec_abort(st.error());
         return out;
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), key,
                                    const T& v)
      {
         ::psio::size_stream s;  // packsize pass — counts, no allocation
         detail::key_impl::encode_value(v, s);
         return s.size;
      }

      // The key format has one encoding per value, so both validation
      // levels accept exactly the canonical bytes of one T.
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), key, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::key_impl::read_document<T>(bytes, nullptr);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>), key,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::key_impl::read_document<T>(bytes, nullptr);
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>), key,
                                           T*,
                                           std::span<const char> bytes)
      {
         auto out = std::make_unique<T>();
         if (auto st = detail::key_impl::read_document<T>(bytes, out.get()); !st.ok())
            ::psio::codec_abort(st.error());
         return out;
      }
   };

   // ── to_key: value → memcmp-sortable `key` bytes ─────────────────────────────
   // Returns std::array<char, N> when the encoded size is a compile-time constant
   // (fixed-width keys — integers, floats, enums, fixed reflected structs): encoded
   // straight into the array on the stack, no packsize pass, no allocation. Returns
   // std::vector<char> when the size depends on the value (strings, vectors, optionals).
   // Take the result with `auto`; both expose .data()/.size().
   template <typename T>
   auto to_key(const T& v)
   {
      constexpr auto fixed = detail::key_impl::key_fixed_size<T>();
      if constexpr (fixed.has_value())
      {
         std::array<char, *fixed> out;
         fast_buf_stream          fs{out.data(), out.size()};
         detail::key_impl::encode_value(v, fs);
         return out;
      }
      else
      {
         size_stream ss;
         detail::key_impl::encode_value(v, ss);
         std::vector<char> out(ss.size);
         fast_buf_stream   fs{out.data(), out.size()};
         detail::key_impl::encode_value(v, fs);
         return out;
      }
   }

   // ── Composite keys ──────────────────────────────────────────────────────────
   //
   // A composite key type is a std::tuple or a reflected record; its elements are the
   // tuple's elements / the record's fields, in order.

   // The key of the first K elements of `Key` — the prefix every full key whose leading
   // elements equal `elems...` starts with. K = sizeof...(elems) may be 0 (the empty
   // prefix: every key) up to the full element count (then it is the full key). Each
   // argument must convert implicitly to the corresponding element type of `Key` and is
   // encoded AS that type, so `key_prefix<std::tuple<std::uint32_t, std::string>>(7)`
   // yields the four bytes of a uint32, never those of an int.
   template <detail::key_impl::Composite Key, typename... Prefix>
   std::vector<char> key_prefix(const Prefix&... elems)
   {
      using E = detail::key_impl::key_elements<Key>;
      static_assert(sizeof...(Prefix) <= E::count,
                    "psio::key_prefix: more elements than the key type has");
      std::vector<char> out;
      [&]<std::size_t... Is>(std::index_sequence<Is...>)
      {
         static_assert((detail::key_impl::prefix_element_ok_v<typename E::template type<Is>, Prefix> && ...),
                       "psio::key_prefix: an element does not convert to the key's "
                       "element type at its position");
         size_stream ss;
         (detail::key_impl::encode_prefix_element<typename E::template type<Is>>(elems, ss), ...);
         out.resize(ss.size);
         fast_buf_stream fs{out.data(), out.size()};
         (detail::key_impl::encode_prefix_element<typename E::template type<Is>>(elems, fs), ...);
      }(std::index_sequence_for<Prefix...>{});
      return out;
   }

   // The exclusive upper bound of a prefix scan: the smallest byte string greater than
   // every byte string that starts with `prefix`. The keys with that prefix are exactly
   // the keys k with prefix <= k < bound. It is `prefix` without its trailing 0xFF bytes
   // and with the last remaining byte incremented. nullopt means there is no bound — the
   // prefix is empty or all 0xFF, so the range runs to the end of the key space.
   inline std::optional<std::vector<char>> key_prefix_upper_bound(std::span<const char> prefix)
   {
      std::size_t n = prefix.size();
      while (n > 0 && static_cast<unsigned char>(prefix[n - 1]) == 0xff)
         --n;
      if (n == 0)
         return std::nullopt;
      std::vector<char> out(prefix.begin(), prefix.begin() + static_cast<std::ptrdiff_t>(n));
      out.back() = static_cast<char>(static_cast<unsigned char>(out.back()) + 1);
      return out;
   }

}  // namespace psio
