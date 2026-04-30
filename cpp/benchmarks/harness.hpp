#pragma once
//
// libraries/psio/cpp/benchmarks/harness.hpp — timing + reporting helpers.
//
// Sub-ns ops (e.g. small-record decode at ~0.2 ns/iter) need more
// iterations than slower ones, otherwise per-trial timer noise
// dominates. The harness auto-tunes:
//
//   1. Calibration trial — run a small batch (1024 iters), measure
//      total ns, decide how many iters fit in the target trial
//      duration (default 50 ms) given the per-iter cost. Cap at
//      iters_max so very fast ops don't blow trial time.
//   2. Warmup trial — run once at the calibrated iter count, discard
//      result. Burns in cache and branch predictor.
//   3. Measurement trials — run `trials` (default 7) times, return
//      min + median + stddev.
//
// `min` is what regressions are usually compared on (least-noisy
// estimate of intrinsic cost). `stddev / min` is reported alongside so
// callers can see whether two cells are statistically distinguishable.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <string_view>
#include <vector>

namespace psio_bench {

   struct timing
   {
      double      min_ns    = 0.0;
      double      median_ns = 0.0;
      double      stddev_ns = 0.0;
      std::size_t iters     = 0;  // iters per trial actually used
      int         trials    = 0;
   };

   namespace detail {
      // Run body N times in a tight loop, return total ns elapsed.
      template <typename F>
      inline double run_batch(std::size_t iters, F&& body) noexcept
      {
         auto t0 = std::chrono::steady_clock::now();
         for (std::size_t i = 0; i < iters; ++i)
            body(i);
         auto t1 = std::chrono::steady_clock::now();
         return std::chrono::duration<double, std::nano>(t1 - t0).count();
      }
   }

   // Run `body`, auto-scaling iterations so each trial takes roughly
   // `target_trial_ns` (default 50 ms). Then runs `trials` measurement
   // passes (default 7) plus a discarded warmup. Returns the lowest
   // per-iter cost (min), the median, and the trial-spread stddev.
   template <typename F>
   timing ns_per_iter(std::size_t /*iters_hint*/, F&& body,
                       int trials = 7,
                       double target_trial_ns = 50'000'000.0,  // 50 ms
                       std::size_t iters_max = 4'000'000)
   {
      // ── 1. Calibration: estimate per-iter cost.
      // Start with 1024 iters; if the batch was implausibly short
      // (timer resolution), scale up to 64k.
      std::size_t cal_iters = 1024;
      double      cal_ns    = detail::run_batch(cal_iters, body);
      while (cal_ns < 100'000.0 && cal_iters < (1u << 20))
      {
         cal_iters *= 4;
         cal_ns = detail::run_batch(cal_iters, body);
      }
      const double per_iter_estimate = cal_ns / static_cast<double>(cal_iters);
      const double scaled_iters_d =
         per_iter_estimate > 0.0
            ? target_trial_ns / per_iter_estimate
            : static_cast<double>(iters_max);
      std::size_t iters =
         scaled_iters_d > static_cast<double>(iters_max)
            ? iters_max
            : (scaled_iters_d < 1024.0 ? std::size_t{1024}
                                       : static_cast<std::size_t>(scaled_iters_d));

      // ── 2. Warmup (discarded).
      (void)detail::run_batch(iters, body);

      // ── 3. Measurement trials.
      std::vector<double> samples;
      samples.reserve(trials);
      for (int t = 0; t < trials; ++t)
      {
         double total = detail::run_batch(iters, body);
         samples.push_back(total / static_cast<double>(iters));
      }
      std::sort(samples.begin(), samples.end());
      const double min  = samples.front();
      const double med  = samples[samples.size() / 2];
      double       mean = 0.0;
      for (double s : samples) mean += s;
      mean /= static_cast<double>(samples.size());
      double var = 0.0;
      for (double s : samples) var += (s - mean) * (s - mean);
      var /= static_cast<double>(samples.size());
      const double sd = std::sqrt(var);
      return {min, med, sd, iters, trials};
   }

   // One measurement cell. `library` is "v1" / "v3" / competitor name.
   struct result_row
   {
      std::string shape;
      std::string format;
      std::string library;
      double      enc_ns_min     = 0.0;
      double      dec_ns_min     = 0.0;
      double      val_ns_min     = 0.0;
      double      size_ns_min    = 0.0;
      // Coefficient of variation (stddev / min) for the encode trial,
      // expressed as a percent. Values above ~5 mean the cell's noise
      // exceeds the threshold for treating small ratios as signal.
      double      enc_cv_pct     = 0.0;
      double      dec_cv_pct     = 0.0;
      std::size_t wire_bytes     = 0;
   };

   // Find the paired v1/v3 rows (same shape + format) and compute the
   // v3/v1 ratio for each op. Returns 0.0 where the counterpart row is
   // missing (e.g., a format v1 doesn't support).
   struct pair_ratios
   {
      double enc = 0.0;
      double dec = 0.0;
      double val = 0.0;
      double sz  = 0.0;
      double wire = 0.0;
   };

   inline pair_ratios ratios(const result_row& a, const result_row& b) noexcept
   {
      auto div = [](double n, double d) { return d == 0.0 ? 0.0 : n / d; };
      return {div(a.enc_ns_min, b.enc_ns_min),
              div(a.dec_ns_min, b.dec_ns_min),
              div(a.val_ns_min, b.val_ns_min),
              div(a.size_ns_min, b.size_ns_min),
              (b.wire_bytes == 0 ? 0.0
                                 : static_cast<double>(a.wire_bytes) /
                                      static_cast<double>(b.wire_bytes))};
   }

   // ── Report writers ────────────────────────────────────────────────

   inline std::string format_ns(double ns)
   {
      char buf[32];
      if (ns == 0.0)
         std::snprintf(buf, sizeof(buf), "   n/a");
      else if (ns < 10.0)
         std::snprintf(buf, sizeof(buf), "%6.2f", ns);
      else if (ns < 1000.0)
         std::snprintf(buf, sizeof(buf), "%6.1f", ns);
      else if (ns < 1e6)
         std::snprintf(buf, sizeof(buf), "%5.1fk", ns / 1000.0);
      else
         std::snprintf(buf, sizeof(buf), "%5.1fM", ns / 1e6);
      return buf;
   }

   inline std::string format_bytes(std::size_t n)
   {
      char buf[32];
      if (n < 1024)
         std::snprintf(buf, sizeof(buf), "%zuB", n);
      else if (n < 1024 * 1024)
         std::snprintf(buf, sizeof(buf), "%.1fK", n / 1024.0);
      else
         std::snprintf(buf, sizeof(buf), "%.2fM", n / (1024.0 * 1024.0));
      return buf;
   }

   inline std::string format_ratio(double r)
   {
      char buf[16];
      if (r == 0.0)
         std::snprintf(buf, sizeof(buf), "  —  ");
      else
         std::snprintf(buf, sizeof(buf), "%.2fx", r);
      return buf;
   }

   inline std::string format_pct(double v)
   {
      char buf[16];
      if (v == 0.0)
         std::snprintf(buf, sizeof(buf), "  -");
      else
         std::snprintf(buf, sizeof(buf), "%4.1f%%", v);
      return buf;
   }

   // Emit a markdown table grouped by (shape, format), with one row per
   // library and ratio rows. Writes to `out` (e.g., std::cout or a file
   // stream).
   template <typename Out>
   void report_table(Out& out, const std::vector<result_row>& rows)
   {
      // Gather unique (shape, format) pairs in insertion order.
      std::vector<std::pair<std::string, std::string>> pairs;
      for (const auto& r : rows)
      {
         auto key = std::make_pair(r.shape, r.format);
         if (std::find(pairs.begin(), pairs.end(), key) == pairs.end())
            pairs.push_back(key);
      }

      auto fmt = [&](std::string_view s) {
         out << s;
      };
      fmt("| shape | format | library | enc ns | dec ns | val ns | "
          "size ns | enc cv | dec cv | wire |\n");
      fmt("|---|---|---|---:|---:|---:|---:|---:|---:|---:|\n");

      for (const auto& [shape, format] : pairs)
      {
         for (const auto& r : rows)
         {
            if (r.shape == shape && r.format == format)
            {
               out << "| " << shape << " | " << format << " | "
                   << r.library << " | " << format_ns(r.enc_ns_min)
                   << " | " << format_ns(r.dec_ns_min) << " | "
                   << format_ns(r.val_ns_min) << " | "
                   << format_ns(r.size_ns_min) << " | "
                   << format_pct(r.enc_cv_pct) << " | "
                   << format_pct(r.dec_cv_pct) << " | "
                   << format_bytes(r.wire_bytes) << " |\n";
            }
         }
         // Ratio row for v3 vs v1 when both present.
         const result_row* v1 = nullptr;
         const result_row* v3 = nullptr;
         for (const auto& r : rows)
         {
            if (r.shape == shape && r.format == format)
            {
               if (r.library == "v1") v1 = &r;
               else if (r.library == "v3") v3 = &r;
            }
         }
         if (v1 && v3)
         {
            auto rt = ratios(*v3, *v1);
            // Combined CV — sqrt(cv_v3^2 + cv_v1^2) gives a rough
            // bound on the noise floor of the ratio. If the apparent
            // delta is within that bound the cells are
            // statistically indistinguishable.
            auto comb = [](double a, double b) {
               return std::sqrt(a * a + b * b);
            };
            const double enc_cv = comb(v3->enc_cv_pct, v1->enc_cv_pct);
            const double dec_cv = comb(v3->dec_cv_pct, v1->dec_cv_pct);
            out << "| " << shape << " | " << format
                << " | **v3/v1** | " << format_ratio(rt.enc) << " | "
                << format_ratio(rt.dec) << " | " << format_ratio(rt.val)
                << " | " << format_ratio(rt.sz) << " | "
                << format_pct(enc_cv) << " | " << format_pct(dec_cv)
                << " | " << format_ratio(rt.wire) << " |\n";
         }
      }
   }

   template <typename Out>
   void report_csv(Out& out, const std::vector<result_row>& rows)
   {
      out << "shape,format,library,enc_ns,dec_ns,val_ns,size_ns,"
             "enc_cv_pct,dec_cv_pct,wire_bytes\n";
      for (const auto& r : rows)
      {
         out << r.shape << "," << r.format << "," << r.library << ","
             << r.enc_ns_min << "," << r.dec_ns_min << ","
             << r.val_ns_min << "," << r.size_ns_min << ","
             << r.enc_cv_pct << "," << r.dec_cv_pct << ","
             << r.wire_bytes << "\n";
      }
   }

   // ── Snapshot CSV: one file per bench run, one row per cell ───────
   //
   // Long-format CSV — every row carries the full platform tuple so a
   // regression-diff tool can join two snapshots on
   // (shape, format, library, mode, op) without separately tracking
   // metadata.
   //
   // File path convention:
   //    bench_snapshots/perf_<UTC-ISO-compact>_<commit>.csv
   // e.g.
   //    bench_snapshots/perf_20260427T172345Z_b1536e5.csv
   //
   // Default `bench_snapshots/` is gitignored; commit a snapshot only
   // when intentionally accepting it as a new baseline (`git add -f`).

   struct platform_info
   {
      std::string timestamp_utc;   // ISO 8601, "20260427T172345Z"
      std::string commit_short;    // "b1536e5", "uncommitted", or env value
      std::string os;              // "darwin" / "linux" / "freebsd" / "other"
      std::string arch;            // "arm64" / "x86_64" / "other"
      std::string cpu;             // free-form, may be empty
      std::string compiler;        // "clang 22.1.4" / "gcc 14.2"
      std::string cxx_std;         // "c++23"
      std::string build_type;      // "Release" / "Debug" — from CMake
   };

   //  CSV cells should never embed commas or newlines; these helpers
   //  quote-and-escape conservatively for the few free-form fields
   //  (notes, cpu) where surprise input is possible.
   inline std::string csv_escape(std::string_view s)
   {
      bool needs_quote = false;
      for (char c : s)
         if (c == ',' || c == '"' || c == '\n' || c == '\r')
         {
            needs_quote = true;
            break;
         }
      if (!needs_quote)
         return std::string{s};
      std::string out;
      out.reserve(s.size() + 2);
      out.push_back('"');
      for (char c : s)
      {
         if (c == '"')
            out.push_back('"');
         out.push_back(c);
      }
      out.push_back('"');
      return out;
   }

   //  ISO-ish UTC timestamp for filenames (no separators, Z suffix).
   inline std::string utc_timestamp_compact(std::time_t t = std::time(nullptr))
   {
      std::tm tm{};
#ifdef _WIN32
      gmtime_s(&tm, &t);
#else
      gmtime_r(&t, &tm);
#endif
      char buf[24];
      std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
      return std::string{buf};
   }

   //  Git short hash via `git rev-parse --short HEAD`.  Falls back to
   //  the env var PSIO_BENCH_COMMIT (so CI can override) or the
   //  literal "uncommitted" if neither resolves.  popen is portable
   //  enough for benches; not for production code.
   inline std::string detect_commit_short()
   {
      if (const char* env = std::getenv("PSIO_BENCH_COMMIT");
          env && *env)
         return env;
      std::FILE* p = popen("git rev-parse --short HEAD 2>/dev/null", "r");
      if (!p)
         return "uncommitted";
      char buf[32]{};
      char* line = std::fgets(buf, sizeof(buf), p);
      pclose(p);
      if (!line)
         return "uncommitted";
      std::string s{line};
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
         s.pop_back();
      return s.empty() ? std::string{"uncommitted"} : s;
   }

   inline std::string detect_os()
   {
#if defined(__APPLE__)
      return "darwin";
#elif defined(__linux__)
      return "linux";
#elif defined(__FreeBSD__)
      return "freebsd";
#else
      return "other";
#endif
   }

   inline std::string detect_arch()
   {
#if defined(__aarch64__) || defined(_M_ARM64)
      return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
      return "x86_64";
#else
      return "other";
#endif
   }

   inline std::string detect_compiler()
   {
      std::ostringstream os_;
#if defined(__clang__)
      os_ << "clang " << __clang_major__ << '.' << __clang_minor__
          << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
      os_ << "gcc " << __GNUC__ << '.' << __GNUC_MINOR__ << '.'
          << __GNUC_PATCHLEVEL__;
#else
      os_ << "unknown";
#endif
      return os_.str();
   }

   inline std::string detect_cxx_std()
   {
#if __cplusplus >= 202302L
      return "c++23";
#elif __cplusplus >= 202002L
      return "c++20";
#elif __cplusplus >= 201703L
      return "c++17";
#else
      return "c++<=14";
#endif
   }

   inline platform_info detect_platform_info()
   {
      platform_info p;
      p.timestamp_utc = utc_timestamp_compact();
      p.commit_short  = detect_commit_short();
      p.os            = detect_os();
      p.arch          = detect_arch();
      p.cpu           = "";  // CPU model lookup is platform-specific;
                              //  leave empty for now, fill via env/CMake
                              //  if needed.
      if (const char* env = std::getenv("PSIO_BENCH_CPU"); env && *env)
         p.cpu = env;
      p.compiler   = detect_compiler();
      p.cxx_std    = detect_cxx_std();
#if defined(NDEBUG)
      p.build_type = "Release";
#else
      p.build_type = "Debug";
#endif
      return p;
   }

   //  One row per (shape, format, library, mode, op) — long format.
   struct snapshot_row
   {
      std::string shape;
      std::string format;       // "ssz", "frac32", "(native)" for competitors
      std::string library;      // "psio", "capnp", "flatbuffers",
                                //  "msgpack-cxx", "protobuf", "sszpp"
      std::string mode;         // "static" / "dynamic"
      std::string op;           // "size_of" / "encode_rvalue" /
                                //  "encode_sink" / "decode" /
                                //  "validate" / "view_one" / "view_all"
      double      ns_min      = 0.0;
      double      ns_median   = 0.0;
      double      cv_pct      = 0.0;
      std::size_t wire_bytes  = 0;  // 0 if op doesn't produce bytes
      std::size_t iters       = 0;
      int         trials      = 0;
      std::string notes;        // free-form, optional
   };

   inline constexpr std::string_view kSnapshotCsvHeader =
      "timestamp_utc,commit,os,arch,cpu,compiler,cxx_std,build_type,"
      "shape,format,library,mode,op,"
      "ns_min,ns_median,cv_pct,wire_bytes,iters,trials,notes\n";

   template <typename Out>
   void write_snapshot_csv(Out&                              out,
                           const platform_info&              plat,
                           const std::vector<snapshot_row>&  rows)
   {
      out << kSnapshotCsvHeader;
      for (const auto& r : rows)
      {
         out << plat.timestamp_utc << ','
             << csv_escape(plat.commit_short) << ','
             << plat.os << ',' << plat.arch << ','
             << csv_escape(plat.cpu) << ','
             << csv_escape(plat.compiler) << ','
             << plat.cxx_std << ',' << plat.build_type << ','
             << csv_escape(r.shape) << ','
             << csv_escape(r.format) << ','
             << csv_escape(r.library) << ','
             << r.mode << ',' << r.op << ',';
         char numbuf[64];
         std::snprintf(numbuf, sizeof(numbuf), "%.3f", r.ns_min);
         out << numbuf << ',';
         std::snprintf(numbuf, sizeof(numbuf), "%.3f", r.ns_median);
         out << numbuf << ',';
         std::snprintf(numbuf, sizeof(numbuf), "%.2f", r.cv_pct);
         out << numbuf << ',';
         out << r.wire_bytes << ',' << r.iters << ',' << r.trials << ','
             << csv_escape(r.notes) << '\n';
      }
   }

   //  Default snapshot path: bench_snapshots/perf_<ts>_<commit>.csv,
   //  rooted at the current working directory.  Caller is expected to
   //  create the directory if it doesn't exist.
   inline std::string default_snapshot_path(const platform_info& p)
   {
      std::ostringstream os_;
      os_ << "bench_snapshots/perf_" << p.timestamp_utc << '_'
          << p.commit_short << ".csv";
      return os_.str();
   }

}  // namespace psio_bench
