#pragma once
//
// psio/cpo.hpp — customization point objects.
//
// Per design doc § 5.2. Every operation a format customizes is a CPO;
// each CPO dispatches to `tag_invoke(cpo_obj{}, fmt, args...)` via
// ADL on the format tag.
//
// Format authors opt in by defining `tag_invoke` hidden friends on
// their format tag struct (see format_tag_base.hpp for the CRTP that
// supplies the format-scoped sugar `Fmt::encode(v)` on top).
//
// The CPOs are:
//
//   psio::encode(fmt, value, sink)   — stream-write value into sink
//   psio::decode<T>(fmt, bytes)      — decode bytes into a fresh T
//   psio::size_of(fmt, value)        — byte size the value will produce
//   psio::validate<T>(fmt, bytes)    — structural validation → codec_status
//   psio::validate_strict<T>(fmt, b) — + semantic (spec-carried) checks
//   psio::make_boxed<T>(fmt, bytes)  — decode into a fresh unique_ptr<T>
//
// For CPOs that take an explicit `T` (decode, validate, validate_strict,
// make_boxed) the CPO is a variable template instantiated on T — this
// makes the natural syntax `psio::decode<int>(fmt, bytes)` work and
// lets format authors write `friend … tag_invoke(decltype(psio::decode<int>), …)`.
// For CPOs where `T` is deducible from the value (encode, size_of) the
// CPO is a single inline-constexpr object.

#include <psio/detail/tag_invoke.hpp>
#include <psio/error.hpp>

#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace psio {

   namespace detail {

      // ── encode ────────────────────────────────────────────────────────
      struct encode_fn
      {
         // Stream-write overload. SFINAE via trailing return type so
         // test_fmt's hidden-friend tag_invoke is the only candidate.
         template <typename Fmt, typename T, typename Sink>
         constexpr auto operator()(Fmt fmt, const T& v, Sink& s) const
            noexcept(noexcept(tag_invoke(*this, fmt, v, s)))
               -> decltype(tag_invoke(*this, fmt, v, s))
         {
            return tag_invoke(*this, fmt, v, s);
         }

         // Return-value overload — formats can provide a direct
         // tag_invoke(encode_fn, fmt, v) for a pre-sized fresh buffer.
         template <typename Fmt, typename T>
         constexpr auto operator()(Fmt fmt, const T& v) const
            noexcept(noexcept(tag_invoke(*this, fmt, v)))
               -> decltype(tag_invoke(*this, fmt, v))
         {
            return tag_invoke(*this, fmt, v);
         }
      };

      // ── decode ────────────────────────────────────────────────────────
      template <typename T>
      struct decode_fn
      {
         template <typename Fmt>
         constexpr T operator()(Fmt fmt, std::span<const char> b) const
            noexcept(noexcept(
               tag_invoke(*this, fmt, static_cast<T*>(nullptr), b)))
         {
            return tag_invoke(*this, fmt, static_cast<T*>(nullptr), b);
         }
      };

      // ── size_of ───────────────────────────────────────────────────────
      struct size_fn
      {
         template <typename Fmt, typename T>
         constexpr auto operator()(Fmt fmt, const T& v) const
            noexcept(noexcept(tag_invoke(*this, fmt, v)))
               -> decltype(tag_invoke(*this, fmt, v))
         {
            return tag_invoke(*this, fmt, v);
         }
      };

      // ── validate (structural only, no-throw) ──────────────────────────
      //
      // Returns codec_status. Always available — the only validate API
      // usable under -fno-exceptions (WASM, freestanding). The success
      // path constructs a default codec_status (null unique_ptr, one
      // register-sized return).
      template <typename T>
      struct validate_fn
      {
         template <typename Fmt>
         [[nodiscard]] constexpr codec_status
         operator()(Fmt fmt, std::span<const char> b) const noexcept
         {
            return tag_invoke(*this, fmt, static_cast<T*>(nullptr), b);
         }
      };

      // ── validate_or_throw (structural only, throwing) ─────────────────
      //
      // Throws codec_exception on failure, void on success. Format
      // authors implement this with direct throw-from-check macros so
      // the compiler can elide the entire check chain when it proves no
      // throw is reachable on the success path — the same zero-cost
      // pattern v1 uses. Use this in native code where exceptions are
      // available; fall back to validate (status) under -fno-exceptions.
      template <typename T>
      struct validate_or_throw_fn
      {
         // Constrained operator() — SFINAE'd on tag_invoke availability
         // so `requires { psio::validate_or_throw<T>(fmt, b); }` cleanly
         // reports false for formats that haven't implemented the
         // throwing path. (Without the `requires`, the unconstrained
         // body would generate a hard error during requires-expression
         // probing instead of a clean `false`.)
         template <typename Fmt>
            requires requires(Fmt fmt, std::span<const char> b) {
               tag_invoke(std::declval<const validate_or_throw_fn&>(),
                          fmt, static_cast<T*>(nullptr), b);
            }
         constexpr void
         operator()(Fmt fmt, std::span<const char> b) const
         {
            tag_invoke(*this, fmt, static_cast<T*>(nullptr), b);
         }
      };

      // ── validate_strict (structural + semantic, no-throw) ─────────────
      template <typename T>
      struct validate_strict_fn
      {
         template <typename Fmt>
         [[nodiscard]] constexpr codec_status
         operator()(Fmt fmt, std::span<const char> b) const noexcept
         {
            return tag_invoke(*this, fmt, static_cast<T*>(nullptr), b);
         }
      };

      // ── validate_strict_or_throw (structural + semantic, throwing) ────
      template <typename T>
      struct validate_strict_or_throw_fn
      {
         template <typename Fmt>
            requires requires(Fmt fmt, std::span<const char> b) {
               tag_invoke(
                  std::declval<const validate_strict_or_throw_fn&>(),
                  fmt, static_cast<T*>(nullptr), b);
            }
         constexpr void
         operator()(Fmt fmt, std::span<const char> b) const
         {
            tag_invoke(*this, fmt, static_cast<T*>(nullptr), b);
         }
      };

      // ── make_boxed — heap-allocate + decode in place ──────────────────
      template <typename T>
      struct make_boxed_fn
      {
         template <typename Fmt>
         [[nodiscard]] constexpr std::unique_ptr<T>
         operator()(Fmt fmt, std::span<const char> b) const noexcept
         {
            return tag_invoke(*this, fmt, static_cast<T*>(nullptr), b);
         }
      };

      // ── encode_dynamic / decode_dynamic ────────────────────────────────
      //
      // Runtime-schema counterparts of encode/decode. Each format tag
      // supplies hidden-friend overloads (see dynamic_json.hpp, etc.).
      // These CPOs enable generic transcode across format pairs without
      // a compile-time T.

      struct encode_dynamic_fn
      {
         template <typename Fmt, typename Schema, typename Dv>
         constexpr auto operator()(Fmt fmt, const Schema& sc,
                                   const Dv& dv) const
            -> decltype(tag_invoke(*this, fmt, sc, dv))
         {
            return tag_invoke(*this, fmt, sc, dv);
         }

         // Sink-form overload: write into a pre-existing buffer.
         template <typename Fmt, typename Schema, typename Dv, typename Sink>
         constexpr auto operator()(Fmt fmt, const Schema& sc, const Dv& dv,
                                   Sink& s) const
            -> decltype(tag_invoke(*this, fmt, sc, dv, s))
         {
            return tag_invoke(*this, fmt, sc, dv, s);
         }
      };

      struct decode_dynamic_fn
      {
         template <typename Fmt, typename Schema>
         constexpr auto operator()(Fmt fmt, const Schema& sc,
                                   std::span<const char> bytes) const
            -> decltype(tag_invoke(*this, fmt, sc, bytes))
         {
            return tag_invoke(*this, fmt, sc, bytes);
         }
      };

   }  // namespace detail

   // ── CPO instances ─────────────────────────────────────────────────────
   //
   // Non-templated CPOs — `T` deduced from the value.
   inline constexpr detail::encode_fn encode{};
   inline constexpr detail::size_fn   size_of{};

   // Templated CPOs — `T` specified by the caller. These are variable
   // templates so `psio::decode<int>(fmt, bytes)` parses; each
   // instantiation is a distinct type that `decltype(psio::decode<int>)`
   // can name from a hidden-friend declaration.
   template <typename T>
   inline constexpr detail::decode_fn<T> decode{};

   template <typename T>
   inline constexpr detail::validate_fn<T> validate{};

   template <typename T>
   inline constexpr detail::validate_or_throw_fn<T> validate_or_throw{};

   template <typename T>
   inline constexpr detail::validate_strict_fn<T> validate_strict{};

   template <typename T>
   inline constexpr detail::validate_strict_or_throw_fn<T>
      validate_strict_or_throw{};

   template <typename T>
   inline constexpr detail::make_boxed_fn<T> make_boxed{};

   // Dynamic (schema-driven) CPOs — non-templated; the schema argument
   // carries the type information at runtime.
   inline constexpr detail::encode_dynamic_fn encode_dynamic{};
   inline constexpr detail::decode_dynamic_fn decode_dynamic{};

   // Note: `size_of` rather than `size` to avoid clashing with
   // `psio::size(buffer)` / `psio::size(view)` from the storage API
   // (those take containers, this one takes a value + format). The
   // two overload sets are distinct — one is the byte-count of an
   // encoded blob, the other is the byte-count a value will produce
   // when encoded — and users disambiguate by the argument.

}  // namespace psio
