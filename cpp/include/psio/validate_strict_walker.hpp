#pragma once
//
// psio/validate_strict_walker.hpp — semantic-spec validation walker.
//
// Implements the validate_strict half of design §5.3.3 / §5.4: after a
// format's structural validate succeeds, decode the value and walk the
// reflected fields invoking each spec's `validate(span<const char>)`
// member on the field's payload bytes. Returns the first failure.
//
// Per design §5.3.3: spec types may optionally define
//   static codec_status validate(std::span<const char>) noexcept;
// Specs without this member are ignored — the walker is open to
// third-party spec types in user code (no library modification needed).
//
// Format integration: each format's `tag_invoke(validate_strict<T>, …)`
// is a thin wrapper:
//   1. structural validate (precondition for sound decode)
//   2. decode<T>
//   3. validate_specs_on_value<T>(decoded)
// The walker is format-agnostic — it works on the typed value,
// extracting field byte ranges from the C++ representation rather
// than from the wire (the spec interpretation only depends on the
// payload bytes, not framing).
//
// Coverage:
//   - std::string fields → spec.validate({s.data(), s.size()})
//   - Reflected (record) fields → recurse
//   - std::vector<E> → if E is Reflected, recurse on each element
//   - std::optional<E> → recurse on the value when present
//   - std::variant<Es...> → recurse on the active alternative
//   - element_spec<ChildSpec> on a sequence field → run the child
//     spec's (span)-validate on every element (today: std::string
//     elements; arithmetic / nested-record elements are recursed
//     by the structural walker, separate from element_spec)
//   - Specs without a (span)-validate member are ignored (open SFINAE)
//
// Out of scope (follow-ups):
//   - Specs that need a typed interface (sorted_spec on vec<u32>,
//     length_bound size-only checks). The (span) signature doesn't
//     fit those well; a typed-validate sibling will land separately.

#include <psio/annotate.hpp>   // element_spec
#include <psio/error.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   // Forward declaration so detail helpers can recurse into Records.
   template <typename T>
   [[nodiscard]] inline codec_status
   validate_specs_on_value(const T& value) noexcept;

   namespace detail::vstrict {

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector<std::vector<E, A>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename E>
      struct is_std_optional<std::optional<E>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_std_variant : std::false_type {};
      template <typename... Ts>
      struct is_std_variant<std::variant<Ts...>> : std::true_type {};

      // SFINAE: does S have a static validate(span<const char>) member
      // returning codec_status?
      template <typename S, typename = void>
      struct has_span_validate : std::false_type {};

      template <typename S>
      struct has_span_validate<
         S,
         std::void_t<decltype(S::validate(std::span<const char>{}))>>
         : std::is_same<decltype(S::validate(std::span<const char>{})),
                         codec_status>
      {
      };

      template <typename S>
      inline constexpr bool has_span_validate_v = has_span_validate<S>::value;

      // Detect element_spec<C> entries in an annotation tuple.
      template <typename S>
      struct is_element_spec : std::false_type {};
      template <typename C>
      struct is_element_spec<::psio::element_spec<C>> : std::true_type
      {
         using child_type = C;
      };

      // Get the byte view of a single element for an element_spec
      // child validator. Today: std::string. Other element types
      // fall through and the walker just skips the (span)-only spec
      // — recursion still picks up nested records.
      template <typename E>
      inline std::span<const char> element_byte_view(const E& e) noexcept
      {
         if constexpr (std::is_same_v<E, std::string>)
            return std::span<const char>{e.data(), e.size()};
         else
            return {};
      }

      template <typename E>
      inline constexpr bool element_has_byte_view_v =
         std::is_same_v<E, std::string>;

      // Run every spec's validate(span) on `bytes`. Returns first failure.
      template <typename Tuple>
      codec_status run_specs(std::span<const char> bytes,
                              const Tuple& tup) noexcept
      {
         codec_status err = codec_ok();
         std::apply(
            [&](const auto&... specs) {
               (
                  ([&]() {
                     using S = std::remove_cvref_t<decltype(specs)>;
                     if (!err.ok())
                        return;
                     if constexpr (has_span_validate_v<S>)
                     {
                        codec_status s = S::validate(bytes);
                        if (!s.ok())
                           err = std::move(s);
                     }
                  }()),
                  ...);
            },
            tup);
         return err;
      }

      // Recurse into a value that may itself contain reflected records,
      // vectors-of-records, optionals, or variants. Specs annotated on
      // the *containing field* are NOT processed here — that's the
      // caller's job. This handler only walks structure to find more
      // record fields whose own annotations need checking.
      template <typename V>
      codec_status recurse_into(const V& value) noexcept
      {
         using U = std::remove_cvref_t<V>;
         if constexpr (::psio::Reflected<U>)
         {
            return ::psio::validate_specs_on_value(value);
         }
         else if constexpr (is_std_vector<U>::value)
         {
            using E = typename U::value_type;
            if constexpr (::psio::Reflected<E> ||
                          is_std_vector<E>::value ||
                          is_std_optional<E>::value ||
                          is_std_variant<E>::value)
            {
               for (const auto& e : value)
               {
                  auto st = recurse_into<E>(e);
                  if (!st.ok())
                     return st;
               }
            }
            return codec_ok();
         }
         else if constexpr (is_std_optional<U>::value)
         {
            if (value.has_value())
            {
               using E = typename U::value_type;
               return recurse_into<E>(*value);
            }
            return codec_ok();
         }
         else if constexpr (is_std_variant<U>::value)
         {
            codec_status err = codec_ok();
            std::visit(
               [&](const auto& alt) {
                  using A = std::remove_cvref_t<decltype(alt)>;
                  err     = recurse_into<A>(alt);
               },
               value);
            return err;
         }
         else
         {
            (void)value;
            return codec_ok();
         }
      }

      // Per-field handler: runs (span)-validate specs on the field's
      // payload bytes (today: std::string), then recurses into the
      // field structure to pick up nested records.
      template <typename T, std::size_t I>
      codec_status validate_field(const T& value) noexcept
      {
         using R       = ::psio::reflect<T>;
         using F       = typename R::template member_type<I>;
         constexpr auto MemPtr = R::template member_pointer<I>;
         using EA = ::psio::effective_annotations_for<T, F, MemPtr>;
         constexpr auto annotations = EA::value;

         const F& field_val = value.*MemPtr;

         // (1) Run any (span)-validate specs against this field's bytes.
         if constexpr (std::is_same_v<F, std::string>)
         {
            std::span<const char> bytes{field_val.data(), field_val.size()};
            auto st = run_specs(bytes, annotations);
            if (!st.ok())
               return st;
         }

         // (2) For sequence fields, run any element_spec<C> annotations
         // by invoking C::validate(element_bytes) on each element.
         if constexpr (is_std_vector<F>::value)
         {
            using E = typename F::value_type;
            codec_status err = codec_ok();
            std::apply(
               [&](const auto&... specs) {
                  (
                     ([&]() {
                        if (!err.ok())
                           return;
                        using S = std::remove_cvref_t<decltype(specs)>;
                        if constexpr (is_element_spec<S>::value)
                        {
                           using C = typename is_element_spec<S>::child_type;
                           if constexpr (has_span_validate_v<C> &&
                                         element_has_byte_view_v<E>)
                           {
                              for (const auto& e : field_val)
                              {
                                 auto bv = element_byte_view<E>(e);
                                 codec_status s = C::validate(bv);
                                 if (!s.ok())
                                 {
                                    err = std::move(s);
                                    return;
                                 }
                              }
                           }
                        }
                     }()),
                     ...);
               },
               annotations);
            if (!err.ok())
               return err;
         }

         // (3) Recurse into structural carriers. Specs on inner record
         // fields get checked when validate_specs_on_value processes
         // them.
         return recurse_into<F>(field_val);
      }

   }  // namespace detail::vstrict

   // Public entry point: walk every reflected field of `value`, run
   // each spec's `validate(span)` against the field's payload bytes,
   // and recurse into nested records / vectors / optionals / variants.
   // Returns the first failure, or codec_ok() if everything passes.
   template <typename T>
   [[nodiscard]] inline codec_status
   validate_specs_on_value(const T& value) noexcept
   {
      if constexpr (!::psio::Reflected<T>)
      {
         (void)value;
         return codec_ok();
      }
      else
      {
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            codec_status err = codec_ok();
            ((err.ok()
                 ? (err = detail::vstrict::validate_field<T, Is>(value),
                    void())
                 : void()),
             ...);
            return err;
         }(std::make_index_sequence<N>{});
      }
   }

}  // namespace psio
