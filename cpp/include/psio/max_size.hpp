#pragma once
//
// psio/max_size.hpp — two compile-time numbers of a type, given its shape
// and its fields' length_bound annotations. Both are
// `std::optional<std::size_t>`, nullopt for "no finite number".
//
//   psio::inferred_bound<T>()     THE INFERRED BOUND (docs/pssz-spec.md 3.2):
//       the number offset widths are chosen from. It never reads T's own
//       maxDynamicData cap, and a member whose type has a cap counts as
//       that cap in full. It is an upper bound of T's encoding only when T
//       has no cap: a cap in a wider class makes T's header and slots wider
//       than this number counted them.
//         - some(N) when annotations and declared child budgets provide a
//           finite conservative bound, including container bookkeeping
//         - nullopt when any sub-field is unbounded or the arithmetic
//           overflows
//
//   psio::max_encoded_size<T>()   THE MAXIMUM ENCODED SIZE
//       (docs/pssz-spec.md 3.7): an upper bound of every psio::pssz
//       encoding of T. Every header, slot and offset table is counted at
//       the width the encoder really uses (the width class of the cap for a
//       capped record, of min(inferred bound, position cap) for a list /
//       tuple beneath one), a member counts as its own maximum encoded
//       size, and a capped type is min(cap, that sum) — the cap alone when
//       something inside is unbounded.
//
// With no cap anywhere in T the two are the same number.
//
// Field-level bounds participate through psio's standard annotation
// merge: effective_annotations_for<Rec, F, MemberPtr> gathers
// member > wrapper > type specs, and the walk below reads length_bound
// out of that merged tuple. Member beats wrapper beats type, so a field
// can tighten a type-level default.
//
// The bound is the PSSZ one (docs/pssz-spec.md 3.2) and is computed bottom-up:
// the bound of a type is a closed formula over the bounds of the types it
// holds, never over its own offset width.
//
//   - A list / array of variable elements, a map and a set have an offset
//     table of one slot per element. With at most N elements of at most M
//     bytes each the table is the narrowest of 1, 2, 4 bytes that addresses
//     the whole value: the least W with N * (W + M) <= 2^(8W) - 1 (W = 4 when
//     neither 1 nor 2 does), and the bound is N * (W + M) for that W
//     (`fit` below).
//   - A record and a tuple count their header and each variable member's
//     slot as 4 bytes.
//
// `auto_pssz_width_v<T>` is the width class of the inferred bound (1 through 255,
// 2 through 65535, else 4) unless T has an explicit maxDynamicData budget,
// which then rules. For a list that class is the W the formula chose.

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/shapes.hpp>
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/wire_type.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   namespace detail::max_size_impl {

      // ── Shape classifiers local to this walk ──────────────────────────────

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector<std::vector<E, A>> : std::true_type {};

      template <typename T>
      struct is_std_array : std::false_type {};
      template <typename E, std::size_t N>
      struct is_std_array<std::array<E, N>> : std::true_type
      {
         static constexpr std::size_t n = N;
         using element_type             = E;
      };

      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename U>
      struct is_std_optional<std::optional<U>> : std::true_type
      {
         using inner_type = U;
      };

      template <typename T>
      struct is_std_variant : std::false_type {};
      template <typename... Ts>
      struct is_std_variant<std::variant<Ts...>> : std::true_type {};

      // std::tuple and std::pair: the positional record.
      template <typename T>
      struct is_tuple_like : std::false_type {};
      template <typename... Ts>
      struct is_tuple_like<std::tuple<Ts...>> : std::true_type {};
      template <typename A, typename B>
      struct is_tuple_like<std::pair<A, B>> : std::true_type {};

      // utf8_string<N> carries its byte bound in the type; utf8_string<> is
      // unbounded, exactly like std::string.
      template <typename T>
      struct is_utf8_string : std::false_type {};
      template <std::size_t N>
      struct is_utf8_string<::psio::utf8_string<N>> : std::true_type
      {
         static constexpr std::size_t limit = N;
      };

      // Packed bits: bitvector<N> is ceil(N / 8) fixed bytes; bitlist<N>
      // holds at most N bits and a delimiter bit.
      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio::bitvector<N>> : std::true_type
      {
         static constexpr std::size_t bits = N;
      };
      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio::bitlist<N>> : std::true_type
      {
         static constexpr std::size_t bits = N;
      };

      // Extract the effective count cap from a length_bound: exact wins,
      // then max. min alone does not bound the upper side.
      constexpr std::optional<std::uint32_t>
      bound_max(const length_bound& lb) noexcept
      {
         if (lb.exact)
            return lb.exact;
         if (lb.max)
            return lb.max;
         return std::nullopt;
      }

      // These are conservative PSSZ payload bounds (see the header comment).
      // An enclosing record adds its own field slot separately. Declared child
      // budgets are reserved in full so compatible child growth cannot silently
      // change an inferred parent width.
      using bound = std::optional<std::size_t>;
      constexpr bound add(bound a, bound b) noexcept
      {
         if (!a || !b || *b > std::numeric_limits<std::size_t>::max() - *a)
            return std::nullopt;
         return *a + *b;
      }
      constexpr bound multiply(std::size_t count, bound value) noexcept
      {
         if (count == 0) return 0;
         if (!value || *value > std::numeric_limits<std::size_t>::max() / count)
            return std::nullopt;
         return count * *value;
      }
      // The bound of a container that writes `slots` offset-width words (a
      // list's or array's offset table; a record's header and member slots)
      // and whose other bytes are bounded by `rest`: slots * W + rest for
      // the least W in {1, 2, 4} whose total fits a W-byte offset, W = 4
      // when neither 1 nor 2 does. The three candidates are closed formulas
      // of `slots` and `rest`, which do not depend on W: nothing here is
      // circular. The width class of the result is the W chosen.
      constexpr bound fit(std::size_t slots, bound rest) noexcept
      {
         if (!rest) return std::nullopt;
         if (auto one = add(multiply(slots, bound{1}), rest); one && *one <= 0xFFu)
            return one;
         if (auto two = add(multiply(slots, bound{2}), rest); two && *two <= 0xFFFFu)
            return two;
         return add(multiply(slots, bound{4}), rest);
      }
      // A record's / tuple's header and member slots are counted 4 wide.
      constexpr bound record_fit(std::size_t slots, bound rest) noexcept
      {
         return fit(slots, rest);
      }

      // What the walk computes. The default is the INFERRED BOUND. With
      // `actual` it is the MAXIMUM ENCODED SIZE: the same walk, but each
      // type's words are counted at its real width instead of the one `fit`
      // would pick, a capped type is min(cap, its sum) instead of the cap
      // in full, and `cap` is the position cap (docs/pssz-spec.md 3.6: the
      // explicit cap of the nearest enclosing record) that narrows the
      // lists, arrays, maps, sets and plain tuples beneath a capped record.
      // (The position cap changes the result only for a list / tuple of
      // capped members beneath a capped record of a lower width class; in
      // every other case a narrowed list already exceeds the record's cap
      // and the record's maximum is the cap.)
      struct mode
      {
         bool                       actual = false;
         std::optional<std::size_t> cap    = std::nullopt;
      };
      // The width class of a bound: 4 for none.
      constexpr std::size_t width_class(bound b) noexcept
      {
         return !b ? 4 : *b <= 0xFFu ? 1 : *b <= 0xFFFFu ? 2 : 4;
      }
      // The smaller of two bounds, none counting as infinite.
      constexpr bound least(bound a, bound b) noexcept
      {
         if (!a) return b;
         if (!b) return a;
         return *a < *b ? a : b;
      }
      // `slots` words of `width` bytes and `rest` other bytes.
      constexpr bound at_width(std::size_t slots, std::size_t width, bound rest) noexcept
      {
         return add(multiply(slots, bound{width}), rest);
      }
      template <typename T>
      consteval bound max_bare(mode m = {}) noexcept;

      // Fixed payloads occupy their bytes inline. Adapter payloads and unknown
      // shapes are conservatively treated as variable for bound bookkeeping.
      template <typename T>
      consteval bool fixed_payload() noexcept
      {
         using U = ::psio::wire_type_t<T>;  // what T is encoded as
         if constexpr (::psio::has_adapter_v<U, ::psio::binary_category>)
            return false;
         else if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U> ||
                            ::psio::TimeValue<U> ||
                            std::is_same_v<U, ::psio::uint128> ||
                            std::is_same_v<U, ::psio::int128> ||
                            std::is_same_v<U, ::psio::uint256> || is_bitvector<U>::value)
            return true;
         else if constexpr (is_std_array<U>::value)
            return fixed_payload<typename is_std_array<U>::element_type>();
         else if constexpr (::psio::is_transparent_v<U>)
            return fixed_payload<::psio::transparent_inner_t<U>>();
         else if constexpr (::psio::Record<U>)
         {
            if constexpr (!::psio::is_dwnc_v<U>) return false;
            else
            {
               using R = ::psio::reflect<U>;
               return []<std::size_t... Is>(std::index_sequence<Is...>) {
                  return ((fixed_payload<typename R::template member_type<Is>>() &&
                           !::psio::has_binary_as_override_v<
                              ::psio::wire_type_t<typename R::template member_type<Is>>,
                              typename ::psio::effective_annotations_for<
                                 U, typename R::template member_type<Is>,
                                 R::template member_pointer<Is>>::value_t>) && ...);
               }(std::make_index_sequence<R::member_count>{});
            }
         }
         else return false;
      }

      template <typename T>
      consteval bound child_payload(mode m = {}) noexcept
      {
         constexpr auto budget = ::psio::max_dynamic_data_v<std::remove_cvref_t<T>>;
         if constexpr (budget.has_value())
            return m.actual ? least(budget, max_bare<T>(m)) : bound{budget};
         else return max_bare<T>(m);
      }

      // The count cap of a string / list / map / set / nested position: the
      // position's own length_bound, else the one declared for the type
      // itself (PSIO_TYPE_ATTRS on the member's type).
      template <typename F, typename Annots>
      consteval std::optional<std::uint32_t> position_count(const Annots& annots) noexcept
      {
         if (auto lb = ::psio::find_spec<::psio::length_bound>(annots))
            return bound_max(*lb);
         if constexpr (std::is_class_v<F>)
            if (auto lb = ::psio::find_spec<::psio::length_bound>(
                   ::psio::annotate<::psio::type<F>{}>))
               return bound_max(*lb);
         return std::nullopt;
      }

      // The bound of a value of type FieldType at a position that carries
      // `annots`: a member's effective annotations, or what element(...) /
      // keys(...) / values(...) / at<I>(...) state for a position below one
      // (detail/annotation_tree.hpp). An empty tuple is the bare type.
      //
      // A wrapper type's own bound (psio::bounded<T, N>, psio::utf8_string<N>)
      // is its N; a length_bound written over it is enforced by the codecs
      // but does not change the bound (and so not the width either).
      template <typename FieldType, typename AnnotsTuple>
      consteval bound max_field_with_annots(AnnotsTuple annots, mode m = {}) noexcept;

      // `count` elements of type E at a position whose elements carry
      // `annots`: a list, an array, the entries of a map or set. Fixed-size
      // elements are inline; variable ones have the offset table.
      template <typename E, typename Annots>
      consteval bound sequence_bound(std::size_t count, const Annots& annots,
                                     mode m = {}) noexcept
      {
         const auto elements = multiply(count, max_field_with_annots<E>(annots, m));
         if constexpr (fixed_payload<E>()) return elements;
         else
         {
            if (!m.actual) return fit(count, elements);
            // The table is as wide as the encoder writes it: the width
            // class of min(inferred bound of this position, position cap).
            const auto inferred =
               fit(count, multiply(count, max_field_with_annots<E>(annots)));
            return at_width(count, width_class(least(inferred, m.cap)), elements);
         }
      }

      // `elements` is the mode of the elements. With `width` the header
      // and slots are counted that wide (the maximum encoded size);
      // without, at the width `fit` picks (the inferred bound).
      template <typename T, typename Annots, std::size_t... Is>
      consteval bound tuple_bound(const Annots& annots, std::index_sequence<Is...>,
                                  mode                       elements = {},
                                  std::optional<std::size_t> width    = std::nullopt) noexcept
      {
         PSIO_REQUIRE_TUPLE_SIZE(sizeof...(Is));
         // The header, and one slot per variable element.
         std::size_t slots = 1;
         bound       rest  = 0;
         ((slots += fixed_payload<std::remove_const_t<std::tuple_element_t<Is, T>>>() ? 0 : 1,
           rest = add(rest, max_field_with_annots<std::remove_const_t<std::tuple_element_t<Is, T>>>(
                               ::psio::detail::ann::at_specs<Is>(annots), elements))),
          ...);
         return width ? at_width(slots, *width, rest) : record_fit(slots, rest);
      }
      // An annotated tuple (a method's argument tuple, reflect.hpp): a
      // record of its own. The inferred bound is that of its elements under
      // their annotations — its cap is not in it, as for a capped record.
      // The maximum encoded size counts the header and slots at the width
      // class of the cap (else of the inferred bound), gives the elements
      // the cap as their position cap, and is at most the cap.
      template <typename U>
      consteval bound annotated_tuple_bound(mode m) noexcept
      {
         using T        = typename U::tuple_type;
         const auto seq = std::make_index_sequence<std::tuple_size_v<T>>{};
         const auto inferred = tuple_bound<T>(U::psio_annotations, seq);
         if (!m.actual) return inferred;
         const bound own = ::psio::max_dynamic_data_v<U>;
         const auto  sum = tuple_bound<T>(U::psio_annotations, seq, mode{true, own},
                                          width_class(own ? own : inferred));
         return own ? least(own, sum) : sum;
      }
      template <typename T, typename Annots, std::size_t... Is>
      consteval bound variant_bound(const Annots& annots, std::index_sequence<Is...>,
                                    mode m = {}) noexcept
      {
         // `widest` is never reset to empty. Emptying an engaged optional and
         // then copying it is not a constant expression for clang with
         // libstdc++ (the copy reads the destroyed value), and every use of
         // this function is at compile time.
         bound widest      = 0;
         bool  unbounded   = false;
         auto  alternative = [&](bound payload) {
            if (!payload)
               unbounded = true;
            else if (*payload > *widest)
               widest = payload;
         };
         (alternative(max_field_with_annots<std::variant_alternative_t<Is, T>>(
             ::psio::detail::ann::at_specs<Is>(annots), m)),
          ...);
         if (unbounded)
            return std::nullopt;
         return add(widest, 1);
      }

      template <typename FieldType, typename AnnotsTuple>
      consteval bound max_field_with_annots(AnnotsTuple annots, mode m) noexcept
      {
         using F = std::remove_cvref_t<FieldType>;
         namespace ann = ::psio::detail::ann;
         if constexpr (::psio::has_binary_as_override_v<F, AnnotsTuple>)
            return std::nullopt; // opaque alternate presentation
         else if constexpr (::psio::max_dynamic_data_v<F>.has_value())
            // A capped type: the cap in full for the inferred bound; never
            // more than the cap for the maximum encoded size.
            return m.actual ? least(::psio::max_dynamic_data_v<F>, max_bare<F>(m))
                            : bound{::psio::max_dynamic_data_v<F>};
         else if constexpr (std::is_same_v<F, std::string> || ::psio::detail::is_nested_v<F>)
         {
            // Bytes: a string's, or a nested value's (which has no bound of
            // its own).
            auto count = position_count<F>(annots);
            return count ? bound{*count} : bound{};
         }
         else if constexpr (is_utf8_string<F>::value)
         {
            if constexpr (F::is_bounded) return is_utf8_string<F>::limit;
            else
            {
               auto count = position_count<F>(annots);
               return count ? bound{*count} : bound{};
            }
         }
         else if constexpr (is_std_vector<F>::value)
         {
            auto count = position_count<F>(annots);
            if (!count) return std::nullopt;
            return sequence_bound<typename F::value_type>(*count, ann::element_specs(annots), m);
         }
         else if constexpr (ann::is_bounded<F>::value)
            return sequence_bound<typename F::value_type>(F::max_size_value,
                                                          ann::element_specs(annots), m);
         else if constexpr (::psio::detail::is_assoc_v<F>)
         {
            // A map / set is the list of its entries / elements.
            auto count = position_count<F>(annots);
            if (!count) return std::nullopt;
            return sequence_bound<::psio::detail::assoc_entry_t<F>>(
               *count, ann::entry_specs<F>(annots), m);
         }
         else if constexpr (is_std_optional<F>::value)
            // One selector is conservative for fixed nonempty payloads, whose
            // existing optional representation needs no selector.
            return add(max_field_with_annots<typename F::value_type>(annots, m), 1);
         else if constexpr (is_std_array<F>::value)
            // N elements, and — when they are not inline fixed payloads —
            // the offset table (docs/pssz-spec.md 5.2).
            return sequence_bound<typename is_std_array<F>::element_type>(
               is_std_array<F>::n, ann::element_specs(annots), m);
         else if constexpr (is_std_variant<F>::value)
            // One selector byte, then the widest alternative's payload
            // (docs/pssz-spec.md 3.2). Unbounded if any alternative is.
            return variant_bound<F>(annots,
                                    std::make_index_sequence<std::variant_size_v<F>>{}, m);
         else if constexpr (requires { typename F::psio_annotated_tuple; })
            // A tuple carrying its own element annotations (a method's
            // argument tuple, reflect.hpp): those, not the position's.
            return annotated_tuple_bound<F>(m);
         else if constexpr (is_tuple_like<F>::value)
            // A tuple (a pair is the 2-tuple) is the extensible positional
            // record of its elements: the header, then per element its
            // payload and, when it is not an inline fixed payload, an
            // offset slot. An element is bounded by its type or by the
            // at<I>(...) written for it; a bare string or list element is
            // unbounded and so is the tuple.
         {
            const auto seq      = std::make_index_sequence<std::tuple_size_v<F>>{};
            const auto inferred = tuple_bound<F>(annots, seq);
            if (!m.actual) return inferred;
            // A plain tuple takes the width of its position: the width
            // class of min(its inferred bound, the position cap).
            return tuple_bound<F>(annots, seq, m, width_class(least(inferred, m.cap)));
         }
         else return max_bare<F>(m);
      }

      template <typename T>
      consteval bound max_bare(mode m) noexcept
      {
         using U = ::psio::wire_type_t<T>;  // what T is encoded as
         if constexpr (::psio::has_adapter_v<U, ::psio::binary_category>)
            return std::nullopt;
         else if constexpr (std::is_same_v<U, bool>) return 1;
         else if constexpr (std::is_same_v<U, ::psio::uint256>) return 32;
         else if constexpr (std::is_same_v<U, ::psio::uint128> ||
                            std::is_same_v<U, ::psio::int128>) return 16;
         else if constexpr (std::is_enum_v<U>) return sizeof(std::underlying_type_t<U>);
         else if constexpr (::psio::TimeValue<U>) return sizeof(::psio::time_rep_t<U>);
         else if constexpr (std::is_arithmetic_v<U>) return sizeof(U);
         // docs/pssz-spec.md 3.2: Bitvector(N) is its ceil(N / 8) bytes;
         // Bitlist(N) is bounded by ceil(N / 8) + 1 (its bits and the
         // delimiter).
         else if constexpr (is_bitvector<U>::value) return (is_bitvector<U>::bits + 7) / 8;
         else if constexpr (is_bitlist<U>::value) return (is_bitlist<U>::bits + 7) / 8 + 1;
         else if constexpr (::psio::is_transparent_v<U>)
         {
            // The member's bound, its declared budget included; with the
            // bounds the type gives its member (transparent.hpp), the bound
            // of that bounded type.
            if constexpr (::psio::transparent_member_checked_v<U>)
               return max_field_with_annots<::psio::transparent_inner_t<U>>(
                  ::psio::transparent_member_annotations<U>::value, m);
            else
               return child_payload<::psio::transparent_inner_t<U>>(m);
         }
         else if constexpr (::psio::Record<U>)
         {
            using R = ::psio::reflect<U>;
            // The header (extensible records), and one slot per variable
            // member.
            // A record is a boundary for the position cap: its members are
            // beneath its own cap, or beneath none.
            const bound own   = ::psio::max_dynamic_data_v<U>;
            const mode  inner = m.actual ? mode{true, own} : mode{};
            std::size_t slots = ::psio::is_dwnc_v<U> ? 0 : 1;
            bound       rest  = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto field = [&]<std::size_t I>() {
                  // (As the type the member is encoded as: wire_type.hpp.)
                  using F = ::psio::wire_type_t<typename R::template member_type<I>>;
                  using eff = ::psio::effective_annotations_for<
                     U, F, R::template member_pointer<I>>;
                  if constexpr (!fixed_payload<F>() ||
                                ::psio::has_binary_as_override_v<F, typename eff::value_t>)
                     ++slots;
                  rest = add(rest, max_field_with_annots<F>(eff::value, inner));
               };
               (field.template operator()<Is>(), ...);
            }(std::make_index_sequence<R::member_count>{});
            if (!m.actual) return record_fit(slots, rest);
            // The maximum encoded size: the header and slots at the
            // record's real width — the width class of its cap, else of
            // its inferred bound — and never more than the cap.
            const auto sum =
               at_width(slots, width_class(own ? own : max_bare<U>()), rest);
            return own ? least(own, sum) : sum;
         }
         else if constexpr (requires { typename U::psio_annotated_tuple; })
            // An annotated tuple (a method's argument tuple). Its cap is
            // not in its inferred bound (docs/pssz-spec.md 3.2), as for a
            // capped record; the cap rules the width and is what the tuple
            // contributes to the inferred bound of a type that holds it
            // (max_field_with_annots).
            return annotated_tuple_bound<U>(m);
         else if constexpr (std::is_same_v<U, std::string> || ::psio::detail::is_assoc_v<U> ||
                            is_std_vector<U>::value || is_std_optional<U>::value ||
                            is_utf8_string<U>::value || is_std_array<U>::value ||
                            ::psio::detail::ann::is_bounded<U>::value ||
                            is_std_variant<U>::value || is_tuple_like<U>::value ||
                            requires { typename U::psio_annotated_tuple; })
            // Containers, strings and positional records: the bare position
            // (no annotation; a wrapper's own bound is in its type).
            return max_field_with_annots<U>(std::tuple<>{}, m);
         else return std::nullopt;
      }

   }  // namespace detail::max_size_impl

   // ── inferred_bound ────────────────────────────────────────────────────
   //
   // THE INFERRED BOUND of T (docs/pssz-spec.md 3.2): the number an offset
   // width is chosen from when T has no explicit cap, or nullopt when any
   // sub-field is unbounded. It does not depend on T's own maxDynamicData;
   // a child record with a declared budget reserves that budget in full,
   // not only its present fields. For a type with no cap it is also an
   // upper bound of the encoding; for a capped type it is NOT (the cap can
   // put the type in a wider class than this number) — that is
   // max_encoded_size below.
   template <typename T>
   consteval std::optional<std::size_t> inferred_bound() noexcept
   {
      return detail::max_size_impl::max_bare<T>();
   }

   // ── max_encoded_size ──────────────────────────────────────────────────
   //
   // THE MAXIMUM ENCODED SIZE of T (docs/pssz-spec.md 3.7): no psio::pssz
   // encoding of a T is longer, or nullopt when there is no finite maximum
   // (an unbounded member and no cap). Offsets are counted at the widths
   // the encoder uses, members at their own maximum encoded size, and a
   // capped type is min(cap, sum). It keeps the two conservatisms of the
   // inferred bound (one selector byte for every optional, ceil(N / 8) + 1
   // for a bitlist), so it can exceed the largest value by those bytes.
   // The explicit-width profiles (pssz8 / pssz16 / pssz32) are not covered.
   template <typename T>
   consteval std::optional<std::size_t> max_encoded_size() noexcept
   {
      using namespace detail::max_size_impl;
      using U = ::psio::wire_type_t<T>;  // what T is encoded as
      const auto sum = max_bare<U>(mode{true, std::nullopt});
      if constexpr (max_dynamic_data_v<U>.has_value())
         return least(max_dynamic_data_v<U>, sum);
      else
         return sum;
   }

   // ── effective_max_dynamic_v ───────────────────────────────────────────
   //
   // docs/pssz-spec.md 3.1 / 16: the number a type's automatic offset width
   // is the width class of.
   //
   //   explicit maxDynamicData(N)  → N. An explicit cap always rules: it is
   //                                 the type's lifetime width promise and is
   //                                 not reduced to the bound inferred from
   //                                 the fields the type has today
   //   no cap                      → inferred_bound<T>(); nullopt when a
   //                                 field is unbounded
   //
   // There is no min(explicit cap, inferred bound). auto_pssz_width<T>() is
   // the width class of this value (4 for nullopt). It is not the largest
   // encoding: that is max_encoded_size<T>().
   template <typename T>
   inline constexpr std::optional<std::size_t> effective_max_dynamic_v = []
   {
      constexpr auto cap = max_dynamic_data_v<T>;
      if constexpr (cap.has_value())
         return cap;
      else
         return inferred_bound<T>();
   }();

}  // namespace psio
