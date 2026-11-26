// The two write modes (docs/annotations.md "Reading and writing: the two
// modes"; gofractally/psio#14): what the validated write costs over the
// fast write, per format and per kind of declaration.
//
//   fast       psio::to_F(v, sink)            holds nothing against the type
//   validated  psio::encode_F_checked(v, out) refuses a violating value
//
// The values are inside their declarations, so both modes write the same
// bytes; the difference is the price of the checks. ns per encode, the best
// of `trials` runs of `iters` encodes each.
//
//   psio_bench_write_modes [iters] [trials]

#include <psio/flatbuf.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

struct Account
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(Account,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance)
};
struct AccountPlain
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountPlain, id, owner, tags, balance)
};
struct Text
{
   std::string body;
   PSIO_REFLECT(Text, attr(body, utf8 | max<4096>))
};
struct UniqueIds
{
   std::vector<std::uint64_t> ids;
   PSIO_REFLECT(UniqueIds, attr(ids, unique | max<1024>))
};
struct BoundedList
{
   std::vector<std::uint32_t> ids;
   PSIO_REFLECT(BoundedList, attr(ids, max<1024>))
};
struct SortedNames
{
   std::vector<std::string> names;
   PSIO_REFLECT(SortedNames, attr(names, sorted | max<256> | element(utf8 | max<32>)))
};

namespace
{
   int         g_iters  = 20000;
   int         g_trials = 9;
   std::size_t g_sink   = 0;

   template <typename F>
   double best(F&& body)
   {
      double result = 1e300;
      for (int t = 0; t < g_trials; ++t)
      {
         const auto t0 = std::chrono::steady_clock::now();
         for (int i = 0; i < g_iters; ++i)
            body();
         const auto t1 = std::chrono::steady_clock::now();
         result = std::min(result, std::chrono::duration<double, std::nano>(t1 - t0).count() / g_iters);
      }
      return result;
   }

   template <typename T>
   void row(const char* name, const T& value)
   {
      std::vector<char> out;
      out.reserve(1 << 16);
      std::string text;
      text.reserve(1 << 16);
      auto fail = [&](const psio::codec_status& st) {
         if (!st.ok())
         {
            std::fprintf(stderr, "%s: %s\n", name, std::string(st.error().what).c_str());
            std::exit(1);
         }
      };
      struct cell
      {
         const char* format;
         double      fast, validated;
      };
      std::vector<cell> cells;
      cells.push_back({"pssz", best([&] { out.clear(); psio::to_pssz(value, out); g_sink += out.size(); }),
                       best([&] { out.clear(); fail(psio::encode_pssz_checked(value, out)); g_sink += out.size(); })});
      cells.push_back({"key", best([&] { out.clear(); psio::to_key(value, out); g_sink += out.size(); }),
                       best([&] { out.clear(); fail(psio::encode_key_checked(value, out)); g_sink += out.size(); })});
      cells.push_back({"json", best([&] { text.clear(); psio::to_json(value, text); g_sink += text.size(); }),
                       best([&] { text.clear(); fail(psio::encode_json_checked(value, text)); g_sink += text.size(); })});
      cells.push_back({"flatbuf", best([&] { psio::to_flatbuf(value, out); g_sink += out.size(); }),
                       best([&] { out.clear(); fail(psio::encode_flatbuf_checked(value, out)); g_sink += out.size(); })});
      cells.push_back({"protobuf", best([&] { out.clear(); psio::to_protobuf(value, out); g_sink += out.size(); }),
                       best([&] { out.clear(); fail(psio::encode_protobuf_checked(value, out)); g_sink += out.size(); })});
      for (const cell& c : cells)
         std::printf("| %-12s | %-8s | %9.1f | %9.1f | %6.1fx |\n", name, c.format, c.fast,
                     c.validated, c.validated / c.fast);
   }
}  // namespace

int main(int argc, char** argv)
{
   if (argc > 1)
      g_iters = std::atoi(argv[1]);
   if (argc > 2)
      g_trials = std::atoi(argv[2]);
   std::printf("| value        | format   | fast ns   | valid. ns | ratio   |\n");
   std::printf("|--------------|----------|-----------|-----------|---------|\n");
   row("Account", Account{7, "alice-the-account-owner", {"vip", "beta", "early-adopter", "eu"}, 100});
   row("AccountPlain",
       AccountPlain{7, "alice-the-account-owner", {"vip", "beta", "early-adopter", "eu"}, 100});
   row("Text4000", Text{std::string(4000, 'x')});
   {
      UniqueIds v;
      v.ids.resize(1000);
      std::iota(v.ids.begin(), v.ids.end(), 1000);
      row("Unique1000", v);
   }
   {
      BoundedList v;
      v.ids.resize(1000);
      std::iota(v.ids.begin(), v.ids.end(), 1);
      row("List1000", v);
   }
   {
      SortedNames v;
      for (int i = 0; i < 200; ++i)
      {
         char buf[32];
         std::snprintf(buf, sizeof buf, "name-%05d-of-a-user", i);
         v.names.push_back(buf);
      }
      row("Sorted200", v);
   }
   return g_sink == 0;
}
