#pragma once
//
// libraries/psio/cpp/benchmarks/compress_bench.hpp
//
// Compressed-wire-size cell for psio_bench_vs_externals.  Sits in a
// separate header so the existing per-format / per-shape bench code
// in bench_psio_vs_externals.cpp doesn't need to be refactored — the
// main file just calls run_compress_block(rows) once near the end.
//
// What it measures:
//
//   For each (format, shape) pair where the shape is in the
//   realistic-shape set, encode the shape with that format, then run
//   four compressors:
//
//     compress_lz4    — LZ4_compress_default (default, fast)
//     compress_lz4hc  — LZ4_compress_HC at level 9 (slow encode,
//                       better ratio, decompress speed unchanged)
//     compress_zstd1  — ZSTD_compress at level 1 (fast)
//     compress_zstd3  — ZSTD_compress at level 3 (zstd's default)
//
//   …plus two decompress timings:
//
//     decompress_lz4   — LZ4_decompress_safe on the lz4 (default)
//                        output
//     decompress_zstd  — ZSTD_decompress on the zstd-3 output
//
// Compression level doesn't affect decompression speed for either
// codec — one decompress measurement per codec suffices.
//
// Wire-bytes column:
//   compress_*  — COMPRESSED size (what hits the wire)
//   decompress_*— ORIGINAL raw size (what the consumer ends up with)
//
// zstd is gated by PSIO_HAVE_ZSTD (find_package); when zstd isn't
// available the zstd cells are quietly skipped.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/bson.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson.hpp>
#include <psio/pssz.hpp>
#include <psio/ssz.hpp>
#include <psio/wit.hpp>

#include "harness.hpp"
#include "shapes.hpp"

#if PSIO_HAVE_LZ4
#  include <lz4.h>
#  include <lz4hc.h>
#endif

#if PSIO_HAVE_ZSTD
#  include <zstd.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio_bench::compress {

   //  Tag types for plumbing each codec through one bench helper.
   struct lz4_default_tag {};
   struct lz4_hc9_tag     {};
   struct zstd1_tag       {};
   struct zstd3_tag       {};

   //  size_estimate(tag, src_len) — upper bound on compressed size.
#if PSIO_HAVE_LZ4
   inline int compress_bound_lz4(int n)
   {
      return LZ4_compressBound(n);
   }
#endif

   //  encode_one(tag, src, dst) — return compressed length, or 0 on
   //  buffer-too-small (dst must already be sized via the bound).
#if PSIO_HAVE_LZ4
   inline int encode_one(lz4_default_tag,
                          const char* src, int src_len,
                          char* dst, int dst_cap)
   {
      return LZ4_compress_default(src, dst, src_len, dst_cap);
   }
   inline int encode_one(lz4_hc9_tag,
                          const char* src, int src_len,
                          char* dst, int dst_cap)
   {
      // Level 9 is the LZ4_HC default (LZ4HC_CLEVEL_DEFAULT == 9).
      return LZ4_compress_HC(src, dst, src_len, dst_cap, 9);
   }
#endif
#if PSIO_HAVE_ZSTD
   inline std::size_t encode_one(zstd1_tag,
                                  const char* src, std::size_t src_len,
                                  char* dst, std::size_t dst_cap)
   {
      const std::size_t r = ZSTD_compress(dst, dst_cap, src, src_len, 1);
      return ZSTD_isError(r) ? 0 : r;
   }
   inline std::size_t encode_one(zstd3_tag,
                                  const char* src, std::size_t src_len,
                                  char* dst, std::size_t dst_cap)
   {
      const std::size_t r = ZSTD_compress(dst, dst_cap, src, src_len, 3);
      return ZSTD_isError(r) ? 0 : r;
   }
#endif

   //  Append one snapshot row.
   inline void emit(std::vector<snapshot_row>& out,
                     const std::string& shape, const std::string& format,
                     const std::string& op,
                     double ns_min, double ns_med, double cv_pct,
                     std::size_t wire_bytes,
                     std::size_t iters, int trials,
                     std::string notes = "")
   {
      out.push_back(snapshot_row{
         .shape      = shape,
         .format     = format,
         .library    = "psio",
         .mode       = "compressed",
         .op         = op,
         .ns_min     = ns_min,
         .ns_median  = ns_med,
         .cv_pct     = cv_pct,
         .wire_bytes = wire_bytes,
         .iters      = iters,
         .trials     = trials,
         .notes      = std::move(notes),
      });
   }

   //  cv_pct(timing) — same convention as bench_psio_cell.
   inline double cv_pct(const timing& t)
   {
      return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
   }

   //  Run one (Fmt, Shape) cell through the compression block.
   //  Pre-encodes K varied input buffers so the compressor sees real
   //  variation in the bench loop (anti-DCE), but reports the wire
   //  size of bufs[0] in the same K-fold-stable way the existing
   //  bench cells do.
   //
   //  Templated on Fmt so each format gets its own measurement.  The
   //  varied-input rotation matches bench_psio_cell's kAntiDceK
   //  convention; we keep K small (8) to limit memory pressure on
   //  the 50 KB BlockOfTransactions shape.
   template <typename Fmt, typename T>
   void run_cell(std::vector<snapshot_row>& out,
                  const std::string& shape, const std::string& format,
                  Fmt fmt, const T& v)
   {
      constexpr std::size_t K = 8;

      //  Pre-encode K buffers via psio::encode(fmt, …).  buf_t is
      //  whatever encode returns for this format — vector<char> for
      //  most, std::string for psio::json.
      using buf_t = std::remove_cvref_t<decltype(psio::encode(fmt, v))>;
      std::array<buf_t, K> raw;
      for (std::size_t i = 0; i < K; ++i)
      {
         //  Reuse the same vary() pipeline from the main bench file
         //  is impossible from a separate TU without a header — but
         //  we don't need it here because compression speed/ratio
         //  doesn't constant-fold across loop iterations the way the
         //  encode hot path does.  Identical K buffers are fine.
         raw[i] = psio::encode(fmt, v);
      }
      const std::size_t raw_bytes = raw[0].size();

      //  Each codec gets its own scratch buffer sized for the worst-
      //  case bound.  We reuse the buffer across iterations.

#if PSIO_HAVE_LZ4
      const int lz4_bound = LZ4_compressBound(static_cast<int>(raw_bytes));
      std::vector<char> lz4_dst(static_cast<std::size_t>(lz4_bound));
      std::size_t lz4_default_size = 0;
      std::size_t lz4_hc9_size     = 0;

      // ── compress_lz4 (default, fast) ───────────────────────────────
      {
         volatile std::size_t sink = 0;
         auto t = ns_per_iter(0u, [&](std::size_t i) {
            const auto& src = raw[i & (K - 1)];
            const int n = LZ4_compress_default(
               src.data(), lz4_dst.data(),
               static_cast<int>(src.size()),
               static_cast<int>(lz4_dst.size()));
            sink ^= static_cast<std::size_t>(n);
            if (i == 0) lz4_default_size = static_cast<std::size_t>(n);
         });
         (void)sink;
         if (lz4_default_size == 0)
            lz4_default_size = static_cast<std::size_t>(LZ4_compress_default(
               raw[0].data(), lz4_dst.data(),
               static_cast<int>(raw_bytes),
               static_cast<int>(lz4_dst.size())));
         emit(out, shape, format, "compress_lz4",
              t.min_ns, t.median_ns, cv_pct(t),
              lz4_default_size, t.iters, t.trials,
              "LZ4_compress_default (level=1, fast)");
      }

      // ── compress_lz4hc (level 9, better ratio) ─────────────────────
      {
         volatile std::size_t sink = 0;
         auto t = ns_per_iter(0u, [&](std::size_t i) {
            const auto& src = raw[i & (K - 1)];
            const int n = LZ4_compress_HC(
               src.data(), lz4_dst.data(),
               static_cast<int>(src.size()),
               static_cast<int>(lz4_dst.size()),
               9);
            sink ^= static_cast<std::size_t>(n);
            if (i == 0) lz4_hc9_size = static_cast<std::size_t>(n);
         });
         (void)sink;
         if (lz4_hc9_size == 0)
            lz4_hc9_size = static_cast<std::size_t>(LZ4_compress_HC(
               raw[0].data(), lz4_dst.data(),
               static_cast<int>(raw_bytes),
               static_cast<int>(lz4_dst.size()),
               9));
         emit(out, shape, format, "compress_lz4hc",
              t.min_ns, t.median_ns, cv_pct(t),
              lz4_hc9_size, t.iters, t.trials,
              "LZ4_compress_HC level=9");
      }

      // ── decompress_lz4 — single decompression speed measurement ────
      {
         //  Use the lz4_default_size output as the canonical input.
         //  Pre-build it once so the decompress loop just runs over
         //  the same compressed buffer.
         std::vector<char> compressed(lz4_default_size);
         {
            int n = LZ4_compress_default(
               raw[0].data(), compressed.data(),
               static_cast<int>(raw_bytes),
               static_cast<int>(compressed.size()));
            (void)n;
         }
         std::vector<char> dec_dst(raw_bytes);
         volatile std::size_t sink = 0;
         auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
            int n = LZ4_decompress_safe(
               compressed.data(), dec_dst.data(),
               static_cast<int>(compressed.size()),
               static_cast<int>(dec_dst.size()));
            sink ^= static_cast<std::size_t>(n);
            if (n > 0) sink ^= static_cast<unsigned char>(dec_dst[0]);
         });
         (void)sink;
         emit(out, shape, format, "decompress_lz4",
              t.min_ns, t.median_ns, cv_pct(t),
              raw_bytes, t.iters, t.trials,
              "LZ4_decompress_safe; wire_bytes is original-uncompressed size");
      }
#endif  // PSIO_HAVE_LZ4

#if PSIO_HAVE_ZSTD
      const std::size_t zstd_bound = ZSTD_compressBound(raw_bytes);
      std::vector<char> zstd_dst(zstd_bound);
      std::size_t zstd1_size = 0;
      std::size_t zstd3_size = 0;

      // ── compress_zstd1 ─────────────────────────────────────────────
      {
         volatile std::size_t sink = 0;
         auto t = ns_per_iter(0u, [&](std::size_t i) {
            const auto& src = raw[i & (K - 1)];
            const std::size_t n = ZSTD_compress(
               zstd_dst.data(), zstd_dst.size(),
               src.data(), src.size(), 1);
            const std::size_t got = ZSTD_isError(n) ? 0 : n;
            sink ^= got;
            if (i == 0) zstd1_size = got;
         });
         (void)sink;
         if (zstd1_size == 0)
         {
            std::size_t n = ZSTD_compress(
               zstd_dst.data(), zstd_dst.size(),
               raw[0].data(), raw[0].size(), 1);
            zstd1_size = ZSTD_isError(n) ? 0 : n;
         }
         emit(out, shape, format, "compress_zstd1",
              t.min_ns, t.median_ns, cv_pct(t),
              zstd1_size, t.iters, t.trials,
              "ZSTD_compress level=1");
      }

      // ── compress_zstd3 (zstd default) ──────────────────────────────
      {
         volatile std::size_t sink = 0;
         auto t = ns_per_iter(0u, [&](std::size_t i) {
            const auto& src = raw[i & (K - 1)];
            const std::size_t n = ZSTD_compress(
               zstd_dst.data(), zstd_dst.size(),
               src.data(), src.size(), 3);
            const std::size_t got = ZSTD_isError(n) ? 0 : n;
            sink ^= got;
            if (i == 0) zstd3_size = got;
         });
         (void)sink;
         if (zstd3_size == 0)
         {
            std::size_t n = ZSTD_compress(
               zstd_dst.data(), zstd_dst.size(),
               raw[0].data(), raw[0].size(), 3);
            zstd3_size = ZSTD_isError(n) ? 0 : n;
         }
         emit(out, shape, format, "compress_zstd3",
              t.min_ns, t.median_ns, cv_pct(t),
              zstd3_size, t.iters, t.trials,
              "ZSTD_compress level=3 (zstd default)");
      }

      // ── decompress_zstd ────────────────────────────────────────────
      {
         //  Decompression speed is independent of compression level —
         //  measure once on the level-3 output (the default users are
         //  most likely to ship).
         std::vector<char> compressed(zstd3_size);
         {
            std::size_t n = ZSTD_compress(
               compressed.data(), compressed.size(),
               raw[0].data(), raw[0].size(), 3);
            (void)n;
         }
         std::vector<char> dec_dst(raw_bytes);
         volatile std::size_t sink = 0;
         auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
            std::size_t n = ZSTD_decompress(
               dec_dst.data(), dec_dst.size(),
               compressed.data(), compressed.size());
            const std::size_t got = ZSTD_isError(n) ? 0 : n;
            sink ^= got;
            if (got > 0) sink ^= static_cast<unsigned char>(dec_dst[0]);
         });
         (void)sink;
         emit(out, shape, format, "decompress_zstd",
              t.min_ns, t.median_ns, cv_pct(t),
              raw_bytes, t.iters, t.trials,
              "ZSTD_decompress; wire_bytes is original-uncompressed size");
      }
#endif  // PSIO_HAVE_ZSTD

      //  Also emit the raw-size baseline row so the report has a
      //  zero-overhead anchor for the same (shape, format) tuple
      //  inside the compression block.  ns_min == 0 by convention —
      //  there's no encode time being measured here, just a record
      //  of the uncompressed wire size.
      emit(out, shape, format, "raw_size",
           0.0, 0.0, 0.0, raw_bytes, 0u, 0,
           "uncompressed psio::encode(fmt, v).size()");
   }

   //  Driver: run every supported (format, shape) pair in the
   //  realistic-shape set through run_cell().  All five realistic
   //  shapes encode in every format (fracpack now supports
   //  vector<variable-element> via the ported offset-table walker).
   //
   //  Public entry point: run_compress_block(rows).
   template <typename Fmt, typename T>
   struct fmt_supports : std::true_type {};

   template <typename Fmt, typename T>
   void cell(std::vector<snapshot_row>& out, const std::string& shape,
              const std::string& format, Fmt fmt, const T& v)
   {
      if constexpr (fmt_supports<Fmt, T>::value)
         run_cell(out, shape, format, fmt, v);
   }

   //  Register every (format) × (shape) pair on one realistic value.
   //  Mirrors run_shape() in the main bench file but for the
   //  compression block only.
   template <typename T>
   void run_shape(std::vector<snapshot_row>& out,
                   const std::string& shape, const T& v)
   {
      cell(out, shape, "ssz",      psio::ssz{},     v);
      cell(out, shape, "pssz",     psio::pssz{},    v);
      cell(out, shape, "fracpack", psio::frac32{},  v);
      cell(out, shape, "bin",      psio::bin{},     v);
      cell(out, shape, "borsh",    psio::borsh{},   v);
      cell(out, shape, "bincode",  psio::bincode{}, v);
      cell(out, shape, "avro",     psio::avro{},    v);
      cell(out, shape, "protobuf", psio::protobuf{},v);
      cell(out, shape, "msgpack",  psio::msgpack{}, v);
      cell(out, shape, "wit",      psio::wit{},     v);
      cell(out, shape, "json",     psio::json{},    v);
      cell(out, shape, "bson",     psio::bson{},    v);
      cell(out, shape, "capnp",    psio::capnp{},   v);
      cell(out, shape, "flatbuf",  psio::flatbuf{}, v);

      //  pjson uses from_struct/to_struct rather than the encode CPO.
      //  Its compressed-size story should be in the table too — emit
      //  via a raw byte-array path.
      {
         auto bytes = ::psio::from_struct(v);
         //  Wrap into a span<const char> compatible buffer for the
         //  same compressors.  We re-use run_cell's machinery by
         //  feeding it a synthetic format adapter.

#if PSIO_HAVE_LZ4
         const std::size_t raw_bytes = bytes.size();
         const int bound =
            LZ4_compressBound(static_cast<int>(raw_bytes));
         std::vector<char> lz4_dst(static_cast<std::size_t>(bound));

         std::size_t lz4_default_size = 0;
         std::size_t lz4_hc9_size     = 0;
         {
            volatile std::size_t sink = 0;
            auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
               int n = LZ4_compress_default(
                  reinterpret_cast<const char*>(bytes.data()),
                  lz4_dst.data(),
                  static_cast<int>(raw_bytes),
                  static_cast<int>(lz4_dst.size()));
               sink ^= static_cast<std::size_t>(n);
               lz4_default_size = static_cast<std::size_t>(n);
            });
            (void)sink;
            emit(out, shape, "pjson", "compress_lz4",
                 t.min_ns, t.median_ns, cv_pct(t),
                 lz4_default_size, t.iters, t.trials,
                 "LZ4_compress_default (level=1, fast)");
         }
         {
            volatile std::size_t sink = 0;
            auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
               int n = LZ4_compress_HC(
                  reinterpret_cast<const char*>(bytes.data()),
                  lz4_dst.data(),
                  static_cast<int>(raw_bytes),
                  static_cast<int>(lz4_dst.size()),
                  9);
               sink ^= static_cast<std::size_t>(n);
               lz4_hc9_size = static_cast<std::size_t>(n);
            });
            (void)sink;
            emit(out, shape, "pjson", "compress_lz4hc",
                 t.min_ns, t.median_ns, cv_pct(t),
                 lz4_hc9_size, t.iters, t.trials,
                 "LZ4_compress_HC level=9");
         }
         {
            std::vector<char> compressed(lz4_default_size);
            int n = LZ4_compress_default(
               reinterpret_cast<const char*>(bytes.data()),
               compressed.data(),
               static_cast<int>(raw_bytes),
               static_cast<int>(compressed.size()));
            (void)n;
            std::vector<char> dec_dst(raw_bytes);
            volatile std::size_t sink = 0;
            auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
               int dn = LZ4_decompress_safe(
                  compressed.data(), dec_dst.data(),
                  static_cast<int>(compressed.size()),
                  static_cast<int>(dec_dst.size()));
               sink ^= static_cast<std::size_t>(dn);
            });
            (void)sink;
            emit(out, shape, "pjson", "decompress_lz4",
                 t.min_ns, t.median_ns, cv_pct(t),
                 raw_bytes, t.iters, t.trials,
                 "LZ4_decompress_safe; wire_bytes is original size");
         }
#endif

#if PSIO_HAVE_ZSTD
         const std::size_t zb = ZSTD_compressBound(bytes.size());
         std::vector<char> zstd_dst(zb);
         std::size_t z1 = 0;
         std::size_t z3 = 0;
         {
            volatile std::size_t sink = 0;
            auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
               std::size_t n = ZSTD_compress(
                  zstd_dst.data(), zstd_dst.size(),
                  bytes.data(), bytes.size(), 1);
               z1 = ZSTD_isError(n) ? 0 : n;
               sink ^= z1;
            });
            (void)sink;
            emit(out, shape, "pjson", "compress_zstd1",
                 t.min_ns, t.median_ns, cv_pct(t),
                 z1, t.iters, t.trials,
                 "ZSTD_compress level=1");
         }
         {
            volatile std::size_t sink = 0;
            auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
               std::size_t n = ZSTD_compress(
                  zstd_dst.data(), zstd_dst.size(),
                  bytes.data(), bytes.size(), 3);
               z3 = ZSTD_isError(n) ? 0 : n;
               sink ^= z3;
            });
            (void)sink;
            emit(out, shape, "pjson", "compress_zstd3",
                 t.min_ns, t.median_ns, cv_pct(t),
                 z3, t.iters, t.trials,
                 "ZSTD_compress level=3 (zstd default)");
         }
         {
            std::vector<char> compressed(z3);
            std::size_t n = ZSTD_compress(
               compressed.data(), compressed.size(),
               bytes.data(), bytes.size(), 3);
            (void)n;
            std::vector<char> dec_dst(bytes.size());
            volatile std::size_t sink = 0;
            auto t = ns_per_iter(0u, [&](std::size_t /*i*/) {
               std::size_t dn = ZSTD_decompress(
                  dec_dst.data(), dec_dst.size(),
                  compressed.data(), compressed.size());
               sink ^= ZSTD_isError(dn) ? 0u : dn;
            });
            (void)sink;
            emit(out, shape, "pjson", "decompress_zstd",
                 t.min_ns, t.median_ns, cv_pct(t),
                 bytes.size(), t.iters, t.trials,
                 "ZSTD_decompress; wire_bytes is original size");
         }
#endif
         emit(out, shape, "pjson", "raw_size",
              0.0, 0.0, 0.0, bytes.size(), 0u, 0,
              "uncompressed from_struct(v).size()");
      }
   }

   //  Public entry point — caller invokes once per bench run.
   inline void run_compress_block(std::vector<snapshot_row>& out)
   {
      run_shape(out, "HttpApiResponse",
                ::psio_bench::realistic_http_response());
      run_shape(out, "BlockOfTransactions(100)",
                ::psio_bench::realistic_block(100));
      run_shape(out, "ConfigTree",
                ::psio_bench::realistic_config_tree());
      run_shape(out, "TimeSeriesChunk(1024)",
                ::psio_bench::realistic_time_series(1024));
      run_shape(out, "MixedDocument",
                ::psio_bench::realistic_mixed_document());
   }

}  // namespace psio_bench::compress
