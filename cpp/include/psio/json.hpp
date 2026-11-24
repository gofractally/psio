#pragma once
//
// psio/json.hpp — the JSON text formats: `psio::json` and `psio::ijson`.
//
// Both tags write to a `std::string` (or append to a `std::vector<char>`)
// and read a `std::span<const char>` of UTF-8 text. They share one reflection-driven engine and differ in three
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
//                 `{"Label": value}` where Label is the alternative's label
//                 (variant_label.hpp: "uint32", "string", the record's
//                 name, ..., or what psio::variant_labels<V> states).
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
//   std::string_view,
//   const char*, "literal"    the same, for writers only (text has no
//                             non-owning reader)
//   std::span<T>              for writers only: the list of its elements; a
//                             span of bytes is the byte sequence it views
//   psio::view<T, F, S>       for writers only: the text of the value it
//                             views (view.hpp; psio::pssz_view<T>)
//   std::vector, std::array,
//   psio::bounded             [ ... ]
//   std::optional             null or the value
//   schema_types::Box<T>      the T it holds (transparent, as in pssz)
//   untagged std::variant     the bare value of its alternative, in both
//                             formats (untagged.hpp): the alternatives are
//                             told apart by the kind of JSON value
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
#include <psio/format.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/get_type_name.hpp>
#include <psio/variant_label.hpp>
#include <psio/json_cursor.hpp>
#include <psio/json_value.hpp>
#include <psio/reflect.hpp>
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/untagged.hpp>
#include <psio/wire_type.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for, byte_array, ...

#include <algorithm>
#include <array>
#include <bit>
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

   // How `to_json` / `to_ijson` / `encode_json_checked` give the text room.
   // The text is the same either way.
   //
   //   exact      two walks of the value: the text is measured
   //              (`json_size`), the sink is given room for exactly that
   //              — one allocation at most, none when the sink has the
   //              capacity — and the text is written. Peak memory is the
   //              text. Costs up to twice the time for text made of
   //              numbers and short strings; less time than `growth` for
   //              large strings and hex, which are measured without being
   //              read.
   //   growth     one walk: the capacity the sink has is used first (text
   //              that fits what the caller reserved never reallocates),
   //              then doubled when it runs out. A reallocation copies the
   //              text so far while both buffers are live: up to three
   //              times the text at the peak, up to twice held at the end.
   //   automatic  `exact` where memory is fixed and an allocation is not
   //              returned (WebAssembly), `growth` elsewhere. Define
   //              PSIO_JSON_SIZING (to `exact` or `growth`, the same in
   //              every translation unit) to choose for a whole program.
   enum class json_sizing : std::uint8_t
   {
      automatic,
      exact,
      growth,
   };

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

      // A psio::view (view.hpp) is written as the value it views.
      template <typename T>
      struct is_psio_view : std::false_type
      {
      };
      template <typename X, typename F, ::psio::storage S>
      struct is_psio_view<::psio::view<X, F, S>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_span : std::false_type
      {
      };
      template <typename E, std::size_t N>
      struct is_std_span<std::span<E, N>> : std::true_type
      {
      };
      // `const char*`, `char*` and `char[N]`: text, for writers.
      template <typename T>
      inline constexpr bool is_c_string_v =
         (std::is_pointer_v<T> &&
          std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>) ||
         (std::is_array_v<T> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, char>);

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
      // A view of bytes is written as the bytes it views (std::span of
      // char / unsigned char / std::byte, const or not).
      template <typename E, std::size_t N>
         requires is_byte_elem_v<std::remove_cv_t<E>>
      struct byte_seq<std::span<E, N>> : std::true_type
      {
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

      // The label of variant alternative I (variant_label.hpp): the key of
      // the tagged form.
      template <typename V, std::size_t I>
      constexpr std::string_view alt_name() noexcept
      {
         return ::psio::variant_label<V, I>();
      }

      // The labels of V, as a lookup table source.
      template <typename V>
      using alt_names = ::psio::variant_label_table<V>;

      // ── Untagged variants (untagged.hpp) ──────────────────────────────────
      //
      // The kinds of JSON value a T may be written as, in either format (a
      // bit per json_kind). A reader of an untagged variant picks the
      // alternative by the kind of the value it finds, so the alternatives'
      // sets must not overlap. The sets are those of both formats together
      // — a 64-bit integer is a number or a string, bytes an array or a
      // string — so that one variant is untagged the same way in both.
      inline constexpr unsigned kind_null = 1, kind_boolean = 2, kind_number = 4,
                                kind_string = 8, kind_array = 16, kind_object = 32,
                                kind_any = 63;
      constexpr unsigned kind_bit(json_kind k) noexcept
      {
         switch (k)
         {
            case json_kind::null: return kind_null;
            case json_kind::boolean: return kind_boolean;
            case json_kind::number: return kind_number;
            case json_kind::string: return kind_string;
            case json_kind::array: return kind_array;
            case json_kind::object: return kind_object;
         }
         return 0;
      }
      template <typename Fmt, typename T>
      constexpr unsigned json_kinds() noexcept
      {
         if constexpr (dispatch_adapter_v<Fmt, T>)
            return kind_string;
         else if constexpr (is_psio_view<T>::value)
            return json_kinds<Fmt, std::remove_cvref_t<decltype(std::declval<const T&>().unpack())>>();
         else if constexpr (std::is_same_v<T, ::psio::json_value>)
            return kind_any;
         else if constexpr (is_box<T>::value)
            return json_kinds<Fmt, typename is_box<T>::elem>();
         else if constexpr (::psio::is_transparent_v<T>)
            return json_kinds<Fmt, ::psio::transparent_inner_t<T>>();
         else if constexpr (::psio::detail::is_nested_v<T>)
            return json_kinds<Fmt, typename ::psio::detail::is_nested<T>::inner>();
         else if constexpr (std::is_same_v<T, bool>)
            return kind_boolean;
         else if constexpr (std::is_enum_v<T> && !std::is_same_v<T, std::byte>)
            return ::psio::ReflectedEnum<T>
                      ? kind_string | kind_number
                      : (sizeof(T) >= 8 ? kind_number | kind_string : kind_number);
         else if constexpr (is_wide_int_v<T>)
            return kind_number | kind_string;
         else if constexpr (std::is_same_v<T, std::byte> || std::is_integral_v<T>)
            return sizeof(T) >= 8 ? kind_number | kind_string : kind_number;
         else if constexpr (::psio::TickTextValue<T>)
            return kind_number | kind_string;
         else if constexpr (::psio::TimePointValue<T>)
            return kind_string;
         else if constexpr (std::is_floating_point_v<T>)
            return kind_number;
         else if constexpr (std::is_same_v<T, std::string> ||
                            std::is_same_v<T, std::string_view> || is_c_string_v<T> ||
                            is_utf8_string<T>::value)
            return kind_string;
         else if constexpr (byte_seq<T>::value || is_byte_array<T>::value ||
                            is_bounded_bytes<T>::value)
            return kind_array | kind_string;
         else if constexpr (is_bounded<T>::value || is_std_array<T>::value ||
                            is_std_vector<T>::value || is_std_span<T>::value)
            return kind_array;
         else if constexpr (::psio::detail::is_map_v<T>)
            return kind_object | kind_array;
         else if constexpr (::psio::detail::is_assoc_v<T>)
            return kind_array;
         else if constexpr (is_std_optional<T>::value)
            return kind_null | json_kinds<Fmt, typename T::value_type>();
         else if constexpr (is_std_variant<T>::value)
         {
            if constexpr (::psio::is_untagged_v<T>)
               return []<std::size_t... Is>(std::index_sequence<Is...>) {
                  return (0u | ... | json_kinds<Fmt, std::variant_alternative_t<Is, T>>());
               }(std::make_index_sequence<std::variant_size_v<T>>{});
            else
               return kind_array | kind_object;
         }
         else if constexpr (requires { typename T::psio_annotated_tuple; })
            return kind_object | kind_array;
         else if constexpr (is_std_tuple<T>::value)
            return kind_array;
         else if constexpr (Record<T>)
            return kind_object;
         else
            return kind_any;
      }
      template <typename Fmt, typename V>
      struct untagged_kinds
      {
         static constexpr auto value = []<std::size_t... Is>(std::index_sequence<Is...>) {
            return std::array<unsigned, sizeof...(Is)>{
               json_kinds<Fmt, std::variant_alternative_t<Is, V>>()...};
         }(std::make_index_sequence<std::variant_size_v<V>>{});
         // No kind of value is two alternatives'.
         static constexpr bool distinct = [] {
            for (std::size_t a = 0; a < value.size(); ++a)
               for (std::size_t b = a + 1; b < value.size(); ++b)
                  if (value[a] & value[b])
                     return false;
            return true;
         }();
      };

      // Sets a sink's size; growing (`!shrink`), the new bytes need no value.
      inline void set_sink_size(std::string& s, std::size_t size, bool shrink)
      {
#if defined(__cpp_lib_string_resize_and_overwrite)
         if (!shrink)
            s.resize_and_overwrite(size, [&](char*, std::size_t) { return size; });
         else
            s.resize(size);
#else
         (void)shrink;
         s.resize(size);
#endif
      }
      inline void set_sink_size(std::vector<char>& v, std::size_t size, bool)
      {
         v.resize(size);
      }

      // ── Encoding ──────────────────────────────────────────────────────────
      //
      // The writer owns a region of memory as a raw buffer: text is written
      // through a pointer after one capacity check per value (`reserve`).
      // Three kinds of destination:
      //
      //   a sink     a std::string or a std::vector<char>, appended to. The
      //              writer takes the capacity the sink already has before
      //              it allocates (text that fits what the caller reserved
      //              never reallocates), then grows by doubling. The sink's
      //              size is set once at the end (`finish`); while a writer
      //              is live the sink's size is the buffer's extent.
      //   a span     exactly `size` bytes the caller sized with a counting
      //              pass (`ijson_size`): nothing is written outside it. A
      //              worst-case reservation that does not fit the bytes left
      //              goes to a scratch buffer and is copied back.
      //   nothing    the counting pass: the text is measured, not kept.
// (the steps of a small text are flattened into its caller: they are a
// measurable part of writing one)
#if defined(__GNUC__) || defined(__clang__)
#define PSIO_JSON_INLINE __attribute__((always_inline)) inline
#else
#define PSIO_JSON_INLINE inline
#endif
      struct writer
      {
         std::string_view format;
         bool             fail_fast = true;
         std::string_view err{};
         // The status of a violated invariant, reported unchanged.
         ::psio::codec_error invariant{};
         bool                has_invariant = false;

         // The text is appended to `sink`.
         writer(std::string& sink, std::string_view format, bool fail_fast = true)
             : writer(&sink, sink.data(), sink.size(), &extend<std::string>,
                      // (the capacity every std::string has is nobody's reservation)
                      sink.capacity() > std::string{}.capacity() ? sink.capacity() : 0, format,
                      fail_fast)
         {
         }
         writer(std::vector<char>& sink, std::string_view format, bool fail_fast = true)
             : writer(&sink, sink.data(), sink.size(), &extend<std::vector<char>>,
                      sink.capacity(), format, fail_fast)
         {
         }
         // The same for a sink the caller has given a first extent: its
         // bytes from `text` on are room, not text.
         struct room_from
         {
            std::size_t text;
         };
         writer(std::string& sink, room_from from, std::string_view format, bool fail_fast = true)
             : writer(&sink, sink.data(), sink.size(), &extend<std::string>, 0, format, fail_fast)
         {
            p_ = base_ + from.text;
         }
         // The text is written to [dst, dst + size) and must be exactly
         // that long (`exact()` after `finish()`).
         writer(char* dst, std::size_t size, std::string_view format, bool fail_fast = true)
             : format(format),
               fail_fast(fail_fast),
               p_(dst),
               base_(dst),
               end_(dst + size),
               dst_(dst),
               cap_(size),
               fixed_(true)
         {
         }
         // The text is measured only: `size()` after `finish()`.
         struct count_only
         {
         };
         writer(count_only, std::string_view format, bool fail_fast = true)
             : format(format),
               fail_fast(fail_fast),
               p_(scratch_),
               base_(scratch_),
               end_(scratch_ + sizeof(scratch_)),
               fixed_(true)
         {
         }
         writer(const writer&)            = delete;
         writer& operator=(const writer&) = delete;

         // Room for `k` more bytes; returns where they go. `k` may be a
         // worst case (at most `worst_case` bytes more than is written):
         // what is not committed is not text.
         static constexpr std::size_t worst_case = 128;
         char* reserve(std::size_t k)
         {
            if (room() < k) [[unlikely]]
               grow(k);
            return p_;
         }
         // Room that is likely to be needed (a list's worth of elements):
         // one growth for a sink nobody reserved, the capacity of one
         // somebody did, nothing for an exact destination.
         void hint(std::size_t k)
         {
            if (!fixed_ && room() < k)
               grow_hint(k);
         }
         // The bytes [reserve(), end) are now text.
         void commit(const char* end) noexcept { p_ = const_cast<char*>(end); }
         void put(char c)
         {
            *reserve(1) = c;
            ++p_;
         }
         void append(std::string_view text)
         {
            if (room() < text.size()) [[unlikely]]
            {
               if (fixed_)
                  return append_fixed(text);
               grow(text.size());
            }
            std::memcpy(p_, text.data(), text.size());
            p_ += text.size();
         }
         void append(const char* b, const char* e)
         {
            append(std::string_view{b, static_cast<std::size_t>(e - b)});
         }
         template <std::size_t N>
         void literal(const std::array<char, N>& text)
         {
            if (room() < N) [[unlikely]]
               return append(std::string_view{text.data(), N});
            std::memcpy(p_, text.data(), N);
            p_ += N;
         }
         // Whether the text is only measured: a writer of long runs (a
         // string, hex) then adds its exact length with `count`.
         bool counting() const noexcept { return fixed_ && !dst_; }
         // (counting: where the bytes would go does not matter)
         void count(std::size_t k) noexcept { before_ += k; }
         // The length of the text so far (for a sink: with what it held).
         std::size_t size() const noexcept
         {
            return before_ + static_cast<std::size_t>(p_ - base_);
         }
         // Drops the text after its first `n` bytes (`n <= size()`): a
         // value that turned out to have no text is taken back.
         void truncate(std::size_t n)
         {
            flush();
            if (base_ == dst_)
               p_ = base_ + n;
            else
            {
               // (counting, or a span that overflowed)
               before_ = n;
               p_      = base_;
            }
         }
         // Ends the buffer phase: a sink is exactly the text.
         PSIO_JSON_INLINE void finish()
         {
            flush(true);
            if (fixed_)
               return;
            const std::size_t n = size();
            if (n != cap_)
               set_extent(n, resize::exact);
            set_main(n);
         }
         // An exact destination, after `finish`: it holds the text and
         // nothing else.
         bool exact() const noexcept { return !overflow_ && size() == cap_; }
         bool overflowed() const noexcept { return overflow_; }

         void fail(std::string_view what)
         {
            if (fail_fast)
            {
               const auto at = static_cast<std::uint32_t>(size());
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
         // How a sink's size is set.
         enum class resize : std::uint8_t
         {
            exact,   // to `need`
            within,  // towards `need` (doubling), within the capacity it has: no allocation
            grow,    // to at least `need` (doubling): allocates when the capacity is short
         };
         // Sets the sink's size (`extent`, before and after) and returns
         // its data. Bytes added need no value.
         using extend_fn = char* (*)(void* sink, std::size_t need, std::size_t& extent, resize how);
         PSIO_JSON_INLINE char* set_extent(std::size_t need, resize how)
         {
            // (the calls are direct so that they can be inlined)
            dst_ = extend_ == &extend<std::string> ? extend<std::string>(sink_, need, cap_, how)
                                                   : extend<std::vector<char>>(sink_, need, cap_, how);
            if (cap_ > sink_capacity_)
               sink_capacity_ = cap_;
            return dst_;
         }

         writer(void* sink, char* data, std::size_t size, extend_fn extend, std::size_t capacity,
                std::string_view format, bool fail_fast)
             : format(format),
               fail_fast(fail_fast),
               p_(data + size),
               base_(data),
               end_(data + size),
               sink_(sink),
               extend_(extend),
               dst_(data),
               cap_(size),
               honour_(capacity > size),
               sink_capacity_(capacity)
         {
         }

         // The region being written: [base_, end_), text up to p_. It is
         // the destination ([dst_, dst_ + cap_): the sink's buffer or the
         // caller's span) or a scratch buffer.
         // (p_ and end_ apart: loaded as a pair right after p_ is stored,
         // they are not forwarded from the store on every CPU)
         char* p_;
         char* base_;
         char* end_;
         // Text bytes that precede base_ (when it is a scratch buffer).
         std::size_t before_ = 0;
         // A sink.
         void*     sink_   = nullptr;
         extend_fn extend_ = nullptr;
         // The destination; counting has none.
         char*       dst_ = nullptr;
         std::size_t cap_ = 0;
         // A span or counting: nothing grows.
         bool fixed_ = false;
         // A span: more text than it holds was written (and dropped).
         bool overflow_ = false;
         // A sink that came with capacity to spare, not yet used up: it is
         // reallocated only for text that does not fit it. `hint_` is the
         // size a sink is given when it is reallocated.
         bool        honour_ = false;
         std::size_t hint_   = 0;
         // The sink's capacity, as far as the writer knows (at least).
         std::size_t sink_capacity_ = 0;
         // Where a reservation goes that the destination has no room for.
         std::unique_ptr<char[]> big_;
         std::size_t             big_size_ = 0;
         char                    scratch_[worst_case];

         std::size_t room() const noexcept { return static_cast<std::size_t>(end_ - p_); }
         // The region is the destination, holding `n` bytes of text.
         void set_main(std::size_t n) noexcept
         {
            base_   = dst_;
            p_      = dst_ + n;
            end_    = dst_ + cap_;
            before_ = 0;
         }
         // The region is a scratch buffer of at least `k` bytes; the text
         // so far stays where it is.
         void set_scratch(std::size_t k)
         {
            before_           = size();
            char*       s     = scratch_;
            std::size_t avail = sizeof(scratch_);
            if (k > avail)
            {
               if (k > big_size_)
               {
                  big_.reset(new char[k]);
                  big_size_ = k;
               }
               s     = big_.get();
               avail = big_size_;
            }
            base_ = p_ = s;
            end_       = s + avail;
         }

         template <typename S>
         static constexpr bool zero_fills = !std::is_same_v<S, std::string>;
         template <typename S>
         PSIO_JSON_INLINE static char* extend(void* sink, std::size_t need, std::size_t& extent,
                                              resize how)
         {
            auto& s = *static_cast<S*>(sink);
            if (how != resize::exact)
            {
               // By doubling. Within the capacity too (a
               // std::vector<char> gives its new bytes a value: a small
               // text must not pay for a large reservation).
               std::size_t want = std::max({need, extent * 2, std::size_t{128}});
               if (how == resize::within)
               {
                  const std::size_t cap = s.capacity();
                  want                  = zero_fills<S> ? std::min(want, cap) : cap;
               }
               need = std::max(want, extent);
            }
            set_sink_size(s, need, how == resize::exact);
            extent = need;
            return s.data();
         }

         void grow(std::size_t k)
         {
            flush();
            if (room() >= k)
               return;
            if (fixed_)
               return set_scratch(k);
            const std::size_t n = static_cast<std::size_t>(p_ - base_);
            if (honour_)
            {
               if (n + k <= sink_capacity_)
               {
                  set_extent(n + k, resize::within);
                  return set_main(n);
               }
               // The capacity the caller reserved is short of `k`. A
               // worst case may still fit: it is written to scratch, and
               // `flush` makes room for what was committed, so the sink
               // is reallocated only for text that does not fit it.
               if (k <= worst_case)
                  return set_scratch(k);
               honour_ = false;
            }
            set_extent(std::max(n + k, hint_), resize::grow);
            set_main(n);
         }
         void grow_hint(std::size_t k)
         {
            flush();
            const std::size_t n = static_cast<std::size_t>(p_ - base_);
            hint_               = std::max(hint_, n + k);
            // (a sink that was reserved is given the hinted size only if
            // its capacity turns out too small for the text)
            if (!honour_)
               set_extent(hint_, resize::grow);
            else if (hint_ <= sink_capacity_)
               set_extent(hint_, resize::within);
            set_main(n);
         }
         // What was written to scratch goes to its place, and the region
         // is the destination again (counting: the scratch buffer, whole;
         // a span that overflowed: empty). `last`: no more text follows.
         void flush(bool last = false)
         {
            if (base_ != dst_)
               flush_scratch(last);
         }
         void flush_scratch(bool last)
         {
            const std::size_t len = static_cast<std::size_t>(p_ - base_);
            const std::size_t at  = before_;
            const std::size_t pos = at + len;
            if (!fixed_)
            {
               if (pos > cap_)
               {
                  if (last)
                     set_extent(pos, resize::exact);
                  else if (pos <= sink_capacity_)
                     set_extent(pos, resize::within);
                  else
                  {
                     // Text the sink's capacity does not hold.
                     honour_ = false;
                     set_extent(std::max(pos, hint_), resize::grow);
                  }
               }
               if (len)
                  std::memcpy(dst_ + at, base_, len);
               return set_main(pos);
            }
            if (!dst_)
            {
               before_ = pos;
               base_ = p_ = scratch_;
               end_       = scratch_ + sizeof(scratch_);
               return;
            }
            if (!overflow_ && pos <= cap_)
            {
               if (len)
                  std::memcpy(dst_ + at, base_, len);
               return set_main(pos);
            }
            // Text longer than the span: counted, not written.
            overflow_ = true;
            base_ = p_ = end_ = scratch_;
            before_           = pos;
         }
         // Text the region has no room for, a span or counting.
         void append_fixed(std::string_view text)
         {
            flush();
            if (room() >= text.size())
            {
               std::memcpy(p_, text.data(), text.size());
               p_ += text.size();
               return;
            }
            if (dst_)
            {
               before_   = size();
               overflow_ = true;
               base_ = p_ = end_ = scratch_;
            }
            before_ += text.size();
         }
      };

#undef PSIO_JSON_INLINE

      // The length of the text write_string writes for sv, which it adds
      // to a counting writer. The same checks, the same failures.
#if defined(__GNUC__) || defined(__clang__)
      __attribute__((noinline))
#endif
      inline void count_string(writer& w, std::string_view sv, bool nonchar)
      {
         std::size_t total = sv.size() + 2;
         const auto* p     = reinterpret_cast<const unsigned char*>(sv.data());
         const auto* end   = p + sv.size();
         while (true)
         {
            while (end - p >= 8)
            {
               std::uint64_t word;
               std::memcpy(&word, p, 8);
               if (string_special(word))
                  break;
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
                  w.fail("json: string is not valid UTF-8");
                  break;
               }
               if (nonchar && is_noncharacter(cp))
               {
                  w.fail("ijson: string holds a Unicode noncharacter");
                  break;
               }
               p += len;
               continue;
            }
            if (c < 0x20)
               total += (c == '\n' || c == '\r' || c == '\t') ? 1 : 5;
            else if (c == '"' || c == '\\')
               total += 1;
            ++p;
         }
         w.count(total);
      }

      // Quotes and escapes sv. The text must be UTF-8; `nonchar` also
      // refuses Unicode noncharacters (I-JSON).
      inline void write_string(writer& w, std::string_view sv, bool nonchar)
      {
         if (w.counting()) [[unlikely]]
            return count_string(w, sv, nonchar);
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
            // An escape: two or six bytes instead of one. The room asked
            // for is exactly what the rest of the string needs if it
            // holds no further escape (an exact destination has that
            // much and may have no more).
            char letter = 0;
            switch (c)
            {
               case '"':  letter = '"'; break;
               case '\\': letter = '\\'; break;
               case '\n': letter = 'n'; break;
               case '\r': letter = 'r'; break;
               case '\t': letter = 't'; break;
               default: break;
            }
            w.commit(out);
            const std::size_t rest = static_cast<std::size_t>(end - p);
            if (letter)
            {
               out    = w.reserve(rest + 2);
               *out++ = '\\';
               *out++ = letter;
            }
            else
            {
               static constexpr char hex[] = "0123456789abcdef";
               out    = w.reserve(rest + 6);
               *out++ = '\\';
               *out++ = 'u';
               *out++ = '0';
               *out++ = '0';
               *out++ = hex[c >> 4];
               *out++ = hex[c & 0xF];
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
         if (w.counting())
            return w.count(2 * n + 2);
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

      // The number of decimal digits of v.
      constexpr std::size_t decimal_digits(std::uint64_t v) noexcept
      {
         constexpr std::uint64_t pow10[20] = {1ull,
                                              10ull,
                                              100ull,
                                              1000ull,
                                              10000ull,
                                              100000ull,
                                              1000000ull,
                                              10000000ull,
                                              100000000ull,
                                              1000000000ull,
                                              10000000000ull,
                                              100000000000ull,
                                              1000000000000ull,
                                              10000000000000ull,
                                              100000000000000ull,
                                              1000000000000000ull,
                                              10000000000000000ull,
                                              100000000000000000ull,
                                              1000000000000000000ull,
                                              10000000000000000000ull};
         v |= 1;
         const std::size_t t = (static_cast<std::size_t>(std::bit_width(v)) * 1233) >> 12;
         return t + 1 - (v < pow10[t]);
      }
      static_assert(decimal_digits(0) == 1 && decimal_digits(9) == 1 && decimal_digits(10) == 2 &&
                    decimal_digits(9999999999999999999ull) == 19 &&
                    decimal_digits(10000000000000000000ull) == 20 &&
                    decimal_digits(~std::uint64_t{0}) == 20);

      template <typename Fmt, int_mode IM, typename I>
      void encode_integer(I v, writer& w)
      {
         bool quoted = int_as_string_v<Fmt, IM, I>;
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
         if constexpr (sizeof(I) <= 8)
            if (w.counting()) [[unlikely]]
            {
               // Measured without being written.
               const bool          negative  = v < 0;
               const std::uint64_t magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(v)
                                                        : static_cast<std::uint64_t>(v);
               return w.count(decimal_digits(magnitude) + negative + 2 * quoted);
            }
         char* const out = w.reserve(48);
         char*       p   = out;
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
            if constexpr (::psio::detail::ann::has_value_checks_v<F, eff>)
               if (auto st = ::psio::check_member_annotations(
                      fref, eff_for::value, "json: member length_bound violated", "json");
                   !st.ok())
                  w.fail(st.error());
            constexpr int_mode mode = field_mode<Fmt, eff>(eff_for::value);
            encode_value<Fmt, mode>(fref, w);
         }
      }

      // ── psio::json_value (json_value.hpp): any JSON value ─────────────────
      //
      // Whether `token` is exactly one RFC 8259 number.
      inline bool is_number_token(std::string_view token)
      {
         if (token.empty())
            return false;
         reader r{token.data(), token.data(), token.data() + token.size()};
         return skip_number(r) && r.p == r.end;
      }
      // The text of `v`: compact, members in their order, the last value
      // of a key an object holds twice. `nonchar`: the I-JSON string rule.
      inline void write_any(const ::psio::json_value& v, writer& w, bool nonchar,
                            std::size_t depth = 0)
      {
         using value_t = ::psio::json_value;
         if (v.is_null())
            w.append("null");
         else if (const bool* b = v.as_bool())
            w.append(*b ? std::string_view{"true"} : std::string_view{"false"});
         else if (const auto* n = v.as_number())
         {
            if (is_number_token(n->text))
               w.append(std::string_view{n->text});
            else
            {
               w.fail("json: a json_value number is not a JSON number");
               w.append("null");
            }
         }
         else if (const auto* text = v.as_string())
            write_string(w, std::string_view{*text}, nonchar);
         else if (depth >= kMaxValidationDepth)
         {
            // (deeper than any reader accepts)
            w.fail("json: max depth exceeded");
            w.append("null");
         }
         else if (const value_t::array* items = v.as_array())
         {
            w.put('[');
            bool first = true;
            for (const value_t& item : *items)
            {
               if (!first)
                  w.put(',');
               first = false;
               write_any(item, w, nonchar, depth + 1);
            }
            w.put(']');
         }
         else if (const value_t::object* members = v.as_object())
         {
            w.put('{');
            bool first = true;
            for (std::size_t i = 0; i < members->size(); ++i)
            {
               // A key held twice: its last value, once.
               bool later = false;
               for (std::size_t j = i + 1; j < members->size() && !later; ++j)
                  later = (*members)[j].first == (*members)[i].first;
               if (later)
                  continue;
               if (!first)
                  w.put(',');
               first = false;
               write_string(w, std::string_view{(*members)[i].first}, nonchar);
               w.put(':');
               write_any((*members)[i].second, w, nonchar, depth + 1);
            }
            w.put('}');
         }
      }
      // Reads one value of any shape. A repeated key keeps its last value,
      // at the position of the first. `out == nullptr`: checked only.
      template <typename C>
      bool read_any(C& c, ::psio::json_value* out)
      {
         using value_t = ::psio::json_value;
         if (!out)
            return c.skip();
         json_kind k;
         if (!c.kind(k))
            return false;
         std::string_view text;
         bool             flag = false;
         switch (k)
         {
            case json_kind::null:
               if (!c.null())
                  return false;
               out->value = std::monostate{};
               return true;
            case json_kind::boolean:
               if (!c.boolean(flag))
                  return false;
               out->value = flag;
               return true;
            case json_kind::number:
               if (!c.number(text))
                  return false;
               out->value = ::psio::json_numeral{std::string{text}};
               return true;
            case json_kind::string:
               if (!c.string(text, flag))
                  return false;
               out->value = std::string{text};
               return true;
            case json_kind::array:
            {
               if (!c.begin_array())
                  return false;
               value_t::array items;
               for (;;)
               {
                  if (!c.next_element(flag))
                     return false;
                  if (!flag)
                     break;
                  if (!read_any(c, &items.emplace_back()))
                     return false;
               }
               out->value = std::move(items);
               return true;
            }
            case json_kind::object:
            {
               if (!c.begin_object())
                  return false;
               value_t::object members;
               for (;;)
               {
                  if (!c.next_field(flag, text))
                     return false;
                  if (!flag)
                     break;
                  value_t* slot = nullptr;
                  for (auto& m : members)
                     if (m.first == text)
                     {
                        slot = &m.second;
                        break;
                     }
                  if (!slot)
                     slot = &members.emplace_back(std::string{text}, value_t{}).second;
                  if (!read_any(c, slot))
                     return false;
               }
               out->value = std::move(members);
               return true;
            }
         }
         return c.fail("json: unexpected character at value start");
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
         else if constexpr (is_psio_view<T>::value)
         {
            // A view (of validated bytes) is the text of the value it
            // views: the same text as the owning type's. Written only.
            const auto value = v.unpack();
            encode_value<Fmt, IM>(value, w);
         }
         else if constexpr (std::is_same_v<T, ::psio::json_value>)
            write_any(v, w, Fmt::i_json);
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
         {
            // A member outside the bounds its type gives it has no text.
            if constexpr (::psio::transparent_member_checked_v<T>)
               if (auto st = ::psio::check_transparent_member(
                      v, "json: member length_bound violated", "json");
                   !st.ok())
                  w.fail(st.error());
            encode_value<Fmt, IM>(::psio::transparent_inner(v), w);
         }
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
         else if constexpr (std::is_same_v<T, std::string> ||
                            std::is_same_v<T, std::string_view>)
            write_string(w, std::string_view{v}, Fmt::i_json);
         else if constexpr (is_c_string_v<T>)
         {
            // A string literal / char array (its characters up to the
            // terminator) or a C string. Written only: text has no
            // non-owning reader. A null pointer is no string.
            if constexpr (std::is_array_v<T>)
               write_string(w, std::string_view{v, ::strnlen(v, std::extent_v<T>)}, Fmt::i_json);
            else if (v)
               write_string(w, std::string_view{v}, Fmt::i_json);
            else
            {
               w.fail("json: null C string");
               w.append("null");
            }
         }
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
                            is_std_vector<T>::value || is_std_span<T>::value)
         {
            // (a std::span — written only — is the list of its elements)
            using E = std::remove_cv_t<typename T::value_type>;
            // Elements of a constant text bound: one reservation for all.
            if constexpr (constexpr std::size_t bound = text_bound<Fmt, E>(); bound != 0)
               w.hint(v.size() * (bound + 1) + 2);
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
         else if constexpr (is_std_variant<T>::value && ::psio::is_untagged_v<T>)
         {
            // Untagged (untagged.hpp): the bare value of the alternative.
            static_assert(untagged_kinds<Fmt, T>::distinct,
                          "psio::json: the alternatives of an untagged variant must be told "
                          "apart by the kind of JSON value each is written as (null, boolean, "
                          "number, string, array, object; a 64-bit integer may be a number or "
                          "a string, bytes an array or a string)");
            std::visit(
               [&](const auto& alt) { encode_value<Fmt, int_mode::natural>(alt, w); }, v);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            if constexpr (Fmt::i_json)
            {
               // Named form: {"AlternativeName": value}.
               PSIO_REQUIRE_VARIANT_LABELS(T);
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
            // A method's arguments (docs/json.md "Method arguments"): an
            // object keyed by parameter name, in declaration order, when the
            // method names its parameters; the positional array otherwise.
            if constexpr (::psio::annotated_tuple_named_v<T>)
            {
               w.put('{');
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (((Is ? w.put(',') : void()),
                    write_string(w, T::meta_type::names[Is], Fmt::i_json), w.put(':'),
                    encode_value<Fmt, int_mode::natural>(std::get<Is>(v.tuple()), w)),
                   ...);
               }(std::make_index_sequence<std::tuple_size_v<typename T::tuple_type>>{});
               w.put('}');
            }
            else
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

      // Encodes one whole value and leaves the sink as the text.
      template <typename Fmt, typename T>
      void encode_document(const T& v, writer& w)
      {
         if constexpr (constexpr std::size_t bound = text_bound<Fmt, T>(); bound != 0)
            w.hint(bound);
         encode_value<Fmt, int_mode::natural>(v, w);
         w.finish();
      }

      // What a writer that does not fail fast ended with.
      inline codec_status writer_status(const writer& w, std::size_t at)
      {
         if (w.err.empty())
            return codec_ok();
         if (w.has_invariant)
            return codec_status{w.invariant};
         return codec_fail(w.err, static_cast<std::uint32_t>(at), w.format);
      }

      // The exact length of the text of `v`, in `size`: the walk of the
      // writer with nothing written, so every check runs and a value
      // without a text fails as it does when it is written.
      template <typename Fmt, typename T>
      codec_status measure_document(const T& v, std::size_t& size, bool fail_fast)
      {
         writer w{writer::count_only{}, Fmt::name, fail_fast};
         encode_document<Fmt>(v, w);
         size = w.size();
         return writer_status(w, 0);
      }

      // Writes the text of `v` to [dst, dst + size), `size` being what
      // measure_document gave for the same value. Nothing is written
      // outside the span whatever the value is by now.
      template <typename Fmt, typename T>
      codec_status write_document(const T& v, char* dst, std::size_t size, bool fail_fast)
      {
         writer w{dst, size, Fmt::name, fail_fast};
         encode_document<Fmt>(v, w);
         if (auto st = writer_status(w, 0); !st.ok())
            return st;
         if (!w.exact())
         {
            const codec_error error{"json: the value changed between sizing and writing", 0,
                                    Fmt::name};
            if (fail_fast)
               ::psio::codec_abort(error);
            return codec_status{error};
         }
         return codec_ok();
      }

      // What json_sizing::automatic is.
#if defined(PSIO_JSON_SIZING)
      inline constexpr ::psio::json_sizing default_sizing = ::psio::json_sizing::PSIO_JSON_SIZING;
#elif defined(__wasm__)
      inline constexpr ::psio::json_sizing default_sizing = ::psio::json_sizing::exact;
#else
      inline constexpr ::psio::json_sizing default_sizing = ::psio::json_sizing::growth;
#endif

      // Appends the text of `v` to `sink`. On failure (not fail-fast) the
      // sink is restored.
      template <typename Fmt, typename T, typename Sink>
      codec_status append_document(const T& v, Sink& sink, ::psio::json_sizing how, bool fail_fast)
      {
         const std::size_t start = sink.size();
         if (how == ::psio::json_sizing::automatic)
            how = default_sizing;
         if (how == ::psio::json_sizing::growth)
         {
            writer w{sink, Fmt::name, fail_fast};
            encode_document<Fmt>(v, w);
            auto st = writer_status(w, start);
            if (!st.ok())
               sink.resize(start);
            return st;
         }
         std::size_t size = 0;
         if (auto st = measure_document<Fmt>(v, size, fail_fast); !st.ok())
            return st;
         if (sink.capacity() - start < size)
            sink.reserve(start + size);
         set_sink_size(sink, start + size, false);
         auto st = write_document<Fmt>(v, sink.data() + start, size, fail_fast);
         if (!st.ok())
            sink.resize(start);
         return st;
      }

      // Reads one value of any shape from `c` and writes its compact text
      // (no whitespace; strings re-escaped by the writer's rule; a number as
      // its token; members in order, a repeated key repeated). The value is
      // checked exactly as a skipped one is.
      template <typename C>
      bool capture_value(C& c, writer& w)
      {
         json_kind k;
         if (!c.kind(k))
            return false;
         std::string_view text;
         bool             flag = false;
         switch (k)
         {
            case json_kind::object:
               if (!c.begin_object())
                  return false;
               w.put('{');
               for (bool first = true;; first = false)
               {
                  if (!c.next_field(flag, text))
                     return false;
                  if (!flag)
                     break;
                  if (!first)
                     w.put(',');
                  write_string(w, text, false);
                  w.put(':');
                  if (!capture_value(c, w))
                     return false;
               }
               w.put('}');
               return true;
            case json_kind::array:
               if (!c.begin_array())
                  return false;
               w.put('[');
               for (bool first = true;; first = false)
               {
                  if (!c.next_element(flag))
                     return false;
                  if (!flag)
                     break;
                  if (!first)
                     w.put(',');
                  if (!capture_value(c, w))
                     return false;
               }
               w.put(']');
               return true;
            case json_kind::string:
               if (!c.string(text, flag))
                  return false;
               write_string(w, text, false);
               return true;
            case json_kind::number:
               if (!c.number(text))
                  return false;
               w.append(text);
               return true;
            case json_kind::boolean:
               if (!c.boolean(flag))
                  return false;
               w.append(flag ? std::string_view{"true"} : std::string_view{"false"});
               return true;
            case json_kind::null:
               if (!c.null())
                  return false;
               w.append("null");
               return true;
         }
         return c.fail("json: unexpected character at value start");
      }

      // Reads one value of any shape from `c` and writes it laid out for a
      // person: one member or element per line, `indent` spaces per level,
      // `"key": value`, an empty array or object as `[]` / `{}`. Strings
      // and numbers as capture_value writes them.
      template <typename C>
      bool pretty_value(C& c, writer& w, std::size_t indent, std::size_t level)
      {
         json_kind k;
         if (!c.kind(k))
            return false;
         if (k != json_kind::object && k != json_kind::array)
            return capture_value(c, w);
         const bool       object = k == json_kind::object;
         std::string_view key;
         bool             has = false;
         auto newline = [&](std::size_t depth) {
            w.put('\n');
            char* out = w.reserve(depth * indent);
            std::memset(out, ' ', depth * indent);
            w.commit(out + depth * indent);
         };
         if (!(object ? c.begin_object() : c.begin_array()))
            return false;
         w.put(object ? '{' : '[');
         bool any = false;
         for (;;)
         {
            if (!(object ? c.next_field(has, key) : c.next_element(has)))
               return false;
            if (!has)
               break;
            if (any)
               w.put(',');
            any = true;
            newline(level + 1);
            if (object)
            {
               write_string(w, key, false);
               w.append(": ");
            }
            if (!pretty_value(c, w, indent, level + 1))
               return false;
         }
         if (any)
            newline(level);
         w.put(object ? '}' : ']');
         return true;
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
            if constexpr (::psio::detail::ann::has_value_checks_v<F, eff>)
            {
               // The member's annotations are part of its type: its
               // length_bound / utf8 / sorted / unique and those of every
               // position below it.
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

      // "json: missing required member "amount" of Transfer": the message
      // for member I of T, in static storage (a cursor keeps a view).
      template <typename T, std::size_t I>
      struct missing_member
      {
         static constexpr std::string_view prefix = "json: missing required member \"";
         static constexpr std::string_view name   = ::psio::reflect<T>::template member_name<I>;
         static constexpr std::string_view middle = "\" of ";
         static constexpr std::string_view type   = ::psio::reflect<T>::name;
         static constexpr auto             storage = [] {
            std::array<char, prefix.size() + name.size() + middle.size() + type.size()> text{};
            std::size_t at = 0;
            for (std::string_view part : {prefix, name, middle, type})
               for (char ch : part)
                  text[at++] = ch;
            return text;
         }();
         static constexpr std::string_view value{storage.data(), storage.size()};
      };

      // The same for parameter I of a named argument list T
      // (psio::annotated_tuple): "json: missing required member "amount"".
      template <typename T, std::size_t I>
      struct missing_argument
      {
         static constexpr std::string_view prefix = "json: missing required member \"";
         static constexpr std::string_view name{T::meta_type::names[I]};
         static constexpr auto             storage = [] {
            std::array<char, prefix.size() + name.size() + 1> text{};
            std::size_t                                       at = 0;
            for (std::string_view part : {prefix, name, std::string_view{"\""}})
               for (char ch : part)
                  text[at++] = ch;
            return text;
         }();
         static constexpr std::string_view value{storage.data(), storage.size()};
      };

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
         // member is an error that names the first one.
         std::string_view missing{};
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
                   else if (missing.empty())
                      missing = missing_member<T, Is>::value;
                }()),
               ...);
         }(std::make_index_sequence<N>{});
         if (!missing.empty())
            return r.c.fail(missing);
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
         else if constexpr (std::is_same_v<T, ::psio::json_value>)
            return read_any(r.c, out);
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
         {
            if constexpr (::psio::transparent_member_checked_v<T>)
            {
               // Checked on the value read, so it is materialised even when
               // only validating (as an annotated record member is).
               T        scratch{};
               T* const dst = out ? out : &scratch;
               if (!read_value<Fmt, IM, ::psio::transparent_inner_t<T>>(
                      r, &::psio::transparent_inner(*dst)))
                  return false;
               if (auto st = ::psio::check_transparent_member(
                      *dst, "json: member length_bound violated", "json");
                   !st.ok())
                  return r.c.fail(st.error().what);
               return true;
            }
            else
               return read_value<Fmt, IM, ::psio::transparent_inner_t<T>>(
                  r, out ? &::psio::transparent_inner(*out) : nullptr);
         }
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
            if constexpr (::psio::is_untagged_v<T>)
            {
               // Untagged (untagged.hpp): the alternative whose values are
               // written as the kind of value found.
               static_assert(untagged_kinds<Fmt, T>::distinct,
                             "psio::json: the alternatives of an untagged variant must be told "
                             "apart by the kind of JSON value each is written as (null, "
                             "boolean, number, string, array, object; a 64-bit integer may be "
                             "a number or a string, bytes an array or a string)");
               json_kind kind;
               if (!r.c.kind(kind))
                  return false;
               const unsigned bit = kind_bit(kind);
               for (std::size_t index = 0; index < N; ++index)
                  if (untagged_kinds<Fmt, T>::value[index] & bit)
                     return alternative(index);
               (void)has;
               return r.c.fail("json: the value is none of the untagged variant's alternatives");
            }
            else if constexpr (Fmt::i_json)
            {
               PSIO_REQUIRE_VARIANT_LABELS(T);
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
            // A named argument list reads from either form: the object
            // keyed by parameter name (any member order, the last of a
            // repeated key, unknown keys skipped, a missing optional
            // argument empty — the record rule) or the positional array.
            bool as_object = false;
            if constexpr (::psio::annotated_tuple_named_v<T>)
            {
               json_kind kind;
               if (!r.c.kind(kind))
                  return false;
               as_object = kind == json_kind::object;
            }
            if (as_object)
            {
               if constexpr (::psio::annotated_tuple_named_v<T>)
               {
                  constexpr std::size_t N = std::tuple_size_v<Tuple>;
                  if (!r.c.begin_object())
                     return false;
                  std::array<bool, N + 1> seen{};
                  std::string_view        key;
                  bool                    more = false;
                  if (!r.c.next_field(more, key))
                     return false;
                  while (more)
                  {
                     bool found = false, ok = true;
                     [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                        (void)((key == T::meta_type::names[Is]
                                   ? (found = true, seen[Is] = true,
                                      ok = read_value<Fmt, int_mode::natural,
                                                      std::tuple_element_t<Is, Tuple>>(
                                         r, &std::get<Is>(*dst)),
                                      true)
                                   : false) ||
                               ...);
                     }(std::make_index_sequence<N>{});
                     if (!ok)
                        return false;
                     if (!found && !r.c.skip())
                        return false;
                     if (!r.c.next_field(more, key))
                        return false;
                  }
                  std::string_view missing{};
                  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                     (([&] {
                         if (seen[Is])
                            return;
                         if constexpr (is_std_optional<std::tuple_element_t<Is, Tuple>>::value)
                            std::get<Is>(*dst).reset();
                         else if (missing.empty())
                            missing = missing_argument<T, Is>::value;
                      }()),
                      ...);
                  }(std::make_index_sequence<N>{});
                  if (!missing.empty())
                     return r.c.fail(missing);
               }
            }
            else if (!read_value<Fmt, int_mode::natural, Tuple>(r, dst))
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
      codec_status read_with(C& cursor, T* out)
      {
         typed_reader<C> r{cursor};
         if (read_value<Fmt, int_mode::natural, T>(r, out))
            (void)cursor.finish();
         if (!r.invariant.ok())
            return std::move(r.invariant);
         return cursor.status(Fmt::name);
      }

      // Reads `bytes` as one T through the syntax layer `parser` selects.
      template <typename Fmt, typename T>
      codec_status read_document(std::span<const char> bytes, T* out,
                                 json_parser parser = json_parser::automatic)
      {
#if PSIO_HAVE_SIMDJSON
         if (use_simdjson(bytes, parser))
         {
            if (parser == json_parser::simdjson)
            {
               simdjson_cursor cursor(bytes, Fmt::i_json);
               return read_with<Fmt, T>(cursor, out);
            }
            simdjson_index_cursor cursor(bytes, Fmt::i_json);
            return read_with<Fmt, T>(cursor, out);
         }
#else
         (void)parser;
#endif
         json_text_cursor cursor(bytes, Fmt::i_json);
         return read_with<Fmt, T>(cursor, out);
      }

      // `text` has at least json_padding readable bytes after it: simdjson
      // parses it in place, with no copy.
      template <typename Fmt, typename T>
      codec_status read_document(padded_json text, T* out,
                                 json_parser parser = json_parser::automatic)
      {
#if PSIO_HAVE_SIMDJSON
         if (use_simdjson(text.text, parser))
         {
            if (parser == json_parser::simdjson)
            {
               simdjson_cursor cursor(text, Fmt::i_json);
               return read_with<Fmt, T>(cursor, out);
            }
            simdjson_index_cursor cursor(text, Fmt::i_json);
            return read_with<Fmt, T>(cursor, out);
         }
#else
         (void)parser;
#endif
         json_text_cursor cursor(text.text, Fmt::i_json);
         return read_with<Fmt, T>(cursor, out);
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
         //
         // Appended to a std::string or a std::vector<char> (an HTTP body).
         template <typename T, typename Sink>
            requires(std::same_as<Sink, std::string> || std::same_as<Sink, std::vector<char>>)
         friend void tag_invoke(decltype(::psio::encode), Derived, const T& v, Sink& sink)
         {
            if constexpr (default_sizing == ::psio::json_sizing::growth)
            {
               writer w{sink, Derived::name};
               encode_document<Derived>(v, w);
            }
            else
               (void)append_document<Derived>(v, sink, default_sizing, true);
         }

         template <typename T>
         friend std::string tag_invoke(decltype(::psio::encode), Derived,
                                       const T& v)
         {
            if constexpr (default_sizing == ::psio::json_sizing::growth)
            {
               constexpr std::size_t bound = text_bound<Derived, T>();
               if constexpr (bound != 0 && bound <= 64)
               {
                  // A small text of bounded length is written on the
                  // stack: the string is made once, at its length.
                  char   text[bound + writer::worst_case];
                  writer w{text, sizeof(text), Derived::name};
                  encode_document<Derived>(v, w);
                  if (!w.overflowed()) [[likely]]
                     return std::string(text, w.size());
               }
               // The first extent is made with the string (a string just
               // made and resized out of line is read back before its
               // stores have settled: a measurable part of a small text).
               // (a bounded text gets its bound: one allocation)
               std::string out(bound > 128 && bound <= 1024 ? bound : std::size_t{128}, '\0');
               writer      w{out, writer::room_from{0}, Derived::name};
               encode_document<Derived>(v, w);
               return out;
            }
            else
            {
               std::string out;
               (void)append_document<Derived>(v, out, default_sizing, true);
               return out;
            }
         }

         // ── decode (fail-fast) ──────────────────────────────────────────
         template <typename T>
         friend T tag_invoke(decltype(::psio::decode<T>), Derived, T*,
                             std::span<const char> bytes)
         {
            T out{};
            if (auto st = read_document<Derived, T>(bytes, &out);
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
            return read_document<Derived, T>(bytes, &out);
         }

         // ── size_of ─────────────────────────────────────────────────────
         //
         // The exact length of the text, measured: the writer's walk with
         // nothing written (no buffer for the text is made).
         template <typename T>
         friend std::size_t tag_invoke(decltype(::psio::size_of), Derived,
                                       const T& v)
         {
            std::size_t size = 0;
            (void)measure_document<Derived>(v, size, true);
            return size;
         }

         // ── validate / validate_strict ──────────────────────────────────
         //
         // Typed: the text must be one value of T's shape.
         template <typename T>
         friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                        Derived, T*,
                                        std::span<const char> bytes) noexcept
         {
            // (A type's maxDynamicData caps its encoded PSSZ size, not the
            // length of its text: the text of a value within the cap may be
            // longer than the cap.)
            return read_document<Derived, T>(bytes, nullptr);
         }

         template <typename T>
         friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                        Derived, T*,
                                        std::span<const char> bytes) noexcept
         {
            return read_document<Derived, T>(bytes, nullptr);
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
            if (!read_document<Derived, T>(bytes, out.get())
                    .ok())
               return nullptr;
            return out;
         }
      };

      // What a json_ref holds: a reference to an lvalue, the value of an
      // rvalue.
      template <typename V>
      using held_t = std::conditional_t<std::is_lvalue_reference_v<V>,
                                        const std::remove_cvref_t<V>&, std::remove_cvref_t<V>>;

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

   // ── Typed reads from an open cursor ────────────────────────────────────
   //
   // For a reader written by hand over a JsonCursor (json_cursor.hpp) that
   // delegates the typed parts of its document to PSIO:
   //
   //    psio::json_text_cursor c{text, true};          // or a simdjson cursor
   //    psio::json_reader      r{psio::ijson{}, c};
   //    bool has = false, known = false;  std::string_view key;
   //    if (!c.begin_object()) ...
   //    while (c.next_field(has, key) && has)
   //    {
   //       if (key == "rowid")      ok = r.read(rowid);            // any T
   //       else if (key == "raw")   ok = r.raw(raw_text);          // kept unparsed
   //       else                     ok = r.read_member(key, row, known) &&
   //                                     (known || r.skip());      // a member of T
   //    }
   //    if (!c.finish() || !r.status().ok()) ...
   //
   // Each function reads exactly one value at the cursor, by the rules of
   // the typed reader (it IS the typed reader), and returns false after
   // recording the error; `status()` is the first error: a violated
   // invariant's own status, else the cursor's. Nothing throws or aborts.
   // The json_reader must outlive every use of the cursor's status: an
   // invariant's message may be text the reader owns.
   template <detail::json_impl::json_family Fmt, JsonCursor C>
   class json_reader
   {
      detail::json_impl::typed_reader<C> r_;

     public:
      json_reader(Fmt, C& cursor) : r_{cursor} {}
      json_reader(const json_reader&)            = delete;
      json_reader& operator=(const json_reader&) = delete;

      C& cursor() noexcept { return r_.c; }

      // One value of T, stored in `out` (unspecified on failure).
      template <typename T>
      [[nodiscard]] bool read(T& out)
      {
         return detail::json_impl::read_value<Fmt, detail::json_impl::int_mode::natural, T>(r_,
                                                                                           &out);
      }
      // One value of T, checked and not kept.
      template <typename T>
      [[nodiscard]] bool validate()
      {
         return detail::json_impl::read_value<Fmt, detail::json_impl::int_mode::natural, T>(
            r_, nullptr);
      }
      // The member `key` of the reflected record T, read into `record` by
      // the member's own rules (its annotations, its integer form). When T
      // has no member of that name `known` is false and nothing is
      // consumed: the caller reads or skips the value itself.
      template <detail::json_impl::Record T>
      [[nodiscard]] bool read_member(std::string_view key, T& record, bool& known)
      {
         constexpr std::size_t N     = ::psio::reflect<T>::member_count;
         const std::size_t     index = ::psio::reflect<T>::find_index(key);
         known                       = index < N;
         if (!known)
            return r_.c.status(Fmt::name).ok();
         return detail::json_impl::member_readers<Fmt, T, C>::table[index](r_, &record);
      }
      // The compact text of the next value, appended to `text`: no
      // whitespace, strings escaped as PSIO's writers escape them, a number
      // as it was written, the members of an object in their order (a
      // repeated key repeated). On failure `text` is restored.
      [[nodiscard]] bool raw(std::string& text)
      {
         const std::size_t         start = text.size();
         detail::json_impl::writer w{text, Fmt::name, false};
         const bool                ok = detail::json_impl::capture_value(r_.c, w);
         w.finish();
         if (ok && w.err.empty())
            return true;
         text.resize(start);
         return ok ? r_.c.fail(w.err) : false;
      }
      // One value of any shape, checked and dropped.
      [[nodiscard]] bool skip() { return r_.c.skip(); }

      [[nodiscard]] codec_status status() const
      {
         if (!r_.invariant.ok())
            return codec_status{r_.invariant.error()};
         return r_.c.status(Fmt::name);
      }
   };
   template <typename Fmt, typename C>
   json_reader(Fmt, C&) -> json_reader<Fmt, C>;

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
      return detail::json_impl::read_document<Fmt, T>(text, &out, parser);
   }

   // A string literal / C string is its characters, without the terminator
   // (a char array converted to a span would include the NUL, which no JSON
   // text ends with).
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status decode_json_checked(
      Fmt fmt, const char* text, T& out, json_parser parser = json_parser::automatic)
   {
      const std::string_view view{text};
      return decode_json_checked(fmt, std::span<const char>{view.data(), view.size()}, out,
                                 parser);
   }

   // The same for text the caller has padded (padded_json): simdjson then
   // parses it in place instead of copying it into a padded buffer.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status decode_json_checked(
      Fmt, padded_json text, T& out, json_parser parser = json_parser::automatic)
   {
      out = T{};
      return detail::json_impl::read_document<Fmt, T>(text, &out, parser);
   }

   // Checks that `text` is one T without building it.
   template <typename T, detail::json_impl::json_family Fmt>
   [[nodiscard]] codec_status validate_json(Fmt, std::span<const char> text,
                                            json_parser parser = json_parser::automatic)
   {
      return detail::json_impl::read_document<Fmt, T>(text, nullptr, parser);
   }
   template <typename T, detail::json_impl::json_family Fmt>
   [[nodiscard]] codec_status validate_json(Fmt fmt, const char* text,
                                            json_parser parser = json_parser::automatic)
   {
      const std::string_view view{text};
      return validate_json<T>(fmt, std::span<const char>{view.data(), view.size()}, parser);
   }
   template <typename T, detail::json_impl::json_family Fmt>
   [[nodiscard]] codec_status validate_json(Fmt, padded_json text,
                                            json_parser parser = json_parser::automatic)
   {
      return detail::json_impl::read_document<Fmt, T>(text, nullptr, parser);
   }

   // Appends the text of `value` to `out`, a std::string or a
   // std::vector<char>. On failure `out` is restored.
   template <detail::json_impl::json_family Fmt, typename T, typename Sink>
      requires(std::same_as<Sink, std::string> || std::same_as<Sink, std::vector<char>>)
   [[nodiscard]] codec_status encode_json_checked(Fmt, const T& value, Sink& out,
                                                  json_sizing how = json_sizing::automatic)
   {
      return detail::json_impl::append_document<Fmt>(value, out, how, false);
   }

   // The length of the text of `value`, in `size`: what `json_size` /
   // `ijson_size` return, without throwing or aborting for a value that has
   // no text.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status json_size_checked(Fmt, const T& value, std::size_t& size)
   {
      return detail::json_impl::measure_document<Fmt>(value, size, false);
   }

   // `to_json` / `to_ijson` with the sizing named (json_sizing above):
   //
   //    auto text = psio::to_ijson(value, psio::json_sizing::exact);   // one allocation: the text
   //    psio::to_ijson(value, body, psio::json_sizing::exact);         // appended; `body` grown once
   template <typename T>
   [[nodiscard]] std::string to_json(const T& value, json_sizing how)
   {
      std::string out;
      (void)detail::json_impl::append_document<json>(value, out, how, true);
      return out;
   }
   template <typename T>
   [[nodiscard]] std::string to_ijson(const T& value, json_sizing how)
   {
      std::string out;
      (void)detail::json_impl::append_document<ijson>(value, out, how, true);
      return out;
   }
   template <typename T, typename Sink>
      requires(std::same_as<Sink, std::string> || std::same_as<Sink, std::vector<char>>)
   void to_json(const T& value, Sink& out, json_sizing how)
   {
      (void)detail::json_impl::append_document<json>(value, out, how, true);
   }
   template <typename T, typename Sink>
      requires(std::same_as<Sink, std::string> || std::same_as<Sink, std::vector<char>>)
   void to_ijson(const T& value, Sink& out, json_sizing how)
   {
      (void)detail::json_impl::append_document<ijson>(value, out, how, true);
   }

   // ── The text of a value as a member of a packed value ──────────────────
   //
   // A member that is "the JSON text of a value" — the body of an HTTP reply
   // inside the arguments of an action inside a call — can be given the
   // text, or the VALUE: `psio::json_of(value)` / `psio::ijson_of(value)`
   // are sources of a byte list (std::vector<char>) whose content is
   // `to_json(value)` / `to_ijson(value)`, as `psio::packed(value)`
   // (pssz.hpp) is of one whose content is `to_pssz(value)`. The PSSZ
   // encoder sizes the text (`json_size`) and writes it straight into the
   // buffer of the enclosing value: the text never has a buffer of its own.
   //
   //    struct Reply   { std::string type; std::vector<char> body; };   // the wire type
   //    template <typename B> struct ReplyOf { std::string type; B body; };
   //    psio::to_pssz(ReplyOf{"application/json", psio::ijson_of(trace)})
   //       == psio::to_pssz(Reply{"application/json", to_ijson_bytes(trace)})
   //
   // An lvalue is referred to and must outlive the encode; an rvalue is
   // moved into the source. A value without a text (a string that is not
   // UTF-8, a non-finite float, a violated bound or invariant) fails the
   // enclosing encode before anything is written. The text is measured
   // once, when the source is first checked or sized, and the length is
   // kept: make the source where it is packed, and do not change the value
   // in between (a text of another length is refused when it is written,
   // through psio::codec_abort; nothing is written out of bounds).
   template <detail::json_impl::json_family Fmt, typename Held>
   class json_ref
   {
      Held                held_;
      mutable std::size_t size_     = 0;
      mutable bool        measured_ = false;

    public:
      using source_type = std::remove_cvref_t<Held>;
      using format_type = Fmt;
      // (what the PSSZ encoder looks for: pssz.hpp, in_place_text)
      using psio_in_place_text = Fmt;

      template <typename V>
         requires std::constructible_from<Held, V&&>
      constexpr explicit json_ref(V&& value) : held_(std::forward<V>(value))
      {
      }
      [[nodiscard]] constexpr const source_type& source() const noexcept { return held_; }

      // Whether the value has a text; measures it.
      [[nodiscard]] codec_status check() const
      {
         if (measured_)
            return codec_ok();
         auto st   = detail::json_impl::measure_document<Fmt>(held_, size_, false);
         measured_ = st.ok();
         return st;
      }
      // The number of bytes it writes: the length of the text.
      [[nodiscard]] std::size_t size() const
      {
         if (!measured_)
         {
            (void)detail::json_impl::measure_document<Fmt>(held_, size_, true);
            measured_ = true;
         }
         return size_;
      }
      // Writes the text to [dst, dst + size()).
      void write(char* dst) const
      {
         (void)detail::json_impl::write_document<Fmt>(held_, dst, size(), true);
      }
   };
   template <detail::json_impl::json_family Fmt, typename Held>
   struct wire_type<json_ref<Fmt, Held>>
   {
      using type = std::vector<char>;
   };

   template <typename V>
   [[nodiscard]] constexpr auto json_of(V&& value)
   {
      return json_ref<json, detail::json_impl::held_t<V>>{std::forward<V>(value)};
   }
   template <typename V>
   [[nodiscard]] constexpr auto ijson_of(V&& value)
   {
      return json_ref<ijson, detail::json_impl::held_t<V>>{std::forward<V>(value)};
   }

   // ── Pretty printing ────────────────────────────────────────────────────
   //
   // `format_json` lays any JSON text out for a person:
   //
   //    psio::format_json(psio::to_ijson(value))
   //
   //    {
   //        "name": "n",
   //        "tags": [
   //            "a",
   //            "b"
   //        ],
   //        "empty": {}
   //    }
   //
   // One member or element per line, `indent` spaces per level (4), a space
   // after the colon, empty containers closed on the spot. The values are
   // untouched: numbers as written, strings escaped as PSIO escapes them,
   // members in their order. The text must be one JSON value (it is checked
   // as every reader checks it); what comes out is the same value.
   struct json_layout
   {
      std::size_t indent = 4;  // spaces per nesting level
   };
   // Appends the laid-out text of `json` to `out` (a std::string or a
   // std::vector<char>). On failure `out` is restored.
   template <typename Sink>
      requires(std::same_as<Sink, std::string> || std::same_as<Sink, std::vector<char>>)
   [[nodiscard]] codec_status format_json(std::string_view json, Sink& out, json_layout layout = {})
   {
      const std::size_t         start = out.size();
      json_text_cursor          c{std::span<const char>{json.data(), json.size()}, false};
      detail::json_impl::writer w{out, "json", false};
      const bool ok = detail::json_impl::pretty_value(c, w, layout.indent, 0) && c.finish();
      w.finish();
      if (ok && w.err.empty())
         return codec_ok();
      out.resize(start);
      if (!ok)
         return c.status("json");
      return codec_fail(w.err, 0, "json");
   }
   // The same, fail-fast: a codec_exception (abort without exceptions) for
   // text that is not JSON.
   [[nodiscard]] inline std::string format_json(std::string_view json, json_layout layout = {})
   {
      std::string out;
      if (auto st = format_json(json, out, layout); !st.ok())
         ::psio::codec_abort(st.error());
      return out;
   }

   // ── psio::json_value in the binary formats ─────────────────────────────
   //
   // A json_value is, in PSSZ and every other binary format, its compact
   // psio::json text: the bytes of a UTF-8 string (json_value.hpp). The
   // checking readers accept any well-formed JSON text; `validate_strict`
   // only the text a writer produces (compact, one value per key), so that
   // a value has one encoding.
   namespace detail::json_impl
   {
      struct json_value_binary
      {
         static std::string text_of(const ::psio::json_value& v)
         {
            std::string out;
            writer      w{out, "json"};
            encode_document<::psio::json>(v, w);
            return out;
         }
         static std::size_t packsize(const ::psio::json_value& v) { return text_of(v).size(); }
         template <typename Sink>
         static void encode(const ::psio::json_value& v, Sink& out)
         {
            const std::string text = text_of(v);
            out.insert(out.end(), text.begin(), text.end());
         }
         static ::psio::json_value decode(std::span<const char> bytes)
         {
            ::psio::json_value out;
            if (auto st = read_document<::psio::json, ::psio::json_value>(bytes, &out); !st.ok())
               ::psio::codec_abort(st.error());
            return out;
         }
         static codec_status validate(std::span<const char> bytes) noexcept
         {
            return read_document<::psio::json, ::psio::json_value>(bytes, nullptr);
         }
         static codec_status validate_strict(std::span<const char> bytes) noexcept
         {
            ::psio::json_value value;
            if (auto st = read_document<::psio::json, ::psio::json_value>(bytes, &value);
                !st.ok())
               return st;
            std::string text;
            writer      w{text, "json", false};
            encode_document<::psio::json>(value, w);
            if (!w.err.empty() || text.size() != bytes.size() ||
                !std::equal(text.begin(), text.end(), bytes.begin()))
               return codec_fail("json: a json_value's bytes are not its compact text", 0,
                                 "json");
            return codec_ok();
         }
      };
   }  // namespace detail::json_impl

}  // namespace psio

PSIO_ADAPTER(::psio::json_value,
             ::psio::binary_category,
             ::psio::detail::json_impl::json_value_binary)

#include <psio/detail/json_assoc.hpp>
