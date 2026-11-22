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
//   integers, enums           decimal (an enum is its underlying integer)
//   float, double             shortest round-trip decimal (std::to_chars)
//   std::string, utf8_string  "..." with JSON escaping
//   std::vector, std::array,
//   psio::bounded             [ ... ]
//   std::optional             null or the value
//   std::tuple                [ e0, e1, ... ] in order; tuple<> is []
//   reflected record          {"field": value, ...} in declaration order
//   text adapter              "..." holding the adapter's text
//                             (PSIO_ADAPTER(T, psio::text_category, ...), or a
//                             member-level psio::as<Tag>)
//
// Integer form per member: psio::json_number / psio::json_string and
// psio::ijson_number / psio::ijson_string (annotate.hpp) force a number or a
// string for one format.
//
// Writing fails for a value that has no JSON text: a string that is not
// valid UTF-8 (a std::string is raw bytes and may hold anything), and under
// ijson a non-finite float or a string holding a Unicode noncharacter. The
// fail-fast `encode` raises codec_exception (aborts when exceptions are
// disabled); `encode_json_checked` returns a codec_status instead.
//
// Reading. One typed reader defines the accepted set; `validate`,
// `validate_strict`, `decode_json_checked` and `decode` all run it:
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
#include <psio/detail/validate_depth.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/get_type_name.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for, byte_array, ...

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

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
      struct is_bounded_bytes : std::false_type
      {
      };
      template <typename E, std::size_t N>
         requires(std::is_same_v<E, char> || std::is_same_v<E, unsigned char> ||
                  std::is_same_v<E, std::byte>)
      struct is_bounded_bytes<::psio::bounded<E, N>> : std::true_type
      {
      };

      template <typename T>
      concept Record = ::psio::Reflected<T> && !std::is_enum_v<T>;

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

      template <typename V>
      constexpr bool alt_names_unique() noexcept
      {
         constexpr std::size_t N = std::variant_size_v<V>;
         return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            const std::array<std::string_view, N> names{alt_name<V, Is>()...};
            for (std::size_t a = 0; a < N; ++a)
               for (std::size_t b = a + 1; b < N; ++b)
                  if (names[a] == names[b])
                     return false;
            return true;
         }(std::make_index_sequence<N>{});
      }

      // ── UTF-8 ─────────────────────────────────────────────────────────────

      // Length (2..4) of the well-formed multi-byte sequence at p, or 0.
      // Rejects overlong forms, surrogates and values above U+10FFFF.
      inline unsigned utf8_multibyte(const unsigned char* p,
                                     const unsigned char* end,
                                     std::uint32_t&       cp) noexcept
      {
         const unsigned char b = *p;
         unsigned            n;
         std::uint32_t       min;
         if ((b & 0xE0) == 0xC0)
         {
            n   = 2;
            cp  = b & 0x1F;
            min = 0x80;
         }
         else if ((b & 0xF0) == 0xE0)
         {
            n   = 3;
            cp  = b & 0x0F;
            min = 0x800;
         }
         else if ((b & 0xF8) == 0xF0)
         {
            n   = 4;
            cp  = b & 0x07;
            min = 0x10000;
         }
         else
            return 0;
         if (static_cast<std::size_t>(end - p) < n)
            return 0;
         for (unsigned k = 1; k < n; ++k)
         {
            if ((p[k] & 0xC0) != 0x80)
               return 0;
            cp = (cp << 6) | (p[k] & 0x3F);
         }
         if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
         return n;
      }

      constexpr bool is_noncharacter(std::uint32_t cp) noexcept
      {
         return (cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE;
      }

      inline void append_utf8(std::string& out, std::uint32_t cp)
      {
         if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
         else if (cp < 0x800)
         {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
         }
         else if (cp < 0x10000)
         {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
         }
         else
         {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
         }
      }

      // ── Encoding ──────────────────────────────────────────────────────────

      struct writer
      {
         std::string&     s;
         std::string_view format;
         bool             fail_fast = true;
         std::string_view err{};

         void fail(std::string_view what)
         {
            if (fail_fast)
               ::psio::codec_abort(::psio::codec_error{
                  what, static_cast<std::uint32_t>(s.size()), format});
            if (err.empty())
               err = what;
         }
      };

      // Quotes and escapes sv. The text must be UTF-8; `nonchar` also
      // refuses Unicode noncharacters (I-JSON).
      inline void write_string(writer& w, std::string_view sv, bool nonchar)
      {
         std::string& s = w.s;
         s.push_back('"');
         const auto* const base =
            reinterpret_cast<const unsigned char*>(sv.data());
         const auto* const end = base + sv.size();
         const auto*       p   = base;
         const auto*       run = base;
         auto flush = [&] {
            s.append(reinterpret_cast<const char*>(run),
                     static_cast<std::size_t>(p - run));
         };
         while (p < end)
         {
            const unsigned char c = *p;
            if (c >= 0x80)
            {
               std::uint32_t  cp = 0;
               const unsigned n  = utf8_multibyte(p, end, cp);
               if (n == 0)
               {
                  w.fail("json: string is not valid UTF-8");
                  break;
               }
               if (nonchar && is_noncharacter(cp))
               {
                  w.fail("ijson: string holds a Unicode noncharacter");
                  break;
               }
               p += n;
               continue;
            }
            if (c >= 0x20 && c != '"' && c != '\\')
            {
               ++p;
               continue;
            }
            flush();
            switch (c)
            {
               case '"':  s += R"(\")"; break;
               case '\\': s += R"(\\)"; break;
               case '\n': s += R"(\n)"; break;
               case '\r': s += R"(\r)"; break;
               case '\t': s += R"(\t)"; break;
               default:
               {
                  static constexpr char hex[] = "0123456789abcdef";
                  const char buf[6] = {'\\', 'u', '0', '0', hex[c >> 4],
                                       hex[c & 0xF]};
                  s.append(buf, 6);
               }
            }
            ++p;
            run = p;
         }
         flush();
         s.push_back('"');
      }

      inline void write_hex(std::string& s, const unsigned char* data,
                            std::size_t n)
      {
         static constexpr char hex[] = "0123456789ABCDEF";
         const std::size_t     at    = s.size();
         s.resize(at + 2 * n + 2);
         char* out = s.data() + at;
         *out++    = '"';
         for (std::size_t i = 0; i < n; ++i)
         {
            *out++ = hex[data[i] >> 4];
            *out++ = hex[data[i] & 0xF];
         }
         *out = '"';
      }

      template <typename Fmt, int_mode IM, typename T>
      void encode_value(const T& v, writer& w);

      template <typename Fmt, int_mode IM, typename I>
      void encode_integer(I v, writer& w)
      {
         char buf[48];
         auto r = std::to_chars(buf + 1, buf + sizeof(buf) - 1, v);
         bool quoted = int_as_string_v<Fmt, IM, I>;
         if constexpr (IM == int_mode::natural && int_as_string_v<Fmt, IM, I>)
         {
            // Natural ijson: a wide integer JavaScript holds exactly
            // (|v| <= 2^53 - 1) is a bare number; anything larger is a string.
            constexpr I safe = static_cast<I>((std::uint64_t{1} << 53) - 1);
            quoted           = v > safe;
            if constexpr (std::is_signed_v<I>)
               quoted = quoted || v < -safe;
         }
         if (quoted)
         {
            buf[0]   = '"';
            *r.ptr++ = '"';
            w.s.append(buf, r.ptr);
         }
         else
            w.s.append(buf + 1, r.ptr);
      }

      template <typename Fmt, int_mode IM, typename T>
      void encode_value(const T& v, writer& w)
      {
         std::string& s = w.s;
         // Adapter check — if the type registered a text adapter (and it
         // doesn't delegate back to this format), delegate and write the
         // result as a JSON string. The adapter produces the payload
         // unquoted; JSON owns the surrounding quotes and escaping.
         //
         // Must be the FIRST branch so the trailing static_assert is
         // discarded for adapter-encoded types.
         if constexpr (::psio::format_should_dispatch_adapter_v<Fmt, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                         ::psio::text_category>;
            std::string payload;
            Proj::encode(v, payload);
            write_string(w, std::string_view{payload}, Fmt::i_json);
         }
         else if constexpr (std::is_same_v<T, bool>)
            s += v ? "true" : "false";
         else if constexpr (std::is_enum_v<T> && !std::is_same_v<T, std::byte>)
            encode_integer<Fmt, IM>(
               static_cast<std::underlying_type_t<T>>(v), w);
         else if constexpr (std::is_same_v<T, std::byte>)
            encode_integer<Fmt, IM>(static_cast<unsigned char>(v), w);
         else if constexpr (std::is_integral_v<T>)
            encode_integer<Fmt, IM>(v, w);
         else if constexpr (std::is_floating_point_v<T>)
         {
            if constexpr (Fmt::i_json)
               if (!std::isfinite(v))
               {
                  w.fail("ijson: non-finite float has no JSON text");
                  s += "null";
                  return;
               }
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof(buf), v);
            s.append(buf, r.ptr);
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
            write_hex(s, reinterpret_cast<const unsigned char*>(v.data()),
                      v.size());
         else if constexpr (is_byte_array<T>::value)
            encode_value<Fmt, IM>(v.storage(), w);
         else if constexpr (is_std_array<T>::value ||
                            is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            s.push_back('[');
            bool first = true;
            for (const E& x : v)
            {
               if (!first)
                  s.push_back(',');
               first = false;
               encode_value<Fmt, IM>(x, w);
            }
            s.push_back(']');
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
               encode_value<Fmt, IM>(*v, w);
            else
               s += "null";
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
                             ? (s.push_back('{'),
                                write_string(w, alt_name<T, Is>(), false),
                                s.push_back(':'),
                                encode_value<Fmt, int_mode::natural>(
                                   *std::get_if<Is>(&v), w),
                                s.push_back('}'), true)
                             : false) ||
                         ...);
               }(std::make_index_sequence<std::variant_size_v<T>>{});
            }
            else
            {
               // Positional tagged form: [index, value].
               s.push_back('[');
               char buf[24];
               auto r = std::to_chars(buf, buf + sizeof(buf), v.index());
               s.append(buf, r.ptr);
               s.push_back(',');
               std::visit(
                  [&](const auto& alt) {
                     encode_value<Fmt, int_mode::natural>(alt, w);
                  },
                  v);
               s.push_back(']');
            }
         }
         else if constexpr (is_std_tuple<T>::value)
         {
            s.push_back('[');
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (((Is ? s.push_back(',') : void()),
                 encode_value<Fmt, int_mode::natural>(std::get<Is>(v), w)),
                ...);
            }(std::make_index_sequence<std::tuple_size_v<T>>{});
            s.push_back(']');
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            s.push_back('{');
            bool first = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      if (!first)
                         s.push_back(',');
                      first = false;
                      write_string(w, R::template member_name<Is>, false);
                      s.push_back(':');

                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);

                      // Member-level presentation override: if the
                      // field's effective annotations include
                      // as_spec<Tag>, use adapter<F, Tag> for this field
                      // (member beats type, §5.3.7).
                      using eff_for = ::psio::effective_annotations_for<
                         T, F, R::template member_pointer<Is>>;
                      using eff = typename eff_for::value_t;
                      if constexpr (::psio::has_as_override_v<eff>)
                      {
                         using Tag  = ::psio::adapter_tag_of_t<eff>;
                         using Proj =
                            ::psio::adapter<std::remove_cvref_t<F>, Tag>;
                         std::string payload;
                         Proj::encode(fref, payload);
                         write_string(w, std::string_view{payload},
                                      Fmt::i_json);
                      }
                      else
                      {
                         // A member outside its declared length_bound has
                         // no text: the reader would reject it.
                         if constexpr (::psio::has_spec_v<::psio::length_bound, eff>)
                            if (!::psio::detail::length_bound_ok(
                                   fref, std::get<::psio::length_bound>(
                                            eff_for::value)))
                               w.fail("json: member length_bound violated");
                         constexpr int_mode mode =
                            field_mode<Fmt, eff>(eff_for::value);
                         encode_value<Fmt, mode>(fref, w);
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            s.push_back('}');
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::json: unsupported type in encode_value");
         }
      }

      // ── Reading ───────────────────────────────────────────────────────────
      //
      // One typed reader. With `out == nullptr` it validates without
      // building a value; otherwise it also stores the value. Every failure
      // is reported through the reader (no exceptions, no aborts), so the
      // same code serves validate, the checked decode and guests.

      struct reader
      {
         const char*      begin;
         const char*      p;
         const char*      end;
         bool             lenient_missing = false;
         std::string_view err{};
         const char*      err_at = nullptr;

         bool fail(std::string_view what) noexcept
         {
            if (err.empty())
            {
               err    = what;
               err_at = p;
            }
            return false;
         }

         void ws() noexcept
         {
            while (p < end &&
                   (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
               ++p;
         }

         // Skips whitespace, then consumes c if it is next.
         bool take(char c) noexcept
         {
            ws();
            if (p < end && *p == c)
            {
               ++p;
               return true;
            }
            return false;
         }

         bool keyword(std::string_view kw) noexcept
         {
            if (static_cast<std::size_t>(end - p) >= kw.size() &&
                std::memcmp(p, kw.data(), kw.size()) == 0)
            {
               p += kw.size();
               return true;
            }
            return false;
         }
      };

      constexpr int hex_digit(char c) noexcept
      {
         if (c >= '0' && c <= '9') return c - '0';
         if (c >= 'a' && c <= 'f') return c - 'a' + 10;
         if (c >= 'A' && c <= 'F') return c - 'A' + 10;
         return -1;
      }

      inline bool read_hex4(reader& r, std::uint32_t& v) noexcept
      {
         if (r.end - r.p < 4)
            return r.fail("json: truncated \\uXXXX escape");
         v = 0;
         for (int k = 0; k < 4; ++k)
         {
            const int d = hex_digit(r.p[k]);
            if (d < 0)
               return r.fail("json: bad hex digit in \\uXXXX");
            v = (v << 4) | static_cast<std::uint32_t>(d);
         }
         r.p += 4;
         return true;
      }

      // Reads a JSON string; r.p must be at its opening quote. Appends the
      // unescaped UTF-8 to *out when given. `escaped` reports whether the
      // text between the quotes differs from the value.
      inline bool read_string(reader& r, std::string* out, bool nonchar,
                              bool* escaped = nullptr)
      {
         if (r.p >= r.end || *r.p != '"')
            return r.fail("json: expected a string");
         ++r.p;
         const char* run = r.p;
         auto flush = [&] {
            if (out)
               out->append(run, static_cast<std::size_t>(r.p - run));
         };
         if (escaped)
            *escaped = false;
         while (true)
         {
            if (r.p >= r.end)
               return r.fail("json: unterminated string");
            const unsigned char c = static_cast<unsigned char>(*r.p);
            if (c == '"')
            {
               flush();
               ++r.p;
               return true;
            }
            if (c == '\\')
            {
               flush();
               if (escaped)
                  *escaped = true;
               if (r.end - r.p < 2)
                  return r.fail("json: trailing backslash in string");
               const char e = r.p[1];
               r.p += 2;
               std::uint32_t cp;
               switch (e)
               {
                  case '"':  cp = '"'; break;
                  case '\\': cp = '\\'; break;
                  case '/':  cp = '/'; break;
                  case 'b':  cp = '\b'; break;
                  case 'f':  cp = '\f'; break;
                  case 'n':  cp = '\n'; break;
                  case 'r':  cp = '\r'; break;
                  case 't':  cp = '\t'; break;
                  case 'u':
                  {
                     if (!read_hex4(r, cp))
                        return false;
                     if (cp >= 0xDC00 && cp <= 0xDFFF)
                        return r.fail("json: unpaired surrogate escape");
                     if (cp >= 0xD800 && cp <= 0xDBFF)
                     {
                        std::uint32_t lo = 0;
                        if (r.end - r.p < 2 || r.p[0] != '\\' ||
                            r.p[1] != 'u')
                           return r.fail("json: unpaired surrogate escape");
                        r.p += 2;
                        if (!read_hex4(r, lo))
                           return false;
                        if (lo < 0xDC00 || lo > 0xDFFF)
                           return r.fail("json: unpaired surrogate escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                     }
                     if (nonchar && is_noncharacter(cp))
                        return r.fail("ijson: Unicode noncharacter in string");
                     break;
                  }
                  default:
                     r.p -= 1;
                     return r.fail("json: invalid escape character in string");
               }
               if (out)
                  append_utf8(*out, cp);
               run = r.p;
               continue;
            }
            if (c < 0x20)
               return r.fail("json: unescaped control character in string");
            if (c < 0x80)
            {
               ++r.p;
               continue;
            }
            std::uint32_t  cp = 0;
            const unsigned n  = utf8_multibyte(
               reinterpret_cast<const unsigned char*>(r.p),
               reinterpret_cast<const unsigned char*>(r.end), cp);
            if (n == 0)
               return r.fail("json: string is not valid UTF-8");
            if (nonchar && is_noncharacter(cp))
               return r.fail("ijson: Unicode noncharacter in string");
            r.p += n;
         }
      }

      // Reads an object key. `key` views the input when the key has no
      // escapes, else `scratch`.
      inline bool read_key(reader& r, std::string& scratch,
                           std::string_view& key, bool nonchar)
      {
         r.ws();
         const char* start   = r.p;
         bool        escaped = false;
         if (!read_string(r, nullptr, nonchar, &escaped))
            return false;
         if (!escaped)
         {
            key = std::string_view{start + 1,
                                   static_cast<std::size_t>(r.p - start - 2)};
            return true;
         }
         r.p = start;
         scratch.clear();
         if (!read_string(r, &scratch, nonchar))
            return false;
         key = scratch;
         return true;
      }

      // RFC 8259 number grammar; r.p at its first character.
      inline bool skip_number(reader& r) noexcept
      {
         const char* p = r.p;
         if (p < r.end && *p == '-')
            ++p;
         if (p >= r.end)
            return r.fail("json: truncated number");
         if (*p == '0')
            ++p;
         else if (*p >= '1' && *p <= '9')
         {
            ++p;
            while (p < r.end && *p >= '0' && *p <= '9')
               ++p;
         }
         else
            return r.fail("json: number missing integer part");
         if (p < r.end && *p == '.')
         {
            ++p;
            const char* frac = p;
            while (p < r.end && *p >= '0' && *p <= '9')
               ++p;
            if (p == frac)
               return r.fail("json: number missing fractional digits");
         }
         if (p < r.end && (*p == 'e' || *p == 'E'))
         {
            ++p;
            if (p < r.end && (*p == '+' || *p == '-'))
               ++p;
            const char* exp = p;
            while (p < r.end && *p >= '0' && *p <= '9')
               ++p;
            if (p == exp)
               return r.fail("json: number missing exponent digits");
         }
         r.p = p;
         return true;
      }

      // Skips one well-formed JSON value of any shape (the value of an
      // unknown key). `unique_keys` applies the I-JSON string rule
      // (no noncharacters) inside it; repeated keys are not an error.
      inline bool skip_value(reader& r, std::size_t depth, bool unique_keys)
      {
         r.ws();
         if (r.p >= r.end)
            return r.fail("json: unexpected end of input");
         switch (*r.p)
         {
            case '{':
            {
               if (depth >= kMaxValidationDepth)
                  return r.fail("json: max depth exceeded");
               ++r.p;
               if (r.take('}'))
                  return true;
               std::string scratch;
               while (true)
               {
                  std::string_view key;
                  if (!read_key(r, scratch, key, unique_keys))
                     return false;
                  if (!r.take(':'))
                     return r.fail("json: expected ':' after object key");
                  if (!skip_value(r, depth + 1, unique_keys))
                     return false;
                  if (r.take('}'))
                     break;
                  if (!r.take(','))
                     return r.fail("json: expected ',' or '}' in object");
               }
               return true;
            }
            case '[':
            {
               if (depth >= kMaxValidationDepth)
                  return r.fail("json: max depth exceeded");
               ++r.p;
               if (r.take(']'))
                  return true;
               while (true)
               {
                  if (!skip_value(r, depth + 1, unique_keys))
                     return false;
                  if (r.take(']'))
                     return true;
                  if (!r.take(','))
                     return r.fail("json: expected ',' or ']' in array");
               }
            }
            case '"':
               return read_string(r, nullptr, unique_keys);
            case 't':
               return r.keyword("true") || r.fail("json: malformed keyword");
            case 'f':
               return r.keyword("false") || r.fail("json: malformed keyword");
            case 'n':
               return r.keyword("null") || r.fail("json: malformed keyword");
            default:
               if (*r.p == '-' || (*r.p >= '0' && *r.p <= '9'))
                  return skip_number(r);
               return r.fail("json: unexpected character at value start");
         }
      }

      // Canonical decimal integer over exactly [b, e): 0 | -?[1-9][0-9]*.
      template <typename I>
      bool parse_integer(const char* b, const char* e, I& v) noexcept
      {
         const char* q = b;
         if (q < e && *q == '-')
         {
            if (!std::is_signed_v<I>)
               return false;
            ++q;
         }
         if (q >= e)
            return false;
         if (*q == '0')
         {
            if (q != b || q + 1 != e)
               return false;
         }
         for (const char* d = q; d < e; ++d)
            if (*d < '0' || *d > '9')
               return false;
         const auto res = std::from_chars(b, e, v);
         return res.ec == std::errc{} && res.ptr == e;
      }

      template <typename Fmt, int_mode IM, typename I>
      bool read_integer(reader& r, I* out) noexcept
      {
         constexpr bool as_string = int_as_string_v<Fmt, IM, I>;
         r.ws();
         if (r.p >= r.end)
            return r.fail("json: unexpected end of input");
         I v{};
         if (*r.p == '"')
         {
            if constexpr (!as_string)
               return r.fail("json: expected a number, found a string");
            const char* b = r.p + 1;
            const char* q = b;
            while (q < r.end && *q != '"')
               ++q;
            if (q >= r.end)
               return r.fail("json: unterminated string");
            if (!parse_integer(b, q, v))
               return r.fail("json: integer string is not a canonical "
                             "in-range decimal");
            r.p = q + 1;
         }
         else
         {
            if constexpr (as_string && IM == int_mode::string)
               return r.fail("json: expected an integer string");
            const char* b = r.p;
            const char* q = b;
            if (q < r.end && *q == '-')
               ++q;
            while (q < r.end && *q >= '0' && *q <= '9')
               ++q;
            if (q == b)
               return r.fail("json: expected an integer");
            if (q < r.end && (*q == '.' || *q == 'e' || *q == 'E'))
               return r.fail("json: integer has a fraction or exponent");
            if (!parse_integer(b, q, v))
               return r.fail("json: integer is not a canonical in-range "
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
                  return r.fail("ijson: bare integer outside the safe range "
                                "must be a string");
            }
            r.p = q;
         }
         if (out)
            *out = v;
         return true;
      }

      template <typename F>
      bool read_float(reader& r, F* out)
      {
         r.ws();
         const char* b = r.p;
         if (b >= r.end || !(*b == '-' || (*b >= '0' && *b <= '9')))
            return r.fail("json: expected a number");
         if (!skip_number(r))
            return false;
         F          v{};
         const auto res = std::from_chars(b, r.p, v);
         if (res.ec == std::errc::result_out_of_range)
         {
            // Overflow is an error; underflow rounds toward zero.
            const std::string token(b, r.p);
            const double      wide = std::strtod(token.c_str(), nullptr);
            v = static_cast<F>(wide);
            if (!std::isfinite(wide) || !std::isfinite(v))
            {
               r.p = b;
               return r.fail("json: number out of range");
            }
         }
         else if (res.ec != std::errc{} || res.ptr != r.p)
         {
            r.p = b;
            return r.fail("json: malformed number");
         }
         if (!std::isfinite(v))
         {
            r.p = b;
            return r.fail("json: number out of range");
         }
         if (out)
            *out = v;
         return true;
      }

      // Hex string into a byte sequence.
      template <typename T>
      bool read_hex(reader& r, T* out)
      {
         r.ws();
         if (r.p >= r.end || *r.p != '"')
            return r.fail("ijson: expected a hex string");
         const char* b = r.p + 1;
         const char* q = b;
         while (q < r.end && hex_digit(*q) >= 0)
            ++q;
         if (q >= r.end)
            return r.fail("json: unterminated string");
         if (*q != '"')
         {
            r.p = q;
            return r.fail("ijson: non-hex character in hex string");
         }
         const std::size_t digits = static_cast<std::size_t>(q - b);
         if (digits & 1)
            return r.fail("ijson: odd-length hex string");
         const std::size_t n = digits / 2;
         if constexpr (is_std_array<T>::value)
         {
            if (n != std::tuple_size_v<T>)
               return r.fail("ijson: hex string has the wrong length");
         }
         if (out)
         {
            if constexpr (!is_std_array<T>::value)
               out->resize(n);
            auto* dst = reinterpret_cast<unsigned char*>(out->data());
            for (std::size_t i = 0; i < n; ++i)
               dst[i] = static_cast<unsigned char>(
                  (hex_digit(b[2 * i]) << 4) | hex_digit(b[2 * i + 1]));
         }
         r.p = q + 1;
         return true;
      }

      template <typename Fmt, int_mode IM, typename T>
      bool read_value(reader& r, T* out, std::size_t depth);

      // "[" e0 "," e1 ... "]" with exactly N elements read by `element(i)`.
      template <typename Element>
      bool read_fixed_array(reader& r, std::size_t n, std::size_t depth,
                            Element&& element)
      {
         if (!r.take('['))
            return r.fail("json: expected '['");
         if (depth >= kMaxValidationDepth)
            return r.fail("json: max depth exceeded");
         for (std::size_t i = 0; i < n; ++i)
         {
            if (i && !r.take(','))
               return r.fail("json: array has too few elements");
            if (!i)
            {
               r.ws();
               if (r.p < r.end && *r.p == ']')
                  return r.fail("json: array has too few elements");
            }
            if (!element(i))
               return false;
         }
         if (!r.take(']'))
            return r.fail("json: array has too many elements or is unterminated");
         return true;
      }

      // Reads the value of member I of record T (the reader is at it).
      // `depth` is the record's own depth.
      template <typename Fmt, typename T, std::size_t I>
      bool read_member(reader& r, T* out, std::size_t depth)
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
            using Tag  = ::psio::adapter_tag_of_t<eff>;
            using Proj = ::psio::adapter<std::remove_cvref_t<F>, Tag>;
            std::string payload;
            r.ws();
            if (!read_string(r, &payload, Fmt::i_json))
               return false;
            const std::span<const char> text{payload};
            if (auto st = Proj::validate(text); !st.ok())
               return r.fail(st.error().what);
            if (fptr)
               *fptr = Proj::decode(text);
            return true;
         }
         else
         {
            constexpr int_mode mode = field_mode<Fmt, eff>(eff_for::value);
            if constexpr (::psio::has_spec_v<::psio::length_bound, eff>)
            {
               // The member's length_bound is part of its type. It is
               // checked on the value read, so the member is materialized
               // even when only validating.
               F  scratch{};
               F* dst = fptr ? fptr : &scratch;
               r.ws();
               const char* value_at = r.p;
               if (!read_value<Fmt, mode, F>(r, dst, depth + 1))
                  return false;
               if (!::psio::detail::length_bound_ok(
                      *dst, std::get<::psio::length_bound>(eff_for::value)))
               {
                  r.p = value_at;
                  return r.fail("json: member length_bound violated");
               }
               return true;
            }
            else
               return read_value<Fmt, mode, F>(r, fptr, depth + 1);
         }
      }

      // Member readers by index, for keys that arrive out of order.
      template <typename Fmt, typename T>
      struct member_readers
      {
         using fn = bool (*)(reader&, T*, std::size_t);
         static constexpr auto table =
            []<std::size_t... Is>(std::index_sequence<Is...>) {
               return std::array<fn, sizeof...(Is)>{&read_member<Fmt, T, Is>...};
            }(std::make_index_sequence<::psio::reflect<T>::member_count>{});
      };

      template <typename Fmt, int_mode IM, typename T>
      bool read_value(reader& r, T* out, std::size_t depth)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<Fmt, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                         ::psio::text_category>;
            std::string payload;
            r.ws();
            if (!read_string(r, &payload, Fmt::i_json))
               return false;
            const std::span<const char> text{payload};
            if (auto st = Proj::validate(text); !st.ok())
               return r.fail(st.error().what);
            if (out)
               *out = Proj::decode(text);
            return true;
         }
         else if constexpr (std::is_same_v<T, bool>)
         {
            r.ws();
            if (r.keyword("true"))
            {
               if (out) *out = true;
               return true;
            }
            if (r.keyword("false"))
            {
               if (out) *out = false;
               return true;
            }
            return r.fail("json: expected true or false");
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
            std::underlying_type_t<T> v{};
            if (!read_integer<Fmt, IM, std::underlying_type_t<T>>(r, &v))
               return false;
            if (out) *out = static_cast<T>(v);
            return true;
         }
         else if constexpr (std::is_integral_v<T>)
            return read_integer<Fmt, IM, T>(r, out);
         else if constexpr (std::is_floating_point_v<T>)
            return read_float<T>(r, out);
         else if constexpr (std::is_same_v<T, std::string>)
         {
            r.ws();
            if (out) out->clear();
            return read_string(r, out, Fmt::i_json);
         }
         else if constexpr (is_utf8_string<T>::value)
         {
            std::string text;
            r.ws();
            const char* at = r.p;
            if (!read_string(r, &text, Fmt::i_json))
               return false;
            if (text.size() > T::max_size_value)
            {
               r.p = at;
               return r.fail("json: string exceeds its bound");
            }
            if (out) *out = T{std::move(text)};
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
            return read_value<Fmt, IM>(r, out ? &out->storage() : nullptr,
                                       depth);
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            return read_fixed_array(
               r, std::tuple_size_v<T>, depth, [&](std::size_t i) {
                  return read_value<Fmt, IM, E>(r, out ? &(*out)[i] : nullptr,
                                                depth + 1);
               });
         }
         else if constexpr (Fmt::i_json && is_bounded_bytes<T>::value)
         {
            // Written as one hex string (its storage is a byte sequence),
            // so it is read as one.
            std::vector<typename T::value_type> bytes;
            r.ws();
            const char* at = r.p;
            if (!read_hex(r, &bytes))
               return false;
            if (bytes.size() > T::max_size_value)
            {
               r.p = at;
               return r.fail("json: array exceeds its bound");
            }
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
            if (!r.take('['))
               return r.fail("json: expected '['");
            if (depth >= kMaxValidationDepth)
               return r.fail("json: max depth exceeded");
            if (r.take(']'))
               return true;
            std::size_t count = 0;
            while (true)
            {
               if constexpr (is_bounded<T>::value)
                  if (count >= T::max_size_value)
                     return r.fail("json: array exceeds its bound");
               ++count;
               if (dst)
               {
                  E element{};
                  if (!read_value<Fmt, IM, E>(r, &element, depth + 1))
                     return false;
                  dst->push_back(std::move(element));
               }
               else if (!read_value<Fmt, IM, E>(r, nullptr, depth + 1))
                  return false;
               if (r.take(']'))
                  return true;
               if (!r.take(','))
                  return r.fail("json: expected ',' or ']' in array");
            }
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V = typename T::value_type;
            r.ws();
            if (r.p < r.end && *r.p == 'n')
            {
               if (!r.keyword("null"))
                  return r.fail("json: malformed keyword");
               if (out) out->reset();
               return true;
            }
            if (!out)
               return read_value<Fmt, IM, V>(r, nullptr, depth);
            V value{};
            if (!read_value<Fmt, IM, V>(r, &value, depth))
               return false;
            *out = std::move(value);
            return true;
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
                                         r, nullptr, depth + 1);
                                   A value{};
                                   if (!read_value<Fmt, int_mode::natural, A>(
                                          r, &value, depth + 1))
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
            if constexpr (Fmt::i_json)
            {
               static_assert(alt_names_unique<T>(),
                             "psio::ijson: variant alternatives must have "
                             "distinct type names");
               if (!r.take('{'))
                  return r.fail("ijson: expected a variant object");
               if (depth >= kMaxValidationDepth)
                  return r.fail("json: max depth exceeded");
               std::string      scratch;
               std::string_view key;
               const char*      at = (r.ws(), r.p);
               if (at < r.end && *at == '}')
                  return r.fail("ijson: variant object must have one key");
               if (!read_key(r, scratch, key, true))
                  return false;
               std::size_t index = N;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (void)((key == alt_name<T, Is>() ? (index = Is, true)
                                                   : false) ||
                         ...);
               }(std::make_index_sequence<N>{});
               if (index == N)
               {
                  r.p = at;
                  return r.fail("ijson: unknown variant alternative");
               }
               if (!r.take(':'))
                  return r.fail("json: expected ':' after object key");
               if (!alternative(index))
                  return false;
               if (!r.take('}'))
                  return r.fail("ijson: variant object must have one key");
               return true;
            }
            else
            {
               if (!r.take('['))
                  return r.fail("json: expected a [index, value] variant");
               if (depth >= kMaxValidationDepth)
                  return r.fail("json: max depth exceeded");
               std::uint32_t index = 0;
               if (!read_integer<Fmt, int_mode::natural, std::uint32_t>(
                      r, &index))
                  return false;
               if (index >= N)
                  return r.fail("json: variant index out of range");
               if (!r.take(','))
                  return r.fail("json: expected ',' in variant");
               if (!alternative(index))
                  return false;
               if (!r.take(']'))
                  return r.fail("json: expected ']' after variant value");
               return true;
            }
         }
         else if constexpr (is_std_tuple<T>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<T>;
            return read_fixed_array(r, N, depth, [&](std::size_t i) {
               bool ok = false;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (void)((i == Is
                             ? (ok = read_value<Fmt, int_mode::natural,
                                                std::tuple_element_t<Is, T>>(
                                   r, out ? &std::get<Is>(*out) : nullptr,
                                   depth + 1),
                                true)
                             : false) ||
                         ...);
               }(std::make_index_sequence<N>{});
               return ok;
            });
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            constexpr std::size_t N = R::member_count;
            if (!r.take('{'))
               return r.fail("json: expected '{'");
            if (depth >= kMaxValidationDepth)
               return r.fail("json: max depth exceeded");
            // `seen` only records which members were given, to find the
            // missing ones; a repeated key is simply read again.
            std::array<bool, N + 1> seen{};
            std::string             scratch;
            if (!r.take('}'))
            {
               std::string_view key;
               bool             more = true;
               // Reads the member at `key`'s value position, then the
               // separator and the next key (or the closing brace).
               auto advance = [&]() -> bool {
                  if (r.take('}'))
                  {
                     more = false;
                     return true;
                  }
                  if (!r.take(','))
                     return r.fail("json: expected ',' or '}' in object");
                  if (!read_key(r, scratch, key, Fmt::i_json))
                     return false;
                  if (!r.take(':'))
                     return r.fail("json: expected ':' after object key");
                  return true;
               };
               if (!read_key(r, scratch, key, Fmt::i_json))
                  return false;
               if (!r.take(':'))
                  return r.fail("json: expected ':' after object key");
               // Expected order first: writers emit members in declaration
               // order, so each key is compared with the next member's name
               // (a compare against a constant) before any lookup. A member
               // the text leaves out costs one failed compare.
               bool ok = true;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (void)((!more || key != R::template member_name<Is>
                             ? true
                             : (seen[Is] = true,
                                ok = read_member<Fmt, T, Is>(r, out, depth) &&
                                     advance())) &&
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
                     if (!member_readers<Fmt, T>::table[index](r, out, depth))
                        return false;
                  }
                  else if (!skip_value(r, depth + 1, Fmt::i_json))
                     return false;  // unknown key: its value is skipped
                  if (!advance())
                     return false;
               }
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
               return r.fail("json: missing required member");
            return true;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::json: unsupported type in read_value");
         }
      }

      // Reads exactly one value of T spanning the whole text.
      template <typename Fmt, typename T>
      codec_status read_document(std::span<const char> bytes, T* out,
                                 bool lenient_missing)
      {
         reader r{bytes.data(), bytes.data(), bytes.data() + bytes.size(),
                  lenient_missing};
         bool ok = true;
         if (bytes.empty())
            ok = r.fail("json: empty input");
         else
         {
            ok = read_value<Fmt, int_mode::natural, T>(r, out, 0);
            if (ok)
            {
               r.ws();
               if (r.p != r.end)
                  ok = r.fail("json: trailing bytes after value");
            }
         }
         if (ok)
            return codec_ok();
         return codec_fail(r.err,
                           static_cast<std::uint32_t>(r.err_at - r.begin),
                           Fmt::name);
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
            encode_value<Derived, int_mode::natural>(v, w);
         }

         template <typename T>
         friend std::string tag_invoke(decltype(::psio::encode), Derived,
                                       const T& v)
         {
            std::string out;
            writer      w{out, Derived::name};
            encode_value<Derived, int_mode::natural>(v, w);
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
            encode_value<Derived, int_mode::natural>(v, w);
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

   // ── Status-returning entry points ──────────────────────────────────────
   //
   // Usable with exceptions disabled. Neither throws or aborts on bad
   // input or on a value without a JSON text.

   // Reads `text` as one T. On failure `out` is unspecified.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status decode_json_checked(Fmt,
                                                  std::span<const char> text,
                                                  T& out)
   {
      out = T{};
      return detail::json_impl::read_document<Fmt, T>(text, &out, false);
   }

   // Appends the text of `value` to `out`. On failure `out` is restored.
   template <detail::json_impl::json_family Fmt, typename T>
   [[nodiscard]] codec_status encode_json_checked(Fmt, const T& value,
                                                  std::string& out)
   {
      const std::size_t           start = out.size();
      detail::json_impl::writer w{out, Fmt::name, false};
      detail::json_impl::encode_value<Fmt, detail::json_impl::int_mode::natural>(
         value, w);
      if (w.err.empty())
         return codec_ok();
      out.resize(start);
      return codec_fail(w.err, static_cast<std::uint32_t>(start), Fmt::name);
   }

}  // namespace psio
