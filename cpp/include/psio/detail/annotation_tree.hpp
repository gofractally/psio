#pragma once
//
// psio/detail/annotation_tree.hpp — annotations below the member itself.
//
// Included by psio/wrappers.hpp (after the wrapper types). A member's
// annotation tuple may hold position specs (annotate.hpp):
//
//   element(specs)   every element of a list, an array or a set
//   keys(specs)      every key of a map
//   values(specs)    every mapped value of a map
//   at<I>(specs)     element I of a tuple / pair, alternative I of a variant
//
// and a std::optional is looked through. This header is what every consumer
// shares:
//
//   ann::no_ann, ann::element_ann<A>, ann::entry_ann<T, A>, ann::at_ann<A, I>
//       "carriers": types whose `value` is the annotation tuple of one
//       position, derived from the carrier of the position above. A codec
//       threads a carrier down its type walk (it selects nested offset
//       widths); a position with no annotation is always ann::no_ann, so an
//       unannotated walk instantiates exactly what it did before.
//   ann::check_value(value, specs, ...)
//       the value-side check of a whole tree: every length_bound and utf8
//       at every depth.
//   ann::tree_ok<T, Specs>()
//       the compile-time shape check, with one readable message per misuse.
//
// A wrapper type (psio::bounded<T, N>, psio::utf8_string<N>) states the same
// constraint in the type; codecs handle those in their own type walk. The
// two spellings give the same wire, the same widths and the same schema.

#include <psio/annotate.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/error.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio::detail::ann
{
   // ── Shapes ─────────────────────────────────────────────────────────────
   template <typename T>
   struct is_optional : std::false_type {};
   template <typename U>
   struct is_optional<std::optional<U>> : std::true_type {};

   template <typename T>
   struct is_vector : std::false_type {};
   template <typename E, typename A>
   struct is_vector<std::vector<E, A>> : std::true_type {};

   template <typename T>
   struct is_bounded : std::false_type {};
   template <typename E, std::size_t N>
   struct is_bounded<::psio::bounded<E, N>> : std::true_type {};

   template <typename T>
   struct is_array : std::false_type {};
   template <typename E, std::size_t N>
   struct is_array<std::array<E, N>> : std::true_type {};

   template <typename T>
   struct is_utf8_wrapper : std::false_type {};
   template <std::size_t N>
   struct is_utf8_wrapper<::psio::utf8_string<N>> : std::true_type {};

   template <typename T>
   struct is_bitlist : std::false_type {};
   template <std::size_t N>
   struct is_bitlist<::psio::bitlist<N>> : std::true_type {};

   template <typename T>
   struct is_tuple : std::false_type {};
   template <typename... Ts>
   struct is_tuple<std::tuple<Ts...>> : std::true_type {};
   template <typename A, typename B>
   struct is_tuple<std::pair<A, B>> : std::true_type {};

   template <typename T>
   struct is_variant : std::false_type {};
   template <typename... Ts>
   struct is_variant<std::variant<Ts...>> : std::true_type {};

   // A string: raw (std::string) or UTF-8 (the wrapper).
   template <typename T>
   inline constexpr bool is_string_v = std::is_same_v<T, std::string> || is_utf8_wrapper<T>::value;
   // A list: its length_bound counts elements.
   template <typename T>
   inline constexpr bool is_list_v = is_vector<T>::value || is_bounded<T>::value;
   // What `element(...)` reaches.
   template <typename T>
   inline constexpr bool has_elements_v =
      is_list_v<T> || is_array<T>::value || ::psio::detail::is_set_v<T>;
   // What a length_bound constrains.
   template <typename T>
   inline constexpr bool has_length_v = is_string_v<T> || is_list_v<T> ||
                                        ::psio::detail::is_assoc_v<T> ||
                                        ::psio::detail::is_nested_v<T> || is_bitlist<T>::value;

   // ── Position specs in a tuple ──────────────────────────────────────────
   template <typename S>
   struct is_element_spec : std::false_type {};
   template <typename C>
   struct is_element_spec<::psio::element_spec<C>> : std::true_type {};
   template <typename S>
   struct is_keys_spec : std::false_type {};
   template <typename C>
   struct is_keys_spec<::psio::keys_spec<C>> : std::true_type {};
   template <typename S>
   struct is_values_spec : std::false_type {};
   template <typename C>
   struct is_values_spec<::psio::values_spec<C>> : std::true_type {};
   template <std::size_t I, typename S>
   struct is_at_spec : std::false_type {};
   template <std::size_t I, typename C>
   struct is_at_spec<I, ::psio::at_spec<I, C>> : std::true_type {};
   template <typename S>
   struct is_any_at_spec : std::false_type {};
   template <std::size_t I, typename C>
   struct is_any_at_spec<::psio::at_spec<I, C>> : std::true_type
   {
      static constexpr std::size_t index = I;
   };

   template <template <typename> class Is, typename S>
   constexpr auto child_if(const S& spec) noexcept
   {
      if constexpr (Is<S>::value)
         return ::psio::to_spec_tuple(spec.child);
      else
         return std::tuple<>{};
   }
   // The specs written inside element(...) / keys(...) / values(...): a
   // tuple, empty when the position has none.
   template <template <typename> class Is, typename... Ts>
   constexpr auto child_specs(const std::tuple<Ts...>& specs) noexcept
   {
      return std::apply(
         [](const auto&... s) { return std::tuple_cat(child_if<Is>(s)...); }, specs);
   }
   template <std::size_t I, typename S>
   constexpr auto at_child_if(const S& spec) noexcept
   {
      if constexpr (is_at_spec<I, S>::value)
         return ::psio::to_spec_tuple(spec.child);
      else
         return std::tuple<>{};
   }
   template <std::size_t I, typename... Ts>
   constexpr auto at_specs(const std::tuple<Ts...>& specs) noexcept
   {
      return std::apply(
         [](const auto&... s) { return std::tuple_cat(at_child_if<I>(s)...); }, specs);
   }

   template <typename... Ts>
   constexpr auto element_specs(const std::tuple<Ts...>& specs) noexcept
   {
      return child_specs<is_element_spec>(specs);
   }
   template <typename... Ts>
   constexpr auto key_specs(const std::tuple<Ts...>& specs) noexcept
   {
      return child_specs<is_keys_spec>(specs);
   }
   template <typename... Ts>
   constexpr auto value_specs(const std::tuple<Ts...>& specs) noexcept
   {
      return child_specs<is_values_spec>(specs);
   }

   // The annotation of one entry of a map — the pair (key, value) — from the
   // map's keys(...) / values(...); of one element of a set, from its
   // element(...).
   template <typename C, typename... Ts>
   constexpr auto entry_specs(const std::tuple<Ts...>& specs) noexcept
   {
      if constexpr (::psio::detail::is_map_v<C>)
      {
         constexpr auto wrap = []<std::size_t I, typename Child>(const Child& child) {
            if constexpr (std::tuple_size_v<Child> == 0)
               return std::tuple<>{};
            else
               return std::tuple{::psio::at_spec<I, Child>{child}};
         };
         return std::tuple_cat(wrap.template operator()<0>(key_specs(specs)),
                               wrap.template operator()<1>(value_specs(specs)));
      }
      else
         return element_specs(specs);
   }

   // Whether a tuple holds anything a codec's type walk must know: a length
   // bound, UTF-8, or a position spec.
   template <typename S>
   inline constexpr bool is_tree_spec_v =
      std::is_same_v<S, ::psio::length_bound> || std::is_same_v<S, ::psio::utf8_spec> ||
      is_element_spec<S>::value || is_keys_spec<S>::value || is_values_spec<S>::value ||
      is_any_at_spec<S>::value;
   template <typename Tuple>
   inline constexpr bool has_tree_specs_v = false;
   template <typename... Ts>
   inline constexpr bool has_tree_specs_v<std::tuple<Ts...>> = (is_tree_spec_v<Ts> || ...);

   // ── Carriers ───────────────────────────────────────────────────────────
   //
   // Besides the annotation tuple of its position a carrier has `cap`: the
   // explicit maxDynamicData of the nearest enclosing record (or method
   // argument tuple) when the position lies beneath one, with no record in
   // between; no_cap otherwise. Nothing beneath a capped record can be
   // larger than the cap, so the cap is an implicit bound of every position
   // beneath it (docs/pssz-spec.md 3.6): it narrows the offset width of a
   // list, array, map, set, tuple or pair there, and nothing else. Every
   // derived carrier inherits it.
   inline constexpr std::size_t no_cap = static_cast<std::size_t>(-1);
   struct no_ann
   {
      using value_t = std::tuple<>;
      static constexpr value_t     value{};
      static constexpr std::size_t cap = no_cap;
   };
   // A position with no annotation of its own beneath a cap of N bytes.
   template <std::size_t N>
   struct capped
   {
      using value_t = std::tuple<>;
      static constexpr value_t     value{};
      static constexpr std::size_t cap = N;
   };
   template <std::size_t N>
   struct capped_or_none
   {
      using type = capped<N>;
   };
   template <>
   struct capped_or_none<no_cap>
   {
      using type = no_ann;
   };
   // What of carrier A reaches through a type that is its member on the
   // wire (a transparent type, a Box): the cap, not the annotations.
   template <typename A>
   using cap_only = typename capped_or_none<A::cap>::type;

   template <typename Raw, bool = has_tree_specs_v<typename Raw::value_t>>
   struct normalized
   {
      using type = Raw;
   };
   template <typename Raw>
   struct normalized<Raw, false>
   {
      using type = cap_only<Raw>;
   };

   template <typename A>
   struct element_raw
   {
      static constexpr auto value = element_specs(A::value);
      using value_t               = std::remove_cvref_t<decltype(value)>;
      static constexpr std::size_t cap = A::cap;
   };
   template <typename C, typename A>
   struct entry_raw
   {
      static constexpr auto value = entry_specs<C>(A::value);
      using value_t               = std::remove_cvref_t<decltype(value)>;
      static constexpr std::size_t cap = A::cap;
   };
   template <typename A, std::size_t I>
   struct at_raw
   {
      static constexpr auto value = at_specs<I>(A::value);
      using value_t               = std::remove_cvref_t<decltype(value)>;
      static constexpr std::size_t cap = A::cap;
   };

   template <typename A>
   struct element_of
   {
      using type = typename normalized<element_raw<A>>::type;
   };
   template <>
   struct element_of<no_ann>
   {
      using type = no_ann;
   };
   template <typename C, typename A>
   struct entry_of
   {
      using type = typename normalized<entry_raw<C, A>>::type;
   };
   template <typename C>
   struct entry_of<C, no_ann>
   {
      using type = no_ann;
   };
   template <typename A, std::size_t I>
   struct at_of
   {
      using type = typename normalized<at_raw<A, I>>::type;
   };
   template <std::size_t I>
   struct at_of<no_ann, I>
   {
      using type = no_ann;
   };

   // The carrier of a list / array / set element under carrier A.
   template <typename A>
   using element_ann = typename element_of<A>::type;
   // The carrier of one wire entry of map / set C under carrier A.
   template <typename C, typename A>
   using entry_ann = typename entry_of<C, A>::type;
   // The carrier of tuple element / variant alternative I under carrier A.
   template <typename A, std::size_t I>
   using at_ann = typename at_of<A, I>::type;

   // A carrier over a tuple held by another type's static member.
   template <typename Holder>
   struct holder_raw
   {
      using value_t = std::remove_cvref_t<decltype(Holder::value)>;
      static constexpr value_t     value = Holder::value;
      static constexpr std::size_t cap   = no_cap;
   };
   template <typename Holder>
   using holder_ann = typename normalized<holder_raw<Holder>>::type;

   // The length bound a carrier states for its own position.
   template <typename A>
   inline constexpr bool has_length_bound_v =
      ::psio::has_spec_v<::psio::length_bound, typename A::value_t>;
   // Whether a tuple states an enforced UTF-8 constraint (`psio::utf8`).
   template <typename... Ts>
   constexpr bool enforces_utf8(const std::tuple<Ts...>& specs) noexcept
   {
      if constexpr ((std::is_same_v<Ts, ::psio::utf8_spec> || ...))
         return std::get<::psio::utf8_spec>(specs).enforced;
      else
         return false;
   }
   template <typename A>
   inline constexpr bool has_utf8_v = enforces_utf8(A::value);

   // ── Compile-time shape check ───────────────────────────────────────────
   //
   // tree_ok<T, A>(): the annotations of carrier A fit a value of type T at
   // every depth. `Top`: the specs are a member's own (their length_bound /
   // utf8 are checked against the member's shape by
   // effective_annotations_for's applies_to rule, and may use exact / min);
   // below a position spec this function checks them.
   //
   // Below the member a length bound is `max<N>` on a std::string, a
   // std::vector, a map or a set: that is what a schema can say about a
   // nested position (a bounded string / list node). A wrapper type
   // (psio::utf8_string<N>, psio::bounded<T, N>) already states its bound.
   template <typename T>
   inline constexpr bool position_length_v =
      std::is_same_v<T, std::string> || is_vector<T>::value || ::psio::detail::is_assoc_v<T>;

   template <typename A>
   struct keys_raw
   {
      static constexpr auto value = key_specs(A::value);
      using value_t               = std::remove_cvref_t<decltype(value)>;
   };
   template <typename A>
   struct values_raw
   {
      static constexpr auto value = value_specs(A::value);
      using value_t               = std::remove_cvref_t<decltype(value)>;
   };

   template <typename T, typename A, bool Top = true>
   consteval bool tree_ok() noexcept;

   template <typename T, typename A, typename S, bool Top>
   consteval bool spec_ok() noexcept
   {
      if constexpr (is_optional<T>::value)
         // Looked through: the spec is the payload's.
         return spec_ok<typename T::value_type, A, S, Top>();
      else if constexpr (std::is_same_v<S, ::psio::length_bound>)
      {
         static_assert(Top || position_length_v<T>,
                       "psio: max<N> inside element(...), keys(...), values(...) or at<I>(...) "
                       "bounds a std::string (bytes) or a std::vector, map or set (elements); "
                       "this position is none of those");
         if constexpr (!Top)
            static_assert(!std::get<::psio::length_bound>(A::value).exact &&
                             !std::get<::psio::length_bound>(A::value).min,
                          "psio: below the member itself a length bound is max<N>; exact<N> and "
                          "min<N> apply to the member only");
         return true;
      }
      else if constexpr (std::is_same_v<S, ::psio::utf8_spec>)
      {
         static_assert(Top || is_string_v<T>,
                       "psio: utf8 inside element(...), keys(...), values(...) or at<I>(...) "
                       "applies to a string (std::string, psio::utf8_string); this position is "
                       "not a string");
         return true;
      }
      else if constexpr (is_element_spec<S>::value)
      {
         static_assert(has_elements_v<T>,
                       "psio: element(...) annotates the elements of a list (std::vector, "
                       "psio::bounded), a std::array or a set; this member or position is none "
                       "of those (a map takes keys(...) / values(...), a tuple, pair or variant "
                       "takes at<I>(...))");
         if constexpr (::psio::detail::is_set_v<T>)
            return tree_ok<::psio::detail::assoc_key_t<T>, element_raw<A>, false>();
         else if constexpr (has_elements_v<T>)
            return tree_ok<typename T::value_type, element_raw<A>, false>();
         else
            return false;
      }
      else if constexpr (is_keys_spec<S>::value)
      {
         static_assert(::psio::detail::is_map_v<T>,
                       "psio: keys(...) annotates the keys of a map (std::map, "
                       "std::unordered_map); this member or position is not a map (a set, list "
                       "or array takes element(...))");
         if constexpr (::psio::detail::is_map_v<T>)
            return tree_ok<::psio::detail::assoc_key_t<T>, keys_raw<A>, false>();
         else
            return false;
      }
      else if constexpr (is_values_spec<S>::value)
      {
         static_assert(::psio::detail::is_map_v<T>,
                       "psio: values(...) annotates the mapped values of a map (std::map, "
                       "std::unordered_map); this member or position is not a map (a set, list "
                       "or array takes element(...))");
         if constexpr (::psio::detail::is_map_v<T>)
            return tree_ok<typename T::mapped_type, values_raw<A>, false>();
         else
            return false;
      }
      else if constexpr (is_any_at_spec<S>::value)
      {
         constexpr std::size_t I = is_any_at_spec<S>::index;
         static_assert(is_tuple<T>::value || is_variant<T>::value,
                       "psio: at<I>(...) annotates element I of a std::tuple / std::pair or "
                       "alternative I of a std::variant; this member or position is none of "
                       "those (a list, array or set takes element(...))");
         if constexpr (is_tuple<T>::value)
         {
            static_assert(I < std::tuple_size_v<T>,
                          "psio: at<I>(...): I is not an element of this tuple / pair");
            if constexpr (I < std::tuple_size_v<T>)
               return tree_ok<std::remove_const_t<std::tuple_element_t<I, T>>, at_raw<A, I>,
                              false>();
            else
               return false;
         }
         else if constexpr (is_variant<T>::value)
         {
            static_assert(I < std::variant_size_v<T>,
                          "psio: at<I>(...): I is not an alternative of this variant");
            if constexpr (I < std::variant_size_v<T>)
               return tree_ok<std::variant_alternative_t<I, T>, at_raw<A, I>, false>();
            else
               return false;
         }
         else
            return false;
      }
      else
         return true;
   }

   template <typename T, typename A, bool Top, typename... Ss>
   consteval bool tree_ok_impl(std::type_identity<std::tuple<Ss...>>) noexcept
   {
      return (spec_ok<T, A, Ss, Top>() && ... && true);
   }
   template <typename T, typename A, bool Top>
   consteval bool tree_ok() noexcept
   {
      return tree_ok_impl<T, A, Top>(
         std::type_identity<std::remove_cvref_t<decltype(A::value)>>{});
   }

   // ── Value-side check ───────────────────────────────────────────────────
   //
   // Every length_bound and utf8 of the tree against `value`. The first
   // violation is returned: `length_message` for a length bound (each format
   // has its own text), the utf8_spec validator's status for UTF-8.
   constexpr bool count_ok(std::size_t count, const ::psio::length_bound& lb) noexcept
   {
      return !((lb.max && count > *lb.max) || (lb.exact && count != *lb.exact) ||
               (lb.min && count < *lb.min));
   }

   template <typename T, typename... Ss>
   codec_status check_value(const T& value, const std::tuple<Ss...>& specs,
                            const char* length_message, std::string_view format) noexcept
   {
      using Specs = std::tuple<Ss...>;
      if constexpr (!has_tree_specs_v<Specs>)
         return codec_ok();
      else if constexpr (is_optional<T>::value)
      {
         if (!value.has_value())
            return codec_ok();
         return check_value(*value, specs, length_message, format);
      }
      else
      {
         if constexpr (::psio::has_spec_v<::psio::length_bound, Specs> && has_length_v<T>)
            if (!count_ok(value.size(), std::get<::psio::length_bound>(specs)))
               return codec_fail(length_message, 0, format);
         if constexpr (::psio::has_spec_v<::psio::utf8_spec, Specs> && is_string_v<T>)
            if (enforces_utf8(specs))
               if (auto st = ::psio::utf8_spec::validate(
                      std::span<const char>(value.data(), value.size()));
                   !st.ok())
                  return st;
         if constexpr (has_elements_v<T>)
         {
            if constexpr (has_tree_specs_v<std::remove_cvref_t<decltype(element_specs(specs))>>)
            {
               const auto inner = element_specs(specs);
               for (const auto& item : value)
                  if (auto st = check_value(item, inner, length_message, format); !st.ok())
                     return st;
            }
         }
         else if constexpr (::psio::detail::is_map_v<T>)
         {
            const auto key_inner   = key_specs(specs);
            const auto value_inner = value_specs(specs);
            if constexpr (has_tree_specs_v<std::remove_cvref_t<decltype(key_inner)>> ||
                          has_tree_specs_v<std::remove_cvref_t<decltype(value_inner)>>)
               for (const auto& item : value)
               {
                  if (auto st = check_value(item.first, key_inner, length_message, format);
                      !st.ok())
                     return st;
                  if (auto st = check_value(item.second, value_inner, length_message, format);
                      !st.ok())
                     return st;
               }
         }
         else if constexpr (is_tuple<T>::value)
         {
            codec_status status;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                   if (status.ok())
                      status = check_value(std::get<Is>(value), at_specs<Is>(specs),
                                           length_message, format);
                }()),
                ...);
            }(std::make_index_sequence<std::tuple_size_v<T>>{});
            return status;
         }
         else if constexpr (is_variant<T>::value)
         {
            codec_status status;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                   if (value.index() == Is)
                      status = check_value(std::get<Is>(value), at_specs<Is>(specs),
                                           length_message, format);
                }()),
                ...);
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return status;
         }
         return codec_ok();
      }
   }

   // Whether writing through a mutable view of this member could break a
   // constraint stated below the member itself (mutable_view.hpp).
   template <typename Tuple>
   inline constexpr bool has_position_specs_v = false;
   template <typename... Ts>
   inline constexpr bool has_position_specs_v<std::tuple<Ts...>> =
      ((is_element_spec<Ts>::value || is_keys_spec<Ts>::value || is_values_spec<Ts>::value ||
        is_any_at_spec<Ts>::value) ||
       ...);

}  // namespace psio::detail::ann
