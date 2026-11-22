// Typed JSON / I-JSON text: decode and encode, per syntax layer.
//
//   psio_bench_json_text [filter]
//
// Shapes
//   small      a flat record of 5 members
//   medium     a nested record: strings, a variant, optionals and a
//              20-element vector of records
//   large      an array of 10 000 records
//   wideint    u64 values beyond 2^53 and u128 values
//   wide200    a record of 200 members, keys in declared order
//   wide200r   the same text with the keys in reverse order (decode only)
//
// Implementations (decode)
//   text       the hand-written lexer            (json_parser::text)
//   ondemand   simdjson on-demand, input copied  (json_parser::simdjson)
//   index      simdjson stage 1 + PSIO's token walk, input copied
//                                                (json_parser::simdjson_index)
//   index-p    the same on caller-padded input: no copy
//   auto       what json_parser::automatic picks
// all with the compile-time perfect-hash member lookup and the
// expected-order fast path.
//
// The "before" column of the report comes from this same file compiled
// against the headers of the previous commit, where the reader was the
// hand-written one with a linear member scan:
//
//   git worktree add /tmp/psio-base d0da34e
//   clang++ -std=c++23 -O3 -DNDEBUG -DPSIO_BENCH_BASELINE \
//       -I/tmp/psio-base/cpp/include -Icpp/tests -Icpp/benchmarks \
//       cpp/benchmarks/bench_json_text.cpp -o bench_json_text_base
//
// (d0da34e cannot declare a 200-member record: PSIO_REFLECT stopped at 64.
// For the wide200 rows the baseline tree carries the PSIO_PP_VARIADIC_TO_SEQ
// macro of reflect.hpp and nothing else from later commits.)

#include <psio/ext_int.hpp>
#include <psio/json.hpp>

#include "harness.hpp"
#include "wide_record_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bench
{
   struct Small
   {
      std::uint32_t id = 0;
      std::string   name;
      bool          active = false;
      double        score = 0;
      std::uint64_t count = 0;
   };
   PSIO_REFLECT(Small, id, name, active, score, count)

   struct Address
   {
      std::string                street;
      std::string                city;
      std::optional<std::string> region;
      std::uint32_t              zip = 0;
   };
   PSIO_REFLECT(Address, street, city, region, zip)

   struct LineItem
   {
      std::uint32_t                sku = 0;
      std::string                  title;
      std::uint32_t                quantity = 0;
      double                       price = 0;
      std::optional<std::uint32_t> discount;
   };
   PSIO_REFLECT(LineItem, sku, title, quantity, price, discount)

   struct Card
   {
      std::string   last4;
      std::uint32_t expiry = 0;
   };
   PSIO_REFLECT(Card, last4, expiry)

   using Payment = std::variant<std::uint32_t, std::string, Card>;

   struct Medium
   {
      std::uint64_t              order_id = 0;
      std::string                customer;
      std::string                email;
      Address                    shipping;
      std::optional<Address>     billing;
      Payment                    payment;
      std::vector<LineItem>      items;
      std::optional<std::string> note;
      std::vector<std::string>   tags;
      bool                       gift = false;
   };
   PSIO_REFLECT(Medium, order_id, customer, email, shipping, billing, payment, items, note, tags,
                gift)

   struct WideInt
   {
      std::uint64_t              a = 0;
      std::uint64_t              b = 0;
      std::int64_t               c = 0;
      std::int64_t               d = 0;
      std::uint64_t              e = 0;
      std::uint64_t              f = 0;
      psio::uint128              g = 0;
      psio::uint128              h = 0;
      psio::int128               i = 0;
      psio::uint128              j = 0;
      std::vector<std::uint64_t> balances;
   };
   PSIO_REFLECT(WideInt, a, b, c, d, e, f, g, h, i, j, balances)

   inline Small small()
   {
      return {1234567, "a small record", true, 0.8125, 9007199254740991ull};
   }
   inline Medium medium()
   {
      Medium m;
      m.order_id = 9007199254740993ull;
      m.customer = "Ada Lovelace";
      m.email    = "ada@example.org";
      m.shipping = {"12 Analytical Engine Way", "London", "Greater London", 10100};
      m.billing  = Address{"1 Difference Street", "Cambridge", std::nullopt, 20200};
      m.payment  = Card{"4242", 1230};
      for (std::uint32_t i = 0; i < 20; ++i)
         m.items.push_back({100000 + i * 7, "item number " + std::to_string(i) + " of the order",
                            1 + i % 4, 9.99 + i * 1.25,
                            i % 3 ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{5 + i}});
      m.note = "leave with the \"porter\" \\ ring twice";
      m.tags = {"priority", "fragile", "gift-wrap"};
      m.gift = true;
      return m;
   }
   inline std::vector<Small> large()
   {
      std::vector<Small> v;
      for (std::uint32_t i = 0; i < 10000; ++i)
         v.push_back({i * 2654435761u, "record " + std::to_string(i), (i & 1) != 0, i * 0.37,
                      std::uint64_t{i} * 1000003ull});
      return v;
   }
   inline WideInt wideint()
   {
      WideInt w;
      w.a = 18446744073709551615ull;
      w.b = 9007199254740993ull;
      w.c = -9223372036854775807ll - 1;
      w.d = 9223372036854775807ll;
      w.e = 12345678901234567890ull;
      w.f = 42;
      w.g = (psio::uint128{1} << 100) + 12345;
      w.h = ~psio::uint128{0};
      w.i = -(psio::int128{1} << 90);
      w.j = 7;
      for (std::uint64_t k = 0; k < 32; ++k)
         w.balances.push_back(18446744073709551615ull - k * 0x0123456789abcdefull);
      return w;
   }
   inline wide_fixture::Wide200 wide200()
   {
      wide_fixture::Wide200 w;
      std::uint32_t         n = 0;
      psio::reflect<wide_fixture::Wide200>::for_each_field(
         w, [&](auto, std::string_view, std::uint32_t& f) { f = ++n * 7919; });
      return w;
   }
   // The members of a one-level object text, in reverse order.
   inline std::string reversed(const std::string& text)
   {
      std::vector<std::string_view> members;
      std::size_t                   at = 1;
      while (at < text.size() - 1)
      {
         std::size_t end = text.find(',', at);
         if (end == std::string::npos)
            end = text.size() - 1;
         members.push_back(std::string_view{text}.substr(at, end - at));
         at = end + 1;
      }
      std::string out = "{";
      for (std::size_t i = members.size(); i-- > 0;)
      {
         out += members[i];
         out += i ? "," : "}";
      }
      return out;
   }

   inline std::string g_filter;
   inline std::size_t g_sink = 0;

   // Many short trials, the minimum reported: short trials keep the minimum
   // meaningful on a machine that is doing other work. (The harness's
   // ns_per_iter runs at least 1024 iterations a trial, which is seconds
   // for the 10 000-record shape; this uses its batch timer directly.)
   template <typename F>
   psio_bench::timing measure(F&& body)
   {
      using psio_bench::detail::run_batch;
      std::size_t iters = 1;
      double      ns    = run_batch(iters, body);
      while (ns < 4'000'000.0 && iters < (std::size_t{1} << 30))
      {
         iters *= ns < 400'000.0 ? 8 : 2;
         ns = run_batch(iters, body);
      }
      const int           trials = 60;
      std::vector<double> samples;
      for (int t = 0; t < trials; ++t)
         samples.push_back(run_batch(iters, body) / static_cast<double>(iters));
      std::sort(samples.begin(), samples.end());
      return {samples.front(), samples[samples.size() / 2], 0.0, iters, trials};
   }

   inline void report(std::string_view shape, std::string_view format, std::string_view op,
                      std::string_view impl, std::size_t bytes, const psio_bench::timing& t)
   {
      const double mbps = static_cast<double>(bytes) / t.min_ns * 1000.0;
      std::printf("%-9s %-6s %-7s %-11s %8zu %12.1f %9.1f\n", std::string(shape).c_str(),
                  std::string(format).c_str(), std::string(op).c_str(),
                  std::string(impl).c_str(), bytes, t.min_ns, mbps);
      std::fflush(stdout);
   }

   template <typename Fmt, typename T>
   void decode_cells(std::string_view shape, const std::string& text)
   {
      const std::string_view      format = Fmt::name;
      const std::span<const char> in{text};
#ifdef PSIO_BENCH_BASELINE
      {
         T    out{};
         auto t = measure([&](std::size_t) {
            if (!psio::decode_json_checked(Fmt{}, in, out).ok())
               std::abort();
            g_sink += sizeof(out);
         });
         report(shape, format, "decode", "before", text.size(), t);
      }
#else
      auto cell = [&](std::string_view impl, psio::json_parser parser) {
         T    out{};
         auto t = measure([&](std::size_t) {
            if (!psio::decode_json_checked(Fmt{}, in, out, parser).ok())
               std::abort();
            g_sink += sizeof(out);
         });
         report(shape, format, "decode", impl, text.size(), t);
      };
      cell("text", psio::json_parser::text);
#if PSIO_HAVE_SIMDJSON
      cell("ondemand", psio::json_parser::simdjson);
      cell("index", psio::json_parser::simdjson_index);
      {
         std::string padded = text;
         const auto  view   = psio::padded_json::of(padded);
         T           out{};
         auto        t      = measure([&](std::size_t) {
            if (!psio::decode_json_checked(Fmt{}, view, out, psio::json_parser::simdjson_index)
                    .ok())
               std::abort();
            g_sink += sizeof(out);
         });
         report(shape, format, "decode", "index-p", text.size(), t);
      }
#endif
      cell("auto", psio::json_parser::automatic);
#endif
   }

   template <typename Fmt, typename T>
   void encode_cell(std::string_view shape, const T& value)
   {
      std::string out;
      auto        t = measure([&](std::size_t) {
         out.clear();
         if (!psio::encode_json_checked(Fmt{}, value, out).ok())
            std::abort();
         g_sink += out.size();
      });
#ifdef PSIO_BENCH_BASELINE
      report(shape, Fmt::name, "encode", "before", out.size(), t);
#else
      report(shape, Fmt::name, "encode", "after", out.size(), t);
#endif
   }

   template <typename T>
   void shape(std::string_view name, const T& value)
   {
      if (!g_filter.empty() && name.find(g_filter) == std::string_view::npos)
         return;
      decode_cells<psio::json, T>(name, psio::encode(psio::json{}, value));
      decode_cells<psio::ijson, T>(name, psio::encode(psio::ijson{}, value));
      encode_cell<psio::json, T>(name, value);
      encode_cell<psio::ijson, T>(name, value);
   }
}  // namespace bench

int main(int argc, char** argv)
{
   using namespace bench;
   if (argc > 1)
      g_filter = argv[1];
   std::printf("%-9s %-6s %-7s %-11s %8s %12s %9s\n", "shape", "format", "op", "impl", "bytes",
               "ns/op", "MB/s");
   shape("small", small());
   shape("medium", medium());
   shape("large", large());
   shape("wideint", wideint());
   shape("wide200", wide200());
   if (g_filter.empty() || std::string_view{"wide200r"}.find(g_filter) != std::string_view::npos)
   {
      using W = wide_fixture::Wide200;
      decode_cells<psio::json, W>("wide200r", reversed(psio::encode(psio::json{}, wide200())));
      decode_cells<psio::ijson, W>("wide200r", reversed(psio::encode(psio::ijson{}, wide200())));
   }
   return g_sink == 0;
}
