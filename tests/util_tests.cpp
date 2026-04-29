// libraries/psio/cpp/tests/util_tests.cpp
//
// Coverage for the small utility headers ported from psio v1:
// bytes_view, to_hex, untagged.

#include <psio/bytes_view.hpp>
#include <psio/to_hex.hpp>
#include <psio/untagged.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("bytes_view aliases compile", "[util][bytes_view]")
{
   std::vector<std::uint8_t> v{1, 2, 3};
   psio::bytes_view         b{v};
   psio::mutable_bytes_view mb{v};
   REQUIRE(b.size() == 3);
   REQUIRE(mb.size() == 3);
   STATIC_REQUIRE(std::is_same_v<psio::bytes_view,
                                  std::span<const std::uint8_t>>);
   STATIC_REQUIRE(std::is_same_v<psio::mutable_bytes_view,
                                  std::span<std::uint8_t>>);
}

TEST_CASE("to_hex / from_hex round-trip", "[util][hex]")
{
   const char           bytes[] = {'\x00', '\x01', '\x7F', '\xFF'};
   const std::string    h       = psio::to_hex(std::span<const char>{bytes, 4});
   REQUIRE(h == "00017FFF");

   std::vector<char> back;
   REQUIRE(psio::from_hex(h, back));
   REQUIRE(back.size() == 4);
   REQUIRE(back[0] == '\x00');
   REQUIRE(back[1] == '\x01');
   REQUIRE(static_cast<unsigned char>(back[2]) == 0x7F);
   REQUIRE(static_cast<unsigned char>(back[3]) == 0xFF);
}

TEST_CASE("from_hex rejects malformed input", "[util][hex]")
{
   std::vector<char> back;
   REQUIRE_FALSE(psio::from_hex("0G", back));  // G not a hex digit
}

namespace {
   struct MyVariantTag { std::variant<int, std::string> v; };

   // ADL-discoverable opt-in.
   constexpr bool psio_is_untagged(const MyVariantTag*) { return true; }
}

TEST_CASE("psio_is_untagged ADL hook", "[util][untagged]")
{
   // Local override picks up via ADL (or unqualified lookup since it
   // sits in the anonymous namespace).
   STATIC_REQUIRE(psio_is_untagged(static_cast<const MyVariantTag*>(nullptr))
                  == true);
   // Default for any other type — falls through to the psio:: primary.
   STATIC_REQUIRE(psio::psio_is_untagged(
                     static_cast<const std::vector<int>*>(nullptr))
                  == false);
}
