#pragma once
//
// psio/transcode.hpp — format-to-format transcoding.
//
// Two entry points, covering the compile-time and runtime cases of the
// schema-is-contract DX (design § 5.2.6):
//
//   template <typename T, typename FromFmt, typename ToFmt>
//   auto psio::transcode<T>(FromFmt, std::span<const char> src, ToFmt)
//        → ToFmt::buffer_type
//
//       Compile-time T form. Decodes through `T` and re-encodes. This is
//       always safe and works for any pair of formats that both have
//       encode/decode CPOs for T.
//
//   psio::transcode(schema, fromFmt, src, toFmt)
//        → dst-side buffer_type
//
//       Runtime-schema form. Walks the wire through dynamic_value then
//       re-encodes on the other side. Requires both formats to supply a
//       dynamic codec (currently JSON; SSZ/frac/bin lands in follow-up
//       14d as `tag_invoke` overloads of encode_dynamic/decode_dynamic
//       for each format tag).
//
// The dynamic path accepts JSON on either side today. The template path
// accepts any pair. Between the two, this is enough for most users in
// MVP; the dynamic/bin/SSZ decoders arrive later without changing this
// surface.

#include <psio/cpo.hpp>
#include <psio/dynamic_bin.hpp>
#include <psio/dynamic_frac.hpp>
#include <psio/dynamic_json.hpp>
#include <psio/dynamic_pssz.hpp>
#include <psio/dynamic_ssz.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/json.hpp>
#include <psio/schema.hpp>

#include <span>
#include <string>
#include <vector>

namespace psio {

   // ── Compile-time T transcode ───────────────────────────────────────────

   template <typename T, typename FromFmt, typename ToFmt>
   auto transcode(FromFmt src_tag, std::span<const char> src_bytes,
                  ToFmt dst_tag)
   {
      T v = ::psio::decode<T>(src_tag, src_bytes);
      return ::psio::encode(dst_tag, v);
   }

   // ── Runtime-schema transcode ───────────────────────────────────────────
   //
   // Only JSON↔JSON works in this phase — it's the only format with a
   // dynamic codec. Cross-format runtime transcode becomes available as
   // each format's dynamic codec lands.

   inline std::string transcode_via_json(const schema&         sc,
                                         std::span<const char> src_bytes)
   {
      dynamic_value dv = json_decode_dynamic(sc, src_bytes);
      return json_encode_dynamic(sc, dv);
   }

   // Cross-format runtime transcode: SSZ bytes in → JSON string out,
   // driven entirely by the runtime schema. No compile-time T.
   inline std::string transcode_ssz_to_json(const schema&         sc,
                                            std::span<const char> src_bytes)
   {
      dynamic_value dv = ssz_decode_dynamic(sc, src_bytes);
      return json_encode_dynamic(sc, dv);
   }

   // Inverse: JSON string in → SSZ bytes out.
   inline std::vector<char>
   transcode_json_to_ssz(const schema& sc, std::span<const char> src_bytes)
   {
      dynamic_value dv = json_decode_dynamic(sc, src_bytes);
      return ssz_encode_dynamic(sc, dv);
   }

   // ── Generic runtime-schema transcode ───────────────────────────────────
   //
   // Works across any pair of formats that both provide dynamic-codec
   // tag_invoke overloads (encode_dynamic / decode_dynamic CPOs).
   // Today that covers ssz and json; other formats get the same two
   // overloads in follow-up work.
   template <typename FromFmt, typename ToFmt>
   auto transcode(FromFmt               from_fmt,
                  const schema&         sc,
                  std::span<const char> src_bytes,
                  ToFmt                 to_fmt)
   {
      dynamic_value dv = ::psio::decode_dynamic(from_fmt, sc, src_bytes);
      return ::psio::encode_dynamic(to_fmt, sc, dv);
   }

}  // namespace psio
