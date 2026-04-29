// libraries/psio/cpp/benchmarks/bench_nested_encode.cpp
//
// Baseline: encode-pass timing for every psio format on a balanced
// binary-tree shape at depths 2..7.  Designed to expose the recursive
// size-walk cost on length-prefixed length-delimited formats — the
// fractal O(D × N) pattern we already eliminated for protobuf via the
// stack-cached size pipeline.
//
// Each level doubles the leaf count; depth-D tree has 2^(D-1) leaves
// and 2^D - 1 nodes total.  Leaf carries an int64 + a short string so
// every level has both a primitive and a length-delimited member,
// giving the size walk something to do at every recursion step.
//
// Output: a markdown table per format, ns-per-encode (min of 7 trials)
// at each depth, plus the nested-cost ratio (depth-7 / depth-2 per
// node) — a tight ratio means O(N), a growing ratio means fractal.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/frac.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/wit_abi.hpp>

#include "harness.hpp"

#include <cstdio>
#include <string>
#include <vector>

// ── balanced binary tree, depth fixed by type ────────────────────────
struct NLeaf
{
   std::int64_t id;
   std::string  label;
};
PSIO_REFLECT(NLeaf, id, label)

struct N1 { NLeaf a; NLeaf b; };
PSIO_REFLECT(N1, a, b)
struct N2 { N1 a; N1 b; };
PSIO_REFLECT(N2, a, b)
struct N3 { N2 a; N2 b; };
PSIO_REFLECT(N3, a, b)
struct N4 { N3 a; N3 b; };  // depth 5, 16 leaves
PSIO_REFLECT(N4, a, b)
struct N5 { N4 a; N4 b; };  // depth 6, 32 leaves
PSIO_REFLECT(N5, a, b)
struct N6 { N5 a; N5 b; };  // depth 7, 64 leaves
PSIO_REFLECT(N6, a, b)

template <typename T>
struct depth_of;
template <> struct depth_of<NLeaf> { static constexpr int v = 1; };
template <> struct depth_of<N1>    { static constexpr int v = 2; };
template <> struct depth_of<N2>    { static constexpr int v = 3; };
template <> struct depth_of<N3>    { static constexpr int v = 4; };
template <> struct depth_of<N4>    { static constexpr int v = 5; };
template <> struct depth_of<N5>    { static constexpr int v = 6; };
template <> struct depth_of<N6>    { static constexpr int v = 7; };

template <typename T>
constexpr int leaves_of()
{
   return 1 << (depth_of<T>::v - 1);
}

// ── wit_abi: synthetic "format tag" wrapping the lower/lift API. ─────
struct wit_abi_format        {};  // naïve bump (resizes vector per alloc)
struct wit_abi_presized_format {};  // pre-reserved via wit_abi_total_bytes

template <typename T>
std::vector<std::uint8_t> wit_abi_encode(const T& value)
{
   psio::buffer_store_policy p;
   const std::uint32_t       dest = p.alloc(psio::wit_abi_align_v<T>,
                                       psio::wit_abi_size_v<T>);
   psio::wit_abi_lower_fields(value, p, dest);
   return std::move(p.buf);
}

template <typename T>
std::vector<std::uint8_t> wit_abi_encode_presized(const T& value)
{
   //  One pre-pass with size_store_policy gives the exact total bytes
   //  the real lower would allocate.  Reserving up front means
   //  buffer_store_policy::alloc never needs to grow the vector.
   const std::uint32_t       total = psio::wit_abi_total_bytes(value);
   psio::buffer_store_policy p;
   p.buf.reserve(total);
   const std::uint32_t dest =
      p.alloc(psio::wit_abi_align_v<T>, psio::wit_abi_size_v<T>);
   psio::wit_abi_lower_fields(value, p, dest);
   return std::move(p.buf);
}

// ── one cell: encode T via Fmt, returns ns-per-encode (min of 7) ──
template <typename Fmt, typename T>
double bench_encode(const T& v)
{
   // Auto-tuned timing harness (50 ms target per trial, 7 trials).
   auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
      if constexpr (std::is_same_v<Fmt, wit_abi_format>)
      {
         auto bytes = wit_abi_encode(v);
         asm volatile("" : : "r"(bytes.data()) : "memory");
      }
      else if constexpr (std::is_same_v<Fmt, wit_abi_presized_format>)
      {
         auto bytes = wit_abi_encode_presized(v);
         asm volatile("" : : "r"(bytes.data()) : "memory");
      }
      else
      {
         auto bytes = psio::encode(Fmt{}, v);
         asm volatile("" : : "r"(bytes.data()) : "memory");
      }
   });
   return t.min_ns;
}

// Just the size_of pass (no encoding bytes).
template <typename Fmt, typename T>
double bench_size_only(const T& v)
{
   if constexpr (std::is_same_v<Fmt, wit_abi_format>)
   {
      // wit_abi: only the static record size is consteval.
      auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
         std::size_t n = psio::wit_abi_size_v<std::remove_cvref_t<decltype(v)>>;
         asm volatile("" : "+r"(n) : : "memory");
      });
      return t.min_ns;
   }
   else if constexpr (std::is_same_v<Fmt, wit_abi_presized_format>)
   {
      auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
         std::size_t n = psio::wit_abi_total_bytes(v);
         asm volatile("" : "+r"(n) : : "memory");
      });
      return t.min_ns;
   }
   else
   {
      auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
         std::size_t n = psio::size_of(Fmt{}, v);
         asm volatile("" : "+r"(n) : : "memory");
      });
      return t.min_ns;
   }
}

// Encode into a reused std::vector<char>& (the sink path).
template <typename Fmt, typename T>
double bench_sink(const T& v)
{
   if constexpr (std::is_same_v<Fmt, wit_abi_format>)
   {
      // wit_abi has no in-place "sink" CPO; reuse a buffer_store_policy.
      psio::buffer_store_policy p;
      auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
         p.buf.clear();
         p.bump = 0;
         const std::uint32_t dest = p.alloc(
            psio::wit_abi_align_v<std::remove_cvref_t<decltype(v)>>,
            psio::wit_abi_size_v<std::remove_cvref_t<decltype(v)>>);
         psio::wit_abi_lower_fields(v, p, dest);
      });
      return t.min_ns;
   }
   else if constexpr (std::is_same_v<Fmt, wit_abi_presized_format>)
   {
      psio::buffer_store_policy p;
      // Reserve once outside the loop so the second-and-later
      // iterations measure pure encode + size walk, not allocation.
      p.buf.reserve(psio::wit_abi_total_bytes(v) * 2);
      auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
         p.buf.clear();
         p.bump = 0;
         const std::uint32_t dest = p.alloc(
            psio::wit_abi_align_v<std::remove_cvref_t<decltype(v)>>,
            psio::wit_abi_size_v<std::remove_cvref_t<decltype(v)>>);
         psio::wit_abi_lower_fields(v, p, dest);
      });
      return t.min_ns;
   }
   else
   {
      std::vector<char> sink;
      sink.reserve(4096);
      auto t = psio_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
         sink.clear();
         psio::encode(Fmt{}, v, sink);
      });
      return t.min_ns;
   }
}

template <typename T>
T make_tree();

template <>
NLeaf make_tree<NLeaf>() { return {42, "leaf"}; }

template <>
N1 make_tree<N1>() { return {make_tree<NLeaf>(), make_tree<NLeaf>()}; }

template <>
N2 make_tree<N2>() { return {make_tree<N1>(), make_tree<N1>()}; }

template <>
N3 make_tree<N3>() { return {make_tree<N2>(), make_tree<N2>()}; }

template <>
N4 make_tree<N4>() { return {make_tree<N3>(), make_tree<N3>()}; }

template <>
N5 make_tree<N5>() { return {make_tree<N4>(), make_tree<N4>()}; }

template <>
N6 make_tree<N6>() { return {make_tree<N5>(), make_tree<N5>()}; }

// ── per-format runner ────────────────────────────────────────────────
template <typename Fmt>
struct fmt_traits;

template <>
struct fmt_traits<psio::frac32>
{
   static constexpr const char* name = "frac32";
};
template <>
struct fmt_traits<psio::frac16>
{
   static constexpr const char* name = "frac16";
};
template <>
struct fmt_traits<wit_abi_format>
{
   static constexpr const char* name = "wit_abi (bump)";
};
template <>
struct fmt_traits<wit_abi_presized_format>
{
   static constexpr const char* name = "wit_abi (pre-sized)";
};
template <>
struct fmt_traits<psio::pssz>
{
   static constexpr const char* name = "pssz";
};
template <>
struct fmt_traits<psio::ssz>
{
   static constexpr const char* name = "ssz";
};
template <>
struct fmt_traits<psio::bin>
{
   static constexpr const char* name = "bin";
};
template <>
struct fmt_traits<psio::borsh>
{
   static constexpr const char* name = "borsh";
};
template <>
struct fmt_traits<psio::bincode>
{
   static constexpr const char* name = "bincode";
};
template <>
struct fmt_traits<psio::avro>
{
   static constexpr const char* name = "avro";
};
template <>
struct fmt_traits<psio::protobuf>
{
   static constexpr const char* name = "protobuf";
};

template <typename Fmt>
void run_one_format()
{
   std::printf("\n## %s\n\n", fmt_traits<Fmt>::name);
   std::printf("| depth | leaves | bytes | rvalue | sink | size_of | "
               "size%% |\n");
   std::printf("|------:|-------:|------:|-------:|-----:|--------:|"
               "------:|\n");

   auto row = [&](auto v) {
      using T = std::remove_cvref_t<decltype(v)>;
      std::size_t wire_bytes;
      if constexpr (std::is_same_v<Fmt, wit_abi_format>)
         wire_bytes = wit_abi_encode(v).size();
      else if constexpr (std::is_same_v<Fmt, wit_abi_presized_format>)
         wire_bytes = wit_abi_encode_presized(v).size();
      else
         wire_bytes = psio::encode(Fmt{}, v).size();
      double rv = bench_encode<Fmt>(v);
      double sk = bench_sink<Fmt>(v);
      double sz = bench_size_only<Fmt>(v);
      const int    d  = depth_of<T>::v;
      const int    L  = leaves_of<T>();
      std::printf("| %5d | %6d | %5zu | %6.1f | %4.1f | %7.1f | "
                  "%5.0f%% |\n",
                  d, L, wire_bytes, rv, sk, sz, 100.0 * sz / rv);
   };

   row(make_tree<N1>());
   row(make_tree<N2>());
   row(make_tree<N3>());
   row(make_tree<N4>());
   row(make_tree<N5>());
   row(make_tree<N6>());
}

int main()
{
   std::printf("# Nested-encode baseline\n");
   std::printf("\nBalanced binary tree of `Leaf { i64 id; string label; }`,\n");
   std::printf("depths 2..7. ns/leaf below should stay constant if the\n");
   std::printf("encoder is O(N); growing ns/leaf means fractal cost.\n");

   run_one_format<psio::protobuf>();
   run_one_format<psio::frac32>();
   run_one_format<psio::frac16>();
   run_one_format<psio::pssz>();
   run_one_format<psio::ssz>();
   run_one_format<psio::bin>();
   run_one_format<psio::borsh>();
   run_one_format<psio::bincode>();
   run_one_format<psio::avro>();
   run_one_format<wit_abi_format>();
   run_one_format<wit_abi_presized_format>();

   return 0;
}
