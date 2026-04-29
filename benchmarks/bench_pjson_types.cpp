// bench_pjson_types.cpp — per-type encode + decode + view-access timing.
//
// For each value type pjson supports, measure:
//   * encode  — value → pjson bytes (full doc with header)
//   * decode  — pjson bytes → value tree (pjson::decode)
//   * view    — pjson bytes → typed accessor (pjson_view::as_*)
//
// "view" is the hot path real consumers use; "decode" materializes the
// pjson_value variant tree (slower secondary surface).
//
// Run:  psio_bench_pjson_types [iters]   (default 200,000)

#include <psio/pjson.hpp>
#include <psio/pjson_view.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

   template <typename Fn>
   double run_loop(int iters, Fn&& fn)
   {
      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < iters; ++i)
         fn();
      auto t1 = std::chrono::steady_clock::now();
      return std::chrono::duration<double>(t1 - t0).count();
   }

   void hdr()
   {
      std::printf("%-32s  %4s  %9s  %9s  %9s\n", "type / value", "B",
                  "encode ns", "decode ns", "view ns");
      std::printf("%-32s  %4s  %9s  %9s  %9s\n",
                  "------------------------------",
                  "----", "---------", "---------", "---------");
   }

   void row(const char* label, std::size_t bytes,
            double enc_ns, double dec_ns, double view_ns)
   {
      std::printf("%-32s  %4zu  %9.1f  %9.1f  %9.1f\n", label, bytes,
                  enc_ns, dec_ns, view_ns);
   }

   template <typename Sink, typename Bench>
   void bench_one(const char* label, int iters, psio::pjson_value v,
                  Bench&& view_extract)
   {
      auto bytes = psio::pjson::encode(v);
      Sink sink{};
      double enc_secs = run_loop(iters, [&] {
         auto b = psio::pjson::encode(v);
         sink ^= static_cast<Sink>(b.size());
      });
      double dec_secs = run_loop(iters, [&] {
         auto out = psio::pjson::decode({bytes.data(), bytes.size()});
         (void)out;
      });
      double view_secs = run_loop(iters, [&] {
         auto vv = psio::pjson_view{bytes.data(), bytes.size()};
         sink ^= static_cast<Sink>(view_extract(vv));
      });
      row(label, bytes.size(), enc_secs / iters * 1e9,
          dec_secs / iters * 1e9, view_secs / iters * 1e9);
      // Anti-DCE; sink is volatile-ish.
      asm volatile("" ::"r"(sink) : "memory");
   }

}  // namespace

int main(int argc, char** argv)
{
   using psio::pjson_array;
   using psio::pjson_bytes;
   using psio::pjson_null;
   using psio::pjson_number;
   using psio::pjson_object;
   using psio::pjson_value;
   using psio::pjson_view;

   int iters = 200000;
   if (argc > 1)
      iters = std::atoi(argv[1]);

   std::printf("iters=%d\n\n", iters);
   hdr();

   bench_one<int>("null", iters, pjson_value{pjson_null{}},
                  [](pjson_view v) { return v.is_null() ? 1 : 0; });

   bench_one<int>("bool true", iters, pjson_value{true},
                  [](pjson_view v) { return v.as_bool() ? 1 : 0; });

   bench_one<std::int64_t>("uint_inline (5)", iters,
                           pjson_value{static_cast<std::int64_t>(5)},
                           [](pjson_view v) { return v.as_int64(); });

   bench_one<std::int64_t>("int 1B (200)", iters,
                           pjson_value{static_cast<std::int64_t>(200)},
                           [](pjson_view v) { return v.as_int64(); });

   bench_one<std::int64_t>("int 4B (1234567890)", iters,
                           pjson_value{static_cast<std::int64_t>(1234567890LL)},
                           [](pjson_view v) { return v.as_int64(); });

   bench_one<std::int64_t>("int 8B (i64 max)", iters,
                           pjson_value{
                               std::numeric_limits<std::int64_t>::max()},
                           [](pjson_view v) { return v.as_int64(); });

   bench_one<std::int64_t>(
       "int128 (1<<100)", iters,
       pjson_value{
           pjson_number{(static_cast<__int128>(1) << 100) + 7, 0}},
       [](pjson_view v) {
          return static_cast<std::int64_t>(v.as_int128() >> 64);
       });

   bench_one<std::uint64_t>("double short (3.14)", iters,
                            pjson_value{3.14},
                            [](pjson_view v) {
                               return std::bit_cast<std::uint64_t>(
                                   v.as_double());
                            });

   bench_one<std::uint64_t>("double long (1/7)", iters,
                            pjson_value{1.0 / 7.0},
                            [](pjson_view v) {
                               return std::bit_cast<std::uint64_t>(
                                   v.as_double());
                            });

   bench_one<std::size_t>(
       "string_short (5 chars)", iters,
       pjson_value{std::string("hello")},
       [](pjson_view v) { return v.as_string().size(); });

   bench_one<std::size_t>(
       "string (50 chars)", iters,
       pjson_value{std::string(50, 'x')},
       [](pjson_view v) { return v.as_string().size(); });

   bench_one<std::size_t>(
       "string (500 chars)", iters,
       pjson_value{std::string(500, 'x')},
       [](pjson_view v) { return v.as_string().size(); });

   bench_one<std::size_t>(
       "bytes (32B)", iters,
       pjson_value{pjson_bytes(32, 0xAB)},
       [](pjson_view v) { return v.as_bytes().size(); });

   {
      pjson_array arr;
      for (int i = 0; i < 10; ++i)
         arr.push_back(pjson_value{static_cast<std::int64_t>(i)});
      bench_one<std::int64_t>(
          "array<int>[10]", iters, pjson_value{arr},
          [](pjson_view v) { return v[5].as_int64(); });
   }

   // Number-parse comparison: text → pjson_number via two routes.
   // Demonstrates the cost the JSON pipeline pays today (text → double
   // → from_double → mantissa+scale) vs the proposed direct path
   // (text → mantissa+scale, no double).
   std::printf("\nnumber-parse paths (text → pjson_number):\n");
   {
      const char* texts[] = {"3.14", "0.1", "100", "1234567890",
                             "12345.6789", "1.5e10"};
      for (auto t : texts)
      {
         std::string_view sv = t;
         std::int64_t     sink = 0;
         double from_str_secs = run_loop(iters, [&] {
            auto n = pjson_number::from_string(sv);
            sink ^= static_cast<std::int64_t>(n.mantissa);
         });
         double via_double_secs = run_loop(iters, [&] {
            double d;
            std::from_chars(sv.data(), sv.data() + sv.size(), d);
            auto n = pjson_number::from_double(d);
            sink ^= static_cast<std::int64_t>(n.mantissa);
         });
         std::printf("  %-12s  from_string %5.1f ns   "
                     "from_chars+from_double %5.1f ns   sink=%lld\n",
                     t, from_str_secs / iters * 1e9,
                     via_double_secs / iters * 1e9,
                     static_cast<long long>(sink));
      }
   }

   std::printf("\n");
   {
      pjson_object obj{
          {"a", pjson_value{static_cast<std::int64_t>(1)}},
          {"b", pjson_value{static_cast<std::int64_t>(2)}},
          {"c", pjson_value{static_cast<std::int64_t>(3)}},
          {"d", pjson_value{static_cast<std::int64_t>(4)}},
          {"e", pjson_value{static_cast<std::int64_t>(5)}},
      };
      bench_one<std::int64_t>(
          "object{5 int fields}", iters, pjson_value{obj},
          [](pjson_view v) { return v["c"].as_int64(); });
   }

   return 0;
}
