// Compile-time lookup tables (name_lookup.hpp) and their use in
// psio::reflect<T>: by name, by field number, and by a caller-supplied hash.

#include <psio/name_lookup.hpp>
#include <psio/reflect.hpp>

#include "wide_record_fixture.hpp"

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

namespace
{
   struct Empty
   {
   };
   PSIO_REFLECT(Empty)

   struct One
   {
      int only = 0;
   };
   PSIO_REFLECT(One, only)

   struct Mixed
   {
      int         a = 0;
      int         bb = 0;
      std::string name;
      int         a_rather_long_member_name_beyond_sixteen_bytes = 0;
      int         a_rather_long_member_name_beyond_sixteen_bytez = 0;
   };
   PSIO_REFLECT(Mixed, a, bb, name, a_rather_long_member_name_beyond_sixteen_bytes,
                a_rather_long_member_name_beyond_sixteen_bytez)

   // FNV-1a, as a caller's own method-name hash.
   struct Fnv
   {
      constexpr std::uint64_t operator()(std::string_view s) const noexcept
      {
         std::uint64_t h = 0xcbf29ce484222325ull;
         for (char c : s)
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
         return h;
      }
   };

   // N generated names "nXXXX" with static storage.
   template <std::size_t N>
   struct generated_names
   {
      std::array<std::array<char, 8>, N> text{};
      constexpr generated_names()
      {
         for (std::size_t i = 0; i < N; ++i)
         {
            text[i][0] = 'n';
            std::size_t v = i;
            for (int d = 4; d >= 1; --d, v /= 10)
               text[i][d] = static_cast<char>('0' + v % 10);
         }
      }
   };
   template <std::size_t N>
   inline constexpr generated_names<N> storage{};
   template <std::size_t N>
   constexpr std::array<std::string_view, N> views()
   {
      std::array<std::string_view, N> out{};
      for (std::size_t i = 0; i < N; ++i)
         out[i] = std::string_view{storage<N>.text[i].data(), 5};
      return out;
   }

   // Names that agree in length, first eight and last eight bytes: tier 0
   // cannot tell them apart.
   inline constexpr std::array<std::string_view, 4> same_ends{
      "prefix__AAAA__suffix"sv, "prefix__BBBB__suffix"sv, "prefix__CCCC__suffix"sv,
      "prefix__DDDD__suffix"sv};

   struct SparseNumbers
   {
      static constexpr std::array<std::uint32_t, 4> numbers{7, 1000000, 3, 4000000000u};
   };
   struct DenseNumbers
   {
      static constexpr std::array<std::uint32_t, 4> numbers{5, 2, 9, 1};
   };
   struct IdentityNumbers
   {
      static constexpr std::array<std::uint32_t, 3> numbers{1, 2, 3};
   };
   struct RepeatedNumbers
   {
      static constexpr std::array<std::uint32_t, 3> numbers{4, 2, 4};
   };
}  // namespace

TEST_CASE("name_table: zero, one and many names, at compile time", "[name_lookup]")
{
   constexpr auto none = psio::make_name_table(std::array<std::string_view, 0>{});
   STATIC_REQUIRE(none.find("x") == 0);

   constexpr auto one = psio::make_name_table(std::array<std::string_view, 1>{"only"sv});
   STATIC_REQUIRE(one.find("only") == 0);
   STATIC_REQUIRE(one.find("onlx") == 1);
   STATIC_REQUIRE(one.find("") == 1);

   static constexpr auto names = views<600>();
   static constexpr auto many  = psio::make_name_table(names);
   STATIC_REQUIRE(many.tier == 0);
   STATIC_REQUIRE(many.keys.perfect);
   STATIC_REQUIRE(many.find("n0000") == 0);
   STATIC_REQUIRE(many.find("n0599") == 599);
   STATIC_REQUIRE(many.find("n0600") == 600);
   for (std::size_t i = 0; i < names.size(); ++i)
      REQUIRE(many.find(names[i]) == i);
   // Absent names of every length, including ones longer than any member.
   for (std::string probe = ""; probe.size() < 40; probe += 'q')
      REQUIRE(many.find(probe) == many.npos);
   REQUIRE(many.find("n00000") == many.npos);
   REQUIRE(many.find("N0001") == many.npos);
}

TEST_CASE("name_table: tier 1 when length, head and tail agree", "[name_lookup]")
{
   static constexpr auto t = psio::make_name_table(same_ends);
   STATIC_REQUIRE(t.tier == 1);
   STATIC_REQUIRE(t.keys.perfect);
   STATIC_REQUIRE(t.find("prefix__CCCC__suffix") == 2);
   STATIC_REQUIRE(t.find("prefix__EEEE__suffix") == 4);
   for (std::size_t i = 0; i < same_ends.size(); ++i)
      CHECK(t.find(same_ends[i]) == i);
}

TEST_CASE("name_table: a set no hash separates falls back to a search", "[name_lookup]")
{
   // A repeated name defeats both hashes; the table still builds and a
   // repeated name resolves to its first occurrence.
   static constexpr std::array<std::string_view, 5> names{"b"sv, "a"sv, "b"sv, "ccc"sv,
                                                         "a"sv};
   static constexpr auto t = psio::make_name_table(names);
   STATIC_REQUIRE(t.tier == 2);
   STATIC_REQUIRE(t.find("b") == 0);
   STATIC_REQUIRE(t.find("a") == 1);
   STATIC_REQUIRE(t.find("ccc") == 3);
   STATIC_REQUIRE(t.find("cc") == 5);
   STATIC_REQUIRE(t.find("") == 5);
}

TEST_CASE("hashed_table: lookup by a caller-supplied 64-bit hash", "[name_lookup]")
{
   static constexpr auto names = views<300>();
   static constexpr auto t     = psio::make_hashed_table<Fnv>(names);
   STATIC_REQUIRE(t.unique);
   STATIC_REQUIRE(t.find(Fnv{}("n0123")) == 123);
   STATIC_REQUIRE(t.find(Fnv{}("absent")) == 300);
   for (std::size_t i = 0; i < names.size(); ++i)
   {
      REQUIRE(t.find(Fnv{}(names[i])) == i);
      REQUIRE(t.key(i) == Fnv{}(names[i]));
   }
   REQUIRE(t.find(0) == t.npos);

   // A hash under which two names collide is reported, not hidden.
   struct Constant
   {
      constexpr std::uint64_t operator()(std::string_view) const noexcept { return 7; }
   };
   constexpr auto bad = psio::make_hashed_table<Constant>(same_ends);
   STATIC_REQUIRE(!bad.unique);
}

TEST_CASE("number_table: direct, identity and sparse", "[name_lookup]")
{
   constexpr auto identity = psio::make_number_table<IdentityNumbers>();
   STATIC_REQUIRE(identity.identity);
   STATIC_REQUIRE(identity.find(0) == 3);
   STATIC_REQUIRE(identity.find(1) == 0);
   STATIC_REQUIRE(identity.find(3) == 2);
   STATIC_REQUIRE(identity.find(4) == 3);

   constexpr auto dense = psio::make_number_table<DenseNumbers>();
   STATIC_REQUIRE(!dense.identity);
   STATIC_REQUIRE(dense.unique);
   STATIC_REQUIRE(dense.find(5) == 0);
   STATIC_REQUIRE(dense.find(2) == 1);
   STATIC_REQUIRE(dense.find(9) == 2);
   STATIC_REQUIRE(dense.find(1) == 3);
   STATIC_REQUIRE(dense.find(0) == 4);
   STATIC_REQUIRE(dense.find(3) == 4);
   STATIC_REQUIRE(dense.find(10) == 4);
   STATIC_REQUIRE(dense.find(0xFFFFFFFFu) == 4);

   constexpr auto sparse = psio::make_number_table<SparseNumbers>();
   STATIC_REQUIRE(sparse.unique);
   STATIC_REQUIRE(sparse.find(7) == 0);
   STATIC_REQUIRE(sparse.find(1000000) == 1);
   STATIC_REQUIRE(sparse.find(3) == 2);
   STATIC_REQUIRE(sparse.find(4000000000u) == 3);
   STATIC_REQUIRE(sparse.find(8) == 4);

   constexpr auto repeated = psio::make_number_table<RepeatedNumbers>();
   STATIC_REQUIRE(!repeated.unique);
   STATIC_REQUIRE(repeated.find(4) == 0);
}

TEST_CASE("runtime_name_index: names known only at run time", "[name_lookup]")
{
   std::vector<std::string> storage;
   for (int i = 0; i < 500; ++i)
      storage.push_back("member_" + std::to_string(i * 7919));
   storage.push_back(storage[3]);  // repeated: first occurrence wins
   std::vector<std::string_view> names(storage.begin(), storage.end());
   const psio::runtime_name_index index(names);
   REQUIRE(index.size() == 501);
   for (std::size_t i = 0; i < 500; ++i)
      REQUIRE(index.find(storage[i]) == i);
   REQUIRE(index.find(storage[500]) == 3);
   REQUIRE(index.find("member_") == 501);
   REQUIRE(index.find("") == 501);

   const psio::runtime_name_index empty;
   REQUIRE(empty.find("x") == 0);
}

TEST_CASE("reflect: lookups for 0, 1 and 200 members", "[name_lookup][reflect]")
{
   using E = psio::reflect<Empty>;
   STATIC_REQUIRE(!E::index_of("x").has_value());
   STATIC_REQUIRE(E::find_index("x") == 0);
   STATIC_REQUIRE(!E::index_of_hash<Fnv>(1).has_value());

   using O = psio::reflect<One>;
   STATIC_REQUIRE(O::index_of("only") == 0);
   STATIC_REQUIRE(!O::index_of("onl").has_value());
   STATIC_REQUIRE(O::find_index("nope") == 1);
   STATIC_REQUIRE(O::index_of_field_number(1) == 0);
   STATIC_REQUIRE(!O::index_of_field_number(0).has_value());
   STATIC_REQUIRE(!O::index_of_field_number(2).has_value());

   using M = psio::reflect<Mixed>;
   STATIC_REQUIRE(M::index_of("a") == 0);
   STATIC_REQUIRE(M::index_of("bb") == 1);
   STATIC_REQUIRE(M::index_of("name") == 2);
   STATIC_REQUIRE(M::index_of("a_rather_long_member_name_beyond_sixteen_bytes") == 3);
   STATIC_REQUIRE(M::index_of("a_rather_long_member_name_beyond_sixteen_bytez") == 4);
   STATIC_REQUIRE(!M::index_of("a_rather_long_member_name_beyond_sixteen_bytex").has_value());
   STATIC_REQUIRE(!M::index_of("").has_value());

   using W = psio::reflect<wide_fixture::Wide200>;
   STATIC_REQUIRE(W::member_count == 200);
   STATIC_REQUIRE(W::index_of(W::member_name<0>) == 0);
   STATIC_REQUIRE(W::index_of(W::member_name<137>) == 137);
   STATIC_REQUIRE(W::index_of(W::member_name<199>) == 199);
   STATIC_REQUIRE(W::index_of_field_number(200) == 199);
   STATIC_REQUIRE(!W::index_of_field_number(201).has_value());
   for (std::size_t i = 0; i < W::member_count; ++i)
   {
      REQUIRE(W::index_of(W::_names_array_[i]) == i);
      REQUIRE(W::index_of_hash<Fnv>(Fnv{}(W::_names_array_[i])) == i);
      std::string near{W::_names_array_[i]};
      near.back() ^= 1;
      const auto hit = W::index_of(near);
      REQUIRE((!hit || W::_names_array_[*hit] == near));
   }
   REQUIRE(!W::index_of("field_200").has_value());
   REQUIRE(!W::index_of_hash<Fnv>(12345).has_value());
}

TEST_CASE("reflect: visit by name, number and index reach the right member",
          "[name_lookup][reflect]")
{
   wide_fixture::Wide200 w;
   using W = psio::reflect<wide_fixture::Wide200>;
   for (std::size_t i = 0; i < W::member_count; ++i)
   {
      REQUIRE(W::visit_field_by_name(w, W::_names_array_[i], [&](std::uint32_t& f) {
         f = static_cast<std::uint32_t>(i + 1);
      }));
   }
   std::size_t at = 0;
   W::for_each_field(w, [&](auto, std::string_view, std::uint32_t& f) {
      REQUIRE(f == ++at);
   });
   for (std::uint32_t n = 1; n <= 200; ++n)
      REQUIRE(W::visit_field_by_number(w, n, [&](std::uint32_t& f) { REQUIRE(f == n); }));
   REQUIRE(!W::visit_field_by_number(w, 0, [](std::uint32_t&) {}));
   REQUIRE(!W::visit_field_by_number(w, 201, [](std::uint32_t&) {}));
   REQUIRE(!W::visit_field_by_name(w, "field_", [](std::uint32_t&) {}));
   REQUIRE(W::visit_field_by_index(w, 5, [](std::uint32_t& f) { REQUIRE(f == 6); }));
   REQUIRE(!W::visit_field_by_index(w, 200, [](std::uint32_t&) {}));

   // In a constant expression.
   constexpr int picked = [] {
      Mixed m{.a = 1, .bb = 22};
      int   out = 0;
      psio::reflect<Mixed>::visit_field_by_name(m, "bb", [&](auto& f) {
         if constexpr (std::is_same_v<std::remove_cvref_t<decltype(f)>, int>)
            out = f;
      });
      return out;
   }();
   STATIC_REQUIRE(picked == 22);
}

TEST_CASE("reflect: index_of_hash with a caller hash", "[name_lookup][reflect]")
{
   using M = psio::reflect<Mixed>;
   STATIC_REQUIRE(M::index_of_hash<Fnv>(Fnv{}("name")) == 2);
   STATIC_REQUIRE(M::index_of_hash<Fnv>(Fnv{}("bb")) == 1);
   STATIC_REQUIRE(!M::index_of_hash<Fnv>(Fnv{}("absent")).has_value());
   // A lambda's closure type works as the hash.
   using Len = decltype([](std::string_view s) { return std::uint64_t{s.size()}; });
   STATIC_REQUIRE(psio::reflect<One>::index_of_hash<Len>(4) == 0);
   // Under `Len`, Mixed's last two members collide: index_of_hash<Len> on
   // Mixed is a compile error (name_lookup_collision_fail.cpp).
}
