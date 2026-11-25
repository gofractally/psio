#pragma once
//
// psio/protobuf.hpp — Protocol Buffers (proto3) from the reflection.
//
// A reflected record is a message. User documentation: docs/protobuf.md.
//
// The mapping (psio::emit_protobuf writes the same as `.proto` text):
//
//   bool                               bool
//   int8 / int16 / int32               int32    pb_sint: sint32   pb_fixed: sfixed32
//   uint8 / uint16 / uint32 / char     uint32                     pb_fixed: fixed32
//   int64                              int64    pb_sint: sint64   pb_fixed: sfixed64
//   uint64                             uint64                     pb_fixed: fixed64
//   float / double                     float / double
//   enum with PSIO_REFLECT_ENUM        enum (open: any value its type holds)
//   any other enum                     its underlying integer
//   std::string                        string (UTF-8, checked when read)
//   std::vector<char | unsigned char | std::byte>     bytes
//   std::array<char | unsigned char | std::byte, N>   bytes of exactly N
//                                      (all zero as a member: not written)
//   reflected record                   message
//   std::optional<X>                   optional X (explicit presence)
//   std::vector<X> / std::set<X>       repeated X (packed for scalars)
//   std::map<K, V>                     map<K, V>
//   std::variant<A...>                 oneof (std::monostate first: none set)
//
// Field numbers: psio/detail/field_number.hpp (`field<N>`, else the 1-based
// declaration position). A variant member takes one number per alternative,
// counting up from its own. Numbers are checked at compile time.
//
// Bytes: what the official C++ implementation writes for the same value
// with deterministic serialisation — fields in ascending number order,
// proto3 implicit presence (a scalar / string / bytes member that is zero
// / empty is not written; -0.0 is), packed repeated scalars, map entries
// in key order with both key and value written. A record member that is
// not optional is written unless it is all zero (it encodes to nothing).
//
// Reading: any field order, packed or unpacked repeated scalars, the last
// of a repeated singular field (message fields merge), unknown fields and
// fields of another wire type skipped (not kept), groups skipped. An absent
// member is the proto3 zero value — NOT the C++ member initialiser. One
// reader serves validate and decode, so they accept the same bytes; it is
// safe on arbitrary bytes and allocates only in proportion to the input.

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/cpo.hpp>
#include <psio/detail/field_number.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/reflect.hpp>
#include <psio/value_contract.hpp>
#include <psio/varint/leb128.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio
{
   namespace detail::pb_impl
   {
      enum class int_enc : std::uint8_t { plain, fixed, zigzag };

      namespace wt
      {
         inline constexpr std::uint8_t varint  = 0;
         inline constexpr std::uint8_t fixed64 = 1;
         inline constexpr std::uint8_t len     = 2;
         inline constexpr std::uint8_t sgroup  = 3;
         inline constexpr std::uint8_t egroup  = 4;
         inline constexpr std::uint8_t fixed32 = 5;
      }  // namespace wt

      // Field numbers protobuf allows.
      inline constexpr std::uint32_t max_field_number = (1u << 29) - 1;
      constexpr bool valid_field_number(std::uint64_t n) noexcept
      {
         return n >= 1 && n <= max_field_number && !(n >= 19000 && n <= 19999);
      }
      // A message is at most 2 GiB - 1 (lengths are int32 in every
      // implementation).
      inline constexpr std::size_t max_message_size = 0x7fffffff;

      // ── Type classification ─────────────────────────────────────────
      template <typename E>
      inline constexpr bool is_byte_v = std::is_same_v<E, char> ||
                                        std::is_same_v<E, unsigned char> ||
                                        std::is_same_v<E, std::byte>;

      template <typename T>
      inline constexpr bool is_optional_v = false;
      template <typename X>
      inline constexpr bool is_optional_v<std::optional<X>> = true;

      template <typename T>
      inline constexpr bool is_vector_v = false;
      template <typename X, typename A>
      inline constexpr bool is_vector_v<std::vector<X, A>> = true;

      template <typename T>
      inline constexpr bool is_set_v = false;
      template <typename X, typename A>
      inline constexpr bool is_set_v<std::set<X, std::less<X>, A>> = true;

      template <typename T>
      inline constexpr bool is_map_v = false;
      template <typename K, typename V, typename A>
      inline constexpr bool is_map_v<std::map<K, V, std::less<K>, A>> = true;

      template <typename T>
      inline constexpr bool is_variant_v = false;
      template <typename... A>
      inline constexpr bool is_variant_v<std::variant<A...>> = true;

      template <typename T>
      inline constexpr bool is_byte_array_v = false;
      template <typename E, std::size_t N>
      inline constexpr bool is_byte_array_v<std::array<E, N>> = is_byte_v<E>;

      template <typename T>
      inline constexpr bool is_byte_vector_v = false;
      template <typename E, typename A>
      inline constexpr bool is_byte_vector_v<std::vector<E, A>> = is_byte_v<E>;

      template <typename T>
      inline constexpr bool is_bytes_v = is_byte_vector_v<T> || is_byte_array_v<T>;

      template <typename T>
      inline constexpr bool is_string_v =
         std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

      template <typename T>
      inline constexpr bool is_scalar_v =
         (std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, float> ||
          std::is_same_v<T, double>) &&
         sizeof(T) <= 8;

      // A value with one wire form: what an optional, a list element, a map
      // key / value or a oneof alternative may be.
      template <typename T>
      inline constexpr bool is_single_v =
         is_scalar_v<T> || is_string_v<T> || is_bytes_v<T> || Reflected<T>;

      // A repeated member: a list or set that is not `bytes`.
      template <typename T>
      inline constexpr bool is_repeated_v = (is_vector_v<T> && !is_byte_vector_v<T>) || is_set_v<T>;

      // ── Scalars ─────────────────────────────────────────────────────
      //
      // The integer a scalar is on the wire, as 64 bits (a varint's value,
      // or the bits of a fixed field), and back.

      // The integer type a scalar is read as: an enum's underlying type,
      // `char` as unsigned.
      template <typename S>
      struct int_of
      {
         using type = S;
      };
      template <typename S>
         requires std::is_enum_v<S>
      struct int_of<S>
      {
         using type = std::underlying_type_t<S>;
      };
      template <>
      struct int_of<char>
      {
         using type = unsigned char;
      };
      template <typename S>
      using int_of_t = typename int_of<S>::type;

      template <typename S, int_enc E>
      inline constexpr std::uint8_t scalar_wire_v =
         std::is_same_v<S, float>    ? wt::fixed32
         : std::is_same_v<S, double> ? wt::fixed64
         : E != int_enc::fixed       ? wt::varint
         : sizeof(S) == 8            ? wt::fixed64
                                     : wt::fixed32;

      template <int_enc E, typename S>
      constexpr std::uint64_t scalar_bits(S v) noexcept
      {
         if constexpr (std::is_same_v<S, bool>)
            return v ? 1 : 0;
         else if constexpr (std::is_same_v<S, float>)
            return std::bit_cast<std::uint32_t>(v);
         else if constexpr (std::is_same_v<S, double>)
            return std::bit_cast<std::uint64_t>(v);
         else
         {
            using N = int_of_t<S>;
            if constexpr (::psio::ReflectedEnum<S>)
            {
               static_assert(sizeof(N) <= 4,
                             "psio::protobuf: a reflected enum is 32 bits or fewer");
               // A protobuf enum is an int32: one over uint32 is reinterpreted.
               // (An enum without PSIO_REFLECT_ENUM is its integer.)
               const auto x = static_cast<std::int32_t>(static_cast<N>(v));
               return static_cast<std::uint64_t>(static_cast<std::int64_t>(x));
            }
            else if constexpr (std::is_signed_v<N>)
            {
               const auto x = static_cast<std::int64_t>(static_cast<N>(v));
               if constexpr (E == int_enc::zigzag)
                  return (static_cast<std::uint64_t>(x) << 1) ^ static_cast<std::uint64_t>(x >> 63);
               else if constexpr (E == int_enc::fixed && sizeof(N) < 8)
                  return static_cast<std::uint32_t>(x);
               else
                  return static_cast<std::uint64_t>(x);
            }
            else
               return static_cast<std::uint64_t>(static_cast<N>(v));
         }
      }

      // False: the value does not fit the C++ type.
      template <int_enc E, typename S>
      constexpr bool scalar_from_bits(std::uint64_t bits, S& out) noexcept
      {
         if constexpr (std::is_same_v<S, bool>)
            out = bits != 0;
         else if constexpr (std::is_same_v<S, float>)
            out = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
         else if constexpr (std::is_same_v<S, double>)
            out = std::bit_cast<double>(bits);
         else
         {
            using N = int_of_t<S>;
            if constexpr (sizeof(N) == 8)
            {
               if constexpr (E == int_enc::zigzag)
                  bits = (bits >> 1) ^ (0 - (bits & 1));
               out = static_cast<S>(static_cast<N>(bits));
            }
            else
            {
               // 32 bits on the wire: a longer varint is truncated.
               auto u = static_cast<std::uint32_t>(bits);
               if constexpr (E == int_enc::zigzag)
                  u = (u >> 1) ^ (0 - (u & 1));
               if constexpr (std::is_signed_v<N>)
               {
                  const auto x = static_cast<std::int32_t>(u);
                  if constexpr (sizeof(N) < 4)
                     if (x < std::numeric_limits<N>::min() || x > std::numeric_limits<N>::max())
                        return false;
                  out = static_cast<S>(static_cast<N>(x));
               }
               else
               {
                  if constexpr (sizeof(N) < 4)
                     if (u > std::numeric_limits<N>::max())
                        return false;
                  out = static_cast<S>(static_cast<N>(u));
               }
            }
         }
         return true;
      }

      inline std::size_t varint_size(std::uint64_t v) noexcept
      {
         return ::psio::varint::leb128::scalar::size_u64(v);
      }

      // The bytes of a tag: a field number is below 2^29.
      constexpr std::size_t tag_size(std::uint32_t number) noexcept
      {
         return number < (1u << 4) ? 1 : number < (1u << 11) ? 2 : number < (1u << 18) ? 3 : number < (1u << 25) ? 4 : 5;
      }

      // ── The fields of a message ─────────────────────────────────────
      //
      // One slot per field number: a member, or one alternative of a
      // variant member. In ascending number order (the order written).

      inline constexpr std::uint32_t no_alt = 0xffffffffu;

      struct slot
      {
         std::uint32_t number = 0;
         std::uint32_t member = 0;
         std::uint32_t alt    = no_alt;
      };

      template <typename V>
      inline constexpr std::size_t leading_monostate_v = 0;
      template <typename... A>
      inline constexpr std::size_t leading_monostate_v<std::variant<std::monostate, A...>> = 1;

      template <typename F>
      inline constexpr std::size_t slot_count_v = 1;
      template <typename... A>
      inline constexpr std::size_t slot_count_v<std::variant<A...>> =
         sizeof...(A) - leading_monostate_v<std::variant<A...>>;

      template <typename T, std::size_t I>
      using member_t = std::remove_cvref_t<typename ::psio::reflect<T>::template member_type<I>>;

      template <typename T>
      inline constexpr std::size_t total_slots_v = []<std::size_t... Is>(std::index_sequence<Is...>) {
         return (slot_count_v<member_t<T, Is>> + ... + std::size_t{0});
      }(std::make_index_sequence<::psio::reflect<T>::member_count>{});

      template <typename T>
      inline constexpr auto slots_v = [] {
         std::array<slot, total_slots_v<T>> out{};
         std::size_t                        n = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            const auto add = [&](std::size_t member, std::size_t count, std::size_t first_alt,
                                 bool variant, std::uint32_t number) {
               for (std::size_t j = 0; j < count; ++j)
                  out[n++] = slot{number + static_cast<std::uint32_t>(j),
                                  static_cast<std::uint32_t>(member),
                                  variant ? static_cast<std::uint32_t>(first_alt + j) : no_alt};
            };
            (add(Is, slot_count_v<member_t<T, Is>>, leading_monostate_v<member_t<T, Is>>,
                 is_variant_v<member_t<T, Is>>, ::psio::detail::field_number_v<T, Is>),
             ...);
         }(std::make_index_sequence<::psio::reflect<T>::member_count>{});
         for (std::size_t i = 1; i < n; ++i)
            for (std::size_t j = i; j > 0 && out[j].number < out[j - 1].number; --j)
            {
               const slot t = out[j];
               out[j]       = out[j - 1];
               out[j - 1]   = t;
            }
         return out;
      }();

      template <typename T>
      inline constexpr bool slots_valid_v = [] {
         const auto& s = slots_v<T>;
         for (std::size_t i = 0; i < s.size(); ++i)
         {
            if (!valid_field_number(s[i].number))
               return false;
            if (i && s[i].number == s[i - 1].number)
               return false;
         }
         return true;
      }();

      template <typename T, std::size_t I>
      inline constexpr int_enc member_enc_v = [] {
         using R          = ::psio::reflect<T>;
         using FieldT     = typename R::template member_type<I>;
         constexpr auto P = R::template member_pointer<I>;
         using Anns =
            std::remove_cvref_t<decltype(::psio::effective_annotations_for_v<T, FieldT, P>)>;
         if constexpr (::psio::has_spec_v<::psio::pb_fixed_spec, Anns>)
            return int_enc::fixed;
         else if constexpr (::psio::has_spec_v<::psio::pb_sint_spec, Anns>)
            return int_enc::zigzag;
         else
            return int_enc::plain;
      }();

      // The integer an annotation of a member speaks of: the member, or
      // what an optional / list / set of it holds.
      template <typename F>
      struct enc_leaf
      {
         using type = F;
      };
      template <typename X>
      struct enc_leaf<std::optional<X>>
      {
         using type = X;
      };
      template <typename X, typename A>
      struct enc_leaf<std::vector<X, A>>
      {
         using type = X;
      };
      template <typename X, typename A>
      struct enc_leaf<std::set<X, std::less<X>, A>>
      {
         using type = X;
      };

      template <typename F, int_enc E>
      inline constexpr bool enc_applies_v =
         E == int_enc::plain ||
         (std::is_integral_v<typename enc_leaf<F>::type> &&
          !std::is_same_v<typename enc_leaf<F>::type, bool> &&
          (E == int_enc::fixed || std::is_signed_v<int_of_t<typename enc_leaf<F>::type>>));

      template <typename X, int_enc E>
      inline constexpr std::uint8_t single_wire_v = is_scalar_v<X> ? scalar_wire_v<X, E> : wt::len;

      template <typename T>
      constexpr void check_message() noexcept
      {
         static_assert(Reflected<T>, "psio::protobuf: a message is a reflected record");
         static_assert(slots_valid_v<T>,
                       "psio::protobuf: field numbers must be distinct, within 1..536870911 and "
                       "outside 19000..19999. A std::variant member takes one number per "
                       "alternative, counting up from its own: give the members after it a "
                       "field<N>");
      }

      // ── Sizes ───────────────────────────────────────────────────────

      template <typename T>
      std::size_t message_size(const T& v) noexcept;

      // A value without its tag (with its length where it has one).
      template <int_enc E, typename X>
      std::size_t payload_size(const X& v) noexcept
      {
         if constexpr (is_scalar_v<X>)
         {
            if constexpr (scalar_wire_v<X, E> == wt::fixed32)
               return 4;
            else if constexpr (scalar_wire_v<X, E> == wt::fixed64)
               return 8;
            else if constexpr (std::is_same_v<X, bool>)
               return 1;
            else
               return varint_size(scalar_bits<E>(v));
         }
         else if constexpr (is_string_v<X> || is_bytes_v<X>)
            return varint_size(v.size()) + v.size();
         else
         {
            static_assert(Reflected<X>,
                          "psio::protobuf: unsupported type (docs/protobuf.md, \"The mapping\")");
            const std::size_t n = message_size(v);
            return varint_size(n) + n;
         }
      }

      // The zero value of a fixed-size bytes member (not written).
      template <typename A>
      bool all_zero(const A& a) noexcept
      {
         for (const auto b : a)
            if (b != typename A::value_type{})
               return false;
         return true;
      }

      template <int_enc E, typename F>
      std::size_t field_size(const F& v, std::uint32_t number) noexcept
      {
         const std::size_t tag = tag_size(number);
         if constexpr (is_optional_v<F>)
         {
            using X = typename F::value_type;
            static_assert(is_single_v<X>,
                          "psio::protobuf: std::optional holds a scalar, string, bytes or record");
            return v ? tag + payload_size<E>(*v) : 0;
         }
         else if constexpr (is_byte_array_v<F>)
            return all_zero(v) ? 0 : tag + payload_size<E>(v);
         else if constexpr (is_scalar_v<F>)
            return scalar_bits<E>(v) == 0 ? 0 : tag + payload_size<E>(v);
         else if constexpr (is_string_v<F> || is_bytes_v<F>)
            return v.empty() ? 0 : tag + payload_size<E>(v);
         else if constexpr (is_repeated_v<F>)
         {
            using X = typename F::value_type;
            static_assert(is_single_v<X>,
                          "psio::protobuf: a list holds scalars, strings, bytes or records");
            if (v.empty())
               return 0;
            if constexpr (is_scalar_v<X>)
            {
               std::size_t payload = 0;
               if constexpr (scalar_wire_v<X, E> == wt::fixed32)
                  payload = 4 * v.size();
               else if constexpr (scalar_wire_v<X, E> == wt::fixed64)
                  payload = 8 * v.size();
               else if constexpr (std::is_same_v<X, bool>)
                  payload = v.size();
               else
                  for (const auto& x : v)
                     payload += varint_size(scalar_bits<E>(static_cast<X>(x)));
               return tag + varint_size(payload) + payload;
            }
            else
            {
               std::size_t total = 0;
               for (const auto& x : v)
                  total += tag + payload_size<E>(x);
               return total;
            }
         }
         else if constexpr (is_map_v<F>)
         {
            using K = typename F::key_type;
            using V = typename F::mapped_type;
            static_assert((std::is_integral_v<K> || std::is_same_v<K, std::string>) &&
                             !is_byte_v<K>,
                          "psio::protobuf: a map's key is an integer, a bool or a string");
            static_assert(is_single_v<V>,
                          "psio::protobuf: a map's value is a scalar, string, bytes or record");
            std::size_t total = 0;
            for (const auto& [key, value] : v)
            {
               const std::size_t entry =
                  2 + payload_size<int_enc::plain>(key) + payload_size<int_enc::plain>(value);
               total += tag + varint_size(entry) + entry;
            }
            return total;
         }
         else
         {
            // A record that is all zero is not written, as any member that
            // is not optional.
            static_assert(Reflected<F>,
                          "psio::protobuf: unsupported type (docs/protobuf.md, \"The mapping\")");
            const std::size_t n = message_size(v);
            return n ? tag + varint_size(n) + n : 0;
         }
      }

      template <typename T, std::size_t K>
      std::size_t slot_size(const T& v) noexcept
      {
         constexpr slot s = slots_v<T>[K];
         constexpr auto I = static_cast<std::size_t>(s.member);
         using F          = member_t<T, I>;
         constexpr auto E = member_enc_v<T, I>;
         const F&       m = v.*(::psio::reflect<T>::template member_pointer<I>);
         if constexpr (is_variant_v<F>)
         {
            using A = std::variant_alternative_t<s.alt, F>;
            static_assert(is_single_v<A>,
                          "psio::protobuf: a variant's alternatives are scalars, strings, bytes "
                          "or records (std::monostate first: none)");
            static_assert(E == int_enc::plain,
                          "psio::protobuf: pb_fixed / pb_sint do not apply to a variant");
            if (const A* a = std::get_if<s.alt>(&m))
               return tag_size(s.number) + payload_size<E>(*a);
            return 0;
         }
         else
         {
            static_assert(enc_applies_v<F, E>,
                          "psio::protobuf: pb_fixed is for integers, pb_sint for signed integers "
                          "(a member, or the elements of an optional / list / set)");
            return field_size<E>(m, s.number);
         }
      }

      template <typename T>
      std::size_t message_size(const T& v) noexcept
      {
         check_message<T>();
         return [&]<std::size_t... Ks>(std::index_sequence<Ks...>) {
            return (slot_size<T, Ks>(v) + ... + std::size_t{0});
         }(std::make_index_sequence<total_slots_v<T>>{});
      }

      // ── Writing, back to front ──────────────────────────────────────
      //
      // The last byte first: a length is known when its content has been
      // written, so one walk of the value suffices after the sizing walk.

      //
      // The writer does not test for room: the buffer is exactly what the
      // sizing walk of the same value computed, and encode_into compares the
      // two when the walk is done.
      struct back_writer
      {
         char* base;
         char* p;

         void bytes(const void* data, std::size_t n) noexcept
         {
            if (n)
            {
               p -= n;
               std::memcpy(p, data, n);
            }
         }
         void varint(std::uint64_t v) noexcept
         {
            if (v < 0x80)
            {
               *--p = static_cast<char>(v);
               return;
            }
            const std::size_t n = varint_size(v);
            p -= n;
            char* q = p;
            while (v >= 0x80)
            {
               *q++ = static_cast<char>(v | 0x80);
               v >>= 7;
            }
            *q = static_cast<char>(v);
         }
         void fixed32(std::uint32_t v) noexcept
         {
            if constexpr (std::endian::native == std::endian::big)
               v = std::byteswap(v);
            bytes(&v, 4);
         }
         void fixed64(std::uint64_t v) noexcept
         {
            if constexpr (std::endian::native == std::endian::big)
               v = std::byteswap(v);
            bytes(&v, 8);
         }
         void tag(std::uint32_t number, std::uint8_t wire) noexcept
         {
            varint((static_cast<std::uint64_t>(number) << 3) | wire);
         }
      };

      template <typename T>
      void write_message(back_writer& w, const T& v) noexcept;

      template <int_enc E, typename X>
      void write_payload(back_writer& w, const X& v) noexcept
      {
         if constexpr (is_scalar_v<X>)
         {
            if constexpr (scalar_wire_v<X, E> == wt::fixed32)
               w.fixed32(static_cast<std::uint32_t>(scalar_bits<E>(v)));
            else if constexpr (scalar_wire_v<X, E> == wt::fixed64)
               w.fixed64(scalar_bits<E>(v));
            else
               w.varint(scalar_bits<E>(v));
         }
         else if constexpr (is_string_v<X> || is_bytes_v<X>)
         {
            w.bytes(v.data(), v.size());
            w.varint(v.size());
         }
         else
         {
            const char* end = w.p;
            write_message(w, v);
            w.varint(static_cast<std::uint64_t>(end - w.p));
         }
      }

      template <int_enc E, typename X>
      void write_single(back_writer& w, const X& v, std::uint32_t number) noexcept
      {
         write_payload<E>(w, v);
         w.tag(number, single_wire_v<X, E>);
      }

      // A list of scalars whose memory is their wire form.
      template <typename X, int_enc E>
      inline constexpr bool memcpy_packed_v =
         std::endian::native == std::endian::little && !std::is_same_v<X, bool> &&
         !std::is_enum_v<X> &&
         ((scalar_wire_v<X, E> == wt::fixed32 && sizeof(X) == 4) ||
          (scalar_wire_v<X, E> == wt::fixed64 && sizeof(X) == 8));

      template <int_enc E, typename F>
      void write_field(back_writer& w, const F& v, std::uint32_t number) noexcept
      {
         if constexpr (is_optional_v<F>)
         {
            if (v)
               write_single<E>(w, *v, number);
         }
         else if constexpr (is_byte_array_v<F>)
         {
            if (!all_zero(v))
               write_single<E>(w, v, number);
         }
         else if constexpr (is_scalar_v<F>)
         {
            if (scalar_bits<E>(v) != 0)
               write_single<E>(w, v, number);
         }
         else if constexpr (is_string_v<F> || is_bytes_v<F>)
         {
            if (!v.empty())
               write_single<E>(w, v, number);
         }
         else if constexpr (is_repeated_v<F>)
         {
            using X = typename F::value_type;
            if (v.empty())
               return;
            if constexpr (is_scalar_v<X>)
            {
               const char* end = w.p;
               if constexpr (is_vector_v<F> && memcpy_packed_v<X, E>)
                  w.bytes(v.data(), v.size() * sizeof(X));
               else
                  for (auto at = v.rbegin(); at != v.rend(); ++at)
                     write_payload<E>(w, static_cast<X>(*at));
               w.varint(static_cast<std::uint64_t>(end - w.p));
               w.tag(number, wt::len);
            }
            else
               for (auto at = v.rbegin(); at != v.rend(); ++at)
                  write_single<E>(w, *at, number);
         }
         else if constexpr (is_map_v<F>)
         {
            for (auto at = v.rbegin(); at != v.rend(); ++at)
            {
               const char* end = w.p;
               write_single<int_enc::plain>(w, at->second, 2);
               write_single<int_enc::plain>(w, at->first, 1);
               w.varint(static_cast<std::uint64_t>(end - w.p));
               w.tag(number, wt::len);
            }
         }
         else
         {
            const char* end = w.p;
            write_message(w, v);
            if (w.p != end)
            {
               w.varint(static_cast<std::uint64_t>(end - w.p));
               w.tag(number, wt::len);
            }
         }
      }

      template <typename T, std::size_t K>
      void write_slot(back_writer& w, const T& v) noexcept
      {
         constexpr slot s = slots_v<T>[K];
         constexpr auto I = static_cast<std::size_t>(s.member);
         using F          = member_t<T, I>;
         constexpr auto E = member_enc_v<T, I>;
         const F&       m = v.*(::psio::reflect<T>::template member_pointer<I>);
         if constexpr (is_variant_v<F>)
         {
            if (const auto* a = std::get_if<s.alt>(&m))
               write_single<E>(w, *a, s.number);
         }
         else
            write_field<E>(w, m, s.number);
      }

      template <typename T>
      void write_message(back_writer& w, const T& v) noexcept
      {
         constexpr std::size_t S = total_slots_v<T>;
         [&]<std::size_t... Ks>(std::index_sequence<Ks...>) {
            (write_slot<T, S - 1 - Ks>(w, v), ...);
         }(std::make_index_sequence<S>{});
      }

      template <typename T>
      void encode_into(const T& v, std::vector<char>& sink)
      {
         const std::size_t total = message_size(v);
         if (total > max_message_size)
            ::psio::codec_abort(codec_error{"protobuf: a message exceeds 2 GiB", 0, "protobuf"});
         const std::size_t before = sink.size();
         sink.resize(before + total);
         back_writer w{sink.data() + before, sink.data() + before + total};
         write_message(w, v);
         if (w.p != w.base)
            ::psio::codec_abort(
               codec_error{"protobuf: the value changed while it was written", 0, "protobuf"});
      }

      // ── The zero value ──────────────────────────────────────────────
      //
      // What an absent field reads as. Not the member initialiser.
      template <typename X>
      void clear(X& v)
      {
         if constexpr (is_scalar_v<X>)
            v = static_cast<X>(0);
         else if constexpr (is_byte_array_v<X>)
            v.fill(typename X::value_type{});
         else if constexpr (is_optional_v<X>)
            v.reset();
         else if constexpr (is_variant_v<X>)
         {
            v.template emplace<0>();
            if constexpr (!leading_monostate_v<X>)
               clear(std::get<0>(v));
         }
         else if constexpr (Reflected<X>)
         {
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (clear(v.*(::psio::reflect<X>::template member_pointer<Is>)), ...);
            }(std::make_index_sequence<::psio::reflect<X>::member_count>{});
         }
         else
            v.clear();
      }

      // ── UTF-8 ───────────────────────────────────────────────────────
      inline bool valid_utf8(const char* data, std::size_t n) noexcept
      {
         const auto* p   = reinterpret_cast<const unsigned char*>(data);
         const auto* end = p + n;
         while (p < end)
         {
            if (end - p >= 8)
            {
               std::uint64_t word;
               std::memcpy(&word, p, 8);
               if (!(word & 0x8080808080808080ull))
               {
                  p += 8;
                  continue;
               }
            }
            const unsigned c = *p;
            if (c < 0x80)
            {
               ++p;
               continue;
            }
            const std::size_t left = static_cast<std::size_t>(end - p);
            if (c >= 0xc2 && c <= 0xdf)
            {
               if (left < 2 || (p[1] & 0xc0) != 0x80)
                  return false;
               p += 2;
            }
            else if (c >= 0xe0 && c <= 0xef)
            {
               if (left < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
                  return false;
               if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f))
                  return false;
               p += 3;
            }
            else if (c >= 0xf0 && c <= 0xf4)
            {
               if (left < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 ||
                   (p[3] & 0xc0) != 0x80)
                  return false;
               if ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f))
                  return false;
               p += 4;
            }
            else
               return false;
         }
         return true;
      }

      // ── Reading ─────────────────────────────────────────────────────
      //
      // `out` null: the bytes are checked and nothing is stored. Every read
      // is bounded by the `end` of the enclosing message.

      struct reader
      {
         const char*   base;
         const char*   p;
         std::uint32_t max_depth;
         std::uint32_t depth  = 0;
         const char*   err    = nullptr;
         std::size_t   err_at = 0;

         bool fail(const char* what) noexcept
         {
            if (!err)
            {
               err    = what;
               err_at = static_cast<std::size_t>(p - base);
            }
            return false;
         }

         // A varint of at most `max_bytes` bytes (10: 64 bits, whose last
         // byte holds one bit; 5: 32 bits, whose last byte holds four).
         bool varint(const char* end, std::uint64_t& v) noexcept
         {
            if (p < end && static_cast<unsigned char>(*p) < 0x80) [[likely]]
            {
               v = static_cast<unsigned char>(*p++);
               return true;
            }
            if (end - p >= 2 && static_cast<unsigned char>(p[1]) < 0x80)
            {
               v = (static_cast<unsigned char>(p[0]) & 0x7fu) |
                   (static_cast<std::uint64_t>(static_cast<unsigned char>(p[1])) << 7);
               p += 2;
               return true;
            }
            std::uint64_t out = 0;
            const char*   at  = p;
            for (unsigned shift = 0; shift < 70; shift += 7)
            {
               if (at == end)
                  return fail("protobuf: truncated varint");
               const auto b = static_cast<unsigned char>(*at++);
               if (shift == 63 && b > 1)
                  return fail("protobuf: varint exceeds 64 bits");
               out |= static_cast<std::uint64_t>(b & 0x7f) << shift;
               if (b < 0x80)
               {
                  p = at;
                  v = out;
                  return true;
               }
            }
            return fail("protobuf: varint exceeds 64 bits");
         }
         bool varint32(const char* end, std::uint32_t& v, const char* too_long) noexcept
         {
            if (p < end && static_cast<unsigned char>(*p) < 0x80) [[likely]]
            {
               v = static_cast<unsigned char>(*p++);
               return true;
            }
            std::uint32_t out = 0;
            const char*   at  = p;
            for (unsigned shift = 0; shift < 35; shift += 7)
            {
               if (at == end)
                  return fail("protobuf: truncated varint");
               const auto b = static_cast<unsigned char>(*at++);
               if (shift == 28 && b > 0x0f)
                  return fail(too_long);
               out |= static_cast<std::uint32_t>(b & 0x7f) << shift;
               if (b < 0x80)
               {
                  p = at;
                  v = out;
                  return true;
               }
            }
            return fail(too_long);
         }
         bool tag(const char* end, std::uint32_t& t) noexcept
         {
            return varint32(end, t, "protobuf: tag exceeds 32 bits");
         }
         // A length, and that many bytes are there.
         bool length(const char* end, std::size_t& n) noexcept
         {
            std::uint32_t v = 0;
            if (!varint32(end, v, "protobuf: length exceeds the input"))
               return false;
            if (v > static_cast<std::size_t>(end - p))
               return fail("protobuf: length exceeds the input");
            n = v;
            return true;
         }
         bool fixed32(const char* end, std::uint64_t& v) noexcept
         {
            if (end - p < 4)
               return fail("protobuf: truncated fixed32");
            std::uint32_t x;
            std::memcpy(&x, p, 4);
            if constexpr (std::endian::native == std::endian::big)
               x = std::byteswap(x);
            p += 4;
            v = x;
            return true;
         }
         bool fixed64(const char* end, std::uint64_t& v) noexcept
         {
            if (end - p < 8)
               return fail("protobuf: truncated fixed64");
            std::uint64_t x;
            std::memcpy(&x, p, 8);
            if constexpr (std::endian::native == std::endian::big)
               x = std::byteswap(x);
            p += 8;
            v = x;
            return true;
         }
         bool enter() noexcept
         {
            if (depth >= max_depth)
               return fail("protobuf: nested too deeply");
            ++depth;
            return true;
         }
         void leave() noexcept { --depth; }
      };

      // A field nothing reads. `number` is the field's (a group ends with
      // its own number).
      inline bool skip_field(reader& r, const char* end, std::uint32_t number,
                             std::uint8_t wire) noexcept
      {
         std::uint64_t v = 0;
         std::size_t   n = 0;
         switch (wire)
         {
            case wt::varint:
               return r.varint(end, v);
            case wt::fixed64:
               return r.fixed64(end, v);
            case wt::fixed32:
               return r.fixed32(end, v);
            case wt::len:
               if (!r.length(end, n))
                  return false;
               r.p += n;
               return true;
            case wt::sgroup:
            {
               if (!r.enter())
                  return false;
               for (;;)
               {
                  std::uint32_t t = 0;
                  if (r.p == end)
                     return r.fail("protobuf: unterminated group");
                  if (!r.tag(end, t))
                     return false;
                  if ((t >> 3) == 0)
                     return r.fail("protobuf: field number 0");
                  if ((t & 7) == wt::egroup)
                  {
                     if ((t >> 3) != number)
                        return r.fail("protobuf: a group ends with another field number");
                     break;
                  }
                  if (!skip_field(r, end, t >> 3, static_cast<std::uint8_t>(t & 7)))
                     return false;
               }
               r.leave();
               return true;
            }
            case wt::egroup:
               return r.fail("protobuf: end of a group that was not begun");
            default:
               return r.fail("protobuf: unknown wire type");
         }
      }

      template <typename T>
      bool read_message(reader& r, const char* end, T* out);

      // A scalar from the wire type it is declared with.
      template <int_enc E, typename X>
      bool read_scalar(reader& r, const char* end, X* out)
      {
         std::uint64_t bits = 0;
         if constexpr (scalar_wire_v<X, E> == wt::fixed32)
         {
            if (!r.fixed32(end, bits))
               return false;
         }
         else if constexpr (scalar_wire_v<X, E> == wt::fixed64)
         {
            if (!r.fixed64(end, bits))
               return false;
         }
         else if (!r.varint(end, bits))
            return false;
         X tmp{};
         if (!scalar_from_bits<E>(bits, out ? *out : tmp))
            return r.fail("protobuf: a value does not fit its type");
         return true;
      }

      template <int_enc E, typename X>
      bool read_payload(reader& r, const char* end, X* out)
      {
         if constexpr (is_scalar_v<X>)
            return read_scalar<E>(r, end, out);
         else if constexpr (std::is_same_v<X, std::string>)
         {
            std::size_t n = 0;
            if (!r.length(end, n))
               return false;
            if (!valid_utf8(r.p, n))
               return r.fail("protobuf: a string is not UTF-8");
            if (out)
               out->assign(r.p, n);
            r.p += n;
            return true;
         }
         else if constexpr (is_bytes_v<X>)
         {
            std::size_t n = 0;
            if (!r.length(end, n))
               return false;
            if constexpr (is_byte_array_v<X>)
            {
               if (n != std::tuple_size_v<X>)
                  return r.fail("protobuf: bytes of a fixed size have another length");
            }
            else if (out)
               out->resize(n);
            if (out && n)
               std::memcpy(out->data(), r.p, n);
            r.p += n;
            return true;
         }
         else
         {
            static_assert(Reflected<X>, "psio::protobuf: this type cannot be read");
            std::size_t n = 0;
            if (!r.length(end, n) || !r.enter())
               return false;
            if (!read_message(r, r.p + n, out))
               return false;
            r.leave();
            return true;
         }
      }

      enum class got : std::uint8_t { failed, read, not_mine };

      inline got done(bool ok) noexcept { return ok ? got::read : got::failed; }

      template <typename C, typename X>
      void append(C& c, X&& x)
      {
         if constexpr (is_set_v<C>)
            c.insert(std::forward<X>(x));
         else
            c.push_back(std::forward<X>(x));
      }

      template <int_enc E, typename F>
      got read_field(reader& r, const char* end, std::uint8_t wire, F* out)
      {
         if constexpr (is_optional_v<F>)
         {
            using X = typename F::value_type;
            if (wire != single_wire_v<X, E>)
               return got::not_mine;
            if (out && !*out)
            {
               out->emplace();
               clear(**out);
            }
            return done(read_payload<E>(r, end, out ? &**out : nullptr));
         }
         else if constexpr (is_repeated_v<F>)
         {
            using X = typename F::value_type;
            if constexpr (is_scalar_v<X>)
            {
               if (wire == wt::len)
               {
                  std::size_t n = 0;
                  if (!r.length(end, n))
                     return got::failed;
                  const char* sub = r.p + n;
                  if constexpr (scalar_wire_v<X, E> != wt::varint)
                  {
                     constexpr std::size_t width = scalar_wire_v<X, E> == wt::fixed32 ? 4 : 8;
                     if (n % width)
                        return done(r.fail("protobuf: packed fixed values of a broken length"));
                     if constexpr (is_vector_v<F> && memcpy_packed_v<X, E>)
                     {
                        // The wire is the vector's memory.
                        if (out && n)
                        {
                           const std::size_t old = out->size();
                           out->resize(old + n / width);
                           std::memcpy(out->data() + old, r.p, n);
                        }
                        r.p = sub;
                        return got::read;
                     }
                     else if constexpr (is_vector_v<F>)
                     {
                        if (out)
                           out->reserve(out->size() + n / width);
                     }
                  }
                  while (r.p < sub)
                  {
                     X x{};
                     if (!read_scalar<E>(r, sub, &x))
                        return got::failed;
                     if (out)
                        append(*out, x);
                  }
                  return got::read;
               }
               if (wire != scalar_wire_v<X, E>)
                  return got::not_mine;
               X x{};
               if (!read_scalar<E>(r, end, &x))
                  return got::failed;
               if (out)
                  append(*out, x);
               return got::read;
            }
            else
            {
               if (wire != wt::len)
                  return got::not_mine;
               if (!out)
                  return done(read_payload<E>(r, end, static_cast<X*>(nullptr)));
               if constexpr (is_vector_v<F>)
               {
                  // In place: nothing is moved.
                  out->emplace_back();
                  clear(out->back());
                  return done(read_payload<E>(r, end, &out->back()));
               }
               X x{};
               clear(x);
               if (!read_payload<E>(r, end, &x))
                  return got::failed;
               append(*out, std::move(x));
               return got::read;
            }
         }
         else if constexpr (is_map_v<F>)
         {
            using K = typename F::key_type;
            using V = typename F::mapped_type;
            if (wire != wt::len)
               return got::not_mine;
            std::size_t n = 0;
            if (!r.length(end, n) || !r.enter())
               return got::failed;
            const char* sub = r.p + n;
            K           key{};
            V           value{};
            if (out)
               clear(value);
            while (r.p < sub)
            {
               std::uint32_t t = 0;
               if (!r.tag(sub, t))
                  return got::failed;
               const auto number = t >> 3;
               const auto w      = static_cast<std::uint8_t>(t & 7);
               if (number == 0)
                  return done(r.fail("protobuf: field number 0"));
               bool ok = true;
               if (number == 1 && w == single_wire_v<K, int_enc::plain>)
                  ok = read_payload<int_enc::plain>(r, sub, out ? &key : nullptr);
               else if (number == 2 && w == single_wire_v<V, int_enc::plain>)
                  ok = read_payload<int_enc::plain>(r, sub, out ? &value : nullptr);
               else
                  ok = skip_field(r, sub, number, w);
               if (!ok)
                  return got::failed;
            }
            r.leave();
            if (out)
               out->insert_or_assign(std::move(key), std::move(value));
            return got::read;
         }
         else
         {
            if (wire != single_wire_v<F, E>)
               return got::not_mine;
            return done(read_payload<E>(r, end, out));
         }
      }

      template <typename T, std::size_t K>
      got read_slot(reader& r, const char* end, std::uint8_t wire, T* out)
      {
         constexpr slot s = slots_v<T>[K];
         constexpr auto I = static_cast<std::size_t>(s.member);
         using F          = member_t<T, I>;
         constexpr auto E = member_enc_v<T, I>;
         F* m             = out ? &(out->*(::psio::reflect<T>::template member_pointer<I>)) : nullptr;
         if constexpr (is_variant_v<F>)
         {
            using A = std::variant_alternative_t<s.alt, F>;
            if (wire != single_wire_v<A, E>)
               return got::not_mine;
            if (m && m->index() != s.alt)
            {
               m->template emplace<s.alt>();
               clear(std::get<s.alt>(*m));
            }
            return done(read_payload<E>(r, end, m ? &std::get<s.alt>(*m) : nullptr));
         }
         else
            return read_field<E>(r, end, wire, m);
      }

      // Field number → reader, for a message whose numbers are small (the
      // usual case): one load and an indirect call per field. Otherwise the
      // numbers are compared one by one.
      template <typename T>
      inline constexpr std::uint32_t max_number_v =
         total_slots_v<T> ? slots_v<T>[total_slots_v<T> - 1].number : 0;

      template <typename T>
      inline constexpr bool dense_numbers_v = total_slots_v<T> > 4 && max_number_v<T> <= 255;

      template <typename T>
      using slot_reader = got (*)(reader&, const char*, std::uint8_t, T*);

      template <typename T>
      inline constexpr auto slot_table_v = [] {
         std::array<slot_reader<T>, dense_numbers_v<T> ? max_number_v<T> + 1 : 1> table{};
         if constexpr (dense_numbers_v<T>)
            [&]<std::size_t... Ks>(std::index_sequence<Ks...>) {
               ((table[slots_v<T>[Ks].number] = &read_slot<T, Ks>), ...);
            }(std::make_index_sequence<total_slots_v<T>>{});
         return table;
      }();

      template <typename T>
      bool read_message(reader& r, const char* end, T* out)
      {
         check_message<T>();
         while (r.p < end)
         {
            std::uint32_t t = 0;
            if (!r.tag(end, t))
               return false;
            const std::uint32_t number = t >> 3;
            const auto          wire   = static_cast<std::uint8_t>(t & 7);
            if (number == 0)
               return r.fail("protobuf: field number 0");
            got g = got::not_mine;
            if constexpr (dense_numbers_v<T>)
            {
               if (number <= max_number_v<T>)
                  if (const slot_reader<T> read = slot_table_v<T>[number])
                     g = read(r, end, wire, out);
            }
            else
               [&]<std::size_t... Ks>(std::index_sequence<Ks...>) {
                  (void)((number == slots_v<T>[Ks].number
                             ? (g = read_slot<T, Ks>(r, end, wire, out), true)
                             : false) ||
                         ...);
               }(std::make_index_sequence<total_slots_v<T>>{});
            if (g == got::failed)
               return false;
            if (g == got::not_mine && !skip_field(r, end, number, wire))
               return false;
         }
         return true;
      }

      template <typename T>
      codec_status read_root(std::span<const char> bytes, T* out, std::uint32_t max_depth)
      {
         if (bytes.size() > max_message_size)
            return codec_fail("protobuf: a message exceeds 2 GiB", 0, "protobuf");
         reader r{bytes.data(), bytes.data(), max_depth};
         if (out)
            clear(*out);
         if (!read_message(r, bytes.data() + bytes.size(), out))
            return codec_fail(r.err, static_cast<std::uint32_t>(r.err_at), "protobuf");
         return codec_ok();
      }

   }  // namespace detail::pb_impl

   struct protobuf_limits
   {
      // Messages nested inside the root message (map entries and skipped
      // groups count). The official implementations stop at 100.
      std::uint32_t max_depth = 100;
   };

   struct protobuf : format_tag_base<protobuf>
   {
      using preferred_presentation_category = ::psio::binary_category;

      using limits = protobuf_limits;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), protobuf, const T& v,
                             std::vector<char>& sink)
      {
         detail::pb_impl::encode_into(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), protobuf, const T& v)
      {
         std::vector<char> out;
         detail::pb_impl::encode_into(v, out);
         return out;
      }

      // The value of the bytes; bytes that are not a message of T end the
      // operation (psio::codec_abort). `from_protobuf` is this.
      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), protobuf, T*, std::span<const char> bytes)
      {
         T out{};
         if (auto st = read_checked(bytes, out, limits{}.max_depth); !st.ok())
            ::psio::codec_abort(st.error());
         return out;
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), protobuf, const T& v)
      {
         return detail::pb_impl::message_size(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), protobuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         return check<T>(bytes, limits{});
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>), protobuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         // Protobuf has no canonical form to hold bytes to.
         return check<T>(bytes, limits{});
      }

      // The reader, then what T declares about its values — member bounds,
      // utf8, order, invariants (value_contract.hpp) — on the value read. A
      // member may be written more than once (the last value of a scalar
      // wins, repeated members and messages merge), so the declarations are
      // checked on the finished value, not field by field.
      template <typename T>
      static codec_status read_checked(std::span<const char> bytes, T& out,
                                       std::uint32_t max_depth)
      {
         if (auto st = detail::pb_impl::read_root(bytes, &out, max_depth); !st.ok())
            return st;
         return ::psio::check_value_contract(out, "protobuf: member length_bound violated",
                                             "protobuf");
      }

      // Validate: the reader, storing nothing — unless T declares something
      // about its values, which takes the value to check.
      template <typename T>
      static codec_status check(std::span<const char> bytes, limits lim) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "protobuf"); !st.ok())
            return st;
         if constexpr (::psio::has_value_contract_v<T>)
         {
            T out{};
            return read_checked(bytes, out, lim.max_depth);
         }
         else
            return detail::pb_impl::read_root(bytes, static_cast<T*>(nullptr), lim.max_depth);
      }

      // Decode and validate in one pass (what `from_protobuf` runs).
      template <typename T>
      static codec_status decode_checked(std::span<const char> bytes, T& out, limits lim = {})
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "protobuf"); !st.ok())
            return st;
         return read_checked(bytes, out, lim.max_depth);
      }
   };

   PSIO_FORMAT_HELPERS(protobuf)

   template <typename T>
   [[nodiscard]] codec_status validate_protobuf(std::span<const char> bytes,
                                                protobuf::limits      lim) noexcept
   {
      return protobuf::check<T>(bytes, lim);
   }

   template <typename T>
   [[nodiscard]] T from_protobuf(std::span<const char> bytes, protobuf::limits lim)
   {
      T out{};
      if (auto st = protobuf::decode_checked(bytes, out, lim); !st.ok())
         ::psio::codec_abort(st.error());
      return out;
   }

}  // namespace psio
