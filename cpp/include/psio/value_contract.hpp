#pragma once
//
// psio/value_contract.hpp — what a type declares about its values, checked
// on a value.
//
// A bound declared on a member is part of the type, whatever format the
// value arrives in. The formats that have their own in-place checks (pssz,
// key, json) read the annotations where they read the bytes. A format whose
// validator follows another specification — FlatBuffers, Cap'n Proto,
// protobuf — checks the structure of its bytes first and then holds the
// value they decode to against the declaration, with this walk:
//
//   psio::has_value_contract_v<T>      T, or anything a T holds, declares
//                                      something a value can violate: a
//                                      member's length_bound / utf8 /
//                                      sorted / unique / unique_keys (at any
//                                      depth: element, keys, values, at<I>),
//                                      a transparent type's bounded member,
//                                      or an invariant. False for a type
//                                      without annotations; a validator then
//                                      does no extra work.
//   psio::check_value_contract(v, length_message, format)
//                                      the first violation, or ok.
//
// Order of the walk (the Rust validators report the same violation for the
// same bytes): the members of a record in declaration order; for each
// member what it holds first (elements in order, the entries of a map in
// key order with the key before the mapped value, the active alternative of
// a variant), then the member's own annotations; the invariant of a type
// after everything the value holds.
//
// `length_message` is the text of a length violation ("flatbuf: member
// length_bound violated"); the statuses of utf8_spec, of the order specs and
// of an invariant are passed through. Every status reports position 0: the
// check is of the value, not of a place in the bytes.

#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>   // (before the annotation tree, which names the wrapper types)
#include <psio/detail/annotation_tree.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/error.hpp>
#include <psio/invariant.hpp>
#include <psio/key.hpp>   // the order specs (sorted, unique, unique_keys) compare key bytes
#include <psio/transparent.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio
{
   namespace detail::contract
   {
      template <typename T>
      inline constexpr bool is_sequence_v = false;
      template <typename T, typename A>
      inline constexpr bool is_sequence_v<std::vector<T, A>> = true;
      template <typename T, std::size_t N>
      inline constexpr bool is_sequence_v<std::array<T, N>> = true;

      template <typename T>
      inline constexpr bool is_optional_v = false;
      template <typename T>
      inline constexpr bool is_optional_v<std::optional<T>> = true;

      template <typename T>
      inline constexpr bool is_variant_v = false;
      template <typename... Ts>
      inline constexpr bool is_variant_v<std::variant<Ts...>> = true;

      template <typename T>
      inline constexpr bool is_tuple_v = false;
      template <typename... Ts>
      inline constexpr bool is_tuple_v<std::tuple<Ts...>> = true;
      template <typename A, typename B>
      inline constexpr bool is_tuple_v<std::pair<A, B>> = true;

      template <typename T>
      inline constexpr bool is_bounded_v = false;
      template <typename T, std::size_t N>
      inline constexpr bool is_bounded_v<::psio::bounded<T, N>> = true;

      template <typename T>
      inline constexpr bool is_utf8_string_v = false;
      template <std::size_t N>
      inline constexpr bool is_utf8_string_v<::psio::utf8_string<N>> = true;

      template <typename T>
      concept record = ::psio::Reflected<T> && !std::is_enum_v<T> && !::psio::is_transparent_v<T>;

      // Whether a T, or anything nested in it, has something to check. A
      // type that holds itself is being inspected already: `Seen`.
      template <typename T, typename... Seen>
      consteval bool has_checks() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr ((std::is_same_v<U, Seen> || ...))
            return false;
         else if constexpr (std::is_arithmetic_v<U>)
            return false;
         else if constexpr (::psio::has_invariant_v<U>)
            return true;
         else if constexpr (std::is_enum_v<U>)
            return false;
         else if constexpr (is_bounded_v<U> || is_utf8_string_v<U>)
            return true;  // the wrapper is its bound (and, for text, UTF-8)
         else if constexpr (::psio::is_transparent_v<U>)
            return ::psio::transparent_member_checked_v<U> ||
                   has_checks<::psio::transparent_inner_t<U>, Seen..., U>();
         else if constexpr (::psio::detail::is_map_v<U>)
            return has_checks<::psio::detail::assoc_key_t<U>, Seen..., U>() ||
                   has_checks<typename U::mapped_type, Seen..., U>();
         else if constexpr (::psio::detail::is_set_v<U>)
            return has_checks<::psio::detail::assoc_key_t<U>, Seen..., U>();
         else if constexpr (is_optional_v<U> || is_sequence_v<U>)
            return has_checks<typename U::value_type, Seen..., U>();
         else if constexpr (is_variant_v<U>)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (has_checks<std::variant_alternative_t<Is, U>, Seen..., U>() || ...);
            }(std::make_index_sequence<std::variant_size_v<U>>{});
         else if constexpr (is_tuple_v<U>)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (has_checks<std::tuple_element_t<Is, U>, Seen..., U>() || ...);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         else if constexpr (record<U>)
         {
            using R = ::psio::reflect<U>;
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return ((::psio::detail::ann::has_value_checks_v<
                           std::remove_cvref_t<typename R::template member_type<Is>>,
                           std::remove_cvref_t<typename ::psio::effective_annotations_for<
                              U, typename R::template member_type<Is>,
                              R::template member_pointer<Is>>::value_t>> ||
                        has_checks<typename R::template member_type<Is>, Seen..., U>()) ||
                       ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
            return false;
      }

      template <typename T>
      codec_status check(const T& v, const char* length_message, std::string_view format) noexcept
      {
         if constexpr (!has_checks<T>())
         {
            (void)v;
            return codec_ok();
         }
         else
         {
            if constexpr (::psio::is_transparent_v<T>)
            {
               if (auto st = check(::psio::transparent_inner(v), length_message, format); !st.ok())
                  return st;
               if (auto st = ::psio::check_transparent_member(v, length_message, format); !st.ok())
                  return st;
            }
            else if constexpr (is_utf8_string_v<T>)
            {
               if (v.size() > T::max_size_value)
                  return codec_fail(length_message, 0, format);
               if (auto st = ::psio::utf8_spec::validate(std::span<const char>(v.data(), v.size()));
                   !st.ok())
                  return st;
            }
            else if constexpr (is_bounded_v<T>)
            {
               if (auto st = check(v.storage(), length_message, format); !st.ok())
                  return st;
               if (v.size() > T::max_size_value)
                  return codec_fail(length_message, 0, format);
            }
            else if constexpr (::psio::detail::is_map_v<T>)
            {
               for (const auto& entry : v)
               {
                  if (auto st = check(entry.first, length_message, format); !st.ok())
                     return st;
                  if (auto st = check(entry.second, length_message, format); !st.ok())
                     return st;
               }
            }
            else if constexpr (::psio::detail::is_set_v<T> || is_sequence_v<T>)
            {
               for (const auto& element : v)
                  if (auto st = check(element, length_message, format); !st.ok())
                     return st;
            }
            else if constexpr (is_optional_v<T>)
            {
               if (v.has_value())
                  if (auto st = check(*v, length_message, format); !st.ok())
                     return st;
            }
            else if constexpr (is_variant_v<T>)
            {
               if (auto st = std::visit(
                      [&](const auto& alt) { return check(alt, length_message, format); }, v);
                   !st.ok())
                  return st;
            }
            else if constexpr (is_tuple_v<T>)
            {
               codec_status status;
               std::apply(
                  [&](const auto&... element) {
                     (([&] {
                         if (status.ok())
                            status = check(element, length_message, format);
                      }()),
                      ...);
                  },
                  v);
               if (!status.ok())
                  return status;
            }
            else if constexpr (record<T>)
            {
               using R = ::psio::reflect<T>;
               codec_status status;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  (([&] {
                      if (!status.ok())
                         return;
                      using F   = typename R::template member_type<Is>;
                      using Ann = ::psio::effective_annotations_for<
                         T, F, R::template member_pointer<Is>>;
                      using eff          = std::remove_cvref_t<decltype(Ann::value)>;
                      const auto& member = v.*(R::template member_pointer<Is>);
                      status             = check(member, length_message, format);
                      if constexpr (::psio::detail::ann::has_value_checks_v<
                                       std::remove_cvref_t<F>, eff>)
                         if (status.ok())
                            status = ::psio::check_member_annotations(member, Ann::value,
                                                                      length_message, format);
                   }()),
                   ...);
               }(std::make_index_sequence<R::member_count>{});
               if (!status.ok())
                  return status;
            }
            if constexpr (::psio::has_invariant_v<T>)
               return ::psio::check_invariant(v);
            return codec_ok();
         }
      }
   }  // namespace detail::contract

   // T, or anything a T holds, declares something a value can violate.
   template <typename T>
   inline constexpr bool has_value_contract_v = detail::contract::has_checks<T>();

   // The first declaration `value` violates, or ok.
   template <typename T>
   [[nodiscard]] codec_status check_value_contract(const T& value, const char* length_message,
                                                   std::string_view format) noexcept
   {
      return detail::contract::check(value, length_message, format);
   }

   // The same without a format: whether `value` is a value of its type, as
   // every validating reader and every validated write (`to_F_checked`)
   // understands it. A caller that writes one value to several formats
   // validates it once with this and then uses the fast writes (`to_F`).
   // (A format may refuse more than this for reasons of its own: pssz a
   // total over a `maxDynamicData` cap, json a raw string that is not
   // UTF-8.)
   template <typename T>
   [[nodiscard]] codec_status validate_value(const T& value) noexcept
   {
      return detail::contract::check(value, "psio: member length_bound violated", "psio");
   }

   // For a format's `validate`, after the structure of `bytes` has passed:
   // the value they decode to against what T declares. A type that declares
   // nothing is not decoded. `decode`: () -> T.
   template <typename T, typename Decode>
   [[nodiscard]] codec_status check_decoded_contract(Decode&&         decode,
                                                     const char*      length_message,
                                                     const char*      decode_failed,
                                                     std::string_view format) noexcept
   {
      if constexpr (!has_value_contract_v<T>)
         return codec_ok();
      else
      {
#if defined(__cpp_exceptions)
         try
         {
            const T value = decode();
            return detail::contract::check(value, length_message, format);
         }
         catch (...)
         {
            return codec_fail(decode_failed, 0, format);
         }
#else
         (void)decode_failed;
         const T value = decode();
         return detail::contract::check(value, length_message, format);
#endif
      }
   }

}  // namespace psio
