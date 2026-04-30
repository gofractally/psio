#pragma once
//
// psio/wrappers.hpp — rich wrapper types (opt-in invasive alternative
// to annotations). Per design doc § 5.3.6.
//
// Users who want invariants baked into the type system (rejecting
// out-of-bound values at construction, maintaining sort order on
// insertion, etc.) pick wrappers. Users who want clean types with
// attributes on the reflection pick annotations. Both produce
// identical wire output; the library statically detects conflicts
// when both are present for the same field.
//
// This header also provides:
//   - psio::inherent_annotations<T> — trait exposing each wrapper's
//     implicit annotation tuple so the codec layer can unify wrapper
//     + explicit annotations.
//   - psio::effective_annotations<FieldType, MemberPtr> — merge with
//     compile-time consistency check.
//
// Phase 2 ships the core wrappers (bounded, utf8_string, byte_array,
// bitvector, bitlist) with just enough surface for the codec layer
// to consume them. Rich methods (sorting, binary search, etc.) arrive
// in later phases where needed.

#include <psio/annotate.hpp>
#include <psio/shapes.hpp>

// shape_tag_of<T> lives in shapes.hpp

#include <array>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio {

   // ── bounded<T, N> ──────────────────────────────────────────────────────
   //
   // std::vector-like container with compile-time upper bound N. Throws
   // on push_back / construct when the bound would be violated (if
   // exceptions enabled); otherwise rejects via codec_status at the
   // wire boundary.
   template <typename T, std::size_t N>
   class bounded
   {
      std::vector<T> data_;
    public:
      using value_type     = T;
      using iterator       = typename std::vector<T>::iterator;
      using const_iterator = typename std::vector<T>::const_iterator;

      static constexpr std::size_t max_size_value = N;

      bounded() = default;
      explicit bounded(std::vector<T> v) : data_(std::move(v))
      {
         enforce_bound_();
      }
      bounded(std::initializer_list<T> il) : data_(il) { enforce_bound_(); }

      std::size_t size() const noexcept { return data_.size(); }
      bool        empty() const noexcept { return data_.empty(); }

      iterator       begin() noexcept        { return data_.begin(); }
      iterator       end() noexcept          { return data_.end(); }
      const_iterator begin() const noexcept  { return data_.begin(); }
      const_iterator end() const noexcept    { return data_.end(); }

      T&       operator[](std::size_t i) noexcept        { return data_[i]; }
      const T& operator[](std::size_t i) const noexcept  { return data_[i]; }

      void push_back(const T& v)
      {
         if (data_.size() >= N)
            throw std::length_error("psio::bounded overflow");
         data_.push_back(v);
      }
      void push_back(T&& v)
      {
         if (data_.size() >= N)
            throw std::length_error("psio::bounded overflow");
         data_.push_back(std::move(v));
      }

      // Raw data access for memcpy fast paths in format code.
      const std::vector<T>& storage() const noexcept { return data_; }
      std::vector<T>&       storage() noexcept       { return data_; }

    private:
      void enforce_bound_() const
      {
         if (data_.size() > N)
            throw std::length_error("psio::bounded overflow on construct");
      }
   };

   // ── utf8_string<N> ─────────────────────────────────────────────────────
   //
   // std::string-like with compile-time max byte length N. Constructor
   // validates UTF-8 well-formedness and bound; same for assignment.
   // Access returns std::string_view.
   template <std::size_t N>
   class utf8_string
   {
      std::string data_;
    public:
      static constexpr std::size_t max_size_value = N;

      utf8_string() = default;
      explicit utf8_string(std::string s) : data_(std::move(s))
      {
         enforce_bound_();
         // UTF-8 well-formedness is validated by `validate_strict`
         // on the wire side — construction here only checks the
         // length bound, matching std::string semantics.
      }

      std::size_t size() const noexcept { return data_.size(); }
      const char* data() const noexcept { return data_.data(); }
      const std::string& storage() const noexcept { return data_; }

    private:
      void enforce_bound_() const
      {
         if (data_.size() > N)
            throw std::length_error("psio::utf8_string overflow");
      }
   };

   // ── byte_array<N> ──────────────────────────────────────────────────────
   //
   // Fixed-length N-byte opaque sequence. Thin wrapper over
   // std::array<std::uint8_t, N> so the codec layer can distinguish
   // "opaque bytes of size N" from "array of integers of size N".
   template <std::size_t N>
   class byte_array
   {
      std::array<std::uint8_t, N> data_{};
    public:
      static constexpr std::size_t size_value = N;

      byte_array() = default;
      explicit byte_array(std::array<std::uint8_t, N> a) : data_(a) {}

      std::size_t         size() const noexcept { return N; }
      const std::uint8_t* data() const noexcept { return data_.data(); }
      std::uint8_t*       data() noexcept       { return data_.data(); }

      std::uint8_t& operator[](std::size_t i) noexcept       { return data_[i]; }
      const std::uint8_t& operator[](std::size_t i) const noexcept { return data_[i]; }

      const std::array<std::uint8_t, N>& storage() const noexcept { return data_; }
      std::array<std::uint8_t, N>&       storage() noexcept       { return data_; }
   };

   // ── bitvector<N> / bitlist<N> ──────────────────────────────────────────
   //
   // Minimal bit-packed types the Bitfield concept matches. Byte layout
   // is LSB-first to match SSZ convention (byte 0 bit 0 is bit 0 of
   // the logical sequence). Phase 2 ships the storage; codec logic
   // lands with the format phases.
   template <std::size_t N>
   class bitvector
   {
      static constexpr std::size_t byte_count = (N + 7) / 8;
      std::array<std::uint8_t, byte_count> bytes_{};
    public:
      static constexpr std::size_t size_value  = N;
      static constexpr std::size_t bytes_value = byte_count;

      bool test(std::size_t i) const noexcept
      {
         return (bytes_[i >> 3] >> (i & 7)) & 1;
      }
      void set(std::size_t i, bool v) noexcept
      {
         if (v) bytes_[i >> 3] |= std::uint8_t(1u << (i & 7));
         else   bytes_[i >> 3] &= std::uint8_t(~(1u << (i & 7)));
      }

      const std::uint8_t* data() const noexcept { return bytes_.data(); }
      std::uint8_t*       data() noexcept       { return bytes_.data(); }
      std::size_t         size_bytes() const noexcept { return byte_count; }
   };

   template <std::size_t MaxN>
   class bitlist
   {
      std::vector<bool> bits_;
    public:
      static constexpr std::size_t max_size_value = MaxN;

      std::size_t size() const noexcept { return bits_.size(); }
      bool        test(std::size_t i) const noexcept { return bits_[i]; }
      void        set(std::size_t i, bool v) { bits_[i] = v; }
      void        push_back(bool v)
      {
         if (bits_.size() >= MaxN)
            throw std::length_error("psio::bitlist overflow");
         bits_.push_back(v);
      }

      const std::vector<bool>& storage() const noexcept { return bits_; }
      std::vector<bool>&       storage() noexcept       { return bits_; }

      // Bits packed LSB-first into bytes — the SSZ convention (bit i
      // of the logical sequence lives at byte i/8, bit i%8 of that
      // byte). Does not include the SSZ delimiter bit; callers that
      // want SSZ's "bit_count + delimiter" layout add it on top.
      std::vector<std::uint8_t> bytes() const
      {
         std::vector<std::uint8_t> out((bits_.size() + 7) / 8, 0);
         for (std::size_t i = 0; i < bits_.size(); ++i)
            if (bits_[i])
               out[i >> 3] |= std::uint8_t(1u << (i & 7));
         return out;
      }
   };

   // ── Shape specializations for the new wrappers ────────────────────────

   template <typename T, std::size_t N>
   struct element_of<bounded<T, N>>
   {
      using type = T;
   };

   // byte_array<N> / utf8_string<N> are byte-oriented fixed-length —
   // register as FixedSequence so shape-aware specs (length_bound etc.)
   // find them.
   template <std::size_t N>
   struct is_fixed_sequence<byte_array<N>> : std::true_type
   {
   };
   template <std::size_t N>
   constexpr std::size_t fixed_size_of_v<byte_array<N>> = N;

   template <std::size_t N>
   struct is_fixed_sequence<utf8_string<N>> : std::true_type
   {
   };
   template <std::size_t N>
   constexpr std::size_t fixed_size_of_v<utf8_string<N>> = N;

   template <std::size_t N>
   struct is_bitfield<bitvector<N>> : std::true_type {};
   template <std::size_t N>
   struct is_bitfield<bitlist<N>>   : std::true_type {};

   // ── inherent_annotations trait ─────────────────────────────────────────
   //
   // Each wrapper exposes its implicit annotation tuple. `effective_annotations`
   // merges this with the type-level and member-level explicit annotations
   // below.

   template <typename T>
   struct inherent_annotations
   {
      static constexpr auto value = std::tuple<>{};
   };

   template <typename T, std::size_t N>
   struct inherent_annotations<bounded<T, N>>
   {
      static constexpr auto value = std::tuple{
         length_bound{.max = static_cast<std::uint32_t>(N)}};
   };

   template <std::size_t N>
   struct inherent_annotations<utf8_string<N>>
   {
      static constexpr auto value = std::tuple{
         length_bound{.max = static_cast<std::uint32_t>(N)},
         utf8_spec{.max = static_cast<std::uint32_t>(N)}};
   };

   template <std::size_t N>
   struct inherent_annotations<byte_array<N>>
   {
      static constexpr auto value = std::tuple{
         length_bound{.exact = static_cast<std::uint32_t>(N)}};
   };

   // ── stdlib associative containers carry sorted/unique semantics ────
   //
   // Map-shaped types in the stdlib have intrinsic ordering and key-
   // uniqueness invariants.  Ported from psio v1's
   // `attributes.hpp`, where these registrations lived in the parallel
   // type_attrs_of<T> registry.  v3 routes them through the unified
   // annotation channel.

   template <typename K, typename V, typename C, typename A>
   struct inherent_annotations<std::map<K, V, C, A>>
   {
      static constexpr auto value =
         std::tuple{sorted_spec{.unique = true}, unique_keys_spec{}};
   };

   template <typename K, typename C, typename A>
   struct inherent_annotations<std::set<K, C, A>>
   {
      static constexpr auto value =
         std::tuple{sorted_spec{.unique = true}, unique_keys_spec{}};
   };

   template <typename K, typename V, typename H, typename E, typename A>
   struct inherent_annotations<std::unordered_map<K, V, H, E, A>>
   {
      // Keyed but unordered.  Uniqueness still applies.
      static constexpr auto value = std::tuple{unique_keys_spec{}};
   };

   template <typename K, typename H, typename E, typename A>
   struct inherent_annotations<std::unordered_set<K, H, E, A>>
   {
      static constexpr auto value = std::tuple{unique_keys_spec{}};
   };

   template <typename A>
   struct inherent_annotations<
      std::basic_string<char8_t, std::char_traits<char8_t>, A>>
   {
      // u8string is UTF-8 by definition.
      static constexpr auto value = std::tuple{utf8_spec{}};
   };

   // ── effective_annotations — 3-way merge with precedence ──
   //
   // Sources, in decreasing precedence:
   //   1. Member annotation        — psio::annotate<MemberPtr>
   //   2. Wrapper-inherent        — psio::inherent_annotations<FieldType>
   //   3. Type-level annotation    — psio::annotate<psio::type<EnclosingT>{}>
   //
   // Rules when the same spec type appears in multiple sources:
   //   - Higher-precedence wins; lower-precedence duplicates are dropped.
   //   - Two LOWER-precedence sources (wrapper + type) with the same spec
   //     type and DIFFERENT values → static_assert (ambiguous).
   //   - Two lower-precedence sources with the same spec and SAME value
   //     → deduplicated silently.
   //   - A member-level spec silently overrides any lower-precedence source
   //     (explicit override by the user — no diagnostic).

   namespace detail {

      // True iff A appears (by type, after cvref-strip) in the pack Bs.
      template <typename A, typename... Bs>
      inline constexpr bool in_pack_v =
         (std::is_same_v<std::remove_cvref_t<A>,
                         std::remove_cvref_t<Bs>> || ...);

      // Get the element of a tuple whose type (after cvref-strip) matches
      // Target. Callers must have already confirmed presence via in_pack_v.
      template <typename Target, typename... Ts>
      constexpr const Target& tuple_get_as(const std::tuple<Ts...>& t) noexcept
      {
         return std::get<Target>(t);
      }

      // Subtract tuple B from tuple A at the type level: drop any element
      // of A whose type appears in B.
      template <typename A, typename B>
      struct tuple_minus;

      template <typename... As, typename... Bs>
      struct tuple_minus<std::tuple<As...>, std::tuple<Bs...>>
      {
       private:
         template <typename Elem>
         static constexpr auto keep_or_drop(const Elem& e)
         {
            if constexpr (in_pack_v<Elem, Bs...>)
               return std::tuple<>{};
            else
               return std::tuple<Elem>{e};
         }

       public:
         static constexpr auto apply(const std::tuple<As...>& a) noexcept
         {
            return std::apply(
               [](const auto&... xs)
               { return std::tuple_cat(keep_or_drop(xs)...); },
               a);
         }
      };

      // Assert that every spec common to A and B holds the same value.
      // A and B are both lower-precedence sources (wrapper + type); a
      // real disagreement between them is ambiguity the user must
      // resolve. (Member-level overrides are checked separately by
      // dropping the matched type from lower sources before comparing.)
      template <typename A, typename B>
      struct assert_lower_consistent;

      template <typename... As, typename... Bs>
      struct assert_lower_consistent<std::tuple<As...>, std::tuple<Bs...>>
      {
         static constexpr bool run(const std::tuple<As...>& a,
                                   const std::tuple<Bs...>& b)
         {
            bool ok = true;
            (
               ([&]
                {
                   if constexpr (in_pack_v<As, Bs...>)
                   {
                      using Spec = std::remove_cvref_t<As>;
                      if (!(tuple_get_as<Spec>(a) == tuple_get_as<Spec>(b)))
                         ok = false;
                   }
                }()),
               ...);
            return ok;
         }
      };

      // ── Transparent wrapper unwrap ─────────────────────────────────────
      //
      // Some types are "transparent" w.r.t. annotations: a spec attached
      // to such a field is meaningful for the inner type, not the
      // wrapper itself.  `std::optional<U>` is the first member of this
      // family — saying `length_bound{.max=N}` on `optional<string>`
      // naturally means "the inner string has length ≤ N".  The unwrap
      // is applied recursively so `optional<optional<U>>` resolves to U.
      //
      // Future entries may include `std::expected<T, E>` (forward to T)
      // and `std::variant<T, monostate>` (forward to T when monostate
      // represents absence).  `std::variant<A, B>` with two real
      // alternatives stays opaque — there's no canonical inner type.

      template <typename T>
      struct unwrap_transparent
      {
         using type = T;
      };
      template <typename U>
      struct unwrap_transparent<std::optional<U>>
      {
         using type = typename unwrap_transparent<U>::type;
      };

      // ── Applicability check ────────────────────────────────────────────
      //
      // For each spec in the effective tuple, static_assert that it's
      // applicable to either the field's shape directly OR — when the
      // field is a transparent wrapper — to the inner type's shape.
      // Most specs declare `applies_to = shape_set<...>`; those without
      // it are permissive (apply everywhere).

      template <typename Spec, typename FieldT>
      inline static constexpr bool spec_applies_to_field_v =
         ::psio::spec_applies_to_shape_v<Spec, ::psio::shape_tag_of<FieldT>>
         || ::psio::spec_applies_to_shape_v<
               Spec, ::psio::shape_tag_of<
                        typename unwrap_transparent<FieldT>::type>>;

      template <typename FieldT, typename Tuple>
      struct all_specs_apply;

      template <typename FieldT, typename... Specs>
      struct all_specs_apply<FieldT, std::tuple<Specs...>>
      {
         static constexpr bool value =
            (spec_applies_to_field_v<std::remove_cvref_t<Specs>, FieldT>
             && ...);
      };

   }  // namespace detail

   // Primary: no enclosing-type annotations consulted (e.g. top-level
   // call sites with no record context). Kept for back-compat during
   // the refactor; new format code should use the 3-argument form below.
   template <typename FieldType, auto MemberPtr>
   struct effective_annotations
   {
      using field_t = std::remove_cvref_t<FieldType>;

      static constexpr auto inherent_value =
         inherent_annotations<field_t>::value;
      static constexpr auto member_value = annotate<MemberPtr>;

      using inherent_t = std::remove_cvref_t<decltype(inherent_value)>;
      using member_t   = std::remove_cvref_t<decltype(member_value)>;

      // Member beats wrapper: drop any spec type from inherent that the
      // member supplies, then concatenate.
      static constexpr auto value = std::tuple_cat(
         member_value,
         detail::tuple_minus<inherent_t, member_t>::apply(inherent_value));
   };

   // Three-way form — member > wrapper > type.
   //
   // Consistency rules:
   //   - Member silently overrides wrapper/type for the same spec type.
   //   - Wrapper and type must AGREE on any spec type they both supply
   //     (checked at runtime inside constexpr context; result is a
   //     static_assert).
   template <typename EnclosingT, typename FieldType, auto MemberPtr>
   struct effective_annotations_for
   {
      using field_t = std::remove_cvref_t<FieldType>;

      static constexpr auto member_value   = annotate<MemberPtr>;
      static constexpr auto wrapper_value  = inherent_annotations<field_t>::value;
      static constexpr auto type_value     = annotate<type<EnclosingT>{}>;

      using member_t  = std::remove_cvref_t<decltype(member_value)>;
      using wrapper_t = std::remove_cvref_t<decltype(wrapper_value)>;
      using type_t    = std::remove_cvref_t<decltype(type_value)>;

      // Lower-precedence consistency: wrapper and type must not disagree
      // on any spec they both supply.
      static_assert(
         detail::assert_lower_consistent<wrapper_t, type_t>::run(
            wrapper_value, type_value),
         "psio: wrapper-inherent annotation and type-level annotation "
         "disagree on a spec type. Either fix the type-level annotation "
         "or drop the wrapper. (Member-level specs may still silently "
         "override either source.)");

      // Drop from wrapper any spec type the member supplies; drop from
      // type any spec type either member or wrapper supplies; then
      // concatenate in decreasing precedence.
      static constexpr auto wrapper_minus_member =
         detail::tuple_minus<wrapper_t, member_t>::apply(wrapper_value);
      using wrapper_minus_member_t =
         std::remove_cvref_t<decltype(wrapper_minus_member)>;

      static constexpr auto type_minus_member_wrapper = [] {
         auto step1 =
            detail::tuple_minus<type_t, member_t>::apply(type_value);
         using step1_t = std::remove_cvref_t<decltype(step1)>;
         return detail::tuple_minus<step1_t, wrapper_t>::apply(step1);
      }();

      static constexpr auto value =
         std::tuple_cat(member_value, wrapper_minus_member,
                        type_minus_member_wrapper);

      using value_t = std::remove_cvref_t<decltype(value)>;

      // Applicability check — every spec in the merged tuple must
      // declare that it applies to FieldType's shape (or be permissive
      // by declaring no applies_to set). Catches "max_size on a record"
      // and similar mistakes at annotation time, not at wire time.
      static_assert(
         detail::all_specs_apply<field_t, value_t>::value,
         "psio: a spec in the effective annotations is attached to a "
         "field whose shape is not in the spec's applies_to set. Check "
         "the spec's `applies_to = shape_set<...>` declaration and the "
         "field's shape (e.g. attaching length_bound to a Record is "
         "meaningless).");
   };

   template <typename FieldType, auto MemberPtr>
   inline constexpr auto effective_annotations_v =
      effective_annotations<FieldType, MemberPtr>::value;

   template <typename EnclosingT, typename FieldType, auto MemberPtr>
   inline constexpr auto effective_annotations_for_v =
      effective_annotations_for<EnclosingT, FieldType, MemberPtr>::value;

}  // namespace psio
