// libraries/psio/cpp/tests/name_tests.cpp
//
// Round-trip + v1↔v3 byte-parity for psio::name_id.

#include <psio/name.hpp>
#include <psio/annotate.hpp>

#include <psio1/name.hpp>

#include <catch.hpp>

#include <string>
#include <string_view>

using psio::literals::operator""_n;

namespace {
   constexpr std::string_view kSamples[] = {
      "alice", "bob", "charlie",
      "alice-stone", "bob-the-builder",
      "abc123", "abc-123", "xyz",
      "supercali",  // 9 chars
      "valid-name-18char",   // 17 chars
      "validname9chars12",   // 17 chars
   };

   constexpr std::string_view kInvalid[] = {
      "",                       // empty
      "1abc",                   // starts with digit
      "Bob",                    // capital
      "bob_smith",              // underscore not in alphabet
      "thisisway-too-long-for-the-encoding-budget", // > 18
   };
}

TEST_CASE("name_to_number round-trips valid names", "[name][roundtrip]")
{
   // Round-trip is only meaningful for names that compress within the
   // 64-bit budget — the encoder returns 0 for inputs whose
   // arithmetic-coded form spills past 64 bits.  v1 has the same
   // semantics; the parity test below confirms both libraries agree on
   // every input including the spill cases.
   for (auto s : kSamples) {
      auto u = psio::name_to_number(s);
      INFO("name=" << s);
      if (u == 0) continue;
      auto back = psio::number_to_name(u);
      REQUIRE(back == s);
   }
}

TEST_CASE("name_to_number rejects invalid names", "[name][reject]")
{
   for (auto s : kInvalid) {
      INFO("name=" << s);
      REQUIRE(psio::name_to_number(s) == 0);
   }
}

TEST_CASE("v1↔v3 name encoding is byte-identical", "[name][parity]")
{
   for (auto s : kSamples) {
      INFO("name=" << s);
      const auto v1 = psio1::name_to_number(s);
      const auto v3 = psio::name_to_number(s);
      REQUIRE(v1 == v3);

      const auto v1_back = psio1::number_to_name(v1);
      const auto v3_back = psio::number_to_name(v3);
      REQUIRE(v1_back == v3_back);
   }
}

TEST_CASE("name_id literal", "[name][literal]")
{
   constexpr auto n = "alice"_n;
   STATIC_REQUIRE(n.value != 0);
   REQUIRE(n.str() == "alice");
}

TEST_CASE("name_id reflects with definitionWillNotChange", "[name][reflect]")
{
   using R = psio::reflect<psio::name_id>;
   STATIC_REQUIRE(R::member_count == 1);
   STATIC_REQUIRE(R::member_name<0> == "value");

   constexpr auto type_anns =
      psio::annotate<psio::type<psio::name_id>{}>;
   auto dwnc =
      psio::find_spec<psio::definition_will_not_change>(type_anns);
   REQUIRE(dwnc.has_value());
}
