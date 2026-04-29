// libraries/psio/cpp/benchmarks/bench_varint.cpp
//
// Microbenchmark for the three varint algorithms in
// libraries/psio/cpp/include/psio/varint/. Reports ns/op and bytes/sec
// for encode and decode under four value distributions, with a separate
// section comparing prefix2 BE vs LE on identical inputs.
//
// The fast paths are scalar in this commit (NEON / BMI2 land in the
// follow-up); the bench timing harness, value distributions, and
// reporting are landed first so subsequent fast-path commits can be
// graded against a stable baseline.
//
// Build with: cmake -DPSIO3_ENABLE_BENCHMARKS=ON ...
// Run:        ./bin/psio_bench_varint
//

#include <psio/varint/varint.hpp>

#include "harness.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace v = psio::varint;
using psio_bench::ns_per_iter;
using psio_bench::timing;

namespace {

   // ── Value distributions ─────────────────────────────────────────
   enum class corpus { tiny, small, mixed, max };

   const char* corpus_name(corpus c)
   {
      switch (c)
      {
         case corpus::tiny:  return "tiny  (<32)";
         case corpus::small: return "small (<64k)";
         case corpus::mixed: return "mixed (mostly small, some max)";
         case corpus::max:   return "max   (full u64)";
      }
      return "?";
   }

   constexpr std::size_t corpus_size = 4096;

   std::vector<std::uint64_t> build_corpus(corpus c, std::uint64_t cap)
   {
      std::mt19937_64                  rng(0xC0FFEEULL ^ static_cast<int>(c));
      std::vector<std::uint64_t>       out;
      out.reserve(corpus_size);
      auto clip = [cap](std::uint64_t v) { return v & cap; };
      switch (c)
      {
         case corpus::tiny:
            for (std::size_t i = 0; i < corpus_size; ++i)
               out.push_back(clip(rng() & 0x1F));
            break;
         case corpus::small:
            for (std::size_t i = 0; i < corpus_size; ++i)
               out.push_back(clip(rng() & 0xFFFF));
            break;
         case corpus::mixed:
         {
            std::uniform_int_distribution<int> bucket(0, 99);
            for (std::size_t i = 0; i < corpus_size; ++i)
            {
               const int  b = bucket(rng);
               std::uint64_t v;
               if (b < 70)      v = rng() & 0x1F;          // 70 % tiny
               else if (b < 90) v = rng() & 0xFFFF;        // 20 % small
               else if (b < 99) v = rng() & 0xFFFFFFFFu;   //  9 % medium
               else             v = rng();                 //  1 % full
               out.push_back(clip(v));
            }
            break;
         }
         case corpus::max:
            for (std::size_t i = 0; i < corpus_size; ++i)
               out.push_back(clip(rng()));
            break;
      }
      return out;
   }

   // Pre-encode a corpus into a flat wire-byte buffer + offset table.
   // Decode benches walk the offset table to time each call individually.
   template <typename Encode>
   struct encoded
   {
      std::vector<std::uint8_t> bytes;
      std::vector<std::size_t>  offsets;  // size = corpus_size + 1
   };

   template <typename Encode>
   encoded<Encode> pre_encode(const std::vector<std::uint64_t>& src,
                              Encode                            enc,
                              std::size_t                       max_bytes)
   {
      encoded<Encode> e;
      e.bytes.resize(src.size() * max_bytes + 16);  // +16 tail pad for fast loads
      e.offsets.reserve(src.size() + 1);
      std::size_t pos = 0;
      for (auto v : src)
      {
         e.offsets.push_back(pos);
         pos += enc(e.bytes.data() + pos, v);
      }
      e.offsets.push_back(pos);
      e.bytes.resize(pos + 16);  // keep tail pad for safety
      return e;
   }

   // Anti-DCE: a compiler fence on a value forces the optimizer to
   // treat the value as live (so it can't elide the work that
   // produced it) without introducing a memory side-effect or a load
   // dependency that would itself dominate the timing. Equivalent to
   // Google benchmark's `DoNotOptimize`. The earlier version used
   // `volatile g_sink ^= r.value` which forced a load+xor+store every
   // iteration — at ~3-5 cycles for the dependent RMW, that was
   // larger than the actual decode at sub-nanosecond/op.
#if defined(__GNUC__) || defined(__clang__)
#  define DO_NOT_OPTIMIZE(x) __asm__ __volatile__("" : "+r"(x) : : "memory")
#else
#  define DO_NOT_OPTIMIZE(x) (void)(x)  /* fallback — measurement may regress */
#endif

   // ── Bench helpers ───────────────────────────────────────────────

   struct row
   {
      std::string algo;
      std::string corpus;
      double      enc_ns;
      double      dec_ns;
      double      enc_cv;
      double      dec_cv;
      std::size_t wire_bytes;
   };

   std::vector<row> g_rows;

   template <typename Encode, typename Decode>
   void measure(const char* algo, corpus c, std::uint64_t cap,
                Encode enc, Decode dec, std::size_t max_bytes)
   {
      const auto src = build_corpus(c, cap);

      const auto pre = pre_encode(src, enc, max_bytes);

      // Encode: write each value into a thread-local scratch buffer.
      std::vector<std::uint8_t> scratch(max_bytes + 16);
      const std::size_t         n_src = src.size();
      std::size_t               idx_e = 0;
      const auto                t_enc = ns_per_iter(0, [&](std::size_t) {
         const auto v = src[idx_e];
         idx_e        = (idx_e + 1 == n_src) ? 0 : idx_e + 1;
         std::uint8_t* sptr = scratch.data();
         auto          wrote = enc(sptr, v);
         DO_NOT_OPTIMIZE(wrote);
         DO_NOT_OPTIMIZE(sptr);
         __asm__ __volatile__("" ::: "memory");  // scratch contents live
      });

      // Decode: walk the pre-encoded buffer. Cycle the index with a
      // branch instead of `%`, since modulo costs ~3-5 cycles and at
      // sub-ns decode that distorts the comparison between algos.
      const std::uint8_t* base   = pre.bytes.data();
      const std::size_t   bsize  = pre.bytes.size();
      std::size_t         idx_d = 0;
      const auto          t_dec = ns_per_iter(0, [&](std::size_t) {
         const auto offset = pre.offsets[idx_d];
         idx_d             = (idx_d + 1 == n_src) ? 0 : idx_d + 1;
         auto r            = dec(base + offset, bsize - offset);
         DO_NOT_OPTIMIZE(r.value);
         DO_NOT_OPTIMIZE(r.len);
      });

      const double enc_cv = (t_enc.min_ns == 0.0) ? 0.0
                                                 : 100.0 * t_enc.stddev_ns
                                                      / t_enc.min_ns;
      const double dec_cv = (t_dec.min_ns == 0.0) ? 0.0
                                                 : 100.0 * t_dec.stddev_ns
                                                      / t_dec.min_ns;
      g_rows.push_back({algo, corpus_name(c), t_enc.min_ns, t_dec.min_ns,
                        enc_cv, dec_cv, pre.offsets.back()});
   }

   void print_table()
   {
      std::printf(
         "\n| algorithm        | corpus                          | enc ns | "
         "dec ns | enc cv | dec cv | wire (4096 vals) |\n");
      std::printf(
         "|------------------|---------------------------------|------:|-"
         "-----:|------:|------:|----------------:|\n");
      for (const auto& r : g_rows)
         std::printf("| %-16s | %-31s | %5.2f | %5.2f | %4.1f%% | %4.1f%% | "
                     "%14zu |\n",
                     r.algo.c_str(), r.corpus.c_str(), r.enc_ns, r.dec_ns,
                     r.enc_cv, r.dec_cv, r.wire_bytes);
   }

}  // namespace

int main()
{
   // Sanity baseline: empty body cost.  This is the harness's
   // per-iter floor for `ns_per_iter`; any number printed below has
   // ~this much loop bookkeeping baked in, so anything close to it
   // for actual work indicates a noise-bound measurement.
   {
      const auto t = ns_per_iter(0, [](std::size_t i) { DO_NOT_OPTIMIZE(i); });
      std::printf("\n[harness floor: empty-body iter = %.3f ns, cv = %.1f%%]\n",
                  t.min_ns, 100.0 * t.stddev_ns / t.min_ns);
   }

   constexpr std::uint64_t cap_u64    = ~0ULL;
   constexpr std::uint64_t cap_prefix2 = (1ULL << 62) - 1;

   for (auto c :
        {corpus::tiny, corpus::small, corpus::mixed, corpus::max})
   {
      // LEB128 (uleb64) — full u64 range. Scalar baseline.
      measure(
         "leb128 scalar",
         c, cap_u64,
         [](std::uint8_t* buf, std::uint64_t v) {
            return v::leb128::scalar::encode_u64(buf, v);
         },
         [](const std::uint8_t* p, std::size_t n) {
            return v::leb128::scalar::decode_u64(p, n);
         },
         v::leb128::max_bytes_u64);

      // LEB128 fast path — NEON on aarch64, scalar elsewhere. Same
      // encode bytes (encode is scalar in both); decode goes through
      // the NEON length-detect + branchless extract.
      measure(
         "leb128 fast",
         c, cap_u64,
         [](std::uint8_t* buf, std::uint64_t v) {
            return v::leb128::fast::encode_u64(buf, v);
         },
         [](const std::uint8_t* p, std::size_t n) {
            return v::leb128::fast::decode_u64(p, n);
         },
         v::leb128::max_bytes_u64);

      // prefix3 — full u64 range.
      measure(
         "prefix3",
         c, cap_u64,
         [](std::uint8_t* buf, std::uint64_t v) {
            return v::prefix3::scalar::encode_u64(buf, v);
         },
         [](const std::uint8_t* p, std::size_t n) {
            return v::prefix3::scalar::decode_u64(p, n);
         },
         v::prefix3::max_bytes_u64);

      // prefix2 BE (canonical QUIC) — 62-bit clamp.
      measure(
         "prefix2 BE",
         c, cap_prefix2,
         [](std::uint8_t* buf, std::uint64_t v) {
            return v::prefix2::be::scalar::encode_u64(buf, v);
         },
         [](const std::uint8_t* p, std::size_t n) {
            return v::prefix2::be::fast::decode_u64(p, n);
         },
         v::prefix2::max_bytes_u64);

      // prefix2 LE (sibling for endianness comparison) — 62-bit clamp.
      measure(
         "prefix2 LE",
         c, cap_prefix2,
         [](std::uint8_t* buf, std::uint64_t v) {
            return v::prefix2::le::scalar::encode_u64(buf, v);
         },
         [](const std::uint8_t* p, std::size_t n) {
            return v::prefix2::le::fast::decode_u64(p, n);
         },
         v::prefix2::max_bytes_u64);
   }

   print_table();
   return 0;
}
