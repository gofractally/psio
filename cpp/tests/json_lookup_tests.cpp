// Member lookup in the typed JSON / I-JSON readers: the expected-order fast
// path, the hashed fallback for any other order, repeated and unknown keys,
// and I-JSON variant alternatives by name.

#include <psio/json.hpp>

#include "wide_record_fixture.hpp"

#include <catch.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace
{
   struct Leaf
   {
      std::uint32_t id = 0;
      std::string   tag;
      friend bool   operator==(const Leaf&, const Leaf&) = default;
   };
   PSIO_REFLECT(Leaf, id, tag)

   struct Rec
   {
      std::uint32_t                a = 0;
      std::optional<std::uint32_t> b;
      std::string                  c;
      std::optional<Leaf>          d;
      std::vector<std::uint32_t>   e;
      std::uint64_t                f = 0;
      friend bool                  operator==(const Rec&, const Rec&) = default;
   };
   PSIO_REFLECT(Rec, a, b, c, d, e, f)

   using Alt = std::variant<std::uint32_t, std::string, Leaf, std::vector<std::uint32_t>, bool,
                            std::int64_t>;
   struct Holder
   {
      Alt         v;
      friend bool operator==(const Holder&, const Holder&) = default;
   };
   PSIO_REFLECT(Holder, v)

   template <typename Fmt, typename T>
   std::optional<T> read(std::string_view text)
   {
      T    out{};
      auto st = psio::decode_json_checked(Fmt{}, std::span<const char>{text}, out);
      // validate runs the same reader without storing: it must agree.
      REQUIRE(psio::validate<T>(Fmt{}, std::span<const char>{text}).ok() == st.ok());
      if (!st.ok())
         return std::nullopt;
      return out;
   }

   template <typename Fmt>
   void record_orders()
   {
      const Rec want{1, 2u, "x", Leaf{3, "t"}, {4, 5}, 6};
      const std::vector<std::string> members{
         R"("a":1)", R"("b":2)", R"("c":"x")", R"("d":{"id":3,"tag":"t"})", R"("e":[4,5])",
         R"("f":6)"};
      std::vector<std::size_t> order(members.size());
      std::iota(order.begin(), order.end(), 0);
      std::size_t count = 0;
      do
      {
         std::string text = "{";
         for (std::size_t i = 0; i < order.size(); ++i)
            text += (i ? "," : "") + members[order[i]];
         text += "}";
         INFO(text);
         REQUIRE(read<Fmt, Rec>(text) == want);
         ++count;
      } while (std::next_permutation(order.begin(), order.end()));
      REQUIRE(count == 720);

      // Optional members left out, in and out of order.
      REQUIRE(read<Fmt, Rec>(R"({"a":1,"c":"x","e":[],"f":6})") == Rec{1, {}, "x", {}, {}, 6});
      REQUIRE(read<Fmt, Rec>(R"({"f":6,"e":[],"c":"x","a":1})") == Rec{1, {}, "x", {}, {}, 6});
      // A required member left out.
      REQUIRE(!read<Fmt, Rec>(R"({"a":1,"c":"x","e":[]})"));
      REQUIRE(!read<Fmt, Rec>(R"({"c":"x","e":[],"f":6})"));
      REQUIRE(!read<Fmt, Rec>("{}"));

      // Unknown keys anywhere, including ones that are a prefix or an
      // extension of a member name, the empty key and an escaped key.
      REQUIRE(read<Fmt, Rec>(
                 R"({"zz":[1,{"a":2}],"a":1,"aa":null,"c":"x","":0,"e":[],"ff":"f","f":6,"g":{}})") ==
              Rec{1, {}, "x", {}, {}, 6});
      // An escaped spelling of a member name is that member.
      REQUIRE(read<Fmt, Rec>(R"({"\u0061":1,"c":"x","e":[],"\u0066":6})") ==
              Rec{1, {}, "x", {}, {}, 6});

      // Repeated keys: single pass, the last one wins, every occurrence
      // must still be a value of the member's type.
      REQUIRE(read<Fmt, Rec>(R"({"a":1,"a":2,"c":"x","e":[1],"f":6,"e":[2,3],"c":"y","a":9})") ==
              Rec{9, {}, "y", {}, {2, 3}, 6});
      REQUIRE(read<Fmt, Rec>(R"({"a":1,"c":"x","e":[],"f":6,"b":7,"b":null})") ==
              Rec{1, {}, "x", {}, {}, 6});
      REQUIRE(!read<Fmt, Rec>(R"({"a":"bad","a":2,"c":"x","e":[],"f":6})"));
      REQUIRE(!read<Fmt, Rec>(R"({"a":1,"c":"x","e":[],"f":6,"a":"bad"})"));
      // Syntax errors around keys on both paths.
      REQUIRE(!read<Fmt, Rec>(R"({"a":1,"c":"x","e":[],"f":6,})"));
      REQUIRE(!read<Fmt, Rec>(R"({"a":1 "c":"x","e":[],"f":6})"));
      REQUIRE(!read<Fmt, Rec>(R"({"f":6,"e":[],"c":"x","a" 1})"));
      REQUIRE(!read<Fmt, Rec>(R"({"f":6,"e":[],"c":"x","a":1)"));
   }

   template <typename Fmt>
   void wide_orders()
   {
      using wide_fixture::Wide200;
      using R = psio::reflect<Wide200>;
      Wide200 want;
      std::uint32_t n = 0;
      R::for_each_field(want, [&](auto, std::string_view, std::uint32_t& f) { f = ++n * 3; });
      const std::string declared = psio::encode(Fmt{}, want);

      auto build = [&](const std::vector<std::size_t>& order) {
         std::string text = "{";
         for (std::size_t i = 0; i < order.size(); ++i)
         {
            text += i ? ",\"" : "\"";
            text += R::_names_array_[order[i]];
            text += "\":" + std::to_string((order[i] + 1) * 3);
         }
         return text + "}";
      };
      auto same = [&](const Wide200& got) { return psio::encode(Fmt{}, got) == declared; };

      std::vector<std::size_t> order(200);
      std::iota(order.begin(), order.end(), 0);
      REQUIRE(build(order) == declared);
      REQUIRE(same(*read<Fmt, Wide200>(build(order))));
      std::reverse(order.begin(), order.end());
      REQUIRE(same(*read<Fmt, Wide200>(build(order))));
      std::mt19937 rng(7);
      for (int round = 0; round < 20; ++round)
      {
         std::shuffle(order.begin(), order.end(), rng);
         REQUIRE(same(*read<Fmt, Wide200>(build(order))));
      }
      // One member missing is an error wherever it would have been.
      for (std::size_t drop : {std::size_t{0}, std::size_t{99}, std::size_t{199}})
      {
         std::vector<std::size_t> fewer;
         for (std::size_t i = 0; i < 200; ++i)
            if (i != drop)
               fewer.push_back(i);
         REQUIRE(!read<Fmt, Wide200>(build(fewer)));
      }
   }
}  // namespace

TEST_CASE("json: record members in every order", "[json][lookup]")
{
   record_orders<psio::json>();
}
TEST_CASE("ijson: record members in every order", "[ijson][lookup]")
{
   record_orders<psio::ijson>();
}
TEST_CASE("json: a 200-member record in declared, reversed and shuffled order", "[json][lookup]")
{
   wide_orders<psio::json>();
}
TEST_CASE("ijson: a 200-member record in declared, reversed and shuffled order",
          "[ijson][lookup]")
{
   wide_orders<psio::ijson>();
}

TEST_CASE("ijson: variant alternatives by name", "[ijson][lookup]")
{
   using F = psio::ijson;
   REQUIRE(read<F, Holder>(R"({"v":{"uint32":7}})") == Holder{Alt{std::uint32_t{7}}});
   REQUIRE(read<F, Holder>(R"({"v":{"string":"s"}})") == Holder{Alt{std::string{"s"}}});
   REQUIRE(read<F, Holder>(R"({"v":{"Leaf":{"tag":"t","id":1}}})") == Holder{Alt{Leaf{1, "t"}}});
   REQUIRE(read<F, Holder>(R"({"v":{"uint32[]":[1,2]}})") ==
           Holder{Alt{std::vector<std::uint32_t>{1, 2}}});
   REQUIRE(read<F, Holder>(R"({"v":{"bool":true}})") == Holder{Alt{true}});
   REQUIRE(read<F, Holder>(R"({"v":{"int64":-5}})") == Holder{Alt{std::int64_t{-5}}});
   // An escaped spelling names the same alternative.
   REQUIRE(read<F, Holder>(R"({"v":{"\u0062ool":false}})") == Holder{Alt{false}});
   // Unknown, near-miss and empty names; exactly one key.
   REQUIRE(!read<F, Holder>(R"({"v":{"uint33":7}})"));
   REQUIRE(!read<F, Holder>(R"({"v":{"uint3":7}})"));
   REQUIRE(!read<F, Holder>(R"({"v":{"uint322":7}})"));
   REQUIRE(!read<F, Holder>(R"({"v":{"":7}})"));
   REQUIRE(!read<F, Holder>(R"({"v":{}})"));
   REQUIRE(!read<F, Holder>(R"({"v":{"uint32":7,"uint32":8}})"));
   // Every alternative round-trips through its written name.
   for (const Alt& a : {Alt{std::uint32_t{1}}, Alt{std::string{"x"}}, Alt{Leaf{2, "y"}},
                        Alt{std::vector<std::uint32_t>{3}}, Alt{false}, Alt{std::int64_t{-1}}})
   {
      const Holder h{a};
      REQUIRE(read<F, Holder>(psio::encode(F{}, h)) == h);
      REQUIRE(read<psio::json, Holder>(psio::encode(psio::json{}, h)) == h);
   }
}
