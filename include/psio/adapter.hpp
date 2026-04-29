#pragma once
//
// psio/adapter.hpp — per-type adapter registry.
//
// An adapter is a complete codec surface (packsize / encode / decode /
// validate / validate_strict) scoped to one type × one presentation
// tag. When a format encodes a value whose type has an adapter under
// the format's preferred category, the format delegates every CPO to
// the adapter and treats the result as an opaque payload it frames in
// its usual way.
//
// Two layers of framing is the correct model (design § 5.3.7):
// the outer format owns delimiter + length bookkeeping because the
// adapter's byte count is opaque to it; the adapter owns everything
// inside those bytes. Adapters and outer formats never peek into
// each other's framing.
//
// Registration:
//
//   struct Widget { std::string title; std::vector<int> data; };
//   PSIO_REFLECT(Widget, title, data)
//
//   // Bespoke text adapter (emits XML).
//   struct widget_xml {
//      static std::size_t  packsize(const Widget&);
//      static void         encode(const Widget&, std::string&);
//      static Widget       decode(std::span<const char>);
//      static psio::codec_status validate(std::span<const char>);
//      static psio::codec_status validate_strict(std::span<const char>);
//   };
//   PSIO_ADAPTER(Widget, psio::text_category, widget_xml)
//
//   // Delegating adapter: "encode this object as bin, inline."
//   PSIO_ADAPTER(Widget, psio::binary_category,
//                 psio::delegate_adapter<Widget, psio::bin>)
//
// Tags are nominal types. Formats declare a default tag via
// `using preferred_presentation_category = …` (JSON → text_category;
// ssz / pssz / frac / bin → binary_category). Member annotations can
// override with more specific tags (hex_tag, base58_tag, …) via
// `as<Tag>` entries in the annotation tuple.

#include <psio/annotate.hpp>  // as_spec, presentation tags
#include <psio/cpo.hpp>
#include <psio/error.hpp>

#include <cstddef>
#include <span>
#include <type_traits>

namespace psio {

   // ── Presentation categories ─────────────────────────────────────────
   //
   // Nominal tags that formats use to decide which adapter slot of a
   // type they want to consult. A format whose wire is bytes asks for
   // binary_category; a format whose wire is human-readable text asks
   // for text_category. Additional categories can be added; nothing
   // about the mechanism depends on these two specifically.
   struct binary_category
   {
   };
   struct text_category
   {
   };

   // ── Adapter trait ───────────────────────────────────────────────────
   //
   // Users specialize this to register an adapter for (T, Tag).
   // The primary template says "no adapter." Specializations provide
   // the full CPO surface and set `has_value = true`.
   template <typename T, typename Tag>
   struct adapter
   {
      static constexpr bool has_value = false;
   };

   // Convenience query for format dispatch.
   template <typename T, typename Tag>
   inline constexpr bool has_adapter_v =
      adapter<std::remove_cvref_t<T>, Tag>::has_value;

   // ── Delegating adapter ──────────────────────────────────────────────
   //
   // "Encode this type using an existing format, inline." The outer
   // format still frames the resulting payload with its own
   // delimiters; the inner format owns the payload bytes entirely.
   //
   // Use:
   //
   //   PSIO_ADAPTER(MyType, psio::binary_category,
   //                 psio::delegate_adapter<MyType, psio::bin>)
   //
   template <typename T, typename DelegateFmt>
   struct delegate_adapter
   {
      static constexpr bool has_value = true;

      // Marker picked up by adapter_delegates_to_format_v — allows
      // each format's dispatch to detect adapters that delegate back
      // to itself and fall through to the shape walk instead of
      // recursing.
      using delegated_format = DelegateFmt;

      static std::size_t packsize(const T& v) noexcept
      {
         return ::psio::size_of(DelegateFmt{}, v);
      }

      template <typename Sink>
      static void encode(const T& v, Sink& s)
      {
         ::psio::encode(DelegateFmt{}, v, s);
      }

      static T decode(std::span<const char> b) noexcept
      {
         return ::psio::decode<T>(DelegateFmt{}, b);
      }

      [[nodiscard]] static codec_status
      validate(std::span<const char> b) noexcept
      {
         return ::psio::validate<T>(DelegateFmt{}, b);
      }

      [[nodiscard]] static codec_status
      validate_strict(std::span<const char> b) noexcept
      {
         return ::psio::validate_strict<T>(DelegateFmt{}, b);
      }
   };

   // ── Format category helper ──────────────────────────────────────────
   //
   // Formats declare `using preferred_presentation_category = …`. This
   // trait lets generic code read that out with a safe default of
   // `void` (meaning "format does not consult adapters").
   namespace detail {
      template <typename Fmt, typename = void>
      struct preferred_category_of
      {
         using type = void;
      };
      template <typename Fmt>
      struct preferred_category_of<
         Fmt, std::void_t<typename Fmt::preferred_presentation_category>>
      {
         using type = typename Fmt::preferred_presentation_category;
      };
   }  // namespace detail

   template <typename Fmt>
   using preferred_category_t =
      typename detail::preferred_category_of<Fmt>::type;

   // True iff T has an adapter under the tag Fmt prefers.
   template <typename Fmt, typename T>
   inline constexpr bool format_has_adapter_v =
      !std::is_void_v<preferred_category_t<Fmt>> &&
      adapter<std::remove_cvref_t<T>,
              preferred_category_t<Fmt>>::has_value;

   // The matching adapter type, for use with `if constexpr`.
   template <typename Fmt, typename T>
   using adapter_for_t =
      adapter<std::remove_cvref_t<T>, preferred_category_t<Fmt>>;

   // ── Self-delegation detection ───────────────────────────────────────
   //
   // An adapter whose implementation is `delegate_adapter<T, Fmt>` must
   // not be consulted by Fmt itself — that would recurse forever
   // through Fmt's dispatch. Each format's dispatch uses this trait to
   // skip its own delegating adapters and fall through to the shape
   // walk instead.
   //
   // Third-party adapters with bespoke `encode`/`decode` (not using
   // delegate_adapter) are never self-delegating; this trait defaults
   // to false.
   namespace detail {
      template <typename Adapter, typename Fmt, typename = void>
      struct delegates_to_check : std::false_type
      {
      };

      // PSIO_ADAPTER registers a derived struct whose base is the
      // user-supplied IMPL. Matching by exact type misses that;
      // instead we rely on the `delegated_format` nested alias
      // declared by delegate_adapter, which is inherited.
      template <typename Adapter, typename Fmt>
      struct delegates_to_check<
         Adapter, Fmt,
         std::void_t<typename Adapter::delegated_format>>
         : std::is_same<typename Adapter::delegated_format, Fmt>
      {
      };
   }

   template <typename Adapter, typename Fmt>
   inline constexpr bool adapter_delegates_to_format_v =
      detail::delegates_to_check<Adapter, Fmt>::value;

   // Convenience for format dispatch: "does this format have a
   // non-self-delegating adapter for T under its preferred tag?"
   // If yes, the format should dispatch into the adapter. If no (no
   // adapter, or a self-delegating one), the format should walk the
   // shape.
   template <typename Fmt, typename T>
   inline constexpr bool format_should_dispatch_adapter_v =
      format_has_adapter_v<Fmt, T> &&
      !adapter_delegates_to_format_v<
         adapter<std::remove_cvref_t<T>, preferred_category_t<Fmt>>, Fmt>;

   // ── Member-level adapter override (§5.3.7) ──────────────────────────
   //
   // Scans a tuple of spec values for an `as_spec<Tag>` entry and
   // returns the adapter tag type (or `void` if none present). Formats
   // call this at record-walk time with the merged effective
   // annotations to decide whether a specific field should use a
   // member-overridden adapter instead of the category default.

   namespace detail {
      template <typename Tuple>
      struct find_adapter_tag
      {
         using type = void;
      };

      template <typename T>
      struct is_as_spec : std::false_type
      {
      };

      template <typename Tag>
      struct is_as_spec<::psio::as_spec<Tag>> : std::true_type
      {
      };

      // Lazy lookup: `tag_of_one<Spec>` must only try to read
      // `Spec::adapter_tag` when `Spec` is actually an `as_spec<_>`.
      // `std::conditional_t` would eagerly evaluate both branches, so
      // we indirect through a helper with specialization.
      template <typename Spec, bool = is_as_spec<std::remove_cvref_t<Spec>>::value>
      struct tag_or_void
      {
         using type = void;
      };
      template <typename Spec>
      struct tag_or_void<Spec, true>
      {
         using type = typename std::remove_cvref_t<Spec>::adapter_tag;
      };

      template <typename... Ts>
      struct find_adapter_tag<std::tuple<Ts...>>
      {
       private:
         template <typename Spec>
         using tag_of_one = typename tag_or_void<Spec>::type;

         template <typename... Tags>
         struct first_non_void
         {
            using type = void;
         };

         template <typename Head, typename... Rest>
         struct first_non_void<Head, Rest...>
         {
            using type = std::conditional_t<
               !std::is_void_v<Head>, Head,
               typename first_non_void<Rest...>::type>;
         };

       public:
         using type =
            typename first_non_void<tag_of_one<Ts>...>::type;
      };
   }

   template <typename Tuple>
   using adapter_tag_of_t =
      typename detail::find_adapter_tag<Tuple>::type;

   template <typename Tuple>
   inline constexpr bool has_as_override_v =
      !std::is_void_v<adapter_tag_of_t<Tuple>>;

}  // namespace psio

// ── PSIO_ADAPTER macro ──────────────────────────────────────────────────
//
// Registers the IMPL (last argument) as the adapter for `TYPE` under
// tag `TAG`. Static-asserts that IMPL provides the full codec surface
// so a format never dispatches into an adapter with a missing op.
//
// `__VA_ARGS__` captures everything after TAG so template arguments
// with commas work:
//
//   PSIO_ADAPTER(MyType, psio::binary_category,
//                 psio::delegate_adapter<MyType, psio::bin>)
#define PSIO_ADAPTER(TYPE, TAG, ...)                                        \
   namespace psio {                                                         \
      template <>                                                            \
      struct adapter<TYPE, TAG> : __VA_ARGS__                                \
      {                                                                      \
         static constexpr bool has_value = true;                             \
                                                                             \
         static_assert(                                                      \
            requires(const TYPE& v) {                                        \
               { __VA_ARGS__::packsize(v) }                                  \
                  -> std::convertible_to<std::size_t>;                       \
            },                                                               \
            "psio::adapter: IMPL is missing packsize(const T&)");           \
                                                                             \
         static_assert(                                                      \
            requires(std::span<const char> b) {                              \
               { __VA_ARGS__::decode(b) } -> std::convertible_to<TYPE>;      \
            },                                                               \
            "psio::adapter: IMPL is missing decode(span<const char>)");     \
                                                                             \
         static_assert(                                                      \
            requires(std::span<const char> b) {                              \
               { __VA_ARGS__::validate(b) }                                  \
                  -> std::convertible_to<::psio::codec_status>;             \
            },                                                               \
            "psio::adapter: IMPL is missing validate(span<const char>)");   \
                                                                             \
         static_assert(                                                      \
            requires(std::span<const char> b) {                              \
               { __VA_ARGS__::validate_strict(b) }                           \
                  -> std::convertible_to<::psio::codec_status>;             \
            },                                                               \
            "psio::adapter: IMPL is missing validate_strict");              \
      };                                                                     \
   }
