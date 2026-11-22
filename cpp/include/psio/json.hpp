#pragma once
//
// psio/json.hpp — the JSON text formats: `psio::json` and `psio::ijson`.
//
// Both tags write to a `std::string` and read a `std::span<const char>` of
// UTF-8 text. They share one reflection-driven engine and differ in three
// rules only.
//
//   psio::json    RFC 8259. Every integer is a bare number, byte sequences
//                 are arrays of numbers, std::variant is `[index, value]`.
//
//   psio::ijson   RFC 7493 (I-JSON), the profile JavaScript reads without
//                 loss. A 64-bit integer is a bare number while its magnitude
//                 is at most 2^53 - 1 and a decimal string beyond that
//                 (`"18446744073709551615"`); 8/16/32-bit integers, bool and
//                 floats are always numbers. Byte sequences (std::vector / std::array
//                 of char, unsigned char, std::byte, and psio::byte_array<N>)
//                 are one UPPERCASE hex string. std::variant is
//                 `{"Name": value}` where Name is get_type_name of the
//                 alternative ("uint32", "string", the record's name, ...).
//
// Everything else is the same in both:
//
//   bool                      true / false
//   integers                  decimal
//   uint128, int128, uint256  decimal: a bare number under json, a string
//                             under ijson (always). Read from a decimal
//                             string, a bare number (ijson: |n| <= 2^53 - 1)
//                             or the older "0x..." string
//   enums                     with PSIO_REFLECT_ENUM: the enumerator's name
//                             ("active") for a listed value, the number for
//                             any other; read from "active", 2, "2" or
//                             "active:2" (which must agree). Without it: the
//                             underlying integer
//   float, double             shortest round-trip decimal (std::to_chars);
//                             NaN and the infinities have no text
//   std::string, utf8_string  "..." with JSON escaping
//   std::vector, std::array,
//   psio::bounded             [ ... ]
//   std::optional             null or the value
//   schema_types::Box<T>      the T it holds (transparent, as in pssz)
//   std::tuple                [ e0, e1, ... ] in order; tuple<> is []
//   reflected record          {"field": value, ...} in declaration order
//   chrono duration           its tick count, as an integer (time.hpp)
//   system_clock time_point   an RFC 3339 UTC string, "2026-11-22T10:15:30Z",
//                             with exactly the unit's fraction digits
//   text adapter              "..." holding the adapter's text
//                             (PSIO_ADAPTER(T, psio::text_category, ...), or a
//                             member-level psio::as<Tag>), for a type of any
//                             shape: the adapter replaces the shape's own text
//   transparent type          its member's text (transparent.hpp)
//   psio::nested<T>           the text of the T its bytes hold (nested.hpp);
//                             read as a T and stored as its canonical PSSZ
//                             encoding. Bytes that are not a T have no text
//
// Invariants (invariant.hpp). A value that violates its type's invariant has
// no text, and text that reads as such a value is rejected: every writer and
// every reader runs the check on each value of a type that has one (after a
// text adapter's own `validate`), innermost first, and reports the
// invariant's own status unchanged. A reader that only validates
// materialises those values to check them.
//
// Integer form per member: psio::json_number / psio::json_string and
// psio::ijson_number / psio::ijson_string (annotate.hpp) force a number or a
// string for one format.
//
// Writing fails for a value that has no JSON text: a string that is not
// valid UTF-8 (a std::string is raw bytes and may hold anything), a
// non-finite float, and under ijson a string holding a Unicode noncharacter. The
// fail-fast `encode` raises codec_exception (aborts when exceptions are
// disabled); `encode_json_checked` returns a codec_status instead.
//
// Reading. One typed reader defines the accepted set; `validate`,
// `validate_strict`, `decode_json_checked` and `decode` all run it. It is
// written against the JsonCursor concept (json_cursor.hpp) and runs on
// either syntax layer: the hand-written lexer, or simdjson when
// PSIO_HAVE_SIMDJSON is 1 (json_simdjson.hpp). `json_parser` selects one per
// call; the default takes the fastest for the document, and the choice
// never changes what is accepted:
//
//   - the text must be exactly one value of T's shape, with optional JSON
//     whitespace between tokens and nothing after it;
//   - integers match `0 | -?[1-9][0-9]*` (no leading zero, plus sign,
//     fraction or exponent) and must fit the type. An integer written as a
//     string must be exactly those characters between the quotes;
//   - under ijson a 64-bit integer is read from its string form (any
//     in-range value), and from a bare number when it lies in the range
//     JavaScript represents exactly (|n| <= 2^53 - 1). A larger bare number
//     is rejected: an I-JSON producer could not have written it without
//     loss. A member with a forced form accepts exactly that form;
//   - a hex string has an even number of hex digits of either case (and
//     exactly 2N for a fixed array);
//   - strings must be valid UTF-8, `\u` escapes must pair surrogates, raw
//     control characters are rejected; ijson also rejects noncharacters;
//   - arrays and tuples require their exact element count;
//   - a record accepts its members in any order. An unknown key is skipped
//     (its value must still be well-formed JSON). A missing member is an
//     error unless it is a std::optional, which reads as empty. A repeated
//     key is not an error in either format: each occurrence is read as it
//     is seen (so each must be well-typed) and the last one wins. The
//     writers never emit a repeated key;
//   - an ijson variant is an object with exactly one known alternative
//     name; a json variant is a two-element array with an index in range;
//   - nesting deeper than kMaxValidationDepth arrays/objects is rejected.
//
// The fail-fast `decode` raises the same errors as codec_exception. For
// psio::json only it keeps one historical leniency: a missing non-optional
// member is left value-initialized instead of being an error.
// Untrusted text goes through `validate` or `decode_json_checked`.

#include <psio/adapter.hpp>
#include <psio/cpo.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/detail/validate_depth.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/get_type_name.hpp>
#include <psio/json_cursor.hpp>
#include <psio/reflect.hpp>
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for, byte_array, ...

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#ifndef PSIO_HAVE_SIMDJSON
#define PSIO_HAVE_SIMDJSON 0
#endif
#if PSIO_HAVE_SIMDJSON
#include <psio/json_simdjson.hpp>
#endif

namespace psio::schema_types
{
   // Fwd decl: Box<T> (schema_ir.hpp) owns one T behind a pointer so that a
   // recursive type can hold itself. It is transparent in text as it is in
   // pssz: a Box<T> is written and read as its T.
   template <typename T>
   struct Box;
}  // namespace psio::schema_types

namespace psio {

   struct json;
   struct ijson;

   namespace detail::json_impl {

      // ── Helper traits ─────────────────────────────────────────────────────
      template <typename T>
      struct is_std_array : std::false_type
      {
      };
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_vector : std::false_type
      {
      };
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_optional : std::false_type
      {
      };
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_tuple : std::false_type
      {
      };
      template <typename... Ts>
      struct is_std_tuple<std::tuple<Ts...>> : std::true_type
      {
      };
      // A std::pair is the 2-tuple of its members: [a, b].
      template <typename A, typename B>
      struct is_std_tuple<std::pair<A, B>> : std::true_type
      {
      };

      template <typename T>
      struct is_utf8_string : std::false_type
      {
      };
      template <std::size_t N>
      struct is_utf8_string<::psio::utf8_string<N>> : std::true_type
      {
      };

      template <typename T>
      struct is_byte_array : std::false_type
      {
      };
      template <std::size_t N>
      struct is_byte_array<::psio::byte_array<N>> : std::true_type
      {
      };

      template <typename T>
      struct is_bounded : std::false_type
      {
      };
      template <typename E, std::size_t N>
      struct is_bounded<::psio::bounded<E, N>> : std::true_type
      {
      };

      using ::psio::detail::is_std_variant;

      template <typename T>
      struct is_box : std::false_type
      {
      };
      template <typename U>
      struct is_box<::psio::schema_types::Box<U>> : std::true_type
      {
         using elem = U;
      };

      template <typename T>
      struct is_bounded_bytes : std::false_type
      {
      };
      template <typename E, std::size_t N>
         requires(std::is_same_v<E, char> || std::is_same_v<E, unsigned char> ||
                  std::is_same_v<E, std::byte>)
      struct is_bounded_bytes<::psio::bounded<E, N>> : std::true_type
      {
      };

      // A transparent type (transparent.hpp) is its member, not a record.
      template <typename T>
      concept Record = ::psio::Reflected<T> && !std::is_enum_v<T> &&
                       !::psio::is_transparent_v<T>;

      // An element type whose sequences are byte sequences. signed char /
      // int8_t is a small integer, not a byte.
      template <typename E>
      inline constexpr bool is_byte_elem_v =
         std::is_same_v<E, char> || std::is_same_v<E, unsigned char> ||
         std::is_same_v<E, std::byte>;

      // byte_seq<T>::value — T is one hex string under ijson.
      template <typename T>
      struct byte_seq : std::false_type
      {
      };
      template <typename E, typename A>
         requires is_byte_elem_v<E>
      struct byte_seq<std::vector<E, A>> : std::true_type
      {
         static constexpr bool fixed = false;
      };
      template <typename E, std::size_t N>
         requires is_byte_elem_v<E>
      struct byte_seq<std::array<E, N>> : std::true_type
      {
         static constexpr bool fixed = true;
      };
      template <std::size_t N>
      struct byte_seq<::psio::byte_array<N>> : std::true_type
      {
         static constexpr bool fixed = true;
      };

      // Integers wider than 64 bits. They are decimal in JSON text, whatever
      // text adapter the type has for other consumers (ext_int.hpp's is the
      // "0x" form, still read here and available per member as as<hex_tag>).
      template <typename T>
      inline constexpr bool is_wide_int_v =
         std::is_same_v<T, ::psio::uint128> || std::is_same_v<T, ::psio::int128> ||
         std::is_same_v<T, ::psio::uint256>;

      // The type-level text adapter applies (member-level as<Tag> is
      // handled where the member is known).
      template <typename Fmt, typename T>
      inline constexpr bool dispatch_adapter_v =
         ::psio::format_should_dispatch_adapter_v<Fmt, T> && !is_wide_int_v<T>;

      // How an integer is written in the current position.
      enum class int_mode : std::uint8_t
      {
         natural,  // the format's rule
         number,   // forced by json_number / ijson_number
         string,   // forced by json_string / ijson_string
      };

      template <typename Fmt, typename Eff>
      constexpr int_mode field_mode(const Eff& eff) noexcept
      {
         if constexpr (Fmt::i_json)
         {
            if constexpr (::psio::has_spec_v<::psio::ijson_int_spec, Eff>)
               return std::get<::psio::ijson_int_spec>(eff).form ==
                            ::psio::int_text::string
                         ? int_mode::string
                         : int_mode::number;
         }
         else
         {
            if constexpr (::psio::has_spec_v<::psio::json_int_spec, Eff>)
               return std::get<::psio::json_int_spec>(eff).form ==
                            ::psio::int_text::string
                         ? int_mode::string
                         : int_mode::number;
         }
         (void)eff;
         return int_mode::natural;
      }

      // Integers of 64 bits and wider exceed the 53 bits a JSON number
      // carries through JavaScript.
      template <typename Fmt, int_mode IM, typename I>
      inline constexpr bool int_as_string_v =
         IM == int_mode::string ||
         (IM == int_mode::natural && Fmt::i_json && sizeof(I) >= 8);

      // Name of variant alternative I under ijson.
      template <typename V, std::size_t I>
      constexpr std::string_view alt_name() noexcept
      {
         return std::string_view{get_type_name(
            static_cast<const std::variant_alternative_t<I, V>*>(nullptr))};
      }

      // The alternative names of V, as a lookup table source.
      template <typename V>
      struct alt_names
      {
         static constexpr auto _names_array_ =
            []<std::size_t... Is>(std::index_sequence<Is...>) {
               return std::array<std::string_view, sizeof...(Is)>{alt_name<V, Is>()...};
            }(std::make_index_sequence<std::variant_size_v<V>>{});
      };

      template <typename V>
      constexpr bool alt_names_unique() noexcept
      {
         const auto& names = alt_names<V>::_names_array_;
         for (std::size_t a = 0; a < names.size(); ++a)
            for (std::size_t b = a + 1; b < names.size(); ++b)
               if (names[a] == names[b])
                  return false;
         return true;
      }

      // ── Encoding ──────────────────────────────────────────────────────────
      //
      // The writer owns the tail of the output string as a raw buffer: text
      // is written through a pointer after one capacity check per value
      // (`reserve`), and the string's size is set once at the end
      // (`finish`). While a writer is live, `s.size()` is the buffer's
      // extent, not the text's length; `n` is the length.

      struct writer
      {
         std::string&     s;
         std::string_view format;
         bool             fail_fast = true;
         std::string_view err{};
         std::size_t      n = s.size();  // bytes of `s` that are text
         // The status of a violated invariant, reported unchanged.
         ::psio::codec_error invariant{};
         bool                has_invariant = false;

         // Room for `k` more bytes; returns where they go.
         char* reserve(std::size_t k)
         {
            if (s.size() - n < k) [[unlikely]]
               grow(k);
            return s.data() + n;
         }
         // The bytes [reserve(), end) are now text.
         void commit(const char* end) noexcept
         {
            n = static_cast<std::size_t>(end - s.data());
         }
         void put(char c)
         {
            *reserve(1) = c;
            ++n;
         }
         void append(std::string_view text)
         {
            std::memcpy(reserve(text.size()), text.data(), text.size());
            n += text.size();
         }
         void append(const char* b, const char* e)
         {
            append(std::string_view{b, static_cast<std::size_t>(e - b)});
         }
         template <std::size_t N>
         void literal(const std::array<char, N>& text)
         {
            std::memcpy(reserve(N), text.data(), N);
            n += N;
         }
         // Ends the buffer phase: `s` is exactly the text.
         void finish() { s.resize(n); }

         void fail(std::string_view what)
         {
            if (fail_fast)
            {
               const auto at = static_cast<std::uint32_t>(n);
               finish();
               ::psio::codec_abort(::psio::codec_error{what, at, format});
            }
            if (err.empty())
               err = what;
         }
         // A violated invariant: its own error, not this format's.
         void fail(const ::psio::codec_error& error)
         {
            if (fail_fast)
            {
               finish();
               ::psio::codec_abort(error);
            }
            if (err.empty())
            {
               err           = error.what;
               invariant     = error;
               has_invariant = true;
            }
         }

        private:
         void grow(std::size_t k)
         {
            const std::size_t want = std::max({n + k, s.size() * 2, std::size_t{128}});
#if defined(__cpp_lib_string_resize_and_overwrite)
            s.resize_and_overwrite(want, [&](char*, std::size_t) { return want; });
#else
            s.resize(want);
#endif
         }
      };

      // Quotes and escapes sv. The text must be UTF-8; `nonchar` also
      // refuses Unicode noncharacters (I-JSON).
      inline void write_string(writer& w, std::string_view sv, bool nonchar)
      {
         // Worst case without escapes; each escape reserves again.
         char* out = w.reserve(sv.size() + 2);
         *out++    = '"';
         const auto* p   = reinterpret_cast<const unsigned char*>(sv.data());
         const auto* end = p + sv.size();
         while (true)
         {
            // Plain ASCII, eight bytes at a time.
            while (end - p >= 8)
            {
               std::uint64_t word;
               std::memcpy(&word, p, 8);
               if (string_special(word))
                  break;
               std::memcpy(out, p, 8);
               out += 8;
               p += 8;
            }
            if (p >= end)
               break;
            const unsigned char c = *p;
            if (c >= 0x80)
            {
               std::uint32_t  cp  = 0;
               const unsigned len = utf8_multibyte(p, end, cp);
               if (len == 0)
               {
                  w.commit(out);
                  w.fail("json: string is not valid UTF-8");
                  out = w.reserve(1);
                  break;
               }
               if (nonchar && is_noncharacter(cp))
               {
                  w.commit(out);
                  w.fail("ijson: string holds a Unicode noncharacter");
                  out = w.reserve(1);
                  break;
               }
               for (unsigned k = 0; k < len; ++k)
                  *out++ = static_cast<char>(p[k]);
               p += len;
               continue;
            }
            if (c >= 0x20 && c != '"' && c != '\\')
            {
               *out++ = static_cast<char>(c);
               ++p;
               continue;
            }
            // An escape: up to six bytes instead of one.
            w.commit(out);
            out = w.reserve(static_cast<std::size_t>(end - p) + 7);
            *out++ = '\\';
            switch (c)
            {
               case '"':  *out++ = '"'; break;
               case '\\': *out++ = '\\'; break;
               case '\n': *out++ = 'n'; break;
               case '\r': *out++ = 'r'; break;
               case '\t': *out++ = 't'; break;
               default:
               {
                  static constexpr char hex[] = "0123456789abcdef";
                  *out++ = 'u';
                  *out++ = '0';
                  *out++ = '0';
                  *out++ = hex[c >> 4];
                  *out++ = hex[c & 0xF];
               }
            }
            ++p;
         }
         *out++ = '"';
         w.commit(out);
      }

      // Two uppercase hex digits per byte value.
      inline constexpr auto hex_pairs = [] {
         std::array<char, 512> t{};
         constexpr char        digits[] = "0123456789ABCDEF";
         for (std::size_t i = 0; i < 256; ++i)
         {
            t[2 * i]     = digits[i >> 4];
            t[2 * i + 1] = digits[i & 0xF];
         }
         return t;
      }();

      inline void write_hex(writer& w, const unsigned char* data, std::size_t n)
      {
         char* out = w.reserve(2 * n + 2);
         *out++    = '"';
         for (std::size_t i = 0; i < n; ++i)
         {
            std::memcpy(out, hex_pairs.data() + 2 * std::size_t{data[i]}, 2);
            out += 2;
         }
         *out++ = '"';
         w.commit(out);
      }

      // A member's key as it appears in the text, built at compile time:
      // the separator before it ('{' for the first member, ',' after),
      // the quoted and escaped name, and the colon.
      template <typename T, std::size_t I>
      struct member_key
      {
         static constexpr std::string_view name = ::psio::reflect<T>::template member_name<I>;
         static constexpr std::size_t escaped_size = [] {
            std::size_t size = 0;
            for (char ch : name)
            {
               const auto c = static_cast<unsigned char>(ch);
               size += (c == '"' || c == '\\') ? 2 : c < 0x20 ? 6 : 1;
            }
            return size;
         }();
         static constexpr std::array<char, escaped_size + 4> text = [] {
            std::array<char, escaped_size + 4> out{};
            std::size_t                        at = 0;
            out[at++] = I == 0 ? '{' : ',';
            out[at++] = '"';
            for (char ch : name)
            {
               const auto c = static_cast<unsigned char>(ch);
               if (c == '"' || c == '\\')
               {
                  out[at++] = '\\';
                  out[at++] = ch;
               }
               else if (c < 0x20)
               {
                  constexpr char hex[] = "0123456789abcdef";
                  out[at++] = '\\';
                  out[at++] = 'u';
                  out[at++] = '0';
                  out[at++] = '0';
                  out[at++] = hex[c >> 4];
                  out[at++] = hex[c & 0xF];
               }
               else
                  out[at++] = ch;
            }
            out[at++] = '"';
            out[at++] = ':';
            return out;
         }();
      };

      // The most text a value of T can take, when that is a small constant
      // known from the type alone (numbers, bool, and optionals, fixed
      // arrays, tuples and records of them); 0 otherwise. Lets a list of
      // such elements, and such a value at the top level, reserve once.
      template <typename Fmt, typename T>
      constexpr std::size_t text_bound() noexcept
      {
         if constexpr (dispatch_adapter_v<Fmt, T>)
            return 0;
         else if constexpr (::psio::is_transparent_v<T>)
            return text_bound<Fmt, ::psio::transparent_inner_t<T>>();
         else if constexpr (std::is_same_v<T, bool>)
            return 5;
         else if constexpr (std::is_same_v<T, std::byte>)
            return 5;
         else if constexpr (std::is_enum_v<T>)
         {
            // A name in quotes, or the number.
            std::size_t bound = text_bound<Fmt, std::underlying_type_t<T>>();
            if constexpr (::psio::ReflectedEnum<T>)
               for (auto name : ::psio::reflect_enum<T>::names)
                  bound = std::max(bound, name.size() + 2);
            return bound;
         }
         else if constexpr (is_wide_int_v<T>)
            return 0;
         else if constexpr (::psio::TickTextValue<T>)
            return text_bound<Fmt, ::psio::time_rep_t<T>>();
         else if constexpr (::psio::TimePointValue<T>)
            return ::psio::max_time_text_size + 2;
         else if constexpr (std::is_integral_v<T>)
            // digits, sign, and the quotes of a string form
            return sizeof(T) <= 8
                      ? static_cast<std::size_t>(std::numeric_limits<T>::digits10) + 4
                      : 0;
         else if constexpr (std::is_floating_point_v<T>)
            return 32;
         else if constexpr (is_std_optional<T>::value)
         {
            constexpr std::size_t inner = text_bound<Fmt, typename T::value_type>();
            return inner ? std::max<std::size_t>(inner, 4) : 0;
         }
         else if constexpr (is_box<T>::value)
            // A box is how a type holds itself: its text has no constant
            // bound, and looking through it here would not terminate.
            return 0;
         else if constexpr (Fmt::i_json && byte_seq<T>::value)
         {
            if constexpr (is_std_array<T>::value)
               return 2 * std::tuple_size_v<T> + 2;
            else if constexpr (is_byte_array<T>::value)
               return 2 * sizeof(T) + 2;
            else
               return 0;
         }
         else if constexpr (is_std_array<T>::value)
         {
            constexpr std::size_t inner = text_bound<Fmt, typename T::value_type>();
            return inner && std::tuple_size_v<T> <= 4096
                      ? std::tuple_size_v<T> * (inner + 1) + 2
                      : 0;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            return []<std::size_t... Is>(std::index_sequence<Is...>) -> std::size_t {
               std::size_t total = 2;
               bool        fixed = true;
               (([&] {
                   using F   = typename R::template member_type<Is>;
                   using eff = typename ::psio::effective_annotations_for<
                      T, F, R::template member_pointer<Is>>::value_t;
                   std::size_t inner = 0;
                   if constexpr (!::psio::has_as_override_v<eff>)
                      inner = text_bound<Fmt, F>();
                   if (inner == 0)
                      fixed = false;
                   total += member_key<T, Is>::text.size() + inner;
                }()),
                ...);
               return fixed && total <= 65536 ? total : 0;
            }(std::make_index_sequence<R::member_count>{});
         }
         else
            return 0;
      }

      template <typename Fmt, int_mode IM, typename T>
      void encode_value(const T& v, writer& w);
      template <typename Fmt, int_mode IM, typename T>
      void encode_shape(const T& v, writer& w);

      // Maps and sets (detail/json_assoc.hpp).
      template <typename Fmt, int_mode IM, typename T>
      void encode_assoc(const T& v, writer& w);

      template <typename Fmt, int_mode IM, typename I>
      void encode_integer(I v, writer& w)
      {
         char* const out    = w.reserve(48);
         bool        quoted = int_as_string_v<Fmt, IM, I>;
         if constexpr (IM == int_mode::natural && int_as_string_v<Fmt, IM, I>)
         {
            // Natural ijson: a wide integer JavaScript holds exactly
            // (|v| <= 2^53 - 1) is a bare number; anything larger is a
            // string. One unsigned compare decides either sign.
            constexpr std::uint64_t safe = (std::uint64_t{1} << 53) - 1;
            if constexpr (sizeof(I) > 8)
               // A 128-bit integer is always a decimal string under ijson:
               // one form for the type (amounts), whatever the value.
               quoted = true;
            else if constexpr (std::is_signed_v<I>)
               quoted = static_cast<std::uint64_t>(v) + safe > 2 * safe;
            else
               quoted = static_cast<std::uint64_t>(v) > safe;
         }
         char* p = out;
         *p      = '"';
         p += quoted;
         p  = std::to_chars(p, out + 46, v).ptr;
         *p = '"';
         p += quoted;
         w.commit(p);
      }

      // Member I of record T: its key literal, then its value.
      template <typename Fmt, typename T, std::size_t I>
      void encode_member(const T& v, writer& w)
      {
         using R = ::psio::reflect<T>;
         using F = typename R::template member_type<I>;
         w.literal(member_key<T, I>::text);
         const auto& fref = v.*(R::template member_pointer<I>);

         // Member-level presentation override: if the field's effective
         // annotations include as_spec<Tag>, use adapter<F, Tag> for this
         // field (member beats type, §5.3.7).
         using eff_for =
            ::psio::effective_annotations_for<T, F, R::template member_pointer<I>>;
         using eff = typename eff_for::value_t;
         if constexpr (::psio::has_as_override_v<eff>)
         {
            using Tag  = ::psio::adapter_tag_of_t<eff>;
            using Proj = ::psio::adapter<std::remove_cvref_t<F>, Tag>;
            std::string payload;
            Proj::encode(fref, payload);
            write_string(w, std::string_view{payload}, Fmt::i_json);
         }
         else
         {
            // A member outside its declared annotations (its length_bound
            // / utf8, and those of its elements, keys, values and tuple
            // elements at any depth) has no text: the reader would reject it.
            if constexpr (::psio::detail::ann::has_tree_specs_v<eff>)
               if (auto st = ::psio::check_member_annotations(
                      fref, eff_for::value, "json: member length_bound violated", "json");
                   !st.ok())
                  w.fail(st.error());
            constexpr int_mode mode = field_mode<Fmt, eff>(eff_for::value);
            encode_value<Fmt, mode>(fref, w);
         }
      }

      // Every value is written through here: its text (encode_shape), then
      // — for a type that has one — its invariant, so nested values are
      // checked innermost first.
      template <typename Fmt, int_mode IM, typename T>
      void encode_value(const T& v, writer& w)
      {
         encode_shape<Fmt, IM>(v, w);
         if constexpr (::psio::has_invariant_v<T>)
            if (auto st = ::psio::check_invariant(v); !st.ok())
               w.fail(st.error());
      }

      template <typename Fmt, int_mode IM, typename T>
      void encode_shape(const T& v, writer& w)
      {
         // Adapter check — if the type registered a text adapter (and it
         // doesn't delegate back to this format), delegate and write the
         // result as a JSON string. The adapter produces the payload
         // unquoted; JSON owns the surrounding quotes and escaping.
         //
         // Must be the FIRST branch so the trailing static_assert is
         // discarded for adapter-encoded types.
         if constexpr (dispatch_adapter_v<Fmt, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                         ::psio::text_category>;
            std::string payload;
            Proj::encode(v, payload);
            write_string(w, std::string_view{payload}, Fmt::i_json);
         }
         else if constexpr (is_box<T>::value)
         {
            // Transparent: the text of the boxed value. An empty box (a
            // moved-from one) holds no value to write.
            if (!v.value)
            {
               w.fail("json: null Box");
               w.append("null");
               return;
            }
            encode_value<Fmt, IM>(*v.value, w);
         }
         else if constexpr (::psio::is_transparent_v<T>)
            encode_value<Fmt, IM>(::psio::transparent_inner(v), w);
         else if constexpr (::psio::detail::is_nested_v<T>)
         {
            // The inner value's own text. The bytes are validated before
            // they are decoded: bytes that are not a T have no text.
            typename ::psio::detail::is_nested<T>::inner inner{};
            if (!v.decode_checked(inner).ok())
            {
               w.fail("json: nested bytes are not a valid encoding of their type");
               w.append("null");
               return;
            }
            encode_value<Fmt, IM>(inner, w);
         }
         else if constexpr (std::is_same_v<T, bool>)
            w.append(v ? std::string_view{"true"} : std::string_view{"false"});
         else if constexpr (std::is_enum_v<T> && !std::is_same_v<T, std::byte>)
         {
            // A declared enumerator is its name; any other value (and
            // every value of an enum without PSIO_REFLECT_ENUM) its number.
            if constexpr (::psio::ReflectedEnum<T>)
               if (const auto name = ::psio::reflect_enum<T>::name_of(v))
               {
                  char* p = w.reserve(name->size() + 2);
                  *p++    = '"';
                  std::memcpy(p, name->data(), name->size());
                  p += name->size();
                  *p++ = '"';
                  w.commit(p);
                  return;
               }
            encode_integer<Fmt, IM>(
               static_cast<std::underlying_type_t<T>>(v), w);
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            encode_integer<Fmt, IM>(v, w);
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            // Decimal: a string under ijson, a bare number under json.
            constexpr bool quoted = IM == int_mode::string ||
                                    (IM == int_mode::natural && Fmt::i_json);
            std::string digits;
            if (quoted)
               digits.push_back('"');
            ::psio::ext_int_detail::append_decimal_u256(digits, v);
            if (quoted)
               digits.push_back('"');
            w.append(std::string_view{digits});
         }
         else if constexpr (std::is_same_v<T, std::byte>)
            encode_integer<Fmt, IM>(static_cast<unsigned char>(v), w);
         else if constexpr (std::is_integral_v<T>)
            encode_integer<Fmt, IM>(v, w);
         else if constexpr (::psio::TickTextValue<T>)
            encode_integer<Fmt, IM>(::psio::time_ticks(v), w);
         else if constexpr (::psio::TimePointValue<T>)
         {
            static_assert(::psio::TextTimePoint<T>,
                          "psio::json: a time point has RFC 3339 text only for a "
                          "period of 1/10^k seconds, k = 0..9");
            char* const out = w.reserve(::psio::max_time_text_size + 2);
            *out            = '"';
            char* end       = ::psio::write_time_text(v, out + 1);
            if (!end)
            {
               w.fail("json: time point outside years 0000..9999 has no text");
               w.append("null");
               return;
            }
            *end++ = '"';
            w.commit(end);
         }
         else if constexpr (std::is_floating_point_v<T>)
         {
            // NaN and the infinities have no JSON text, in either format.
            if (!std::isfinite(v))
            {
               w.fail(Fmt::i_json ? "ijson: non-finite float has no JSON text"
                                  : "json: non-finite float has no JSON text");
               w.append("null");
               return;
            }
            char* const out = w.reserve(64);
            w.commit(std::to_chars(out, out + 64, v).ptr);
         }
         else if constexpr (std::is_same_v<T, std::string>)
            write_string(w, std::string_view{v}, Fmt::i_json);
         else if constexpr (is_utf8_string<T>::value)
            write_string(w, std::string_view{v.storage()}, Fmt::i_json);
         else if constexpr (is_bounded<T>::value)
         {
            // The wrapper's storage is reachable unchecked: never write a
            // list its own reader rejects.
            if (v.size() > T::max_size_value)
               w.fail("json: array exceeds its bound");
            encode_value<Fmt, IM>(v.storage(), w);
         }
         else if constexpr (Fmt::i_json && byte_seq<T>::value)
            write_hex(w, reinterpret_cast<const unsigned char*>(v.data()),
                      v.size());
         else if constexpr (is_byte_array<T>::value)
            encode_value<Fmt, IM>(v.storage(), w);
         else if constexpr (is_std_array<T>::value ||
                            is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            // Elements of a constant text bound: one reservation for all.
            if constexpr (constexpr std::size_t bound = text_bound<Fmt, E>(); bound != 0)
               (void)w.reserve(v.size() * (bound + 1) + 2);
            w.put('[');
            bool first = true;
            for (const E& x : v)
            {
               if (!first)
                  w.put(',');
               first = false;
               encode_value<Fmt, IM>(x, w);
            }
            w.put(']');
         }
         else if constexpr (::psio::detail::is_assoc_v<T>)
            encode_assoc<Fmt, IM>(v, w);
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
               encode_value<Fmt, IM>(*v, w);
            else
               w.append("null");
         }
         else if constexpr (is_std_variant<T>::value)
         {
            if constexpr (Fmt::i_json)
            {
               // Named form: {"AlternativeName": value}.
               static_assert(alt_names_unique<T>(),
                             "psio::ijson: variant alternatives must have "
                             "distinct type names");
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (void)((v.index() == Is
                             ? (w.put('{'),
                                write_string(w, alt_name<T, Is>(), false),
                                w.put(':'),
                                encode_value<Fmt, int_mode::natural>(
                                   *std::get_if<Is>(&v), w),
                                w.put('}'), true)
                             : false) ||
                         ...);
               }(std::make_index_sequence<std::variant_size_v<T>>{});
            }
            else
            {
               // Positional tagged form: [index, value].
               w.put('[');
               char* const out = w.reserve(24);
               w.commit(std::to_chars(out, out + 24, v.index()).ptr);
               w.put(',');
               std::visit(
                  [&](const auto& alt) {
                     encode_value<Fmt, int_mode::natural>(alt, w);
                  },
                  v);
               w.put(']');
            }
         }
         else if constexpr (requires { typename T::psio_annotated_tuple; })
         {
            // A tuple carrying element annotations (wrappers.hpp: a
            // method's argument tuple): the tuple's text, for a value
            // inside its annotations.
            if (auto st = ::psio::detail::ann::check_value(
                   v.tuple(), T::psio_annotations, "json: member length_bound violated", "json");
                !st.ok())
               w.fail(st.error());
            encode_value<Fmt, int_mode::natural>(v.tuple(), w);
         }
         else if constexpr (is_std_tuple<T>::value)
         {
            w.put('[');
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (((Is ? w.put(',') : void()),
                 encode_value<Fmt, int_mode::natural>(std::get<Is>(v), w)),
                ...);
            }(std::make_index_sequence<std::tuple_size_v<T>>{});
            w.put(']');
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            if constexpr (R::member_count == 0)
               w.put('{');
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (encode_member<Fmt, T, Is>(v, w), ...);
            }(std::make_index_sequence<R::member_count>{});
            w.put('}');
         }
         else
         {
            PSIO_REJECT_MULTI_ASSOC("psio::json", T);
            static_assert(sizeof(T) == 0,
                          "psio::json: unsupported type in encode_value");
         }
      }

      // Encodes one whole value and leaves `w.s` as the text.
      template <typename Fmt, typename T>
      void encode_document(const T& v, writer& w)
      {
         if constexpr (constexpr std::size_t bound = text_bound<Fmt, T>(); bound != 0)
            (void)w.reserve(bound);
         encode_value<Fmt, int_mode::natural>(v, w);
         w.finish();
      }

      // ── Reading ───────────────────────────────────────────────────────────
      //
      // One typed reader, written against the JsonCursor concept
      // (json_cursor.hpp): it checks the cursor's events against T and never
      // looks at the text, so it runs unchanged over the hand-written lexer
      // and over simdjson. With `out == nullptr` it validates without
      // building a value; otherwise it also stores the value. Every failure
      // is recorded in the cursor (no exceptions, no aborts), so the same
      // code serves validate, the checked decode and guests.

      template <typename C>
      struct typed_reader
      {
         C&   c;
         bool lenient_missing = false;
         // The status of a violated invariant, reported unchanged.
         codec_status invariant{};
      };

      // Canonical decimal integer over exactly `text`: 0 | -?[1-9][0-9]*,
      // in range for I. One pass, exact over the whole 64-bit range.
      template <typename I>
      constexpr bool parse_integer(std::string_view text, I& v) noexcept
      {
         const char*       p = text.data();
         const char* const e = p + text.size();
         bool              negative = false;
         if (p < e && *p == '-')
         {
            if (!std::is_signed_v<I>)
               return false;
            negative = true;
            ++p;
         }
         if (p >= e)
            return false;
         if (*p == '0')
         {
            // "0" alone; no leading zero and no "-0".
            if (negative || p + 1 != e)
               return false;
            v = 0;
            return true;
         }
         if constexpr (sizeof(I) > 8)
         {
            for (const char* d = p; d < e; ++d)
               if (*d < '0' || *d > '9')
                  return false;
            const auto res = std::from_chars(text.data(), e, v);
            return res.ec == std::errc{} && res.ptr == e;
         }
         else
         {
            const std::size_t digits = static_cast<std::size_t>(e - p);
            if (digits > 20)
               return false;
            std::uint64_t acc = 0;
            for (; p < e; ++p)
            {
               const unsigned d = static_cast<unsigned char>(*p) - unsigned{'0'};
               if (d > 9)
                  return false;
               if (digits == 20 && (acc > 1844674407370955161ull ||
                                    (acc == 1844674407370955161ull && d > 5)))
                  return false;
               acc = acc * 10 + d;
            }
            using U = std::make_unsigned_t<I>;
            constexpr std::uint64_t max =
               static_cast<std::uint64_t>(std::numeric_limits<I>::max());
            if (negative)
            {
               if (acc > max + 1)
                  return false;
               v = static_cast<I>(static_cast<U>(0) - static_cast<U>(acc));
            }
            else
            {
               if (acc > max)
                  return false;
               v = static_cast<I>(acc);
            }
            return true;
         }
      }

      template <typename Fmt, int_mode IM, typename I, typename C>
      bool read_integer(typed_reader<C>& r, I* out)
      {
         constexpr bool   as_string = int_as_string_v<Fmt, IM, I>;
         I                v{};
         std::string_view text;
         if constexpr (as_string)
         {
            json_kind kind;
            if (!r.c.kind(kind))
               return false;
            if (kind == json_kind::string)
            {
               bool escaped = false;
               if (!r.c.string(text, escaped))
                  return false;
               if (escaped || !parse_integer(text, v))
                  return r.c.fail("json: integer string is not a canonical "
                                  "in-range decimal");
               if (out)
                  *out = v;
               return true;
            }
            if constexpr (IM == int_mode::string)
               return r.c.fail("json: expected an integer string");
         }
         // The number is taken as its token text and parsed here, exactly:
         // nothing is lost on the way through a double.
         // parse_integer accepts a subset of the number grammar, so a
         // cursor that can hand the token over unchecked need not check it.
         if constexpr (requires { r.c.scalar(text); })
         {
            if (!r.c.scalar(text))
               return false;
         }
         else if (!r.c.number(text))
            return false;
         if (!parse_integer(text, v))
            return r.c.fail("json: integer is not a canonical in-range "
                            "decimal");
         if constexpr (as_string)
         {
            // Natural ijson: a bare number is accepted only where a
            // JavaScript number is exact.
            constexpr I safe = static_cast<I>((std::uint64_t{1} << 53) - 1);
            bool        ok   = v <= safe;
            if constexpr (std::is_signed_v<I>)
               ok = ok && v >= -safe;
            if (!ok)
               return r.c.fail("ijson: bare integer outside the safe range "
                               "must be a string");
         }
         if (out)
            *out = v;
         return true;
      }

      // Decimal (`0 | -?[1-9][0-9]*`) or the legacy `[-]0x<hex>` text of a
      // 128 / 256-bit integer, in range.
      template <typename I>
      bool parse_wide_text(std::string_view text, I& v) noexcept
      {
         const std::string_view body =
            !text.empty() && text[0] == '-' ? text.substr(1) : text;
         if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
         {
            const bool                  negative = body.size() != text.size();
            const std::span<const char> hex{body.data() + 2, body.size() - 2};
            if constexpr (std::is_same_v<I, ::psio::uint256>)
               return !negative && ::psio::ext_int_detail::parse_hex_u256(hex, v).ok();
            else
            {
               ::psio::uint128 magnitude = 0;
               if (!::psio::ext_int_detail::parse_hex_u128(hex, magnitude).ok())
                  return false;
               if constexpr (std::is_signed_v<I>)
               {
                  constexpr ::psio::uint128 half = ::psio::uint128{1} << 127;
                  if (negative ? magnitude > half : magnitude >= half)
                     return false;
                  v = negative ? static_cast<I>(::psio::uint128{0} - magnitude)
                               : static_cast<I>(magnitude);
               }
               else
               {
                  if (negative)
                     return false;
                  v = magnitude;
               }
               return true;
            }
         }
         if constexpr (std::is_same_v<I, ::psio::uint256>)
            return ::psio::ext_int_detail::parse_decimal_u256(text, v);
         else
            return parse_integer(text, v);
      }

      // |v| <= 2^53 - 1: what a bare I-JSON number may hold.
      template <typename I>
      constexpr bool wide_is_safe(const I& v) noexcept
      {
         constexpr std::uint64_t safe = (std::uint64_t{1} << 53) - 1;
         if constexpr (std::is_same_v<I, ::psio::uint256>)
            return v.limb[1] == 0 && v.limb[2] == 0 && v.limb[3] == 0 && v.limb[0] <= safe;
         else if constexpr (std::is_signed_v<I>)
            return v <= static_cast<I>(safe) && v >= -static_cast<I>(safe);
         else
            return v <= static_cast<I>(safe);
      }

      // A 128 / 256-bit integer: a decimal string (or the legacy "0x"
      // string), or a bare decimal number — any in-range value under json,
      // |n| <= 2^53 - 1 under ijson. A forced form accepts that form only.
      template <typename Fmt, int_mode IM, typename I, typename C>
      bool read_wide_integer(typed_reader<C>& r, I* out)
      {
         I         v{};
         json_kind kind;
         if (!r.c.kind(kind))
            return false;
         if (kind == json_kind::string)
         {
            if constexpr (IM == int_mode::number)
               return r.c.fail("json: expected a number, found a string");
            std::string_view text;
            bool             escaped = false;
            if (!r.c.string(text, escaped))
               return false;
            if (escaped || !parse_wide_text(text, v))
               return r.c.fail("json: integer string is not a canonical "
                               "in-range decimal");
         }
         else
         {
            if constexpr (IM == int_mode::string)
               return r.c.fail("json: expected an integer string");
            std::string_view text;
            if (!r.c.number(text))
               return false;
            if (!parse_wide_text(text, v) || text.find_first_of("xX") != text.npos)
               return r.c.fail("json: integer is not a canonical in-range "
                               "decimal");
            if constexpr (Fmt::i_json && IM == int_mode::natural)
               if (!wide_is_safe(v))
                  return r.c.fail("ijson: bare integer outside the safe range "
                                  "must be a string");
         }
         if (out)
            *out = v;
         return true;
      }

      // An enum with PSIO_REFLECT_ENUM names. Accepted: the name ("active"),
      // the number (2, or the numeric string "2"), and both ("active:2",
      // which must agree). An unknown name is an error; a number that names
      // no enumerator is accepted (a C++ enum holds any underlying value).
      template <typename Fmt, int_mode IM, typename E, typename C>
      bool read_named_enum(typed_reader<C>& r, E* out)
      {
         using U = std::underlying_type_t<E>;
         json_kind kind;
         if (!r.c.kind(kind))
            return false;
         if (kind != json_kind::string)
         {
            U v{};
            if (!read_integer<Fmt, IM, U>(r, &v))
               return false;
            if (out)
               *out = static_cast<E>(v);
            return true;
         }
         std::string_view text;
         bool             escaped = false;
         if (!r.c.string(text, escaped))
            return false;
         auto number = [&](std::string_view digits, U& v) {
            return !digits.empty() && parse_integer(digits, v);
         };
         U v{};
         if (!text.empty() && (text[0] == '-' || (text[0] >= '0' && text[0] <= '9')))
         {
            if (!number(text, v))
               return r.c.fail("json: enum number is not a canonical in-range decimal");
         }
         else
         {
            const std::size_t      colon = text.find(':');
            const std::string_view name  = text.substr(0, colon);
            const auto             named = ::psio::reflect_enum<E>::value_of(name);
            if (!named)
               return r.c.fail("json: unknown enumerator name");
            v = static_cast<U>(*named);
            if (colon != std::string_view::npos)
            {
               U stated{};
               if (!number(text.substr(colon + 1), stated))
                  return r.c.fail("json: enum number is not a canonical in-range decimal");
               if (stated != v)
                  return r.c.fail("json: enumerator name and number disagree");
            }
         }
         if (out)
            *out = static_cast<E>(v);
         return true;
      }

      template <typename F, typename C>
      bool read_float(typed_reader<C>& r, F* out)
      {
         if constexpr (std::is_same_v<F, double> && requires(double d) { r.c.real(d); })
         {
            // The syntax layer's own exact double parser.
            double v = 0;
            if (!r.c.real(v))
               return false;
            if (out)
               *out = v;
            return true;
         }
         std::string_view token;
         if (!r.c.number(token))
            return false;
         const char* const b = token.data();
         const char* const e = b + token.size();
         F                 v{};
         const auto        res = std::from_chars(b, e, v);
         if (res.ec == std::errc::result_out_of_range)
         {
            // Overflow is an error; underflow rounds toward zero.
            const std::string copy(token);
            const double      wide = std::strtod(copy.c_str(), nullptr);
            v = static_cast<F>(wide);
            if (!std::isfinite(wide) || !std::isfinite(v))
               return r.c.fail("json: number out of range");
         }
         else if (res.ec != std::errc{} || res.ptr != e)
            return r.c.fail("json: malformed number");
         if (!std::isfinite(v))
            return r.c.fail("json: number out of range");
         if (out)
            *out = v;
         return true;
      }

      // Value of a hex digit, or -1.
      inline constexpr auto hex_values = [] {
         std::array<signed char, 256> t{};
         for (auto& x : t)
            x = -1;
         for (int i = 0; i < 10; ++i)
            t['0' + i] = static_cast<signed char>(i);
         for (int i = 0; i < 6; ++i)
         {
            t['a' + i] = static_cast<signed char>(10 + i);
            t['A' + i] = static_cast<signed char>(10 + i);
         }
         return t;
      }();

      // Hex string into a byte sequence.
      template <typename T, typename C>
      bool read_hex(typed_reader<C>& r, T* out)
      {
         json_kind kind;
         if (!r.c.kind(kind))
            return false;
         if (kind != json_kind::string)
            return r.c.fail("ijson: expected a hex string");
         std::string_view text;
         bool             escaped = false;
         if (!r.c.string(text, escaped))
            return false;
         if (escaped)
            return r.c.fail("ijson: non-hex character in hex string");
         int bad = 0;
         for (const char ch : text)
            bad |= hex_values[static_cast<unsigned char>(ch)];
         if (bad < 0)
            return r.c.fail("ijson: non-hex character in hex string");
         if (text.size() & 1)
            return r.c.fail("ijson: odd-length hex string");
         const std::size_t n = text.size() / 2;
         if constexpr (is_std_array<T>::value)
         {
            if (n != std::tuple_size_v<T>)
               return r.c.fail("ijson: hex string has the wrong length");
         }
         if (out)
         {
            if constexpr (!is_std_array<T>::value)
               out->resize(n);
            auto* dst = reinterpret_cast<unsigned char*>(out->data());
            for (std::size_t i = 0; i < n; ++i)
               dst[i] = static_cast<unsigned char>(
                  (hex_values[static_cast<unsigned char>(text[2 * i])] << 4) |
                  hex_values[static_cast<unsigned char>(text[2 * i + 1])]);
         }
         return true;
      }

      // A text adapter's payload: a JSON string checked by the adapter.
      template <typename Fmt, typename Proj, typename F, typename C>
      bool read_adapted(typed_reader<C>& r, F* out)
      {
         std::string_view payload;
         bool             escaped = false;
         if (!r.c.string(payload, escaped))
            return false;
         const std::span<const char> text{payload};
         if (auto st = Proj::validate(text); !st.ok())
            return r.c.fail(st.error().what);
         if (out)
            *out = Proj::decode(text);
         return true;
      }

      template <typename Fmt, int_mode IM, typename T, typename C>
      bool read_value(typed_reader<C>& r, T* out);
      template <typename Fmt, int_mode IM, typename T, typename C>
      bool read_shape(typed_reader<C>& r, T* out);

      // Maps and sets (detail/json_assoc.hpp).
      template <typename Fmt, int_mode IM, typename T, typename C>
      bool read_assoc(typed_reader<C>& r, T* out);

      // "[" e0 "," e1 ... "]" with exactly N elements read by `element(i)`.
      template <typename C, typename Element>
      bool read_fixed_array(typed_reader<C>& r, std::size_t n, Element&& element)
      {
         if (!r.c.begin_array())
            return false;
         bool has = false;
         for (std::size_t i = 0; i < n; ++i)
         {
            if (!r.c.next_element(has))
               return false;
            if (!has)
               return r.c.fail("json: array has too few elements");
            if (!element(i))
               return false;
         }
         if (!r.c.next_element(has))
            return false;
         if (has)
            return r.c.fail("json: array has too many elements");
         return true;
      }

      // Reads the value of member I of record T (the cursor is at it).
      template <typename Fmt, typename T, std::size_t I, typename C>
      bool read_member(typed_reader<C>& r, T* out)
      {
         using R = ::psio::reflect<T>;
         using F = typename R::template member_type<I>;
         F* fptr = out ? &(out->*(R::template member_pointer<I>)) : nullptr;
         using eff_for =
            ::psio::effective_annotations_for<T, F, R::template member_pointer<I>>;
         using eff = typename eff_for::value_t;
         if constexpr (::psio::has_as_override_v<eff>)
         {
            // Member-level presentation override (as<Tag>).
            using Tag = ::psio::adapter_tag_of_t<eff>;
            return read_adapted<Fmt, ::psio::adapter<std::remove_cvref_t<F>, Tag>>(r, fptr);
         }
         else
         {
            constexpr int_mode mode = field_mode<Fmt, eff>(eff_for::value);
            if constexpr (::psio::detail::ann::has_tree_specs_v<eff>)
            {
               // The member's annotations are part of its type: its
               // length_bound / utf8 and those of every position below it.
               // They are checked on the value read, so the member is
               // materialized even when only validating.
               F  scratch{};
               F* dst = fptr ? fptr : &scratch;
               if (!read_value<Fmt, mode, F>(r, dst))
                  return false;
               if (auto st = ::psio::check_member_annotations(
                      *dst, eff_for::value, "json: member length_bound violated", "json");
                   !st.ok())
                  return r.c.fail(st.error().what);
               return true;
            }
            else
               return read_value<Fmt, mode, F>(r, fptr);
         }
      }

      // Member readers by index, for keys that arrive out of order.
      template <typename Fmt, typename T, typename C>
      struct member_readers
      {
         using fn = bool (*)(typed_reader<C>&, T*);
         static constexpr auto table =
            []<std::size_t... Is>(std::index_sequence<Is...>) {
               return std::array<fn, sizeof...(Is)>{&read_member<Fmt, T, Is, C>...};
            }(std::make_index_sequence<::psio::reflect<T>::member_count>{});
      };

      template <typename Fmt, typename T, typename C>
      bool read_record(typed_reader<C>& r, T* out)
      {
         using R = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         if (!r.c.begin_object())
            return false;
         // `seen` only records which members were given, to find the
         // missing ones; a repeated key is simply read again.
         std::array<bool, N + 1> seen{};
         std::string_view        key;
         bool                    more = false;
         if (!r.c.next_field(more, key))
            return false;
         // Expected order first: writers emit members in declaration order,
         // so each key is compared with the next member's name (a compare
         // against a constant) before any lookup. A member the text leaves
         // out costs one failed compare.
         bool ok = true;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (void)((!more || key != R::template member_name<Is>
                       ? true
                       : (seen[Is] = true,
                          ok = read_member<Fmt, T, Is>(r, out) &&
                               r.c.next_field(more, key))) &&
                   ...);
         }(std::make_index_sequence<N>{});
         if (!ok)
            return false;
         // Whatever is left is out of order, repeated or unknown: one
         // perfect-hash lookup per key (reflect<T>::find_index).
         while (more)
         {
            const std::size_t index = R::find_index(key);
            if (index < N)
            {
               seen[index] = true;
               if (!member_readers<Fmt, T, C>::table[index](r, out))
                  return false;
            }
            else if (!r.c.skip())
               return false;  // unknown key: its value is skipped
            if (!r.c.next_field(more, key))
               return false;
         }
         // A missing std::optional member is empty; any other missing
         // member is an error.
         bool complete = true;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
               ([&]
                {
                   if (seen[Is])
                      return;
                   using F = typename R::template member_type<Is>;
                   if constexpr (is_std_optional<F>::value)
                   {
                      if (out)
                         (out->*(R::template member_pointer<Is>)).reset();
                   }
                   else if (!r.lenient_missing)
                      complete = false;
                }()),
               ...);
         }(std::make_index_sequence<N>{});
         if (!complete)
            return r.c.fail("json: missing required member");
         return true;
      }

      // Every value is read through here: its text (read_shape), then — for
      // a type that has one — its invariant on the value read, which is
      // materialised for that even when only validating.
      template <typename Fmt, int_mode IM, typename T, typename C>
      bool read_value(typed_reader<C>& r, T* out)
      {
         if constexpr (::psio::has_invariant_v<T>)
         {
            T        scratch{};
            T* const dst = out ? out : &scratch;
            if (!read_shape<Fmt, IM, T>(r, dst))
               return false;
            if (auto st = ::psio::check_invariant(*dst); !st.ok())
            {
               const std::string_view what = st.error().what;
               if (r.invariant.ok())
                  r.invariant = std::move(st);
               return r.c.fail(what);
            }
            return true;
         }
         else
            return read_shape<Fmt, IM, T>(r, out);
      }

      template <typename Fmt, int_mode IM, typename T, typename C>
      bool read_shape(typed_reader<C>& r, T* out)
      {
         if constexpr (dispatch_adapter_v<Fmt, T>)
            return read_adapted<Fmt, ::psio::adapter<std::remove_cvref_t<T>,
                                                     ::psio::text_category>>(r, out);
         else if constexpr (is_box<T>::value)
         {
            // Transparent: the boxed value's text, read into the box.
            using E = typename is_box<T>::elem;
            if (!out)
               return read_value<Fmt, IM, E>(r, nullptr);
            if (!out->value)
               out->value = std::make_unique<E>();
            return read_value<Fmt, IM, E>(r, out->value.get());
         }
         else if constexpr (::psio::is_transparent_v<T>)
            return read_value<Fmt, IM, ::psio::transparent_inner_t<T>>(
               r, out ? &::psio::transparent_inner(*out) : nullptr);
         else if constexpr (::psio::detail::is_nested_v<T>)
         {
            // The inner value's text, stored as its canonical encoding. The
            // value is built even when only validating: text describing a
            // value that has no encoding is not a nested value.
            typename ::psio::detail::is_nested<T>::inner inner{};
            if (!read_value<Fmt, IM>(r, &inner))
               return false;
            T value = T::unchecked({});
            if (!T::from_checked(inner, value).ok())
               return r.c.fail("json: nested value has no encoding");
            if (out)
               *out = std::move(value);
            return true;
         }
         else if constexpr (std::is_same_v<T, bool>)
         {
            bool v = false;
            if (!r.c.boolean(v))
               return false;
            if (out) *out = v;
            return true;
         }
         else if constexpr (std::is_same_v<T, std::byte>)
         {
            unsigned char v = 0;
            if (!read_integer<Fmt, IM, unsigned char>(r, &v))
               return false;
            if (out) *out = static_cast<std::byte>(v);
            return true;
         }
         else if constexpr (std::is_enum_v<T>)
         {
            if constexpr (::psio::ReflectedEnum<T>)
               return read_named_enum<Fmt, IM, T>(r, out);
            else
            {
               std::underlying_type_t<T> v{};
               if (!read_integer<Fmt, IM, std::underlying_type_t<T>>(r, &v))
                  return false;
               if (out) *out = static_cast<T>(v);
               return true;
            }
         }
         else if constexpr (is_wide_int_v<T>)
            return read_wide_integer<Fmt, IM, T>(r, out);
         else if constexpr (std::is_integral_v<T>)
            return read_integer<Fmt, IM, T>(r, out);
         else if constexpr (::psio::TickTextValue<T>)
         {
            ::psio::time_rep_t<T> v{};
            if (!read_integer<Fmt, IM, ::psio::time_rep_t<T>>(r, &v))
               return false;
            if (out) *out = ::psio::time_from_ticks<T>(v);
            return true;
         }
         else if constexpr (::psio::TimePointValue<T>)
         {
            static_assert(::psio::TextTimePoint<T>,
                          "psio::json: a time point has RFC 3339 text only for a "
                          "period of 1/10^k seconds, k = 0..9");
            std::string_view text;
            bool             escaped = false;
            if (!r.c.string(text, escaped))
               return false;
            T v{};
            if (const char* err = ::psio::parse_time_text(text, v))
               return r.c.fail(err);
            if (out) *out = v;
            return true;
         }
         else if constexpr (std::is_floating_point_v<T>)
            return read_float<T>(r, out);
         else if constexpr (std::is_same_v<T, std::string>)
         {
            std::string_view text;
            bool             escaped = false;
            if (!r.c.string(text, escaped))
               return false;
            if (out) out->assign(text);
            return true;
         }
         else if constexpr (is_utf8_string<T>::value)
         {
            std::string_view text;
            bool             escaped = false;
            if (!r.c.string(text, escaped))
               return false;
            if (text.size() > T::max_size_value)
               return r.c.fail("json: string exceeds its bound");
            if (out) *out = T{std::string(text)};
            return true;
         }
         else if constexpr (Fmt::i_json && byte_seq<T>::value)
         {
            if constexpr (is_byte_array<T>::value)
               return read_hex(r, out ? &out->storage() : nullptr);
            else
               return read_hex(r, out);
         }
         else if constexpr (is_byte_array<T>::value)
            return read_value<Fmt, IM>(r, out ? &out->storage() : nullptr);
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            return read_fixed_array(r, std::tuple_size_v<T>, [&](std::size_t i) {
               return read_value<Fmt, IM, E>(r, out ? &(*out)[i] : nullptr);
            });
         }
         else if constexpr (Fmt::i_json && is_bounded_bytes<T>::value)
         {
            // Written as one hex string (its storage is a byte sequence),
            // so it is read as one.
            std::vector<typename T::value_type> bytes;
            if (!read_hex(r, &bytes))
               return false;
            if (bytes.size() > T::max_size_value)
               return r.c.fail("json: array exceeds its bound");
            if (out) out->storage() = std::move(bytes);
            return true;
         }
         else if constexpr (is_std_vector<T>::value || is_bounded<T>::value)
         {
            using E = typename T::value_type;
            std::vector<E>* dst = nullptr;
            if constexpr (is_bounded<T>::value)
            {
               if (out) dst = &out->storage();
            }
            else
               dst = out;
            if (dst) dst->clear();
            if (!r.c.begin_array())
               return false;
            std::size_t count = 0;
            while (true)
            {
               bool has = false;
               if (!r.c.next_element(has))
                  return false;
               if (!has)
                  return true;
               if constexpr (is_bounded<T>::value)
                  if (count >= T::max_size_value)
                     return r.c.fail("json: array exceeds its bound");
               ++count;
               if (dst)
               {
                  if constexpr (std::is_same_v<E, bool>)
                  {
                     bool element = false;
                     if (!read_value<Fmt, IM, bool>(r, &element))
                        return false;
                     dst->push_back(element);
                  }
                  else
                  {
                     // Read in place: no move of each element.
                     if (!read_value<Fmt, IM, E>(r, &dst->emplace_back()))
                        return false;
                  }
               }
               else if (!read_value<Fmt, IM, E>(r, nullptr))
                  return false;
            }
         }
         else if constexpr (::psio::detail::is_assoc_v<T>)
            return read_assoc<Fmt, IM, T>(r, out);
         else if constexpr (is_std_optional<T>::value)
         {
            using V = typename T::value_type;
            json_kind kind;
            if (!r.c.kind(kind))
               return false;
            if (kind == json_kind::null)
            {
               if (!r.c.null())
                  return false;
               if (out) out->reset();
               return true;
            }
            if (!out)
               return read_value<Fmt, IM, V>(r, nullptr);
            // A fresh value, built in place.
            return read_value<Fmt, IM, V>(r, &out->emplace());
         }
         else if constexpr (is_std_variant<T>::value)
         {
            constexpr std::size_t N = std::variant_size_v<T>;
            auto alternative = [&](std::size_t index) {
               bool ok = false;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (void)((index == Is
                             ? (ok = [&] {
                                   using A = std::variant_alternative_t<Is, T>;
                                   if (!out)
                                      return read_value<Fmt, int_mode::natural, A>(
                                         r, nullptr);
                                   A value{};
                                   if (!read_value<Fmt, int_mode::natural, A>(
                                          r, &value))
                                      return false;
                                   out->template emplace<Is>(std::move(value));
                                   return true;
                                }(),
                                true)
                             : false) ||
                         ...);
               }(std::make_index_sequence<N>{});
               return ok;
            };
            bool has = false;
            if constexpr (Fmt::i_json)
            {
               static_assert(alt_names_unique<T>(),
                             "psio::ijson: variant alternatives must have "
                             "distinct type names");
               json_kind kind;
               if (!r.c.kind(kind))
                  return false;
               if (kind != json_kind::object)
                  return r.c.fail("ijson: expected a variant object");
               std::string_view key;
               if (!r.c.begin_object() || !r.c.next_field(has, key))
                  return false;
               if (!has)
                  return r.c.fail("ijson: variant object must have one key");
               // Perfect hash over the alternative names, one compare.
               const std::size_t index =
                  ::psio::detail::name_table_for<alt_names<T>>.find(key);
               if (index == N)
                  return r.c.fail("ijson: unknown variant alternative");
               if (!alternative(index) || !r.c.next_field(has, key))
                  return false;
               if (has)
                  return r.c.fail("ijson: variant object must have one key");
               return true;
            }
            else
            {
               json_kind kind;
               if (!r.c.kind(kind))
                  return false;
               if (kind != json_kind::array)
                  return r.c.fail("json: expected a [index, value] variant");
               if (!r.c.begin_array() || !r.c.next_element(has))
                  return false;
               if (!has)
                  return r.c.fail("json: expected a [index, value] variant");
               std::uint32_t index = 0;
               if (!read_integer<Fmt, int_mode::natural, std::uint32_t>(r, &index))
                  return false;
               if (index >= N)
                  return r.c.fail("json: variant index out of range");
               if (!r.c.next_element(has))
                  return false;
               if (!has)
                  return r.c.fail("json: expected ',' in variant");
               if (!alternative(index) || !r.c.next_element(has))
                  return false;
               if (has)
                  return r.c.fail("json: expected ']' after variant value");
               return true;
            }
         }
         else if constexpr (requires { typename T::psio_annotated_tuple; })
         {
            using Tuple = typename T::tuple_type;
            Tuple        scratch{};
            Tuple* const dst = out ? &out->tuple() : &scratch;
            if (!read_value<Fmt, int_mode::natural, Tuple>(r, dst))
               return false;
            if (auto st = ::psio::detail::ann::check_value(
                   *dst, T::psio_annotations, "json: member length_bound violated", "json");
                !st.ok())
               return r.c.fail(st.error().what);
            return true;
         }
         else if constexpr (is_std_tuple<T>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<T>;
            return read_fixed_array(r, N, [&](std::size_t i) {
               bool ok = false;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (void)((i == Is
                             ? (ok = read_value<Fmt, int_mode::natural,
                                                std::tuple_element_t<Is, T>>(
                                   r, out ? &std::get<Is>(*out) : nullptr),
                                true)
                             : false) ||
                         ...);
               }(std::make_index_sequence<N>{});
               return ok;
            });
         }
         else if constexpr (Record<T>)
            return read_record<Fmt, T>(r, out);
         else
         {
            PSIO_REJECT_MULTI_ASSOC("psio::json", T);
            static_assert(sizeof(T) == 0,
                          "psio::json: unsupported type in read_value");
         }
      }

      // Reads exactly one value of T, which must be the whole document.
      template <typename Fmt, typename T, typename C>
      codec_status read_with(C& cursor, T* out, bool lenient_missing)
      {
         typed_reader<C> r{cursor, lenient_missing};
         if (read_value<Fmt, int_mode::natural, T>(r, out))
            (void)cursor.finish();
         if (!r.invariant.ok())
            return std::move(r.invariant);
         return cursor.status(Fmt::name);
      }

      // Reads `bytes` as one T through the syntax layer `parser` selects.
      template <typename Fmt, typename T>
      codec_status read_document(std::span<const char> bytes, T* out,
                                 bool        lenient_missing,
                                 json_parser parser = json_parser::automatic)
      {
#if PSIO_HAVE_SIMDJSON
         if (use_simdjson(bytes, parser))
         {
            if (parser == json_parser::simdjson)
            {
               simdjson_cursor cursor(bytes, Fmt::i_json);
               return read_with<Fmt, T>(cursor, out, lenient_missing);
            }
            simdjson_index_cursor cursor(bytes, Fmt::i_json);
            return read_with<Fmt, T>(cursor, out, lenient_missing);
         }
#else
         (void)parser;
#endif
         json_text_cursor cursor(bytes, Fmt::i_json);
         return read_with<Fmt, T>(cursor, out, lenient_missing);
      }

      // `text` has at least json_padding readable bytes after it: simdjson
      // parses it in place, with no copy.
      template <typename Fmt, typename T>
      codec_status read_document(padded_json text, T* out, bool lenient_missing,
                                 json_parser parser = json_parser::automatic)
      {
#if PSIO_HAVE_SIMDJSON
         if (use_simdjson(text.text, parser))
         {
            if (parser == json_parser::simdjson)
            {
               simdjson_cursor cursor(text, Fmt::i_json);
               return read_with<Fmt, T>(cursor, out, lenient_missing);
            }
            simdjson_index_cursor cursor(text, Fmt::i_json);
            return read_with<Fmt, T>(cursor, out, lenient_missing);
         }
#else
         (void)parser;
#endif
         json_text_cursor cursor(text.text, Fmt::i_json);
         return read_with<Fmt, T>(cursor, out, lenient_missing);
      }

      // ── Tag operations shared by psio::json and psio::ijson ──────────────

      template <typename Derived>
      struct tag_ops : ::psio::format_tag_base<Derived>
      {
         using buffer_type = std::string;

         // Both formats delegate to the text adapter slot when the type
         // registers one (design § 5.3.7).
         using preferred_presentation_category = ::psio::text_category;

         // ── encode ──────────────────────────────────────────────────────
         template <typename T>
         friend void tag_invoke(decltype(::psio::encode), Derived, const T& v,
                                std::string& sink)
         {
            writer w{sink, Derived::name};
            encode_document<Derived>(v, w);
         }

         template <typename T>
         friend std::string tag_invoke(decltype(::psio::encode), Derived,
                                       const T& v)
         {
            std::string out;
            writer      w{out, Derived::name};
            encode_document<Derived>(v, w);
            return out;
         }

         // ── decode (fail-fast) ──────────────────────────────────────────
         template <typename T>
         friend T tag_invoke(decltype(::psio::decode<T>), Derived, T*,
                             std::span<const char> bytes)
         {
            T out{};
            if (auto st = read_document<Derived, T>(bytes, &out,
                                                    !Derived::i_json);
                !st.ok())
               ::psio::codec_abort(st.error());
            return out;
         }

         // Validate and decode in one pass: accepts exactly what
         // `validate` accepts (the checked decode of psio/helpers.hpp).
         template <typename T>
         [[nodiscard]] static codec_status decode_checked(
            std::span<const char> bytes, T& out)
         {
            return read_document<Derived, T>(bytes, &out, false);
         }

         // ── size_of ─────────────────────────────────────────────────────
         //
         // JSON has no compile-time-constant size; we produce the string
         // and return its length.
         template <typename T>
         friend std::size_t tag_invoke(decltype(::psio::size_of), Derived,
                                       const T& v)
         {
            std::string tmp;
            writer      w{tmp, Derived::name};
            encode_document<Derived>(v, w);
            return tmp.size();
         }

         // ── validate / validate_strict ──────────────────────────────────
         //
         // Typed: the text must be one value of T's shape.
         template <typename T>
         friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                        Derived, T*,
                                        std::span<const char> bytes) noexcept
         {
            if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(),
                                                           Derived::name);
                !st.ok())
               return st;
            return read_document<Derived, T>(bytes, nullptr, false);
         }

         template <typename T>
         friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                        Derived, T*,
                                        std::span<const char> bytes) noexcept
         {
            return read_document<Derived, T>(bytes, nullptr, false);
         }

         // ── make_boxed ──────────────────────────────────────────────────
         //
         // Null when the text is not a T.
         template <typename T>
         friend std::unique_ptr<T> tag_invoke(
            decltype(::psio::make_boxed<T>), Derived, T*,
            std::span<const char> bytes) noexcept
         {
            auto out = std::make_unique<T>();
            if (!read_document<Derived, T>(bytes, out.get(), !Derived::i_json)
                    .ok())
               return nullptr;
            return out;
         }
      };

      template <typename Fmt>
      concept json_family =
         std::is_same_v<Fmt, ::psio::json> || std::is_same_v<Fmt, ::psio::ijson>;

   }  // namespace detail::json_impl

   // ── Format tags ────────────────────────────────────────────────────────

   // RFC 8259 JSON: integers are numbers, bytes are arrays of numbers,
   // variants are [index, value].
   struct json : detail::json_impl::tag_ops<json>
   {
      static constexpr const char* name   = "json";
      static constexpr bool        i_json = false;
   };

   // RFC 7493 I-JSON: 64-bit integers beyond +/-(2^53 - 1) are decimal
   // strings, byte sequences are hex strings, variants are {"Name": value}.
   struct ijson : detail::json_impl::tag_ops<ijson>
   {
      static constexpr const char* name   = "ijson";
      static constexpr bool        i_json = true;
   };

   PSIO_FORMAT_HELPERS(json)
   PSIO_FORMAT_HELPERS(ijson)

   // ── Status-returning entry points ──────────────────────────────────────
   //
   // Usable with exceptions disabled. Neither throws or aborts on bad
   // input or on a value without a JSON text.

   // Reads `text` as one T. On failure `out` is unspecified. `parser`
   // selects the syntax layer (json_cursor.hpp); every choice accepts the
   // same texts and reads the same value.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status decode_json_checked(
      Fmt, std::span<const char> text, T& out, json_parser parser = json_parser::automatic)
   {
      out = T{};
      return detail::json_impl::read_document<Fmt, T>(text, &out, false, parser);
   }

   // The same for text the caller has padded (padded_json): simdjson then
   // parses it in place instead of copying it into a padded buffer.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status decode_json_checked(
      Fmt, padded_json text, T& out, json_parser parser = json_parser::automatic)
   {
      out = T{};
      return detail::json_impl::read_document<Fmt, T>(text, &out, false, parser);
   }

   // Checks that `text` is one T without building it.
   template <typename T, detail::json_impl::json_family Fmt>
   [[nodiscard]] codec_status validate_json(Fmt, std::span<const char> text,
                                            json_parser parser = json_parser::automatic)
   {
      return detail::json_impl::read_document<Fmt, T>(text, nullptr, false, parser);
   }
   template <typename T, detail::json_impl::json_family Fmt>
   [[nodiscard]] codec_status validate_json(Fmt, padded_json text,
                                            json_parser parser = json_parser::automatic)
   {
      return detail::json_impl::read_document<Fmt, T>(text, nullptr, false, parser);
   }

   // Appends the text of `value` to `out`. On failure `out` is restored.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status encode_json_checked(Fmt, const T& value,
                                                  std::string& out)
   {
      const std::size_t           start = out.size();
      detail::json_impl::writer w{out, Fmt::name, false};
      detail::json_impl::encode_document<Fmt>(value, w);
      if (w.err.empty())
         return codec_ok();
      out.resize(start);
      if (w.has_invariant)
         return codec_status{w.invariant};
      return codec_fail(w.err, static_cast<std::uint32_t>(start), Fmt::name);
   }

}  // namespace psio

#include <psio/detail/json_assoc.hpp>
