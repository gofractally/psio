#pragma once
// Data shapes of bench_graphql: plain structs, reflected once per engine
// (PSIO_REFLECT in bench_graphql.cpp, PSIO1_REFLECT in bench_graphql_old.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace gqlbench
{
   struct Small
   {
      std::uint32_t id = 0;
      std::string   name;
      std::uint32_t a = 0, b = 0, c = 0;
      double        score  = 0;
      bool          active = false;
   };

   // A table row: 20 fields, of which the queries select 3.
   struct Row20
   {
      std::uint64_t id = 0;
      std::string   name;
      std::string   owner;
      std::uint32_t f3 = 0;
      std::uint32_t f4 = 0;
      std::uint64_t f5 = 0;
      double        f6 = 0;
      bool          f7 = false;
      std::string   f8;
      std::uint32_t f9  = 0;
      std::uint32_t f10 = 0;
      std::int64_t  f11 = 0;
      std::string   f12;
      std::uint16_t f13 = 0;
      std::uint32_t f14 = 0;
      double        f15 = 0;
      std::string   f16;
      std::uint64_t f17 = 0;
      std::uint32_t f18 = 0;
      bool          f19 = false;
   };

   // Large strings and vectors next to two small fields.
   struct WideRow
   {
      std::uint64_t              id    = 0;
      std::string                text1;  // 4 KiB
      std::vector<std::uint64_t> numbers;  // 512
      std::string                text2;  // 4 KiB
      std::vector<std::string>   strings;  // 32 x 64 bytes
      std::uint32_t              count = 0;
   };

   struct Child
   {
      std::uint32_t n = 0;
      std::string   label;
   };
   struct Parent
   {
      std::uint64_t      id = 0;
      std::string        name;
      std::vector<Child> children;
   };

   struct Data
   {
      Small                small;
      std::vector<Row20>   rows100;
      std::vector<Row20>   rows10k;
      std::vector<WideRow> wide;     // 100
      std::vector<Parent>  parents;  // 100 x 10 children
   };

   inline Row20 make_row(std::uint64_t n)
   {
      Row20 r;
      r.id    = 1'000'000 + n;
      r.name  = "account-" + std::to_string(n);
      r.owner = "owner-" + std::to_string(n % 97);
      r.f3    = static_cast<std::uint32_t>(n * 3);
      r.f4    = static_cast<std::uint32_t>(n * 5);
      r.f5    = n * 1'000'003;
      r.f6    = 0.25 * static_cast<double>(n);
      r.f7    = n % 2 == 0;
      r.f8    = "memo for row " + std::to_string(n) + " with some more text in it";
      r.f9    = static_cast<std::uint32_t>(n + 9);
      r.f10   = static_cast<std::uint32_t>(n + 10);
      r.f11   = -static_cast<std::int64_t>(n);
      r.f12   = "tag" + std::to_string(n % 13);
      r.f13   = static_cast<std::uint16_t>(n);
      r.f14   = static_cast<std::uint32_t>(n + 14);
      r.f15   = 1.5 * static_cast<double>(n);
      r.f16   = "https://example.invalid/item/" + std::to_string(n);
      r.f17   = n * 17;
      r.f18   = static_cast<std::uint32_t>(n + 18);
      r.f19   = n % 3 == 0;
      return r;
   }

   inline Data make_data()
   {
      Data d;
      d.small = Small{42, "the answer", 1, 2, 3, 6.5, true};
      for (std::uint64_t n = 0; n < 100; ++n)
         d.rows100.push_back(make_row(n));
      for (std::uint64_t n = 0; n < 10'000; ++n)
         d.rows10k.push_back(make_row(n));
      for (std::uint64_t n = 0; n < 100; ++n)
      {
         WideRow w;
         w.id    = n;
         w.text1 = std::string(4096, static_cast<char>('a' + n % 26));
         w.numbers.assign(512, n);
         w.text2 = std::string(4096, static_cast<char>('A' + n % 26));
         w.strings.assign(32, std::string(64, 'x'));
         w.count = static_cast<std::uint32_t>(n * 2);
         d.wide.push_back(std::move(w));
      }
      for (std::uint64_t n = 0; n < 100; ++n)
      {
         Parent p;
         p.id   = n;
         p.name = "parent-" + std::to_string(n);
         for (std::uint32_t c = 0; c < 10; ++c)
            p.children.push_back(Child{c, "child-" + std::to_string(c)});
         d.parents.push_back(std::move(p));
      }
      return d;
   }

   // The old engine (psio1::gql_query), built in its own translation unit.
   // old_root copies `data` into a root object the old engine can query.
   bool        old_available();
   void*       old_root(const Data& data);
   std::string old_run(void* root, std::string_view query);
}  // namespace gqlbench
