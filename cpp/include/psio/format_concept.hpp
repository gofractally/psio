#pragma once
//
// psio/format_concept.hpp — what a format is, stated as concepts.
//
// Every format is a tag type (`psio::pssz`, `psio::json`, ...). Code that is
// generic over the format takes the tag as a type and can be pointed at
// another format by changing one alias:
//
//     using F = psio::pssz;                       // or psio::bin, psio::json, ...
//     auto bytes = psio::encode(F{}, value);
//     auto back  = psio::decode<T>(F{}, std::span<const char>{bytes});
//
// The concepts say what that code may rely on (gofractally/psio#18):
//
//   psio::format<F>           F is a format tag: it declares its name, its
//                             fresh buffer, what its trusted read does with
//                             bytes that are not a frame, and what its
//                             encode does to a sink.
//   psio::encodes<F, T> ...   one operation for one type (below).
//   psio::codec_for<F, T>     all of them: T can be written fast and
//                             checked, measured, validated and read in F.
//
// and, for what a format may legitimately not have, capabilities the
// generic code tests instead of finding a function missing:
//
//   psio::text_format<F>          the encoding is text (a std::string)
//   psio::appends_to_sink<F>      encode(F{}, v, sink) keeps what the sink held
//   psio::frame_checked_read<F>   the trusted read refuses or parses; it is
//                                 not for validated bytes only
//   psio::view_format<F>          psio::view<T, F> exists      (with the
//   psio::trusted_view_format<F>  ... and its trusted store     format's view
//   psio::mutable_view_format<F>  psio::mutable_view<T, F>      header included)
//   psio::dynamic_format<F, S, V> encode_dynamic / decode_dynamic by a schema
//
// cpp/tests/format_concept_tests.cpp instantiates one generic function for
// every format through an alias and holds each format's capabilities
// against a table, so a capability that is missing is written down.

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/helpers.hpp>

#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psio {

   // ── A format ────────────────────────────────────────────────────────────
   template <typename F>
   concept format =
      std::derived_from<F, format_tag_base<F>> && std::default_initializable<F> &&
      std::is_empty_v<F> && requires {
         typename F::buffer_type;
         { F::name } -> std::convertible_to<std::string_view>;
         { F::trusted_read } -> std::convertible_to<trusted_read_kind>;
         { F::sink_write } -> std::convertible_to<sink_write_kind>;
      };

   // ── One operation, for one type ─────────────────────────────────────────
   //
   // Each is safe to ask in a `requires`: false, never a hard error.

   // `psio::encode(F{}, v)` → `F::buffer_type` (or, for a key of fixed size
   // through `to_key`, an array).
   template <typename F, typename T>
   concept encodes = detail::helpers::encodes<F, T>;

   // `psio::encode(F{}, v, sink)`.
   template <typename F, typename T, typename Sink = typename F::buffer_type>
   concept encodes_into = detail::helpers::encodes_into<F, T, Sink>;

   // `psio::size_of(F{}, v)` → the size of the encoding.
   template <typename F, typename T>
   concept sizes = detail::helpers::sizes<F, T>;

   // `psio::decode<T>(F{}, bytes)` → T: the trusted read.
   template <typename F, typename T>
   concept decodes = detail::helpers::decodes<F, T>;

   // `psio::validate<T>(F{}, bytes)` → codec_status.
   template <typename F, typename T>
   concept validates = detail::helpers::validates<F, T>;

   // The validated write, `to_F_checked(v)` / `encode_F_checked(v, out)`.
   template <typename F, typename T, typename Sink = std::vector<char>>
   concept encodes_checked = detail::helpers::encodes_checked<F, T, Sink>;

   // The validated write is done by the encoder itself (pssz, key, json),
   // not by encoding and then validating the bytes.
   template <typename F, typename T, typename Sink = std::vector<char>>
   concept encodes_checked_natively = detail::helpers::encodes_checked_natively<F, T, Sink>;

   // ── The validating read, by tag ─────────────────────────────────────────
   //
   // `psio::decode_checked<T>(F{}, bytes)` is `from_F<T>(bytes)` for code
   // that has the format as a type: the bytes are held against the format
   // and against what T declares, and a failure is `psio::codec_abort`. Its
   // counterpart on the write side is `psio::encode_checked(F{}, v, out)`
   // (psio/helpers.hpp); the fast write and the trusted read are
   // `psio::encode` and `psio::decode<T>`.
   template <typename T, typename F>
      requires detail::helpers::decodes<F, T> && detail::helpers::validates<F, T>
   [[nodiscard]] T decode_checked(F fmt, std::span<const char> bytes)
   {
      return detail::helpers::decode_checked<T>(fmt, bytes);
   }

   // ── Everything, for one type ────────────────────────────────────────────
   //
   // A type a format does not take as a root (a FlatBuffers root is a table,
   // a Cap'n Proto root a record, a BSON document a record, a key an
   // orderable type) does not satisfy `encodes`, and so not this.
   template <typename F, typename T>
   concept codec_for = format<F> && encodes<F, T> && sizes<F, T> && decodes<F, T> &&
                       validates<F, T> && encodes_checked<F, T>;

   // ── Capabilities ────────────────────────────────────────────────────────

   template <typename F>
   concept text_format = format<F> && std::same_as<typename F::buffer_type, std::string>;

   template <typename F>
   concept appends_to_sink = format<F> && (F::sink_write == sink_write_kind::appends);

   template <typename F>
   concept frame_checked_read =
      format<F> && (F::trusted_read != trusted_read_kind::validated_bytes_only);

   // Views: true once the format's view header is included
   // (psio/pssz_view.hpp, ssz_view.hpp, wit_view.hpp, flatbuf_view.hpp,
   // capnp_view.hpp).
   template <typename F>
   concept view_format = format<F> && detail::helpers::read_view_format<F>;

   template <typename F>
   concept trusted_view_format =
      view_format<F> && requires { requires view_layout::traits<F>::trusted_views; };

   template <typename F>
   concept mutable_view_format = format<F> && detail::helpers::mutable_view_format<F>;

   // Reads and writes by a run-time schema S and a dynamic value V
   // (psio/dynamic_pssz.hpp, dynamic_json.hpp, dynamic_ssz.hpp,
   // dynamic_bin.hpp).
   template <typename F, typename S, typename V>
   concept dynamic_format =
      format<F> && requires(const S& schema, const V& value, std::span<const char> bytes) {
         ::psio::encode_dynamic(F{}, schema, value);
         ::psio::decode_dynamic(F{}, schema, bytes);
      };

}  // namespace psio
