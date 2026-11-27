#pragma once
//
// psio/format_tag_base.hpp — CRTP base for format tags.
//
// Every format tag (`psio::ssz`, `psio::pssz`, ...) inherits from
// `psio::format_tag_base<Derived>` to get the scoped-sugar form:
//
//     psio::encode(psio::pssz{}, v, sink)   // generic CPO form
//     psio::pssz::encode(v, sink)            // scoped sugar — same code
//
// The scoped statics are one-line delegates to the matching CPO.
// They exist purely so users with a single known format at a call
// site can avoid the tag-passed-as-value ceremony.

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/helpers.hpp>

#include <memory>
#include <span>
#include <vector>
#include <utility>

namespace psio {

   // What a format's trusted read (`from_F<T>(psio::trusted{…})`,
   // `psio::decode<T>(F{}, bytes)`) does with bytes that are not a frame of
   // the type (docs/annotations.md 7.2 "On bytes that are not a frame").
   // Every tag declares its kind as `trusted_read`; psio/format_concept.hpp.
   enum class trusted_read_kind
   {
      refuses_non_frames,    // compares what it reads; a failure, no read outside
      parses,                // finds its values by parsing, as the validating read does
      validated_bytes_only,  // compares nothing: for bytes a validator accepted
   };

   // What `psio::encode(F{}, v, sink)` does to a sink that already holds
   // bytes. Every tag declares it as `sink_write`.
   enum class sink_write_kind
   {
      appends,   // the encoding follows what the sink held
      replaces,  // the sink's contents become the encoding
      none,      // the format has no sink overload
   };

   template <typename Derived>
   struct format_tag_base
   {
      // The fresh buffer of `psio::encode(F{}, v)`. A text format
      // redeclares it as std::string.
      using buffer_type = std::vector<char>;

      // ── encode ─────────────────────────────────────────────────────────
      template <typename T, typename Sink>
      static constexpr void encode(const T& v, Sink& s)
         noexcept(noexcept(::psio::encode(Derived{}, v, s)))
      {
         ::psio::encode(Derived{}, v, s);
      }

      template <typename T>
      static constexpr auto encode(const T& v)
         noexcept(noexcept(::psio::encode(Derived{}, v)))
      {
         return ::psio::encode(Derived{}, v);
      }

      // ── decode ─────────────────────────────────────────────────────────
      template <typename T>
      static constexpr T decode(std::span<const char> b) noexcept
      {
         return ::psio::decode<T>(Derived{}, b);
      }

      // ── size_of ────────────────────────────────────────────────────────
      template <typename T>
      static constexpr auto size_of(const T& v) noexcept
      {
         return ::psio::size_of(Derived{}, v);
      }

      // ── validate ───────────────────────────────────────────────────────
      template <typename T>
      [[nodiscard]] static constexpr codec_status
      validate(std::span<const char> b) noexcept
      {
         return ::psio::validate<T>(Derived{}, b);
      }

      template <typename T>
      [[nodiscard]] static constexpr codec_status
      validate_strict(std::span<const char> b) noexcept
      {
         return ::psio::validate_strict<T>(Derived{}, b);
      }

      // ── make_boxed ─────────────────────────────────────────────────────
      template <typename T>
      [[nodiscard]] static constexpr std::unique_ptr<T>
      make_boxed(std::span<const char> b) noexcept
      {
         return ::psio::make_boxed<T>(Derived{}, b);
      }
   };

}  // namespace psio
