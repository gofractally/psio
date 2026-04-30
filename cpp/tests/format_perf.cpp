// Format perf comparison, organised on two axes:
//
//   Schema-required (no field names on wire — receiver needs the
//                    schema to interpret the bytes):
//     - pssz                 (positional + offset table, extensible)
//     - msgpack fixarray     (positional, fixed schema)
//     - protobuf             (numeric field tags + length-delim,
//                             extensible)
//
//   Self-describing (field names on wire — generic readers can
//                    walk the bytes without a schema):
//     - json                 (text)
//     - pjson                (binary, hash-indexed)
//     - msgpack fixmap       (named pairs)
//
// The two axes have different design goals; comparing across them
// is the apples-to-oranges trap.  Comparing within each axis is
// the apples-to-apples that surfaces real codec quality differences.
//
// Build via psio_format_perf target.  Output reports ns/op for
// encode + decode + round-trip on each format, plus encoded size.
//
// Disclaimer: simple steady-state microbenchmark.  Treat as
// rule-of-thumb relative numbers, not absolute throughput claims.

#include <psio/json.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace bench
{
   //  Sub-record exercised inside vector<Sub> to force generic-array
   //  emission across all formats.
   struct Sub
   {
      std::int32_t id;
      std::string  label;
   };
   PSIO_REFLECT(Sub, id, label)

   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;     // bytes-blob path
      std::vector<std::int32_t>   ids;         // typed-array
      std::vector<Sub>            entries;     // generic array of records
      std::optional<std::int32_t> count;
      std::int64_t                seq;
      double                      score;
   };
   PSIO_REFLECT(Bag, name, payload, ids, entries, count, seq, score)

   //  Identical-shape variants used to exercise msgpack's fixmap
   //  (self-describing) record form alongside json/pjson.  Distinct
   //  C++ types so the per-type msgpack_record_form trait can
   //  specialise without affecting the fixarray path on Bag.
   struct SubMap
   {
      std::int32_t id;
      std::string  label;
   };
   PSIO_REFLECT(SubMap, id, label)

   struct BagMap
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;
      std::vector<std::int32_t>   ids;
      std::vector<SubMap>         entries;
      std::optional<std::int32_t> count;
      std::int64_t                seq;
      double                      score;
   };
   PSIO_REFLECT(BagMap, name, payload, ids, entries, count, seq, score)

   inline Bag make_bag()
   {
      return Bag{
         .name    = std::string{"alice-the-quick-brown-fox"},
         .payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .ids     = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .entries = {Sub{1, std::string{"alpha"}},
                     Sub{2, std::string{"beta"}},
                     Sub{3, std::string{"gamma"}},
                     Sub{4, std::string{"delta"}}},
         .count   = 42,
         .seq     = 123456789012LL,
         .score   = 3.14159265358979};
   }

   inline BagMap make_bag_map()
   {
      return BagMap{
         .name    = std::string{"alice-the-quick-brown-fox"},
         .payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .ids     = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .entries = {SubMap{1, std::string{"alpha"}},
                     SubMap{2, std::string{"beta"}},
                     SubMap{3, std::string{"gamma"}},
                     SubMap{4, std::string{"delta"}}},
         .count   = 42,
         .seq     = 123456789012LL,
         .score   = 3.14159265358979};
   }
}  // namespace bench

//  Opt the BagMap / SubMap pair into msgpack's fixmap form.
template <>
struct psio::msgpack_record_form<bench::BagMap>
{
   static constexpr bool as_map = true;
};
template <>
struct psio::msgpack_record_form<bench::SubMap>
{
   static constexpr bool as_map = true;
};

using clk = std::chrono::steady_clock;

template <typename F>
double bench_ns(F&& f, std::size_t iters)
{
   auto t0 = clk::now();
   for (std::size_t i = 0; i < iters; ++i)
      f();
   auto t1 = clk::now();
   auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count();
   return static_cast<double>(ns) / static_cast<double>(iters);
}

int main()
{
   constexpr std::size_t N = 200'000;

   bench::Bag    bag    = bench::make_bag();
   bench::BagMap bagmap = bench::make_bag_map();

   //  Pre-encode once for size reporting + decode benches.
   auto pssz_bytes = psio::encode(psio::pssz{}, bag);
   auto mpa_bytes  = psio::encode(psio::msgpack{}, bag);
   auto pb_bytes   = psio::encode(psio::protobuf{}, bag);
   auto json_bytes = psio::encode(psio::json{}, bagmap);
   auto pjson_bytes = psio::from_struct(bagmap);
   auto mpm_bytes  = psio::encode(psio::msgpack{}, bagmap);

   std::printf(
      "\n=========== Schema-required wire (no field names) =========\n");
   std::printf("  pssz             : %zu bytes\n", pssz_bytes.size());
   std::printf("  msgpack-fixarray : %zu bytes  (%.2fx vs pssz)\n",
               mpa_bytes.size(),
               static_cast<double>(mpa_bytes.size()) /
                  static_cast<double>(pssz_bytes.size()));
   std::printf("  protobuf         : %zu bytes  (%.2fx vs pssz)\n",
               pb_bytes.size(),
               static_cast<double>(pb_bytes.size()) /
                  static_cast<double>(pssz_bytes.size()));

   std::printf(
      "\n=========== Self-describing wire (names on wire) ==========\n");
   std::printf("  json             : %zu bytes\n", json_bytes.size());
   std::printf("  pjson            : %zu bytes  (%.2fx vs json)\n",
               pjson_bytes.size(),
               static_cast<double>(pjson_bytes.size()) /
                  static_cast<double>(json_bytes.size()));
   std::printf("  msgpack-fixmap   : %zu bytes  (%.2fx vs json)\n",
               mpm_bytes.size(),
               static_cast<double>(mpm_bytes.size()) /
                  static_cast<double>(json_bytes.size()));

   //  ── Schema-required ────────────────────────────────────────────
   auto enc_pssz = bench_ns(
      [&] {
         auto out = psio::encode(psio::pssz{}, bag);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_pssz = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::pssz{}, std::span<const char>{pssz_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   auto enc_mpa = bench_ns(
      [&] {
         auto out = psio::encode(psio::msgpack{}, bag);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_mpa = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::msgpack{}, std::span<const char>{mpa_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   auto enc_pb = bench_ns(
      [&] {
         auto out = psio::encode(psio::protobuf{}, bag);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_pb = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::protobuf{}, std::span<const char>{pb_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   std::printf(
      "\n=========== Schema-required ns/op  (lower is better) =====\n");
   std::printf("                       encode     decode     round-trip\n");
   std::printf("  pssz             : %8.1f   %8.1f   %8.1f\n",
               enc_pssz, dec_pssz, enc_pssz + dec_pssz);
   std::printf(
      "  msgpack-fixarray : %8.1f   %8.1f   %8.1f   (%.2fx encode, %.2fx decode vs pssz)\n",
      enc_mpa, dec_mpa, enc_mpa + dec_mpa, enc_pssz / enc_mpa,
      dec_pssz / dec_mpa);
   std::printf(
      "  protobuf         : %8.1f   %8.1f   %8.1f   (%.2fx encode, %.2fx decode vs pssz)\n",
      enc_pb, dec_pb, enc_pb + dec_pb, enc_pssz / enc_pb,
      dec_pssz / dec_pb);

   //  ── Self-describing ────────────────────────────────────────────
   auto enc_json = bench_ns(
      [&] {
         auto out = psio::encode(psio::json{}, bagmap);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_json = bench_ns(
      [&] {
         auto out = psio::decode<bench::BagMap>(
            psio::json{}, std::span<const char>{json_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   auto enc_pj = bench_ns(
      [&] {
         std::vector<std::uint8_t> out;
         psio::to_pjson(bagmap, out);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_pj = bench_ns(
      [&] {
         psio::pjson_view raw{pjson_bytes.data(), pjson_bytes.size()};
         auto v = psio::typed_pjson_view<bench::BagMap>::from_pjson(raw);
         auto out = v.to_struct();
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   auto enc_mpm = bench_ns(
      [&] {
         auto out = psio::encode(psio::msgpack{}, bagmap);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_mpm = bench_ns(
      [&] {
         auto out = psio::decode<bench::BagMap>(
            psio::msgpack{}, std::span<const char>{mpm_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   std::printf(
      "\n=========== Self-describing ns/op  (lower is better) =====\n");
   std::printf("                       encode     decode     round-trip\n");
   std::printf("  json             : %8.1f   %8.1f   %8.1f\n",
               enc_json, dec_json, enc_json + dec_json);
   std::printf(
      "  pjson            : %8.1f   %8.1f   %8.1f   (%.2fx encode, %.2fx decode vs json)\n",
      enc_pj, dec_pj, enc_pj + dec_pj, enc_json / enc_pj,
      dec_json / dec_pj);
   std::printf(
      "  msgpack-fixmap   : %8.1f   %8.1f   %8.1f   (%.2fx encode, %.2fx decode vs json)\n",
      enc_mpm, dec_mpm, enc_mpm + dec_mpm, enc_json / enc_mpm,
      dec_json / dec_mpm);

   return 0;
}
