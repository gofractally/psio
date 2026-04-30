#pragma once
//
// psio/shapes.hpp — Layer 1 shape concepts.
//
// Shapes describe *what the data looks like* — primitive, fixed-size
// sequence, variable-length sequence, optional, variant, bitfield,
// record — independent of which C++ type carries it. Format authors
// dispatch on shapes; user types participate by matching the concepts
// below (or, for non-std types, by specializing the is_* traits).
//
// Semantic variation within a shape (e.g. is this `std::string` text,
// opaque bytes, or hex-on-wire?) lives on reflection annotations in
// psio/annotate.hpp — NOT as separate shape concepts.

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace psio {

   // ── Primitive ───────────────────────────────────────────────────────────
   //
   // Numeric types + bool. Enums are NOT Primitive — they're Enum.
   template <typename T>
   concept Primitive =
       std::same_as<std::remove_cv_t<T>, bool> ||
       std::same_as<std::remove_cv_t<T>, std::uint8_t>  ||
       std::same_as<std::remove_cv_t<T>, std::uint16_t> ||
       std::same_as<std::remove_cv_t<T>, std::uint32_t> ||
       std::same_as<std::remove_cv_t<T>, std::uint64_t> ||
       std::same_as<std::remove_cv_t<T>, std::int8_t>   ||
       std::same_as<std::remove_cv_t<T>, std::int16_t>  ||
       std::same_as<std::remove_cv_t<T>, std::int32_t>  ||
       std::same_as<std::remove_cv_t<T>, std::int64_t>  ||
       std::same_as<std::remove_cv_t<T>, float>         ||
       std::same_as<std::remove_cv_t<T>, double>;

   // ── Enum ────────────────────────────────────────────────────────────────
   template <typename T>
   concept Enum = std::is_enum_v<T>;

   // ── FixedSequence ──────────────────────────────────────────────────────
   //
   // Compile-time-sized sequence. std::array, C arrays, and user types
   // that specialize is_fixed_sequence.
   template <typename T>
   struct is_fixed_sequence : std::false_type
   {
   };

   template <typename T>
   constexpr std::size_t fixed_size_of_v = 0;  // overridden by specs below

   template <typename T, std::size_t N>
   struct is_fixed_sequence<std::array<T, N>> : std::true_type
   {
   };
   template <typename T, std::size_t N>
   constexpr std::size_t fixed_size_of_v<std::array<T, N>> = N;

   template <typename T, std::size_t N>
   struct is_fixed_sequence<T[N]> : std::true_type
   {
   };
   template <typename T, std::size_t N>
   constexpr std::size_t fixed_size_of_v<T[N]> = N;

   template <typename T>
   concept FixedSequence = is_fixed_sequence<std::remove_cvref_t<T>>::value;

   // Associated element trait.
   template <typename T>
   struct element_of
   {
      using type = void;
   };
   template <typename T, std::size_t N>
   struct element_of<std::array<T, N>>
   {
      using type = T;
   };
   template <typename T, std::size_t N>
   struct element_of<T[N]>
   {
      using type = T;
   };
   template <typename T>
   struct element_of<std::vector<T>>
   {
      using type = T;
   };
   template <typename T>
   using element_of_t = typename element_of<std::remove_cvref_t<T>>::type;

   // ── VariableSequence ───────────────────────────────────────────────────
   //
   // Runtime-sized sequence. Must expose size(), begin()/end(), and a
   // way to insert (push_back OR resize + indexed write). Excludes
   // std::string (which is a text shape and goes through annotations)
   // and FixedSequence types.
   template <typename T>
   concept HasSizeAndIterators = requires(T t) {
      { t.size() } -> std::convertible_to<std::size_t>;
      t.begin();
      t.end();
   };

   template <typename T>
   concept HasPushBack = requires(T t, element_of_t<T> e) { t.push_back(e); };

   template <typename T>
   concept VariableSequence =
       HasSizeAndIterators<T> &&
       !FixedSequence<T> &&
       !std::same_as<std::remove_cvref_t<T>, std::string> &&
       HasPushBack<T>;

   // ── Optional ───────────────────────────────────────────────────────────
   template <typename T>
   struct is_optional : std::false_type
   {
   };
   template <typename T>
   struct is_optional<std::optional<T>> : std::true_type
   {
   };

   template <typename T>
   concept Optional = is_optional<std::remove_cvref_t<T>>::value;

   // ── Variant ────────────────────────────────────────────────────────────
   template <typename T>
   struct is_variant : std::false_type
   {
   };
   template <typename... Ts>
   struct is_variant<std::variant<Ts...>> : std::true_type
   {
   };

   template <typename T>
   concept Variant = is_variant<std::remove_cvref_t<T>>::value;

   // ── Bitfield ───────────────────────────────────────────────────────────
   //
   // Types representing a sequence of bits with logical length. Library
   // types psio::bitvector<N> / psio::bitlist<N> (landed in Phase 2
   // via wrappers.hpp) plus std::bitset<N>. User-defined bit types
   // specialize is_bitfield.
   template <typename T>
   struct is_bitfield : std::false_type
   {
   };

   template <typename T>
   concept Bitfield = is_bitfield<std::remove_cvref_t<T>>::value;

   // ── Record ─────────────────────────────────────────────────────────────
   //
   // Types with a psio_reflect_helper overload visible via ADL —
   // i.e. types that have been declared with PSIO_REFLECT (or, in the
   // future, C++26 auto-reflection).
   template <typename T>
   concept Record = reflect<T>::is_reflected;

   // ── Any-shape gate ─────────────────────────────────────────────────────
   //
   // True iff T matches at least one shape — used by codec dispatch as
   // the top-level precondition. Strings are treated as variable-byte
   // shapes via annotation, so they fall through to a default path in
   // the codec layer rather than being a Shape themselves here.
   template <typename T>
   concept Shape = Primitive<T> || Enum<T> || FixedSequence<T> ||
                   VariableSequence<T> || Optional<T> || Variant<T> ||
                   Bitfield<T> || Record<T> ||
                   std::same_as<std::remove_cvref_t<T>, std::string>;

   // ── shape_tag_of<T> ─────────────────────────────────────────────────────
   //
   // Maps a C++ type to one of the nominal shape tags declared in
   // annotate.hpp (PrimitiveShape, VariableSequenceShape, …). Used by
   // `effective_annotations_for` to static_assert each spec against the
   // field's shape via `spec_applies_to_shape_v`.
   //
   // Order of the if-constexpr cascade matters: std::string is a
   // VariableSequence, but we label it ByteStringShape because the
   // byte-oriented specs (length_bound, utf8_spec, hex_spec) target it
   // specifically.
   namespace detail {
      // Associative-container detector — std::map / std::set /
      // std::unordered_map / std::unordered_set.  These are runtime-
      // sized sequences (of pairs or keys), but they don't satisfy the
      // VariableSequence concept because they expose insert/emplace
      // instead of push_back.  Their inherent annotations
      // (sorted_spec, unique_keys_spec) target VariableSequenceShape,
      // so we route them there explicitly rather than introducing a
      // separate MapShape that every existing spec would have to add.
      template <typename T>
      struct is_map_like : std::false_type {};
      template <typename K, typename V, typename C, typename A>
      struct is_map_like<std::map<K, V, C, A>> : std::true_type {};
      template <typename K, typename C, typename A>
      struct is_map_like<std::set<K, C, A>> : std::true_type {};
      template <typename K, typename V, typename H, typename E, typename A>
      struct is_map_like<std::unordered_map<K, V, H, E, A>> : std::true_type {};
      template <typename K, typename H, typename E, typename A>
      struct is_map_like<std::unordered_set<K, H, E, A>> : std::true_type {};

      template <typename T>
      constexpr auto compute_shape_tag_of() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::same_as<U, std::string>)
            return ::psio::ByteStringShape{};
         else if constexpr (Primitive<U>)
            return ::psio::PrimitiveShape{};
         else if constexpr (Enum<U>)
            return ::psio::EnumShape{};
         else if constexpr (is_bitfield<U>::value)
            return ::psio::BitfieldShape{};
         else if constexpr (FixedSequence<U>)
            return ::psio::FixedSequenceShape{};
         else if constexpr (is_optional<U>::value)
            return ::psio::OptionalShape{};
         else if constexpr (is_variant<U>::value)
            return ::psio::VariantShape{};
         else if constexpr (VariableSequence<U>)
            return ::psio::VariableSequenceShape{};
         else if constexpr (is_map_like<U>::value)
            return ::psio::VariableSequenceShape{};
         else if constexpr (Record<U>)
            return ::psio::RecordShape{};
         else
            return ::psio::PrimitiveShape{};  // permissive default
      }
   }

   template <typename T>
   using shape_tag_of = decltype(detail::compute_shape_tag_of<T>());

}  // namespace psio
