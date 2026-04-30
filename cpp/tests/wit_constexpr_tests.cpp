// Tests for psio/wit_constexpr.hpp — consteval WIT text generator.
//
// Cross-checks: the consteval interface block must be byte-identical
// to what wit_gen produces inside its interface block for the same
// Tag.  That parity is the whole point of having two generators.

#include <psio/wit_constexpr.hpp>
#include <psio/wit_gen.hpp>

#include <catch.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// ─── Sample shapes ───────────────────────────────────────────────────

namespace test_witcx
{
   struct datetime
   {
      std::uint64_t seconds     = 0;
      std::uint32_t nanoseconds = 0;
   };
   PSIO_REFLECT(datetime, seconds, nanoseconds)
}  // namespace test_witcx

using test_witcx::datetime;

struct witcx_wall_clock
{
   static datetime now();
   static datetime resolution();
};

PSIO_PACKAGE(witcx_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(witcx_clocks)

PSIO_INTERFACE(witcx_wall_clock,
               types(datetime),
               funcs(func(now), func(resolution)))

// ─── Tests ───────────────────────────────────────────────────────────

TEST_CASE("constexpr_wit: interface_text produces expected literal text",
          "[wit_constexpr]")
{
   constexpr auto pair = psio::constexpr_wit::interface_text<witcx_wall_clock>();
   std::string_view actual{pair.first.data(), pair.second};

   const std::string expected =
      "interface witcx-wall-clock {\n"
      "  record datetime {\n"
      "    seconds: u64,\n"
      "    nanoseconds: u32,\n"
      "  }\n"
      "\n"
      "  now: func() -> datetime;\n"
      "  resolution: func() -> datetime;\n"
      "}\n";

   CHECK(actual == expected);
}

TEST_CASE("constexpr_wit: interface_text matches wit_gen interface block",
          "[wit_constexpr]")
{
   // wit_gen wraps the interface block inside a package + world. Strip
   // those out and compare the inner block.
   auto runtime = psio::generate_wit_text<witcx_wall_clock>(
      "wasi", "clocks", "0.2.3");

   auto begin = runtime.find("interface ");
   auto end   = runtime.find("\n\nworld ");
   REQUIRE(begin != std::string::npos);
   REQUIRE(end != std::string::npos);
   auto runtime_iface = runtime.substr(begin, end - begin) + "\n";

   constexpr auto    pair = psio::constexpr_wit::interface_text<witcx_wall_clock>();
   std::string_view  consteval_iface{pair.first.data(), pair.second};

   CHECK(std::string{consteval_iface} == runtime_iface);
}

TEST_CASE("constexpr_wit: blob carries magic + length + text",
          "[wit_constexpr]")
{
   constexpr auto sz   = psio::constexpr_wit::wit_size<witcx_wall_clock>();
   constexpr auto blob = psio::constexpr_wit::wit_array<witcx_wall_clock>();
   STATIC_REQUIRE(blob.size() == sz);

   // Magic prefix.
   REQUIRE(sz >= psio::constexpr_wit::MAGIC_LEN + 4);
   CHECK(std::memcmp(blob.data(), psio::constexpr_wit::MAGIC,
                     psio::constexpr_wit::MAGIC_LEN) == 0);

   // u32le length immediately after magic.
   const auto*       p   = reinterpret_cast<const std::uint8_t*>(blob.data()) +
                  psio::constexpr_wit::MAGIC_LEN;
   const std::uint32_t len =
      static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
      (static_cast<std::uint32_t>(p[2]) << 16) |
      (static_cast<std::uint32_t>(p[3]) << 24);

   constexpr auto text = psio::constexpr_wit::interface_text<witcx_wall_clock>();
   CHECK(len == text.second);
   CHECK(sz == psio::constexpr_wit::MAGIC_LEN + 4 + len);

   // Text payload after the 4-byte length.
   std::string_view payload{
      reinterpret_cast<const char*>(p + 4), len};
   std::string_view expected{text.first.data(), text.second};
   CHECK(payload == expected);
}

// ─── Param + return type lowering ────────────────────────────────────

struct witcx_misc
{
   static std::uint64_t identity(std::uint64_t x);
   static std::string   greet(std::string name);
};

PSIO_INTERFACE(witcx_misc,
               types(),
               funcs(func(identity, x), func(greet, name)))

TEST_CASE("constexpr_wit: params and returns render correctly",
          "[wit_constexpr]")
{
   constexpr auto    pair = psio::constexpr_wit::interface_text<witcx_misc>();
   std::string_view  text{pair.first.data(), pair.second};

   CHECK(text.find("identity: func(x: u64) -> u64;") != std::string_view::npos);
   CHECK(text.find("greet: func(name: string) -> string;") !=
         std::string_view::npos);
}
