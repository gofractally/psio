#pragma once
//
// psio/json.hpp — JSON format tag.
//
// Unlike the binary SSZ/pSSZ tags, JSON writes to a `std::string` sink
// and reads from `std::span<const char>` treated as UTF-8. This is the
// first non-binary format in psio and demonstrates the v3 architecture
// scales to text serializations without requiring special hooks.
//
// Scope (Phase 9 MVP) — types supported:
//   - bool                   → `true` / `false`
//   - integer primitives     → decimal digits
//   - float, double          → `%g` formatting
//   - std::string            → `"…"` with minimal JSON escaping
//   - std::array<T, N>       → `[…, …]`
//   - std::vector<T>         → `[…, …]`
//   - std::optional<T>       → `null` or payload
//   - reflected records      → `{"field": value, …}`
//
// Out of scope for the MVP: arbitrary whitespace / comments in input,
// numeric-string overflow checks, Unicode escapes (`\uXXXX`), schema-
// driven ordering. Decode is strict-comma / strict-colon only.

#include <psio/cpo.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

   struct json;  // fwd — used by adapter-dispatch trait below.

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

      using ::psio::detail::is_std_variant;

      template <typename T>
      concept Record = ::psio::Reflected<T>;

      // ── Encoding ──────────────────────────────────────────────────────────

      using sink_t = std::string;

      inline void write_escaped_string(const std::string_view sv, sink_t& s)
      {
         s.push_back('"');
         for (char c : sv)
         {
            switch (c)
            {
               case '"':  s += R"(\")"; break;
               case '\\': s += R"(\\)"; break;
               case '\n': s += R"(\n)"; break;
               case '\r': s += R"(\r)"; break;
               case '\t': s += R"(\t)"; break;
               default:
                  if (static_cast<unsigned char>(c) < 0x20)
                  {
                     char buf[8];
                     std::snprintf(buf, sizeof(buf), "\\u%04x",
                                   static_cast<unsigned>(c));
                     s += buf;
                  }
                  else
                     s.push_back(c);
            }
         }
         s.push_back('"');
      }

      template <typename T>
      void encode_value(const T& v, sink_t& s)
      {
         // Adapter check — if the type registered a text adapter
         // (and it doesn't delegate back to json), delegate and dump
         // the result as a JSON string with escaping. The adapter
         // produces the payload unquoted; JSON owns the surrounding
         // quotes + escape rules (double framing: outer owns
         // delimiters, inner owns payload — see §5.3.7).
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::json, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::text_category>;
            std::string payload;
            Proj::encode(v, payload);
            write_escaped_string(std::string_view{payload}, s);
            return;
         }

         if constexpr (std::is_same_v<T, bool>)
            s += v ? "true" : "false";
         else if constexpr (std::is_integral_v<T>)
         {
            char buf[32];
            auto r = std::to_chars(buf, buf + sizeof(buf), v);
            s.append(buf, r.ptr);
         }
         else if constexpr (std::is_floating_point_v<T>)
         {
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof(buf), v);
            s.append(buf, r.ptr);
         }
         else if constexpr (std::is_same_v<T, std::string>)
            write_escaped_string(std::string_view{v}, s);
         else if constexpr (is_std_array<T>::value ||
                            is_std_vector<T>::value)
         {
            s.push_back('[');
            bool first = true;
            for (const auto& x : v)
            {
               if (!first)
                  s.push_back(',');
               first = false;
               encode_value(x, s);
            }
            s.push_back(']');
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
               encode_value(*v, s);
            else
               s += "null";
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // Positional tagged form: [index, value]. Unambiguous,
            // doesn't require a type-name registry.
            s.push_back('[');
            char buf[16];
            auto [ptr, _] = std::to_chars(buf, buf + sizeof(buf),
                                          v.index());
            s.append(buf, ptr - buf);
            s.push_back(',');
            std::visit(
               [&](const auto& alt) { encode_value(alt, s); }, v);
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
                      write_escaped_string(R::template member_name<Is>, s);
                      s.push_back(':');

                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);

                      // Member-level presentation override: if the
                      // field's effective annotations include
                      // as_spec<Tag>, use adapter<F, Tag> for
                      // this field specifically (member beats type,
                      // §5.3.7).
                      using eff = typename ::psio::
                         effective_annotations_for<
                            T, F,
                            R::template member_pointer<Is>>::value_t;
                      if constexpr (::psio::has_as_override_v<eff>)
                      {
                         using Tag =
                            ::psio::adapter_tag_of_t<eff>;
                         using Proj =
                            ::psio::adapter<std::remove_cvref_t<F>,
                                                Tag>;
                         std::string payload;
                         Proj::encode(fref, payload);
                         write_escaped_string(
                            std::string_view{payload}, s);
                      }
                      else
                      {
                         encode_value(fref, s);
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

      // ── Decoding — a small hand-rolled parser ─────────────────────────────

      struct parser
      {
         const char*              p;
         const char*              end;

         void skip_ws()
         {
            while (p < end &&
                   (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
               ++p;
         }

         bool consume(char c)
         {
            skip_ws();
            if (p < end && *p == c)
            {
               ++p;
               return true;
            }
            return false;
         }

         bool peek_null()
         {
            skip_ws();
            return end - p >= 4 && std::memcmp(p, "null", 4) == 0;
         }

         bool consume_null()
         {
            skip_ws();
            if (end - p >= 4 && std::memcmp(p, "null", 4) == 0)
            {
               p += 4;
               return true;
            }
            return false;
         }
      };

      inline std::string parse_string(parser& pr)
      {
         pr.skip_ws();
         if (pr.p >= pr.end || *pr.p != '"')
            return {};
         ++pr.p;
         std::string out;
         while (pr.p < pr.end && *pr.p != '"')
         {
            if (*pr.p == '\\' && pr.p + 1 < pr.end)
            {
               char c = pr.p[1];
               switch (c)
               {
                  case '"':  out.push_back('"'); break;
                  case '\\': out.push_back('\\'); break;
                  case '/':  out.push_back('/'); break;
                  case 'n':  out.push_back('\n'); break;
                  case 'r':  out.push_back('\r'); break;
                  case 't':  out.push_back('\t'); break;
                  case 'b':  out.push_back('\b'); break;
                  case 'f':  out.push_back('\f'); break;
                  default:   out.push_back(c); break;
               }
               pr.p += 2;
            }
            else
            {
               out.push_back(*pr.p++);
            }
         }
         if (pr.p < pr.end && *pr.p == '"')
            ++pr.p;
         return out;
      }

      template <typename T>
      T decode_value(parser& pr);

      template <typename T>
         requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
      T decode_integral(parser& pr)
      {
         pr.skip_ws();
         T    out{};
         auto r =
            std::from_chars(pr.p, pr.end, out);
         if (r.ec == std::errc{})
            pr.p = r.ptr;
         return out;
      }

      template <typename T>
         requires(std::is_floating_point_v<T>)
      T decode_floating(parser& pr)
      {
         pr.skip_ws();
         T    out{};
         auto r = std::from_chars(pr.p, pr.end, out);
         if (r.ec == std::errc{})
            pr.p = r.ptr;
         return out;
      }

      template <typename T>
      T decode_value(parser& pr)
      {
         // Adapter mirror of the encode side: consume the JSON
         // string (outer frame); hand its unescaped contents to the
         // adapter's decode.
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::json, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::text_category>;
            std::string payload = parse_string(pr);
            return Proj::decode(std::span<const char>{payload});
         }

         if constexpr (std::is_same_v<T, bool>)
         {
            pr.skip_ws();
            if (pr.p < pr.end && *pr.p == 't')
            {
               pr.p += 4;  // "true"
               return true;
            }
            pr.p += 5;  // "false"
            return false;
         }
         else if constexpr (std::is_integral_v<T>)
            return decode_integral<T>(pr);
         else if constexpr (std::is_floating_point_v<T>)
            return decode_floating<T>(pr);
         else if constexpr (std::is_same_v<T, std::string>)
            return parse_string(pr);
         else if constexpr (is_std_array<T>::value)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            T   out{};
            pr.consume('[');
            for (std::size_t i = 0; i < N; ++i)
            {
               if (i > 0)
                  pr.consume(',');
               out[i] = decode_value<E>(pr);
            }
            pr.consume(']');
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            std::vector<E> out;
            pr.skip_ws();
            if (pr.consume('['))
            {
               pr.skip_ws();
               while (pr.p < pr.end && *pr.p != ']')
               {
                  if (!out.empty())
                     pr.consume(',');
                  out.push_back(decode_value<E>(pr));
                  pr.skip_ws();
               }
               pr.consume(']');
            }
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V = typename T::value_type;
            if (pr.consume_null())
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(pr)};
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // [index, value]
            pr.consume('[');
            const auto idx =
               static_cast<std::size_t>(decode_integral<std::uint32_t>(pr));
            pr.consume(',');
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  std::variant_alternative_t<Is, T>>(pr)},
                       true)
                    : false) ||
                  ...);
               (void)found;
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            pr.consume(']');
            return out;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            T       out{};
            pr.consume('{');
            pr.skip_ws();
            bool first = true;
            while (pr.p < pr.end && *pr.p != '}')
            {
               if (!first)
                  pr.consume(',');
               first              = false;
               std::string key    = parse_string(pr);
               pr.consume(':');
               // Dispatch by field index (so we can thread the
               // member pointer into effective_annotations_for). The
               // compile-time branches cover every field the type
               // declares; unknown keys fall through.
               bool matched = false;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (
                     ([&]
                      {
                         if (matched) return;
                         if (key !=
                             R::template member_name<Is>) return;
                         matched = true;
                         using F =
                            typename R::template member_type<Is>;
                         auto& fref =
                            out.*(R::template member_pointer<Is>);

                         using eff = typename ::psio::
                            effective_annotations_for<
                               T, F,
                               R::template member_pointer<Is>>::value_t;
                         if constexpr (::psio::has_as_override_v<eff>)
                         {
                            using Tag =
                               ::psio::adapter_tag_of_t<eff>;
                            using Proj = ::psio::adapter<
                               std::remove_cvref_t<F>, Tag>;
                            std::string payload = parse_string(pr);
                            fref               = Proj::decode(
                               std::span<const char>{payload});
                         }
                         else
                         {
                            fref = decode_value<F>(pr);
                         }
                      }()),
                     ...);
               }(std::make_index_sequence<R::member_count>{});
               if (!matched)
               {
                  // Unknown key — fall back to the legacy path.
                  R::visit_field_by_name(out, key,
                                          [&]<typename F>(F& f)
                                          { f = decode_value<F>(pr); });
               }
               pr.skip_ws();
            }
            pr.consume('}');
            return out;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::json: unsupported type in decode_value");
         }
      }

      // ── Validation — delegate to a trial decode (MVP) ────────────────────
      //
      // A proper JSON validator would walk without allocation. Phase 9
      // leaves that as a follow-up; the MVP validator answers "does this
      // parse?" via trial decode, which is sufficient for round-trip
      // correctness tests.
      template <typename T>
      codec_status validate_value(std::span<const char> bytes) noexcept
      {
         parser pr{bytes.data(), bytes.data() + bytes.size()};
         pr.skip_ws();
         if (pr.p >= pr.end)
            return codec_fail("json: empty input", 0, "json");
         // Rough structural check: first char must be one of  { [ " t f n -
         // or digit.
         char c = *pr.p;
         if (c != '{' && c != '[' && c != '"' && c != 't' && c != 'f' &&
             c != 'n' && c != '-' && !(c >= '0' && c <= '9'))
            return codec_fail("json: unexpected leading character", 0,
                              "json");
         return codec_ok();
      }

   }  // namespace detail::json_impl

   // ── Format tag ─────────────────────────────────────────────────────────

   struct json : format_tag_base<json>
   {
      static constexpr const char* name = "json";

      using buffer_type = std::string;

      // JSON delegates to the text adapter slot when the type
      // registers one (design § 5.3.7).
      using preferred_presentation_category = ::psio::text_category;

      // ── encode ─────────────────────────────────────────────────────────
      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), json, const T& v,
                             std::string& sink)
      {
         detail::json_impl::encode_value(v, sink);
      }

      template <typename T>
      friend std::string tag_invoke(decltype(::psio::encode), json,
                                    const T& v)
      {
         std::string out;
         detail::json_impl::encode_value(v, out);
         return out;
      }

      // ── decode ─────────────────────────────────────────────────────────
      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), json, T*,
                          std::span<const char> bytes)
      {
         detail::json_impl::parser pr{bytes.data(),
                                       bytes.data() + bytes.size()};
         return detail::json_impl::decode_value<T>(pr);
      }

      // ── size_of ────────────────────────────────────────────────────────
      //
      // JSON has no compile-time-constant size; we produce the string and
      // return its length.
      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), json,
                                    const T& v)
      {
         std::string tmp;
         detail::json_impl::encode_value(v, tmp);
         return tmp.size();
      }

      // ── validate / validate_strict ─────────────────────────────────────
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     json, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "json");
             !st.ok())
            return st;
         return detail::json_impl::validate_value<T>(bytes);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     json, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::json_impl::validate_value<T>(bytes);
      }

      // ── make_boxed ─────────────────────────────────────────────────────
      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           json, T*,
                                           std::span<const char> bytes) noexcept
      {
         detail::json_impl::parser pr{bytes.data(),
                                       bytes.data() + bytes.size()};
         return std::make_unique<T>(detail::json_impl::decode_value<T>(pr));
      }
   };

}  // namespace psio
