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
// What a format requires of an adapter on untrusted input. `validate`
// (and `validate_strict`) is the gate: every checking reader calls it on
// the payload first and calls `decode` only on a payload it accepted. So
//   - `validate` must report through the status it returns. It must not
//     throw and must not abort, whatever the bytes / text: the checking
//     entry points (validate, decode_json_checked, the schema-driven
//     codecs' handlers, ...) promise a status in builds with and without
//     exceptions, and a guest has no exceptions to catch. A parser that
//     throws or aborts on malformed input (a third-party DER / PEM
//     reader, say) cannot be the validator as it is: catch inside
//     `validate` where exceptions exist, or check the structure with code
//     that reports;
//   - the message of the status `validate` returns is static storage
//     (`codec_fail("...", ...)` with a literal): the text readers keep the
//     view, not the status. (An invariant's status is kept whole, so its
//     message may be built at run time with codec_fail_owned.);
//   - `decode` may assume `validate` accepted the same payload. The
//     trusted, fail-fast paths (psio::decode on bytes already validated)
//     call it without validating: there it may throw or abort like the
//     rest of the fail-fast decode;
//   - text a `text_category` adapter accepts is the type's whole text
//     form (one JSON string); a type invariant, if the type has one, runs
//     on the decoded value afterwards (invariant.hpp, which states the
//     same contract for the check: noexcept, a status, never an abort).
//
// Tags are nominal types. Formats declare a default tag via
// `using preferred_presentation_category = …` (JSON → text_category;
// ssz / pssz / frac / bin → binary_category). Member annotations can
// override with more specific tags (hex_tag, base58_tag, …) via
// `as<Tag>` entries in the annotation tuple.

#include <psio/annotate.hpp>  // as_spec, presentation tags
#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/invariant.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio {

   // ── Presentation categories ─────────────────────────────────────────
   //
   // Nominal tags that formats use to decide which adapter slot of a
   // type they want to consult. A format whose wire is bytes asks for
   // `binary_category`; a format whose wire is human-readable text
   // asks for `text_category`; a format whose wire is the SORT KEY
   // asks for `sortable_binary_category`. The sortable slot is its
   // own category because the wire and sort encodings can differ
   // (e.g. `int32` is little-endian on the wire but sign-flipped
   // big-endian as a sort key). Additional categories can be added;
   // nothing about the mechanism depends on this list specifically.
   struct binary_category
   {
   };
   struct text_category
   {
   };
   struct sortable_binary_category
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

   // ── Custom types in the Schema IR ───────────────────────────────────
   //
   // A type with an identity of its own beyond its wire form — a custom
   // text presentation (a text_category adapter), an invariant
   // (invariant.hpp), a declared schema id (`schemaId("Id")` in PSIO_REFLECT),
   // or several of these — is, in the Schema IR, `Custom{type: its wire
   // type, id}`, whatever its shape: a reflected record, a transparent
   // wrapper, an enum, a std::variant, a std::tuple, a list. The wire type
   // says what the bytes are; the id names the type to the schema-driven
   // codecs, which present and check it through the handler registered for
   // the id (pssz_ir_custom.hpp CustomHandlers). Without a handler the node
   // is its wire type: the wire type's text, structure checked only.
   //
   // The id is, in order: the text adapter implementation's
   //     static constexpr std::string_view schema_id = "AccountNumber";
   // (set it so that C++ and Rust use one string), else the id given to
   // PSIO_INVARIANT, else the one given by `schemaId("Id")` in PSIO_REFLECT,
   // else the type's PSIO_REFLECT name, else its PSIO_REFLECT_ENUM name. A
   // type with none of these (a std::variant, a std::tuple) must provide
   // one: a compile error otherwise. Two of the first three on one type are
   // a compile error: a type has one schema id.
   //
   // `schemaId("Id")` alone is the id and nothing more: a reflected record
   // that a schema-driven tool wants to present its own way (a handler for
   // the id) and that is otherwise an ordinary record. It has no text form of
   // its own, no check, and — unlike a type with an invariant — it is
   // addressable by psio::mutable_view: has_schema_id_v is not consulted by
   // any codec, validator or view.
   //
   // A text adapter that is not the type's presentation in the Schema IR —
   // the "0x" text of the integers wider than 64 bits, which are decimal
   // in JSON and plain integers in the schema (ext_int.hpp) — says so:
   //     static constexpr bool schema_custom = false;
   namespace detail
   {
      template <typename T>
      constexpr bool text_adapter_is_custom() noexcept
      {
         using A = adapter<T, text_category>;
         if constexpr (!A::has_value)
            return false;
         else if constexpr (requires { requires !A::schema_custom; })
            return false;
         else
            return true;
      }
   }  // namespace detail

   // The id `schemaId("Id")` in PSIO_REFLECT gave T, or empty.
   template <typename T>
   constexpr std::string_view reflected_schema_id() noexcept
   {
      using U = std::remove_cvref_t<T>;
      if constexpr (std::is_class_v<U> && Reflected<U>)
      {
         if constexpr (requires { std::string_view{reflect<U>::_psio_schema_id_}; })
            return std::string_view{reflect<U>::_psio_schema_id_};
         else
            return {};
      }
      else
         return {};
   }
   template <typename T>
   inline constexpr bool has_schema_id_v = !reflected_schema_id<T>().empty();

   template <typename T>
   inline constexpr bool schema_custom_v =
      detail::text_adapter_is_custom<std::remove_cvref_t<T>>() || has_invariant_v<T> ||
      has_schema_id_v<T>;

   // ── Reserved Custom ids ─────────────────────────────────────────────
   //
   // reserved_custom_id (reflect.hpp, where the schemaId keyword checks it
   // too): the ids PSIO's own types have in the Schema IR, which no custom
   // type may claim. schema_custom_id refuses one at compile time (every id
   // a C++ type can give is a constant), CustomHandlers::add at run time,
   // and Rust's derives and schema builder likewise (pssz_schema_ir.rs
   // reserved_custom_id).

   namespace detail
   {
      template <typename U>
      constexpr std::string_view declared_schema_custom_id() noexcept
      {
         using A = adapter<U, text_category>;
         static_assert(!has_schema_id_v<U> ||
                          (!requires { std::string_view{A::schema_id}; } &&
                           invariant_schema_id<U>().empty()),
                       "psio: the schema id of this type is declared twice — schemaId(...) "
                       "in PSIO_REFLECT and the `schema_id` of its text adapter or the id "
                       "given to PSIO_INVARIANT");
         if constexpr (requires { std::string_view{A::schema_id}; })
            return std::string_view{A::schema_id};
         else if constexpr (!invariant_schema_id<U>().empty())
            return invariant_schema_id<U>();
         else if constexpr (has_schema_id_v<U>)
            return reflected_schema_id<U>();
         else if constexpr (std::is_class_v<U> && Reflected<U>)
            return reflect<U>::name;
         else
         {
            static_assert(ReflectedEnum<U>,
                          "psio: a custom type that is not a reflected record or enum (a "
                          "std::variant, a std::tuple, ...) needs a schema id: give its text "
                          "adapter a `static constexpr std::string_view schema_id`, or pass "
                          "the id to PSIO_INVARIANT(Type, fn, \"Id\")");
            return reflect_enum<U>::name;
         }
      }
   }  // namespace detail

   template <typename T>
   constexpr std::string_view schema_custom_id() noexcept
   {
      using U = std::remove_cvref_t<T>;
      constexpr std::string_view id = detail::declared_schema_custom_id<U>();
      static_assert(!reserved_custom_id(id),
                    "psio: this custom type's schema id is reserved for one of PSIO's own "
                    "types (bool, char, string, utf8, hex, bitvector, bitlist, map, set, box, json, "
                    "unknown, time-point..., duration..., monotonic...): a schema-driven codec "
                    "would read the built-in meaning into it. Give the type another id: "
                    "`static constexpr std::string_view schema_id` in its text adapter, or "
                    "PSIO_INVARIANT(Type, fn, \"Id\")");
      return id;
   }

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

   // ── Text-only overrides and the binary formats ──────────────────────
   //
   // `as<Tag>` on a member selects adapter<F, Tag> for that member. An
   // adapter that only writes text (its encode takes a std::string: the
   // "0x" form of the wide integers, as<hex_tag>) is a text presentation:
   // it changes the member's JSON and nothing else. The binary formats
   // (pssz, key, bin, frac, ssz, the Schema IR, width inference) honour an
   // override only when its adapter can write bytes
   // (`encode(value, std::vector<char>&)`); otherwise the member is its
   // plain type there.
   namespace detail
   {
      template <typename F, typename Tag>
      concept adapter_writes_bytes = requires(const F& v, std::vector<char>& s) {
         adapter<F, Tag>::encode(v, s);
      };

      template <typename F, typename S>
      constexpr auto keep_for_binary(const S& spec) noexcept
      {
         if constexpr (is_as_spec<S>::value)
         {
            if constexpr (adapter_writes_bytes<F, typename S::adapter_tag>)
               return std::tuple<S>{spec};
            else
               return std::tuple<>{};
         }
         else
            return std::tuple<S>{spec};
      }
   }  // namespace detail

   template <typename F, typename Tuple>
   inline constexpr bool has_binary_as_override_v =
      has_as_override_v<Tuple> &&
      detail::adapter_writes_bytes<std::remove_cvref_t<F>, adapter_tag_of_t<Tuple>>;

   // The annotations of a member of type F as a binary format sees them:
   // without a text-only as<Tag>.
   template <typename F, typename... Ts>
   constexpr auto binary_annotations(const std::tuple<Ts...>& specs) noexcept
   {
      return std::apply(
         [](const auto&... s) {
            return std::tuple_cat(detail::keep_for_binary<std::remove_cvref_t<F>>(s)...);
         },
         specs);
   }

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
