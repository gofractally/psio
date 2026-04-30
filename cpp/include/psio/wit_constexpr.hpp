#pragma once
//
// psio/wit_constexpr.hpp — Consteval WIT text generator.
//
// Companion to psio/wit_gen.hpp.  Both consume the same metadata
// (PSIO_PACKAGE / PSIO_INTERFACE / PSIO_REFLECT); wit_gen runs at
// runtime and uses the heap, this header runs entirely at compile time
// and emits a fixed-size std::array<char, N>.
//
// Intended use: embed the WIT text for an interface directly in the
// guest WASM data section, where pzam_wit promotes it into a
// `component-type:NAME` custom section:
//
//    [[gnu::section(".custom_section.component-type:greeter"), gnu::used]]
//    inline constexpr auto _wit =
//       psio::constexpr_wit::wit_array<greeter>();
//
// Layout of wit_array<Tag>(): magic ("PSIO_WIT\x01" — 9 bytes) + u32le
// length + WIT interface text.  pzam_wit scans for the magic and
// promotes the text to the proper custom section.
//
// Interface-text output is byte-identical to the interface block
// produced by psio::wit_gen for the same Tag (excluding the package
// preamble and world block, which wit_gen emits separately).

#include <psio/reflect.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio
{
   namespace constexpr_wit
   {

      // ── Compile-time fixed-capacity buffer ─────────────────────────
      //
      // 8 KiB is plenty for any single WIT interface; v1 used the same
      // limit and never came close in practice.
      struct buffer
      {
         static constexpr std::size_t MAX       = 8192;
         char                         data[MAX] = {};
         unsigned                     len       = 0;

         constexpr void put(char c) { data[len++] = c; }

         constexpr void append(std::string_view sv)
         {
            for (char c : sv)
               data[len++] = c;
         }

         constexpr void append(const char* s)
         {
            while (*s)
               data[len++] = *s++;
         }
      };

      // ── Type traits (constexpr-friendly, no typeid) ────────────────
      namespace cdetail
      {
         template <typename T>
         struct is_vector : std::false_type
         {
         };
         template <typename E, typename A>
         struct is_vector<std::vector<E, A>> : std::true_type
         {
            using elem = E;
         };

         template <typename T>
         struct is_optional : std::false_type
         {
         };
         template <typename T>
         struct is_optional<std::optional<T>> : std::true_type
         {
            using elem = T;
         };

         template <typename T>
         struct own_inner;
         template <typename T>
         struct own_inner<own<T>>
         {
            using type = T;
         };

         template <typename T>
         struct borrow_inner;
         template <typename T>
         struct borrow_inner<borrow<T>>
         {
            using type = T;
         };

         template <typename F>
         struct fn_decompose;
         template <typename R, typename... Args>
         struct fn_decompose<R (*)(Args...)>
         {
            using ret  = R;
            using args = std::tuple<Args...>;
            template <std::size_t I>
            using arg                          = std::tuple_element_t<I, args>;
            static constexpr std::size_t arity = sizeof...(Args);
         };
      }  // namespace cdetail

      // ── Kebab-case at compile time ─────────────────────────────────
      //
      // Mirrors psio::detail::to_kebab_case (wit_gen) byte-for-byte:
      // '_' collapses to '-' (deduped), uppercase letters get a
      // leading '-' (unless already preceded by '-') and are
      // lowercased.
      constexpr void emit_kebab(buffer& buf, std::string_view s)
      {
         for (std::size_t i = 0; i < s.size(); ++i)
         {
            char c = s[i];
            if (c == '_')
            {
               if (buf.len > 0 && buf.data[buf.len - 1] != '-')
                  buf.put('-');
            }
            else if (c >= 'A' && c <= 'Z')
            {
               if (buf.len > 0 && buf.data[buf.len - 1] != '-')
                  buf.put('-');
               buf.put(static_cast<char>(c + ('a' - 'A')));
            }
            else
            {
               buf.put(c);
            }
         }
      }

      // ── emit_wit_type<T> ───────────────────────────────────────────
      template <typename T>
      constexpr void emit_wit_type(buffer& buf)
      {
         using U = std::remove_cvref_t<T>;

         if constexpr (std::is_same_v<U, bool>)
            buf.append("bool");
         else if constexpr (std::is_same_v<U, std::uint8_t>)
            buf.append("u8");
         else if constexpr (std::is_same_v<U, std::int8_t>)
            buf.append("s8");
         else if constexpr (std::is_same_v<U, std::uint16_t>)
            buf.append("u16");
         else if constexpr (std::is_same_v<U, std::int16_t>)
            buf.append("s16");
         else if constexpr (std::is_same_v<U, std::uint32_t>)
            buf.append("u32");
         else if constexpr (std::is_same_v<U, std::int32_t>)
            buf.append("s32");
         else if constexpr (std::is_same_v<U, std::uint64_t>)
            buf.append("u64");
         else if constexpr (std::is_same_v<U, std::int64_t>)
            buf.append("s64");
         else if constexpr (std::is_same_v<U, float>)
            buf.append("f32");
         else if constexpr (std::is_same_v<U, double>)
            buf.append("f64");
         else if constexpr (std::is_same_v<U, char>)
            buf.append("char");
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            buf.append("string");
         else if constexpr (cdetail::is_vector<U>::value)
         {
            buf.append("list<");
            emit_wit_type<typename cdetail::is_vector<U>::elem>(buf);
            buf.put('>');
         }
         else if constexpr (cdetail::is_optional<U>::value)
         {
            buf.append("option<");
            emit_wit_type<typename cdetail::is_optional<U>::elem>(buf);
            buf.put('>');
         }
         else if constexpr (detail::is_own_ct<U>::value)
         {
            buf.append("own<");
            emit_kebab(buf, std::string_view{
                               reflect<typename cdetail::own_inner<U>::type>::name});
            buf.put('>');
         }
         else if constexpr (detail::is_borrow_ct<U>::value)
         {
            buf.append("borrow<");
            emit_kebab(buf, std::string_view{
                               reflect<typename cdetail::borrow_inner<U>::type>::name});
            buf.put('>');
         }
         else if constexpr (Reflected<U>)
         {
            emit_kebab(buf, std::string_view{reflect<U>::name});
         }
         else
         {
            buf.append("u32");
         }
      }

      // ── emit_record<T> ─────────────────────────────────────────────
      //
      // Walks reflect<T>'s data members.  Output (with a 2-space
      // ambient indent applied by the caller) matches wit_gen's record
      // emission exactly:
      //
      //     record name {
      //       field-a: type-a,
      //       field-b: type-b,
      //     }
      template <typename T, std::size_t... Is>
      constexpr void emit_record_fields(buffer& buf, std::index_sequence<Is...>)
      {
         ((buf.append("    "),
           emit_kebab(buf, std::string_view{reflect<T>::template member_name<Is>}),
           buf.append(": "),
           emit_wit_type<typename reflect<T>::template member_type<Is>>(buf),
           buf.append(",\n")),
          ...);
      }

      template <typename T>
      constexpr void emit_record(buffer& buf)
      {
         if constexpr (is_wit_resource_v<T>)
         {
            // Resources are opaque handles — runtime wit_gen emits a
            // bare `resource T;` when no methods are registered for T.
            // Match that shape exactly.  Resource methods themselves
            // are picked up via a separate PSIO_INTERFACE on T (the
            // tag is the resource type itself).
            buf.append("  resource ");
            emit_kebab(buf, std::string_view{reflect<T>::name});
            buf.append(";\n\n");
            return;
         }
         buf.append("  record ");
         emit_kebab(buf, std::string_view{reflect<T>::name});
         buf.append(" {\n");
         constexpr auto N = reflect<T>::member_count;
         emit_record_fields<T>(buf, std::make_index_sequence<N>{});
         buf.append("  }\n\n");
      }

      template <typename TypesTuple, std::size_t... Is>
      constexpr void emit_types_impl(buffer& buf, std::index_sequence<Is...>)
      {
         (emit_record<std::tuple_element_t<Is, TypesTuple>>(buf), ...);
      }

      template <typename TypesTuple>
      constexpr void emit_types(buffer& buf)
      {
         if constexpr (std::tuple_size_v<TypesTuple> > 0)
            emit_types_impl<TypesTuple>(
               buf,
               std::make_index_sequence<std::tuple_size_v<TypesTuple>>{});
      }

      // ── emit_function ──────────────────────────────────────────────
      template <typename FnPtr, std::size_t... Is>
      constexpr void emit_params(buffer&                                    buf,
                                 const std::initializer_list<const char*>& names,
                                 std::index_sequence<Is...>)
      {
         using D    = cdetail::fn_decompose<FnPtr>;
         auto it    = names.begin();
         unsigned i = 0;
         ((
             (i > 0 ? buf.append(", ") : void()),
             emit_kebab(buf, it != names.end() ? std::string_view{*it++}
                                               : std::string_view{}),
             buf.append(": "),
             emit_wit_type<std::remove_cvref_t<typename D::template arg<Is>>>(buf),
             ++i
          ),
          ...);
      }

      template <typename FnPtr>
      constexpr void emit_return(buffer& buf)
      {
         using R = typename cdetail::fn_decompose<FnPtr>::ret;
         if constexpr (!std::is_void_v<R>)
         {
            buf.append(" -> ");
            emit_wit_type<std::remove_cvref_t<R>>(buf);
         }
      }

      template <typename Tag, std::size_t... Is>
      constexpr void emit_functions(buffer& buf, std::index_sequence<Is...>)
      {
         using info       = ::psio::detail::interface_info<Tag>;
         using func_types = typename info::func_types;

         ((buf.append("  "),
           emit_kebab(buf, info::func_names[Is]),
           buf.append(": func("),
           emit_params<std::tuple_element_t<Is, func_types>>(
              buf, info::param_names[Is],
              std::make_index_sequence<cdetail::fn_decompose<
                 std::tuple_element_t<Is, func_types>>::arity>{}),
           buf.append(")"),
           emit_return<std::tuple_element_t<Is, func_types>>(buf),
           buf.append(";\n")),
          ...);
      }

      // ── interface_text<Tag>() — full interface block ───────────────
      //
      // Layout matches the inner content of wit_gen's
      // wit_emit_interface for the same Tag:
      //
      //     interface name {
      //       record ... { ... }
      //
      //       fn-name: func(...) -> ret;
      //       ...
      //     }
      template <typename Tag>
      consteval auto interface_text()
      {
         using info       = ::psio::detail::interface_info<Tag>;
         constexpr auto N = std::tuple_size_v<typename info::func_types>;

         buffer buf;
         buf.append("interface ");
         emit_kebab(buf, std::string_view{info::name});
         buf.append(" {\n");
         emit_types<typename info::types>(buf);
         emit_functions<Tag>(buf, std::make_index_sequence<N>{});
         buf.append("}\n");

         std::array<char, buffer::MAX> result{};
         for (unsigned i = 0; i < buf.len; ++i)
            result[i] = buf.data[i];
         return std::pair{result, buf.len};
      }

      // ── Magic header for pzam_wit ──────────────────────────────────
      //
      // Distinct from v1's "PSIO1_WIT\x01" so a single host scanning a
      // hybrid binary can route v1- and v3-emitted blobs separately
      // without ambiguity.
      inline constexpr char     MAGIC[]   = "PSIO_WIT\x01";
      inline constexpr unsigned MAGIC_LEN = sizeof(MAGIC) - 1;  // exclude NUL

      template <typename Tag>
      consteval unsigned blob_size()
      {
         return MAGIC_LEN + 4 + interface_text<Tag>().second;
      }

      template <typename Tag>
      consteval auto interface_blob()
      {
         auto     text  = interface_text<Tag>();
         unsigned total = MAGIC_LEN + 4 + text.second;

         std::array<char, buffer::MAX> result{};
         unsigned                      pos = 0;
         for (unsigned i = 0; i < MAGIC_LEN; ++i)
            result[pos++] = MAGIC[i];

         std::uint32_t len = text.second;
         result[pos++]     = static_cast<char>(len & 0xff);
         result[pos++]     = static_cast<char>((len >> 8) & 0xff);
         result[pos++]     = static_cast<char>((len >> 16) & 0xff);
         result[pos++]     = static_cast<char>((len >> 24) & 0xff);

         for (unsigned i = 0; i < text.second; ++i)
            result[pos++] = text.first[i];

         return std::pair{result, total};
      }

      // ── Exact-fit array for embedding in a custom section ─────────
      //
      // wit_array<Tag>() returns a std::array sized to exactly the
      // blob length — suitable for binding to a [[gnu::section(...)]]
      // global so pzam_wit can scan and promote it.
      template <typename Tag>
      consteval unsigned wit_size()
      {
         return blob_size<Tag>();
      }

      template <typename Tag>
      consteval auto wit_array()
      {
         auto                            blob = interface_blob<Tag>();
         std::array<char, blob_size<Tag>()> result{};
         for (unsigned i = 0; i < result.size(); ++i)
            result[i] = blob.first[i];
         return result;
      }

   }  // namespace constexpr_wit
}  // namespace psio
