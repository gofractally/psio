// v1 ↔ psio pSSZ byte-parity tests.
//
// Analog of v1_parity_ssz_tests.cpp for the pSSZ family. Verifies that
// encoding the same value through psio1::to_pssz (v1) and
// psio::encode(psio::pssz_<W>{}, …) produces byte-identical output
// for each width.

#include <psio1/ext_int.hpp>     // v1 uint128/uint256
#include <psio1/from_pssz.hpp>   // v1
#include <psio1/to_pssz.hpp>     // v1
#include <psio1/reflect.hpp>
#include <psio/ext_int.hpp>    // v3 uint128/uint256
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

   template <typename Fmt, typename T>
   std::vector<char> v1_pssz(const T& v)
   {
      return psio1::convert_to_pssz<Fmt>(v);
   }

   template <typename V3Fmt, typename T>
   std::vector<char> v3_pssz(const T& v)
   {
      return psio::encode(V3Fmt{}, v);
   }

}  // namespace

// Paired (v1_format, v3_format) for each width we want to cover.
using pssz32_pair = std::pair<psio1::frac_format_pssz32, psio::pssz32>;
using pssz16_pair = std::pair<psio1::frac_format_pssz16, psio::pssz16>;
using pssz8_pair  = std::pair<psio1::frac_format_pssz8,  psio::pssz8>;

// Templating parity assertions by the pair type keeps each test across
// widths small.

TEMPLATE_TEST_CASE("pssz parity: u32", "[pssz][parity]",
                   pssz32_pair, pssz16_pair, pssz8_pair)
{
   using V1 = typename TestType::first_type;
   using V3 = typename TestType::second_type;
   std::uint32_t v = 0xDEADBEEF;
   REQUIRE(v1_pssz<V1>(v) == v3_pssz<V3>(v));
}

TEMPLATE_TEST_CASE("pssz parity: bool", "[pssz][parity]",
                   pssz32_pair, pssz16_pair, pssz8_pair)
{
   using V1 = typename TestType::first_type;
   using V3 = typename TestType::second_type;
   REQUIRE(v1_pssz<V1>(true)  == v3_pssz<V3>(true));
   REQUIRE(v1_pssz<V1>(false) == v3_pssz<V3>(false));
}

TEMPLATE_TEST_CASE("pssz parity: std::vector of u32",
                   "[pssz][parity][vector]",
                   pssz32_pair, pssz16_pair, pssz8_pair)
{
   using V1 = typename TestType::first_type;
   using V3 = typename TestType::second_type;
   std::vector<std::uint32_t> v{1, 2, 3, 4};
   REQUIRE(v1_pssz<V1>(v) == v3_pssz<V3>(v));
}

TEMPLATE_TEST_CASE("pssz parity: std::string", "[pssz][parity][string]",
                   pssz32_pair, pssz16_pair, pssz8_pair)
{
   using V1 = typename TestType::first_type;
   using V3 = typename TestType::second_type;
   std::string s = "hi there";
   REQUIRE(v1_pssz<V1>(s) == v3_pssz<V3>(s));
}

TEMPLATE_TEST_CASE("pssz parity: std::optional<u32> (fixed, no selector)",
                   "[pssz][parity][optional]",
                   pssz32_pair, pssz16_pair, pssz8_pair)
{
   using V1 = typename TestType::first_type;
   using V3 = typename TestType::second_type;
   std::optional<std::uint32_t> none;
   std::optional<std::uint32_t> some = 42;
   REQUIRE(v1_pssz<V1>(none) == v3_pssz<V3>(none));
   REQUIRE(v1_pssz<V1>(some) == v3_pssz<V3>(some));
}

TEMPLATE_TEST_CASE("pssz parity: std::optional<string> (variable, selector)",
                   "[pssz][parity][optional]",
                   pssz32_pair, pssz16_pair, pssz8_pair)
{
   using V1 = typename TestType::first_type;
   using V3 = typename TestType::second_type;
   std::optional<std::string> none;
   std::optional<std::string> some = std::string("abc");
   REQUIRE(v1_pssz<V1>(none) == v3_pssz<V3>(none));
   REQUIRE(v1_pssz<V1>(some) == v3_pssz<V3>(some));
}

// ── uint128 / uint256 parity ──────────────────────────────────────────

TEST_CASE("pssz parity: uint256 via pssz32",
          "[pssz][parity][extint]")
{
   psio1::uint256  v1v;
   psio::uint256 v3v;
   for (int i = 0; i < 4; ++i)
   {
      v1v.limb[i] = 0xA000 + i;
      v3v.limb[i] = 0xA000 + i;
   }
   REQUIRE(psio1::convert_to_pssz<psio1::frac_format_pssz32>(v1v) ==
           psio::encode(psio::pssz32{}, v3v));
}

TEST_CASE("pssz parity: uint128 via pssz32",
          "[pssz][parity][extint]")
{
   psio1::uint128  v1v = (static_cast<psio1::uint128>(0x1234567890ABCDEFULL)
                         << 64) |
                        0xFEDCBA9876543210ULL;
   psio::uint128 v3v = (static_cast<psio::uint128>(0x1234567890ABCDEFULL)
                         << 64) |
                        0xFEDCBA9876543210ULL;
   REQUIRE(psio1::convert_to_pssz<psio1::frac_format_pssz32>(v1v) ==
           psio::encode(psio::pssz32{}, v3v));
}
