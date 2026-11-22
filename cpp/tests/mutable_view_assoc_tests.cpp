// psio::mutable_view over pairs, maps and nested values: the cases shared
// with the exception-disabled build (mutable_view_assoc_cases.hpp), and the
// exact-validation check Catch can run. The rejected operations are covered
// by the psio_mutable_view_assoc_*_compile_fail tests
// (mutable_view_assoc_fail.cpp).
#include "mutable_view_assoc_cases.hpp"

#include <catch.hpp>

using namespace mv_assoc_cases;

TEMPLATE_TEST_CASE("mutable_view overwrites map values and nested inner values in place",
                   "[mutable_view][assoc][nested]", psio::pssz8, psio::pssz16, psio::pssz32,
                   psio::pssz)
{
   CHECK(run<TestType>() == 0);
}

TEST_CASE("mutable_view of a map: every write leaves a canonical frame",
          "[mutable_view][assoc]")
{
   using Map = std::map<std::uint16_t, Balance>;
   Map map;
   for (std::uint16_t k = 0; k < 20; ++k)
      map[static_cast<std::uint16_t>(k * 257)] = Balance{k, false};
   auto                              bytes = psio::encode(psio::pssz{}, map);
   const psio::mutable_view<Map, psio::pssz> view{bytes};
   REQUIRE(view.size() == map.size());
   std::size_t i = 0;
   for (auto entry : view)
   {
      // Wire order is key order: here the order of the std::map.
      const auto key = entry.key();
      REQUIRE(map.contains(key));
      REQUIRE(entry.value().set(Balance{static_cast<std::uint32_t>(1000 + i), i % 2 == 0}));
      map[key] = Balance{static_cast<std::uint32_t>(1000 + i), i % 2 == 0};
      ++i;
      REQUIRE(psio::validate<Map>(psio::pssz{}, std::span<const char>{bytes}).ok());
      REQUIRE(psio::validate_strict<Map>(psio::pssz{}, std::span<const char>{bytes}).ok());
      REQUIRE(bytes == psio::encode(psio::pssz{}, map));
   }
   REQUIRE(psio::decode<Map>(psio::pssz{}, std::span<const char>{bytes}) == map);
   static_assert(!psio::mutable_view<Map, psio::pssz>::assignable);
   static_assert(!psio::mutable_view<psio::nested<Balance>, psio::pssz>::assignable);
}

TEST_CASE("mutable_view of a nested value: the bytes stay the inner encoding",
          "[mutable_view][nested]")
{
   using Holder = std::tuple<std::uint8_t, psio::nested<Args>, std::string>;
   Holder holder{1, psio::nested<Args>::from(Args{"carol", 1, std::nullopt}), "tail"};
   auto   bytes = psio::encode(psio::pssz{}, holder);
   const psio::mutable_view<Holder, psio::pssz> view{bytes};
   REQUIRE(view.get<1>().get().get<1>().set(0xffffffffffffffffull));
   // A pruned optional inside the nested bytes has no payload to write.
   REQUIRE(!(*view.get<1>().get().get<2>()).set(1));
   std::get<1>(holder) = psio::nested<Args>::from(Args{"carol", 0xffffffffffffffffull, {}});
   REQUIRE(bytes == psio::encode(psio::pssz{}, holder));
   REQUIRE(psio::validate_strict<Holder>(psio::pssz{}, std::span<const char>{bytes}).ok());
   // The key of the nested value follows its bytes.
   const auto decoded = psio::decode<Holder>(psio::pssz{}, std::span<const char>{bytes});
   REQUIRE(std::get<1>(decoded) == std::get<1>(holder));
   REQUIRE(std::get<1>(decoded).validate_strict().ok());
}
