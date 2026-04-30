#pragma once
//
// psio/annotate.hpp — Layer 1 annotation system.
//
// Annotations are pure user data attached to types or members and
// consumed by format authors. Design spec: `.issues/psio-v2-design.md`
// § 5.3.
//
// Producer / consumer model:
//   - Producers (users) specialize `psio::annotate<X>` for
//     `X = &T::field` (member-level) or `X = psio::type<T>{}` (type-level),
//     attaching a tuple of spec structs — pure data, no behavior.
//   - Consumers (formats) query specs via `find_spec<Spec>(anns)` /
//     `has_spec_v<Spec, Tuple>`; unknown specs are ignored.
//
// Resolution rule (applies across the whole library):
//
//   member annotation
//     > wrapper-inherent annotation (field type's `inherent_annotations`)
//       > type annotation (`annotate<type<EnclosingT>{}>`)
//         > format fallback
//
// This same rule governs plain annotations, scalar presentations
// (§5.3.7), and every format-level dispatch. Callers go through
// `effective_annotations<EnclosingT, FieldType, MemberPtr>` (see
// wrappers.hpp), never `annotate` directly, so the three sources are
// merged and conflict-checked in one place.
//
// Key API:
//
//   psio::annotate<&T::field>          → std::tuple<Specs...>
//   psio::annotate<psio::type<T>{}>   → std::tuple<Specs...>
//   psio::find_spec<Spec>(anns)        → std::optional<Spec>
//   psio::has_spec_v<Spec, Tuple>      → bool
//
// Type-level cap accessors:
//
//   psio::is_dwnc_v<T>                 → bool
//      true when the type carries `definition_will_not_change`.
//   psio::all_dwnc_v<T>                → bool
//      true when T and every transitively reachable inner type are
//      DWNC; primitives, strings, and std containers descend into
//      element types.
//   psio::max_fields_v<T>              → optional<size_t>
//   psio::max_dynamic_data_v<T>        → optional<size_t>
//      explicit cap from `maxFields(N)` / `maxDynamicData(N)` keywords
//      in PSIO_REFLECT.
//   psio::effective_max_fields_v<T>    → optional<size_t>
//      min(explicit cap, reflect<T>::member_count) — cap wins downward.
//   psio::effective_max_dynamic_v<T>   → optional<size_t>  (in max_size.hpp)
//      min(explicit cap, max_encoded_size<T>) — cap wins downward.
//
// Cap enforcement helpers (every format consumer reuses these):
//
//   psio::enforce_max_dynamic_cap<T>(total, "fmt")
//      throws codec_exception when total > T's cap.
//   psio::check_max_dynamic_cap<T>(total, "fmt")
//      noexcept variant returning codec_status — for validate paths.

#include <psio/error.hpp>
#include <psio/reflect.hpp>

#include <boost/preprocessor.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psio {

   // ── type<T> wrapper ────────────────────────────────────────────────────
   //
   // C++ NTTPs can't accept bare types, so type<T>{} serves as the
   // value-level stand-in for "annotate the type T itself" (rather than
   // one of its fields). Structural (default ==) so it's usable as an
   // NTTP under C++20's generalized rules.
   template <typename T>
   struct type
   {
      constexpr bool operator==(const type&) const noexcept = default;
   };

   // ── Spec categories ────────────────────────────────────────────────────
   //
   // Static specs influence code generation and emit no runtime work
   // once dispatch is resolved. Runtime specs are checked at validate
   // time. Formats can split the effective-annotation tuple at compile
   // time (`if constexpr` for static, loop for runtime) so `validate()`
   // doesn't pay for specs that are already resolved.
   //
   // Each spec type declares its category as `using spec_category = …`.
   struct static_spec_tag
   {
   };
   struct runtime_spec_tag
   {
   };

   // Fallback trait — specs without an explicit `spec_category` alias
   // default to runtime (safe default: formats treat them as dynamic).
   namespace detail {
      template <typename S, typename = void>
      struct spec_category_of
      {
         using type = runtime_spec_tag;
      };
      template <typename S>
      struct spec_category_of<S, std::void_t<typename S::spec_category>>
      {
         using type = typename S::spec_category;
      };
   }
   template <typename S>
   using spec_category_t = typename detail::spec_category_of<S>::type;

   template <typename S>
   inline constexpr bool is_static_spec_v =
      std::is_same_v<spec_category_t<S>, static_spec_tag>;
   template <typename S>
   inline constexpr bool is_runtime_spec_v =
      std::is_same_v<spec_category_t<S>, runtime_spec_tag>;

   // ── Shape applicability ────────────────────────────────────────────────
   //
   // A spec may declare `using applies_to = shape_set<Shapes...>` — the
   // shapes for which it is meaningful. `effective_annotations` emits a
   // static_assert when a spec is attached to a member whose shape is
   // not in the set. A spec without `applies_to` is treated as
   // applicable to every shape (permissive).
   //
   // Shape tags are the concept names themselves — Primitive, Enum,
   // FixedSequence, VariableSequence, ByteString, Optional, Variant,
   // Bitfield, Record — used only as nominal type tags here, mirrored
   // in shapes.hpp.
   template <typename... ShapeTags>
   struct shape_set
   {
   };

   // Nominal shape-family tags. These mirror the concepts in shapes.hpp
   // and exist at the type level so `applies_to` can name them without
   // dragging in the concept dependency.
   struct PrimitiveShape{};
   struct EnumShape{};
   struct FixedSequenceShape{};
   struct VariableSequenceShape{};
   struct ByteStringShape{};
   struct OptionalShape{};
   struct VariantShape{};
   struct BitfieldShape{};
   struct RecordShape{};

   namespace detail {
      // Shape membership on a (possibly void / non-shape_set) type.
      // The generic form handles `void` — used when a spec has no
      // `applies_to` declaration — by answering `true` (permissive).
      template <typename Shape, typename Set>
      struct shape_in : std::true_type
      {
      };
      template <typename Shape, typename... Shapes>
      struct shape_in<Shape, shape_set<Shapes...>>
         : std::bool_constant<(std::is_same_v<Shape, Shapes> || ...)>
      {
      };

      template <typename S, typename = void>
      struct applies_to_of
      {
         using type = void;  // signals "permissive" — handled above
      };
      template <typename S>
      struct applies_to_of<S, std::void_t<typename S::applies_to>>
      {
         using type = typename S::applies_to;
      };
   }

   template <typename Spec, typename Shape>
   inline constexpr bool spec_applies_to_shape_v =
      detail::shape_in<Shape,
                       typename detail::applies_to_of<Spec>::type>::value;

   // ── Built-in spec structs ──────────────────────────────────────────────
   //
   // Each spec is an aggregate with designated-init-friendly members.
   // Format authors query via `find_spec<SpecType>(annotations)`.

   // Unified length-constraint spec. The SHAPE (not the spec) decides
   // which dimension is being bounded: byte count for ByteStringShape,
   // element count for VariableSequenceShape, bit count for Bitfield.
   //
   // Replaces the pre-consolidation `bytes_spec{.size}` and
   // `max_size_spec{.max}`:
   //   bytes_spec{.size = 48}   ⇒ length_bound{.exact = 48}
   //   max_size_spec{.max = N}  ⇒ length_bound{.max   = N}
   struct length_bound
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<VariableSequenceShape,
                                      ByteStringShape,
                                      BitfieldShape,
                                      FixedSequenceShape>;

      std::optional<std::uint32_t> exact;
      std::optional<std::uint32_t> max;
      std::optional<std::uint32_t> min;

      // Used by the conflict-checker in effective_annotations.
      constexpr bool operator==(const length_bound&) const = default;
   };

   struct utf8_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<ByteStringShape>;

      std::uint32_t max       = 0;   // runtime upper bound; 0 = unbounded
      std::uint32_t field_num = 0;

      constexpr bool operator==(const utf8_spec&) const = default;

      // Semantic validator (design §5.3.3) — invoked by validate_strict
      // on the field's payload bytes (the string contents). Walks the
      // span checking valid UTF-8 byte sequences and (when set) the
      // max byte-length cap.
      [[nodiscard]] static codec_status
      validate(std::span<const char> bytes) noexcept
      {
         // Note: max is captured here as a per-field annotation, but
         // since validate is a static member it can't see the instance.
         // Instance-level max is enforced via length_bound in v3; this
         // static check covers shape-only well-formedness. The const
         // 0 is a sentinel — wire length is whatever the field claims.
         const auto* p   = reinterpret_cast<const unsigned char*>(bytes.data());
         const auto  n   = bytes.size();
         std::size_t i   = 0;
         while (i < n)
         {
            unsigned char b = p[i];
            std::size_t   take;
            if (b < 0x80)
               take = 1;
            else if ((b & 0xE0) == 0xC0)
               take = 2;
            else if ((b & 0xF0) == 0xE0)
               take = 3;
            else if ((b & 0xF8) == 0xF0)
               take = 4;
            else
               return codec_fail("utf8_spec: invalid leading byte",
                                  static_cast<std::uint32_t>(i),
                                  "utf8_spec");
            if (i + take > n)
               return codec_fail("utf8_spec: truncated sequence",
                                  static_cast<std::uint32_t>(i),
                                  "utf8_spec");
            for (std::size_t k = 1; k < take; ++k)
            {
               if ((p[i + k] & 0xC0) != 0x80)
                  return codec_fail("utf8_spec: bad continuation",
                                     static_cast<std::uint32_t>(i + k),
                                     "utf8_spec");
            }
            // Reject overlong + surrogate forms by computing the codepoint
            // and verifying it's in the canonical range for `take`.
            std::uint32_t cp = 0;
            if (take == 1)
               cp = b;
            else if (take == 2)
               cp = ((b & 0x1F) << 6) | (p[i + 1] & 0x3F);
            else if (take == 3)
               cp = ((b & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) |
                    (p[i + 2] & 0x3F);
            else
               cp = ((b & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) |
                    ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
            const std::uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
            if (cp < min_cp[take])
               return codec_fail("utf8_spec: overlong encoding",
                                  static_cast<std::uint32_t>(i),
                                  "utf8_spec");
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
               return codec_fail("utf8_spec: codepoint out of range",
                                  static_cast<std::uint32_t>(i),
                                  "utf8_spec");
            i += take;
         }
         return codec_ok();
      }
   };

   struct hex_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<ByteStringShape>;

      std::uint32_t bytes     = 0;   // source byte count; wire length = 2 * bytes
      std::uint32_t field_num = 0;

      constexpr bool operator==(const hex_spec&) const = default;

      // Semantic validator — every byte must be a hex digit. Length
      // parity (even) is also required since hex strings encode pairs.
      [[nodiscard]] static codec_status
      validate(std::span<const char> bytes) noexcept
      {
         if ((bytes.size() & 1u) != 0u)
            return codec_fail("hex_spec: odd-length hex string", 0,
                               "hex_spec");
         for (std::size_t i = 0; i < bytes.size(); ++i)
         {
            const auto c = static_cast<unsigned char>(bytes[i]);
            const bool ok = (c >= '0' && c <= '9') ||
                            (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
            if (!ok)
               return codec_fail("hex_spec: non-hex character",
                                  static_cast<std::uint32_t>(i),
                                  "hex_spec");
         }
         return codec_ok();
      }
   };

   struct sorted_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<VariableSequenceShape, FixedSequenceShape>;

      bool unique    = false;
      bool ascending = true;

      constexpr bool operator==(const sorted_spec&) const = default;
   };

   // element_spec<S> — apply spec S to every element of an annotated
   // sequence. Lets users say "every element of this vec<string> must
   // be valid UTF-8" without inventing a wrapper type:
   //   PSIO_FIELD_ATTRS(T, names,
   //       psio::element_spec{psio::utf8_spec{}})
   //
   // The child spec's own applies_to must be compatible with the
   // sequence's element type — checked indirectly by the validator
   // dispatching to ChildSpec::validate(element_span).
   template <typename ChildSpec>
   struct element_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<VariableSequenceShape, FixedSequenceShape>;

      ChildSpec child{};

      constexpr bool operator==(const element_spec&) const = default;
   };
   template <typename C>
   element_spec(C) -> element_spec<C>;

   template <typename T>
   struct default_value_spec
   {
      using spec_category = static_spec_tag;
      T value;

      constexpr bool operator==(const default_value_spec&) const = default;
   };
   template <typename T>
   default_value_spec(T) -> default_value_spec<T>;

   template <auto Factory>
   struct default_factory_spec
   {
      using spec_category = static_spec_tag;

      constexpr bool operator==(const default_factory_spec&) const = default;
   };

   struct field_num_spec
   {
      using spec_category = static_spec_tag;
      std::uint32_t value = 0;

      constexpr bool operator==(const field_num_spec&) const = default;
   };

   struct skip_spec
   {
      using spec_category = static_spec_tag;
      bool value = true;

      constexpr bool operator==(const skip_spec&) const = default;
   };

   // Type-level only; spelled out, never abbreviated, so the reader
   // sees the wire-stability commitment they're making.
   struct definition_will_not_change
   {
      using spec_category = static_spec_tag;

      constexpr bool operator==(const definition_will_not_change&) const = default;
   };

   // Type-level cap on declared field count.  Lets formats pick a
   // smaller slot index / vtable-width than the (potentially large)
   // member_count would imply.  The schema author asserts the cap;
   // validators reject decoded inputs whose declared field count
   // exceeds it.
   //
   // Co-exists with `definition_will_not_change`: use both when you
   // want a fixed layout AND a tighter slot-table.
   struct max_fields_spec
   {
      using spec_category = static_spec_tag;
      std::size_t value{};
      constexpr bool operator==(const max_fields_spec&) const = default;
   };

   // Type-level cap on the total dynamic / heap region (in bytes) a
   // single encoded value may occupy across all variable-sized fields.
   //
   // Lets formats with adaptive offset widths (pssz, frac, psch, pjson)
   // pick u8 / u16 instead of u32 when:
   //   1. `definition_will_not_change` is present (no extension hdr),
   //   2. all transitively reachable types are also DWNC,
   //   3. the cap is small enough for the chosen width.
   //
   // The author asserts the cap; encoders that exceed it must throw
   // (silent widening defeats predictable wire size).  Decoders /
   // validators reject input whose dynamic region exceeds the cap.
   struct max_dynamic_data_spec
   {
      using spec_category = static_spec_tag;
      std::size_t value{};
      constexpr bool operator==(const max_dynamic_data_spec&) const = default;
   };

   // ── WIT attribute specs ────────────────────────────────────────────
   //
   // These specs map onto the WIT @-attribute vocabulary at schema
   // emission time.  v1 maintained a parallel `type_attrs_of<T>()` /
   // `member_attrs_of<MP>()` ADL registry; v3 unifies them into the
   // single annotation channel by registering the same information
   // as specs.  The WIT generator (wit_gen.hpp) reads them from
   // `effective_annotations_for<T, F, &T::F>` and emits `@final`,
   // `@sorted`, etc.

   // @final — the record is closed; receivers reject trailing unknown
   // fields.  Distinct from definition_will_not_change (which is a
   // wire-format optimisation choosing Struct over Object); @final
   // is a semantic contract about future schema evolution.
   struct final_spec
   {
      using spec_category = static_spec_tag;
      using applies_to    = shape_set<RecordShape>;
      constexpr bool operator==(const final_spec&) const = default;
   };

   // @canonical — there is exactly one admissible wire form for this
   // type.  Decoders may reject any other byte sequence that round-
   // trips to the same value.
   struct canonical_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const canonical_spec&) const = default;
   };

   // @unique-keys — the keys (or elements, for sets) are unique.
   // sorted_spec.unique covers the same logical claim but only on
   // sequence types; this version applies to map-shaped types and
   // any future associative containers.
   struct unique_keys_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const unique_keys_spec&) const = default;
   };

   // @flags — emit this enum-shaped type as WIT `flags` (bitset)
   // rather than `enum` (single-discriminant choice).
   struct flags_spec
   {
      using spec_category = static_spec_tag;
      using applies_to    = shape_set<EnumShape>;
      constexpr bool operator==(const flags_spec&) const = default;
   };

   // @padding — this member is reserved/padding and should not be
   // exposed in schema output (parsers preserve the slot for binary
   // compatibility but generators skip it from the API surface).
   struct padding_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const padding_spec&) const = default;
   };

   // @since(version="X.Y.Z") — earliest version this item appears in.
   // Stored as a runtime string to keep the annotation expression
   // ergonomic; constexpr-string templating would require a custom
   // FixedString and complicate user-side instantiation.
   struct since_spec
   {
      using spec_category = runtime_spec_tag;
      const char* version = "";
      constexpr bool operator==(const since_spec&) const = default;
   };

   // @unstable(feature="name") — guarded by a feature flag.
   struct unstable_spec
   {
      using spec_category = runtime_spec_tag;
      const char* feature = "";
      constexpr bool operator==(const unstable_spec&) const = default;
   };

   // @deprecated(version="X.Y.Z") — version this item is deprecated as of.
   struct deprecated_spec
   {
      using spec_category = runtime_spec_tag;
      const char* version = "";
      constexpr bool operator==(const deprecated_spec&) const = default;
   };

   // ── Presentation tag types (§5.3.7) ────────────────────────────────────
   //
   // Nominal tag structs used by `as_spec<Tag>` annotations and as the
   // second template parameter of `psio::adapter<T, Tag>`. Any
   // namespace can add more tags; the library pre-ships the common
   // blockchain/protocol ones. These are separate from the broad
   // categories (binary_category / text_category) in adapter.hpp —
   // specific tags let a member-level annotation pick among a type's
   // named adapters, not just its default for a category.
   struct hex_tag
   {
   };
   struct base58_tag
   {
   };
   struct decimal_tag
   {
   };
   struct rfc3339_tag
   {
   };
   struct unix_us_tag
   {
   };
   struct unix_ms_tag
   {
   };

   // Convenience instances for succinct `as<Tag>` callsites.
   inline constexpr hex_tag     hex{};
   inline constexpr base58_tag  base58{};
   inline constexpr decimal_tag decimal{};
   inline constexpr rfc3339_tag rfc3339{};
   inline constexpr unix_us_tag unix_us{};
   inline constexpr unix_ms_tag unix_ms{};

   // ── as_spec — member-level adapter override ───────────────────────────
   //
   // Attached via `psio::annotate<&T::field> = std::tuple{as<hex_tag>}`,
   // this tells format walkers to use `adapter<FieldType, hex_tag>` for
   // THIS field, regardless of the format's category-level default
   // (§5.3.7 member-beats-type precedence).
   template <typename Tag>
   struct as_spec
   {
      using spec_category = static_spec_tag;
      using adapter_tag   = Tag;

      constexpr bool operator==(const as_spec&) const = default;
   };

   // `psio::as<psio::hex_tag>` spelling for annotate tuples.
   // The short alias `psio::as<psio::hex>` doesn't quite work
   // because the template parameter is a TYPE, not a value — users
   // write either `as<hex_tag>` or desugar to `as_spec<hex_tag>{}`
   // directly.
   template <typename Tag>
   inline constexpr as_spec<Tag> as{};

   // ── Convenience factory templates ──────────────────────────────────────
   //
   // Nested `spec` namespace so short names (`bytes`, `utf8`, …) stay
   // free for the storage free functions in the surrounding psio
   // namespace (`psio::bytes(buf)` returns the byte span of a buffer).

   namespace spec {

      // Length-constraint factories — all three desugar to length_bound.
      template <std::uint32_t N>
      inline constexpr length_bound bytes{.exact = N};   // exact wire length

      template <std::uint32_t N>
      inline constexpr length_bound max_size{.max = N};  // upper bound

      template <std::uint32_t N>
      inline constexpr length_bound min_size{.min = N};  // lower bound

      template <std::uint32_t N>
      inline constexpr utf8_spec utf8{.max = N};

      template <std::uint32_t N>
      inline constexpr hex_spec hex{.bytes = N};

      template <std::uint32_t N>
      inline constexpr field_num_spec field{.value = N};

      inline constexpr sorted_spec sorted{};
      inline constexpr sorted_spec sorted_set{.unique = true};

      inline constexpr skip_spec skip{};

      template <auto V>
      inline constexpr default_value_spec<decltype(V)> default_val{V};

      inline constexpr definition_will_not_change definition_will_not_change_v{};

   }  // namespace spec

   // ── Annotation trait ───────────────────────────────────────────────────
   //
   // The primary template is an empty tuple. Users specialize for each
   // field/type they want to annotate; specializations hold a tuple of
   // spec values.

   template <auto X>
   inline constexpr auto annotate = std::tuple<>{};

   // ── find_spec helper ───────────────────────────────────────────────────
   //
   // Format-author-facing query. Returns the spec value if present in
   // the tuple, else nullopt. Spec types are expected to be unique
   // within a given annotation tuple.

   template <typename Wanted, typename... Ts>
   constexpr std::optional<Wanted>
   find_spec(const std::tuple<Ts...>& anns) noexcept
   {
      if constexpr ((std::is_same_v<std::remove_cvref_t<Ts>, Wanted> || ...))
         return std::get<Wanted>(anns);
      else
         return std::nullopt;
   }

   // ── has_spec — compile-time "is this spec present?" ───────────────────
   template <typename Wanted, typename Tuple>
   inline constexpr bool has_spec_v = false;

   template <typename Wanted, typename... Ts>
   inline constexpr bool has_spec_v<Wanted, std::tuple<Ts...>> =
       (std::is_same_v<std::remove_cvref_t<Ts>, Wanted> || ...);

   // ── is_dwnc — "does this type carry definition_will_not_change?" ──────
   //
   // Formats consult this to opt into wire-format shortcuts that are
   // only safe when the user has promised the type's layout won't
   // change (no new fields appended, no field types widened). Today
   // pssz uses it to skip the u{W} fixed_size header; frac uses it to
   // skip the u16 fixed_region header; similar fast paths in other
   // formats follow the same pattern.
   template <typename T>
   inline constexpr bool is_dwnc_v = has_spec_v<
      ::psio::definition_will_not_change,
      std::remove_cvref_t<decltype(annotate<type<T>{}>)>>;

   // ── all_dwnc_v — transitive DWNC across reflected members ─────────────
   //
   // Format consumers use this to certify precondition #2 from the
   // type-level-cap design:
   //   "all transitively reachable types are DWNC"
   //
   // A type qualifies as transitively DWNC when:
   //   - it is itself layout-stable (primitives, std::string, std::byte
   //     spans — which have no extension points), OR
   //   - it carries `definition_will_not_change` AND every member type
   //     is itself transitively DWNC.
   //
   // Container element types (std::vector<E>, std::array<E,N>,
   // std::optional<E>, std::variant<Es...>, std::tuple<Es...>,
   // std::pair<A,B>) recurse into the inner types — their wire layouts
   // are stable in psio's formats; only the contained T can introduce
   // extension headers.
   //
   // Non-Reflected non-container types not enumerated below default to
   // false, biased toward conservative width selection.

   namespace detail::dwnc
   {
      template <typename T>
      struct is_std_vector_ : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector_<std::vector<E, A>> : std::true_type
      {
         using inner = E;
      };

      template <typename T>
      struct is_std_array_ : std::false_type {};
      template <typename E, std::size_t N>
      struct is_std_array_<std::array<E, N>> : std::true_type
      {
         using inner = E;
      };

      template <typename T>
      struct is_std_optional_ : std::false_type {};
      template <typename E>
      struct is_std_optional_<std::optional<E>> : std::true_type
      {
         using inner = E;
      };

      template <typename T>
      struct is_std_variant_ : std::false_type {};
      template <typename... Es>
      struct is_std_variant_<std::variant<Es...>> : std::true_type {};

      template <typename T>
      struct is_std_tuple_ : std::false_type {};
      template <typename... Es>
      struct is_std_tuple_<std::tuple<Es...>> : std::true_type {};

      template <typename T>
      struct is_std_pair_ : std::false_type {};
      template <typename A, typename B>
      struct is_std_pair_<std::pair<A, B>> : std::true_type {};

      template <typename T>
      struct is_std_string_ : std::false_type {};
      template <typename Char, typename Tr, typename A>
      struct is_std_string_<std::basic_string<Char, Tr, A>> : std::true_type {};

      template <typename T>
      consteval bool transitive() noexcept;  // forward

      template <typename Variant, std::size_t... Is>
      consteval bool variant_all(std::index_sequence<Is...>) noexcept
      {
         return (transitive<std::variant_alternative_t<Is, Variant>>() && ...);
      }

      template <typename Tuple, std::size_t... Is>
      consteval bool tuple_all(std::index_sequence<Is...>) noexcept
      {
         return (transitive<std::tuple_element_t<Is, Tuple>>() && ...);
      }

      template <typename T, std::size_t... Is>
      consteval bool record_members(std::index_sequence<Is...>) noexcept
      {
         return (transitive<typename reflect<T>::template member_type<Is>>()
                 && ...);
      }

      template <typename T>
      consteval bool transitive() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_arithmetic_v<U> ||
                       std::is_same_v<U, bool>  ||
                       std::is_same_v<U, std::byte> ||
                       std::is_enum_v<U>)
            return true;
         else if constexpr (is_std_string_<U>::value)
            return true;
         else if constexpr (is_std_vector_<U>::value)
            return transitive<typename is_std_vector_<U>::inner>();
         else if constexpr (is_std_array_<U>::value)
            return transitive<typename is_std_array_<U>::inner>();
         else if constexpr (is_std_optional_<U>::value)
            return transitive<typename is_std_optional_<U>::inner>();
         else if constexpr (is_std_variant_<U>::value)
            return variant_all<U>(
               std::make_index_sequence<std::variant_size_v<U>>{});
         else if constexpr (is_std_tuple_<U>::value)
            return tuple_all<U>(
               std::make_index_sequence<std::tuple_size_v<U>>{});
         else if constexpr (is_std_pair_<U>::value)
            return transitive<typename U::first_type>() &&
                   transitive<typename U::second_type>();
         else if constexpr (::psio::Reflected<U>)
            return is_dwnc_v<U> &&
                   record_members<U>(
                      std::make_index_sequence<reflect<U>::member_count>{});
         else
            return false;
      }
   }  // namespace detail::dwnc

   template <typename T>
   inline constexpr bool all_dwnc_v = detail::dwnc::transitive<T>();

   // ── max_fields_v / max_dynamic_data_v ─────────────────────────────────
   //
   // Format-author-facing accessors for the two type-level caps.  Return
   // std::nullopt when the type didn't declare the cap, letting formats
   // fall back to a width-conservative default.

   template <typename T>
   inline constexpr std::optional<std::size_t> max_fields_v = []
   {
      using TT = std::remove_cvref_t<decltype(annotate<type<T>{}>)>;
      if constexpr (has_spec_v<max_fields_spec, TT>)
         return std::optional<std::size_t>{
            std::get<max_fields_spec>(annotate<type<T>{}>).value};
      else
         return std::optional<std::size_t>{};
   }();

   template <typename T>
   inline constexpr std::optional<std::size_t> max_dynamic_data_v = []
   {
      using TT = std::remove_cvref_t<decltype(annotate<type<T>{}>)>;
      if constexpr (has_spec_v<max_dynamic_data_spec, TT>)
         return std::optional<std::size_t>{
            std::get<max_dynamic_data_spec>(annotate<type<T>{}>).value};
      else
         return std::optional<std::size_t>{};
   }();

   // ── effective_max_fields_v ────────────────────────────────────────────
   //
   // Composes an explicit `maxFields(N)` cap with the reflected
   // `member_count`.  When both are present, the cap WINS DOWNWARD only
   // — it can be tighter than member_count but never looser, since
   // overriding upward defeats the purpose of an upper-bound assertion.
   //
   // Returns std::nullopt only for non-Reflected types where neither a
   // member count nor a cap exists; format consumers fall back to the
   // format-conservative default in that case.

   template <typename T>
   inline constexpr std::optional<std::size_t> effective_max_fields_v = []
   {
      constexpr auto cap = max_fields_v<T>;
      if constexpr (::psio::Reflected<T>)
      {
         constexpr std::size_t mc = reflect<T>::member_count;
         if constexpr (cap.has_value())
            return std::optional<std::size_t>{
               (*cap < mc) ? *cap : mc};
         else
            return std::optional<std::size_t>{mc};
      }
      else
      {
         return cap;  // nullopt unless explicitly capped
      }
   }();

   // `effective_max_dynamic_v<T>` lives in psio/max_size.hpp because it
   // composes the explicit cap with the per-field `length_bound`-
   // derived bound from `max_encoded_size<T>`.  Putting it there keeps
   // the annotate.hpp → max_size.hpp dependency one-way.

   // ── Shared cap-enforcement helpers ────────────────────────────────────
   //
   // Every format whose validate / validate_or_throw / encode hooks
   // need to enforce `maxDynamicData(N)` reuses these.  Keeping the
   // logic in one place ensures consistent error messages and behavior
   // across pssz / frac / psch / pjson / bin / borsh / bincode / ssz /
   // wit / avro / msgpack / protobuf / capnp / flatbuf / json / bson.
   //
   // Semantics (per-format may layer additional checks on top):
   //   - cap absent → no-op (return ok / do nothing).
   //   - cap present, total > cap → throw / return error.
   //   - cap present, total ≤ cap → ok.

   template <typename T>
   inline void enforce_max_dynamic_cap(std::size_t total,
                                        std::string_view format_name)
   {
      constexpr auto cap = max_dynamic_data_v<T>;
      if constexpr (cap.has_value())
      {
         if (total > *cap)
            throw codec_exception{codec_error{
               "encoded size exceeds maxDynamicData cap",
               static_cast<std::uint32_t>(total), format_name}};
      }
   }

   template <typename T>
   inline codec_status check_max_dynamic_cap(
      std::size_t total, std::string_view format_name) noexcept
   {
      constexpr auto cap = max_dynamic_data_v<T>;
      if constexpr (cap.has_value())
      {
         if (total > *cap)
            return codec_status{codec_error{
               "encoded size exceeds maxDynamicData cap",
               static_cast<std::uint32_t>(total), format_name}};
      }
      return codec_status{};
   }

}  // namespace psio

// ── Spec helpers (terse names for use inside annotation expressions) ────
//
// These are the surface that v2 design § 5.3.5 Form 1 calls for:
// `attr(items, max<255> | field<3>)` instead of
// `attr(items, length_bound{.max=255} | field_num_spec{.value=3})`.
// Each helper is a `static_spec_tag` / `runtime_spec_tag` value (not a
// new spec type) — they emit the same struct the underlying spec
// expects, so format code that already reads `length_bound` /
// `field_num_spec` / etc. needs no changes.
//
// Inside the `attr(...)` macro body, `using namespace ::psio;` brings
// these into scope so the user can write `max<255>` rather than
// `psio::max<255>`. Outside that scope, fully-qualified `psio::max<N>`
// is the spelling.

namespace psio {

   template <std::uint32_t N>
   inline constexpr length_bound max{.max = N};

   template <std::uint32_t N>
   inline constexpr length_bound exact{.exact = N};
   // (Note: an alias `bytes<N>` would be ideal here as a near-synonym
   // of `exact<N>` for byte-array contexts, but `psio::bytes` is
   // already a free function defined in psio/buffer.hpp.  Use
   // `exact<N>` for both.)

   template <std::uint32_t N>
   inline constexpr field_num_spec field{.value = N};

   template <std::uint32_t N>
   inline constexpr utf8_spec utf8{.max = N};

   inline constexpr sorted_spec sorted{};
   inline constexpr sorted_spec unique{.unique = true};
   inline constexpr sorted_spec sorted_unique{.unique = true};

   inline constexpr definition_will_not_change dwnc{};

   // WIT attribute helpers — used inside attr(field, ...) expressions.
   inline constexpr final_spec        final_v{};        // `final` is a C++ keyword
   inline constexpr canonical_spec    canonical{};
   inline constexpr unique_keys_spec  unique_keys{};
   inline constexpr flags_spec        flags{};
   inline constexpr padding_spec      padding{};

   // `since`, `unstable`, `deprecated` carry string args, so they're
   // constructed at the call site rather than pre-instantiated:
   //   attr(field, since_spec{.version = "0.2.0"})
   // (No bare `since`/etc. helpers because they need an argument.)

}  // namespace psio

// ── Composition helpers ─────────────────────────────────────────────────

namespace psio {

   // Single-spec → 1-tuple adapter, for uniform composition.
   template <typename A>
   constexpr auto to_spec_tuple(const A& a) noexcept
   {
      return std::tuple<A>{a};
   }
   template <typename... Ts>
   constexpr auto to_spec_tuple(const std::tuple<Ts...>& t) noexcept
   {
      return t;
   }

   // `spec | spec` composes into a tuple. Only enabled for non-arithmetic
   // types so it does not compete with the bitwise `|` on integers.
   template <typename A, typename B>
      requires(!std::is_arithmetic_v<A> && !std::is_arithmetic_v<B>)
   constexpr auto operator|(const A& a, const B& b) noexcept
   {
      return std::tuple_cat(to_spec_tuple(a), to_spec_tuple(b));
   }

}  // namespace psio

// ── PSIO_ATTRS macro (§5.3.5 Form 2) ─────────────────────────────────────
//
// Adds annotations to an already-reflected type without touching the
// reflection macro. Supports both field-level and type-level annotations
// via parenthesized entries. Each entry is `(field_name, spec_expr)` or
// `(type, spec_expr)` — the literal identifier `type` selects the
// type-level slot.
//
//   PSIO_ATTRS(Validator,
//      (pubkey,            psio::length_bound{.exact = 48}),
//      (effective_balance, psio::field_num_spec{.value = 3}),
//      (type,              psio::definition_will_not_change{}))
//
// Each (name, specs) pair expands to one namespace-scope specialization
// of `psio::annotate<X>`. `specs` may be a single spec value or a
// `spec | spec | …` composition; it is wrapped via `to_spec_tuple` so
// both shapes work uniformly.
//
// Also provided for callers that prefer one-entry-per-invocation:
//
//   PSIO_FIELD_ATTRS(Validator, pubkey, psio::length_bound{.exact = 48})
//   PSIO_TYPE_ATTRS(Validator, psio::definition_will_not_change{})

#define PSIO_FIELD_ATTRS(TYPE, FIELD, ...)                         \
   template <>                                                      \
   inline constexpr auto ::psio::annotate<&TYPE::FIELD> =          \
      ::psio::to_spec_tuple(__VA_ARGS__);

#define PSIO_TYPE_ATTRS(TYPE, ...)                                 \
   template <>                                                      \
   inline constexpr auto ::psio::annotate<::psio::type<TYPE>{}> = \
      ::psio::to_spec_tuple(__VA_ARGS__);

// Multi-field form — each entry is `(field_name, (spec_expr))`. The
// inner parens isolate any commas in the spec expression (such as
// those inside designated initializers like `{.max = 128, .min = 4}`)
// from the preprocessor's argument splitter. For type-level
// annotations, call `PSIO_TYPE_ATTRS(TYPE, …)` directly.
#define PSIO_ATTRS_ENTRY_(r, TYPE, ENTRY)                          \
   PSIO_FIELD_ATTRS(TYPE, BOOST_PP_TUPLE_ELEM(0, ENTRY),           \
                     BOOST_PP_REMOVE_PARENS(                        \
                        BOOST_PP_TUPLE_ELEM(1, ENTRY)))

#define PSIO_ATTRS(TYPE, ...)                                      \
   BOOST_PP_SEQ_FOR_EACH(PSIO_ATTRS_ENTRY_, TYPE,                  \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))
