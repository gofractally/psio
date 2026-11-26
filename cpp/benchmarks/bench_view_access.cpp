// What a view costs per access, and what the structural checks of a view
// cost (gofractally/psio#15).
//
//   safe     psio::pssz_view<T>{bytes}                         any bytes
//   trusted  psio::pssz_trusted_view<T>{psio::trusted{bytes}}  the caller's word
//
// The row is the shape of a service's table row: fixed fields and three
// strings. `Order` adds a list of records. Each cell constructs the view
// over bytes the optimiser cannot see through (as a row fetched from a
// table is) and reads through it; "held" cells read through a view that
// already exists.
//
// ns per operation, the best of `trials` runs of `iters` operations each.
//
//   psio_bench_view_access [iters] [trials]

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct Row
{
   std::uint64_t owner   = 0;
   std::uint64_t balance = 0;
   std::uint32_t flags   = 0;
   std::uint32_t nonce   = 0;
   std::string   name;
   std::string   url;
   std::string   memo;
   PSIO_REFLECT(Row, owner, balance, flags, nonce, name, url, memo)
};
struct Item
{
   std::string   product;
   std::uint32_t qty = 0;
   PSIO_REFLECT(Item, product, qty)
};
struct Order
{
   std::uint64_t     id = 0;
   Row               customer;
   std::vector<Item> items;
   PSIO_REFLECT(Order, id, customer, items)
};

namespace
{
   int g_iters  = 2000000;
   int g_trials = 15;

   // The bytes come from somewhere the optimiser cannot see.
   [[gnu::always_inline]] inline std::span<const char> opaque(std::span<const char> s)
   {
      const char* p = s.data();
      std::size_t n = s.size();
      asm volatile("" : "+r"(p), "+r"(n) : : "memory");
      return {p, n};
   }
   template <typename T>
   [[gnu::always_inline]] inline void consume(T value)
   {
      asm volatile("" : : "r"(value) : "memory");
   }

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

   template <typename V>
   [[gnu::always_inline]] inline std::uint64_t all_of(const V& v)
   {
      const std::string_view name{v.name()}, url{v.url()}, memo{v.memo()};
      return v.owner() + v.balance() + v.flags() + v.nonce() + name.size() + url.size() +
             memo.size() + static_cast<unsigned char>(name[0]) +
             static_cast<unsigned char>(url[0]) + static_cast<unsigned char>(memo[0]);
   }

   template <typename RowView, typename OrderView, typename Make>
   void column(const char* kind, std::span<const char> row, std::span<const char> order, Make make)
   {
      std::printf("%-8s", kind);
      // construct + one fixed field
      std::printf(" %8.2f", best([&] { consume(std::uint64_t{make.template of<RowView>(opaque(row)).balance()}); }));
      // construct + the last fixed field
      std::printf(" %8.2f", best([&] { consume(std::uint32_t{make.template of<RowView>(opaque(row)).nonce()}); }));
      // construct + one string (the first), its size and a byte
      std::printf(" %8.2f", best([&] {
                     const std::string_view s{make.template of<RowView>(opaque(row)).name()};
                     consume(s.size() + static_cast<unsigned char>(s[0]));
                  }));
      // construct + the last string
      std::printf(" %8.2f", best([&] {
                     const std::string_view s{make.template of<RowView>(opaque(row)).memo()};
                     consume(s.size() + static_cast<unsigned char>(s[0]));
                  }));
      // construct + every member
      std::printf(" %8.2f", best([&] { consume(all_of(make.template of<RowView>(opaque(row)))); }));
      // a held view: one fixed field, one string, per access
      {
         const auto held = make.template of<RowView>(opaque(row));
         std::printf(" %8.2f", best([&] {
                        const auto* p = &held;
                        asm volatile("" : "+r"(p) : : "memory");
                        consume(std::uint64_t{p->balance()});
                     }));
         std::printf(" %8.2f", best([&] {
                        const auto* p = &held;
                        asm volatile("" : "+r"(p) : : "memory");
                        const std::string_view s{p->url()};
                        consume(s.size() + static_cast<unsigned char>(s[0]));
                     }));
      }
      // a nested record's member, and a list of 16 records walked
      std::printf(" %8.2f", best([&] {
                     consume(std::uint64_t{make.template of<OrderView>(opaque(order)).customer().balance()});
                  }));
      std::printf(" %8.2f", best([&] {
                     std::uint64_t sum = 0;
                     for (auto item : make.template of<OrderView>(opaque(order)).items())
                        sum += item.qty();
                     consume(sum);
                  }));
      std::printf("\n");
   }

   struct make_safe
   {
      template <typename V>
      [[gnu::always_inline]] static V of(std::span<const char> s) { return V{s}; }
   };
   struct make_trusted
   {
      template <typename V>
      [[gnu::always_inline]] static V of(std::span<const char> s) { return V{psio::trusted{s}}; }
   };
}  // namespace

int main(int argc, char** argv)
{
   if (argc > 1) g_iters = std::atoi(argv[1]);
   if (argc > 2) g_trials = std::atoi(argv[2]);
   const Row row{0x1234, 1000000, 3, 42, "alice.account", "https://example.org/alice", "a memo of ordinary length"};
   Order      order{7, row, {}};
   for (std::uint32_t i = 0; i < 16; ++i)
      order.items.push_back(Item{"product-" + std::to_string(i), i});
   const auto row_bytes   = psio::to_pssz(row);
   const auto order_bytes = psio::to_pssz(order);
   std::printf("ns per operation (row %zu bytes, order %zu bytes), best of %d x %d\n",
               row_bytes.size(), order_bytes.size(), g_trials, g_iters);
   std::printf("%-8s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n", "", "fixed", "fixedN", "string", "stringN",
               "all", "held fx", "held st", "nested", "list16");
   column<psio::pssz_view<Row>, psio::pssz_view<Order>, make_safe>("safe", row_bytes, order_bytes, make_safe{});
   column<psio::pssz_trusted_view<Row>, psio::pssz_trusted_view<Order>, make_trusted>(
      "trusted", row_bytes, order_bytes, make_trusted{});
   return 0;
}
