// libraries/psio/cpp/benchmarks/bench_perf_report.cpp
//
// Head-to-head encode/decode/validate/size_of + wire-size measurements
// for every shape × every format × every library. Emits a markdown
// table + CSV of the results. Run as a standalone executable; outputs
// land in PERF_V1_V3_FULL.md and PERF_V1_V3_FULL.csv in the bench's
// working directory.
//
// Libraries wired today: v1 (libraries/psio1/) and v3 (libraries/psio/).
// Add v2 (libraries/psio2/) + competitor adapters in follow-ups.

// clang-format off
// Include-order-sensitive: v1 from_key depends on helpers in to_key.
#include <psio1/fracpack.hpp>
#include <psio1/from_avro.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/from_borsh.hpp>
#include <psio1/to_key.hpp>
#include <psio1/from_key.hpp>
#include <psio1/from_pssz.hpp>
#include <psio1/from_ssz.hpp>
#include <psio1/to_avro.hpp>
#include <psio1/to_bin.hpp>
#include <psio1/to_bincode.hpp>
#include <psio1/to_borsh.hpp>
#include <psio1/to_pssz.hpp>
#include <psio1/to_ssz.hpp>
// clang-format on

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/frac.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/ssz.hpp>

#include "harness.hpp"
#include "shapes.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace psio_bench;

// frac_supports<T> — mark shapes that psio frac can encode AND decode.
// Default true; specialize to false for types containing vector<variable>
// (not yet supported by frac). Follow-up: replace with a proper detection
// concept once frac's decode_vector variable branch lands.
template <typename T>
struct frac_supports : std::true_type {};

template <>
struct frac_supports<V3Order> : std::false_type {};

template <>
struct frac_supports<V1Order> : std::false_type {};

template <>
struct frac_supports<V3OrderBounded> : std::false_type {};

template <>
struct frac_supports<V1OrderBounded> : std::false_type {};

// v1 ssz_validate has no overload for std::optional<std::string> and some
// other composites — the static_assert lives inside template instantiation
// below the SFINAE boundary. Gate manually.
template <typename T>
struct v1_ssz_validate_supports : std::true_type {};

template <>
struct v1_ssz_validate_supports<V1Order> : std::false_type {};
template <>
struct v1_ssz_validate_supports<V1Record> : std::false_type {};

template <>
struct v1_ssz_validate_supports<V1OrderBounded> : std::false_type {};
template <>
struct v1_ssz_validate_supports<V1RecordBounded> : std::false_type {};
template <>
struct v1_ssz_validate_supports<V1FlatRecordBounded> : std::false_type {};
template <>
struct v1_ssz_validate_supports<V1ValidatorListBounded> : std::false_type {};

namespace {

   // kIters is now a hint only — the harness auto-tunes the actual
   // iter count per call so each trial runs ~50 ms. Sub-ns ops get
   // millions of iters; multi-microsecond ops drop to a few hundred.
   constexpr std::size_t kIters  = 20'000;
   constexpr int         kTrials = 7;

   template <typename Bytes>
   std::span<const char> cview(const Bytes& b)
   {
      return {b.data(), b.size()};
   }

   // ── Validate timing shims ─────────────────────────────────────────
   //
   // validate is the hardest CPO to time honestly. Its body has no
   // observable effect when input is valid (just a sequence of
   // range-checks that fall through), so once the compiler inlines
   // the call the entire body collapses to nothing — yielding "0.1
   // ns" measurements that don't reflect work.
   //
   // [[gnu::noinline]] forces a real call. The barrier asm volatile
   // makes `bytes` look modified to the optimizer so it can't hoist.
   // The captured return (codec_status / void via dummy) is escaped
   // so the call site can't be elided.
   //
   // We pay the call/return overhead consistently for both v1 and v3,
   // so the comparison is honest — both bear the same noise floor.

   template <typename T>
   [[gnu::noinline]] static void v1_ssz_validate_call(
      std::span<const char> b)
   {
      psio1::ssz_validate<T>(b);
   }

   template <typename Fmt, typename T>
   [[gnu::noinline]] static psio::codec_status v3_validate_call(
      Fmt fmt, std::span<const char> b)
   {
      return psio::validate<T>(fmt, b);
   }

   // Throwing v3 validate. Mirrors v1's API shape — void on success,
   // codec_exception on failure. Compiler can elide the entire check
   // chain on shapes where range-propagation proves no throw fires.
   template <typename Fmt, typename T>
   [[gnu::noinline]] static void v3_validate_or_throw_call(
      Fmt fmt, std::span<const char> b)
   {
      psio::validate_or_throw<T>(fmt, b);
   }

   // ── v1 format oracles ─────────────────────────────────────────────
   //
   // Each *_v1 helper returns a result_row with timings + wire bytes.
   // Templated on the concrete shape struct.

   template <typename T>
   result_row bench_v1_ssz(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "ssz", "v1"};
      auto bytes = psio1::convert_to_ssz(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_ssz(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         auto dv = psio1::convert_from_ssz<T>(bytes);
         asm volatile("" : : "r,m"(&dv) : "memory");
      }, kTrials);
      auto t_sz  = ns_per_iter(kIters, [&](std::size_t) {
         auto n = psio1::ssz_size(v);
         asm volatile("" : : "r,m"(n) : "memory");
      }, kTrials);
      if constexpr (v1_ssz_validate_supports<T>::value)
      {
         auto t_val = ns_per_iter(kIters, [&](std::size_t) {
            asm volatile("" : : "r"(bytes.data()) : "memory");
            v1_ssz_validate_call<T>(std::span<const char>{bytes});
            asm volatile("" : : : "memory");
         }, kTrials);
         r.val_ns_min = t_val.min_ns;
      }
      r.enc_ns_min  = t_enc.min_ns;
      r.dec_ns_min  = t_dec.min_ns;
      r.enc_cv_pct  = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct  = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      r.size_ns_min = t_sz.min_ns;
      return r;
   }

   template <typename T, typename V1Fmt>
   result_row bench_v1_pssz(std::string shape, std::string fmt_name,
                             const T& v)
   {
      result_row r{std::move(shape), std::move(fmt_name), "v1"};
      auto bytes = psio1::convert_to_pssz<V1Fmt>(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_pssz<V1Fmt>(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         T tmp;
         psio1::convert_from_pssz<V1Fmt, T>(tmp, bytes);
         asm volatile("" : : "r,m"(&tmp) : "memory");
      }, kTrials);
      auto t_sz  = ns_per_iter(kIters, [&](std::size_t) {
         auto n = psio1::pssz_size<V1Fmt>(v);
         asm volatile("" : : "r,m"(n) : "memory");
      }, kTrials);
      r.enc_ns_min  = t_enc.min_ns;
      r.dec_ns_min  = t_dec.min_ns;
      r.enc_cv_pct  = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct  = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      r.size_ns_min = t_sz.min_ns;
      return r;
   }

   template <typename T>
   result_row bench_v1_frac(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "frac32", "v1"};
      auto bytes = psio1::to_frac(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::to_frac(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         T tmp;
         (void)psio1::from_frac<T>(tmp, bytes);
         asm volatile("" : : "r,m"(&tmp) : "memory");
      }, kTrials);
      auto t_sz  = ns_per_iter(kIters, [&](std::size_t) {
         auto n = psio1::fracpack_size(v);
         asm volatile("" : : "r,m"(n) : "memory");
      }, kTrials);
      r.enc_ns_min  = t_enc.min_ns;
      r.dec_ns_min  = t_dec.min_ns;
      r.enc_cv_pct  = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct  = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      r.size_ns_min = t_sz.min_ns;
      return r;
   }

   template <typename T>
   result_row bench_v1_bin(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "bin", "v1"};
      auto bytes = psio1::convert_to_bin(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_bin(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         auto dv = psio1::convert_from_bin<T>(bytes);
         asm volatile("" : : "r,m"(&dv) : "memory");
      }, kTrials);
      r.enc_ns_min = t_enc.min_ns;
      r.dec_ns_min = t_dec.min_ns;
      r.enc_cv_pct = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      return r;
   }

   template <typename T>
   result_row bench_v1_borsh(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "borsh", "v1"};
      auto bytes = psio1::convert_to_borsh(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_borsh(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         auto dv = psio1::convert_from_borsh<T>(bytes);
         asm volatile("" : : "r,m"(&dv) : "memory");
      }, kTrials);
      r.enc_ns_min = t_enc.min_ns;
      r.dec_ns_min = t_dec.min_ns;
      r.enc_cv_pct = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      return r;
   }

   template <typename T>
   result_row bench_v1_bincode(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "bincode", "v1"};
      auto bytes = psio1::convert_to_bincode(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_bincode(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         auto dv = psio1::convert_from_bincode<T>(bytes);
         asm volatile("" : : "r,m"(&dv) : "memory");
      }, kTrials);
      r.enc_ns_min = t_enc.min_ns;
      r.dec_ns_min = t_dec.min_ns;
      r.enc_cv_pct = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      return r;
   }

   template <typename T>
   result_row bench_v1_avro(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "avro", "v1"};
      auto bytes = psio1::convert_to_avro(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_avro(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         auto dv = psio1::convert_from_avro<T>(bytes);
         asm volatile("" : : "r,m"(&dv) : "memory");
      }, kTrials);
      r.enc_ns_min = t_enc.min_ns;
      r.dec_ns_min = t_dec.min_ns;
      r.enc_cv_pct = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      return r;
   }

   template <typename T>
   result_row bench_v1_key(std::string shape, const T& v)
   {
      result_row r{std::move(shape), "key", "v1"};
      auto bytes = psio1::convert_to_key(v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio1::convert_to_key(v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      r.enc_ns_min = t_enc.min_ns;
      return r;
   }

   // ── v3 format oracles ─────────────────────────────────────────────

   template <typename Fmt, typename T>
   result_row bench_v3(std::string shape, std::string fmt_name,
                        Fmt fmt, const T& v)
   {
      result_row r{std::move(shape), std::move(fmt_name), "v3"};
      auto bytes = psio::encode(fmt, v);
      r.wire_bytes = bytes.size();
      auto t_enc = ns_per_iter(kIters, [&](std::size_t) {
         auto b = psio::encode(fmt, v);
         asm volatile("" : : "r,m"(b.data()) : "memory");
      }, kTrials);
      auto t_dec = ns_per_iter(kIters, [&](std::size_t) {
         auto dv = psio::decode<T>(fmt, cview(bytes));
         asm volatile("" : : "r,m"(&dv) : "memory");
      }, kTrials);
      auto t_sz  = ns_per_iter(kIters, [&](std::size_t) {
         auto n = psio::size_of(fmt, v);
         asm volatile("" : : "r,m"(n) : "memory");
      }, kTrials);
      // Two validate APIs exist on v3: status (no-throw, WASM-safe)
      // and or_throw (zero-cost on success in native). We time
      // or_throw whenever the format implements it — that's the
      // apples-to-apples comparison to v1's throwing validate.
      auto t_val = ns_per_iter(kIters, [&](std::size_t) {
         asm volatile("" : : "r"(bytes.data()) : "memory");
         if constexpr (requires {
            psio::validate_or_throw<T>(fmt, cview(bytes));
         })
         {
            v3_validate_or_throw_call<Fmt, T>(fmt, cview(bytes));
            asm volatile("" : : : "memory");
         }
         else
         {
            auto st = v3_validate_call<Fmt, T>(fmt, cview(bytes));
            asm volatile("" : : "r,m"(st) : "memory");
         }
      }, kTrials);
      r.enc_ns_min  = t_enc.min_ns;
      r.dec_ns_min  = t_dec.min_ns;
      r.enc_cv_pct  = t_enc.min_ns > 0.0 ? t_enc.stddev_ns / t_enc.min_ns * 100.0 : 0.0;
      r.dec_cv_pct  = t_dec.min_ns > 0.0 ? t_dec.stddev_ns / t_dec.min_ns * 100.0 : 0.0;
      r.size_ns_min = t_sz.min_ns;
      r.val_ns_min  = t_val.min_ns;
      return r;
   }

   // Gate each format on a compile-time `requires`-expression: if the
   // format's encode doesn't compile for this type (e.g., frac on a
   // vector<variable-element>), skip the row rather than breaking the
   // build. This lets the bench run across shapes with heterogeneous
   // format support.
   template <typename V1, typename V3>
   void run_shape(std::string shape_name, const V1& v1v, const V3& v3v,
                   std::vector<result_row>& out)
   {
      // ssz
      if constexpr (requires { psio1::convert_to_ssz(v1v); })
         out.push_back(bench_v1_ssz(shape_name, v1v));
      if constexpr (requires { psio::encode(psio::ssz{}, v3v); })
         out.push_back(bench_v3(shape_name, "ssz", psio::ssz{}, v3v));

      // pssz32
      if constexpr (requires {
                        psio1::convert_to_pssz<psio1::frac_format_pssz32>(v1v);
                     })
         out.push_back(bench_v1_pssz<V1, psio1::frac_format_pssz32>(
            shape_name, "pssz32", v1v));
      if constexpr (requires { psio::encode(psio::pssz32{}, v3v); })
         out.push_back(bench_v3(shape_name, "pssz32", psio::pssz32{}, v3v));

      // pssz-auto
      if constexpr (requires {
                        psio1::convert_to_pssz<psio1::auto_pssz_format_t<V1>>(
                           v1v);
                     })
         out.push_back(bench_v1_pssz<V1, psio1::auto_pssz_format_t<V1>>(
            shape_name, "pssz-auto", v1v));
      if constexpr (requires { psio::encode(psio::pssz{}, v3v); })
         out.push_back(bench_v3(shape_name, "pssz-auto", psio::pssz{}, v3v));

      // frac32 — skip shapes that transitively contain vector<variable-element>;
      // psio's frac doesn't support that yet (decode_vector static_asserts).
      // The `requires` check passes on encode but fails at decode instantiation,
      // so we use a shape-level flag set by the caller via a partial
      // specialization of `frac_supports<T>`.
      if constexpr (frac_supports<V3>::value)
      {
         if constexpr (requires { psio1::to_frac(v1v); })
            out.push_back(bench_v1_frac(shape_name, v1v));
         out.push_back(bench_v3(shape_name, "frac32", psio::frac32{}, v3v));
      }

      // bin
      if constexpr (requires { psio1::convert_to_bin(v1v); })
         out.push_back(bench_v1_bin(shape_name, v1v));
      if constexpr (requires { psio::encode(psio::bin{}, v3v); })
         out.push_back(bench_v3(shape_name, "bin", psio::bin{}, v3v));

      // borsh
      if constexpr (requires { psio1::convert_to_borsh(v1v); })
         out.push_back(bench_v1_borsh(shape_name, v1v));
      if constexpr (requires { psio::encode(psio::borsh{}, v3v); })
         out.push_back(bench_v3(shape_name, "borsh", psio::borsh{}, v3v));

      // bincode
      if constexpr (requires { psio1::convert_to_bincode(v1v); })
         out.push_back(bench_v1_bincode(shape_name, v1v));
      if constexpr (requires { psio::encode(psio::bincode{}, v3v); })
         out.push_back(bench_v3(shape_name, "bincode", psio::bincode{}, v3v));

      // avro
      if constexpr (requires { psio1::convert_to_avro(v1v); })
         out.push_back(bench_v1_avro(shape_name, v1v));
      if constexpr (requires { psio::encode(psio::avro{}, v3v); })
         out.push_back(bench_v3(shape_name, "avro", psio::avro{}, v3v));

      // NOTE: `key` is scalar-only in psio (memcmp-sortable encoding).
      // Records / vectors / optionals don't fit its model; bench it in
      // a separate scalar-focused harness in a follow-up commit.
   }

}  // namespace

int main()
{
   std::vector<result_row> rows;
   rows.reserve(256);

   run_shape("Point",         v1_point(),    v3_point(),    rows);
   run_shape("NameRecord",    v1_namerec(),  v3_namerec(),  rows);
   run_shape("FlatRecord",    v1_flatrec(),  v3_flatrec(),  rows);
   run_shape("Record",        v1_record(),   v3_record(),   rows);
   run_shape("Validator",     v1_validator(),v3_validator(),rows);
   run_shape("Order",         v1_order(),    v3_order(),    rows);
   run_shape("ValidatorList", v1_vlist(100), v3_vlist(100), rows);

   // Bounded variants — same data, length_bound annotations on every
   // variable field. v1 uses bounded_string<N>/bounded_list<T,N> type
   // wrappers; v3 uses std::* + PSIO_FIELD_ATTRS{length_bound{.max=N}}
   // (the intrusive psio::bounded<T,N> form awaits per-codec dispatch).
   run_shape("FlatRecord!",   v1_flatrec_bounded(),  v3_flatrec_bounded(),  rows);
   run_shape("Record!",       v1_record_bounded(),   v3_record_bounded(),   rows);
   run_shape("Order!",        v1_order_bounded(),    v3_order_bounded(),    rows);
   run_shape("ValidatorList!",v1_vlist_bounded(100), v3_vlist_bounded(100), rows);

   // Console: print the table.
   report_table(std::cout, rows);

   // Files: markdown + csv for archiving.
   {
      std::ofstream md("PERF_V1_V3_FULL.md");
      md << "# psio v1 vs psio full perf report\n\n";
      md << "Columns: encode / decode / validate / size_of ns/op (min "
            "of 5 trials), plus wire bytes.\n";
      md << "`v3/v1` row is ratio (smaller = v3 faster / smaller).\n\n";
      md << "Shapes span tier 1 (8 B DWNC Point) through tier 7 "
            "(12 KB ValidatorList of 100 DWNC records).\n\n";
      report_table(md, rows);
   }
   {
      std::ofstream csv("PERF_V1_V3_FULL.csv");
      report_csv(csv, rows);
   }

   return 0;
}
