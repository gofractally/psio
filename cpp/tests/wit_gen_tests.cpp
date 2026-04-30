// Tests for psio/wit_gen.hpp — runtime WIT generator driven by
// PSIO_INTERFACE / PSIO_REFLECT metadata.
//
// Acceptance gate: byte-parity against the v1 compile-time generator
// (psio1::constexpr_wit::interface_text<Tag>()) for the same shapes the
// WASI 2.3 host bindings use.  v1's runtime generate_wit_text walks the
// legacy reflect::member_functions path that v3 deliberately does not
// support — the constexpr generator is the v1 path that matches v3's
// interface_info-driven design.

#include <psio/wit_gen.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// ─── Sample shapes ───────────────────────────────────────────────────

namespace test_witgen
{
   struct datetime
   {
      std::uint64_t seconds     = 0;
      std::uint32_t nanoseconds = 0;
   };
   PSIO_REFLECT(datetime, seconds, nanoseconds)
}  // namespace test_witgen

using test_witgen::datetime;

// Interface anchor — global scope for PSIO_INTERFACE's `::NAME` ref.
struct witgen_wall_clock
{
   static datetime now();
   static datetime resolution();
};

// v3 package + interface registration.
PSIO_PACKAGE(witgen_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(witgen_clocks)

PSIO_INTERFACE(witgen_wall_clock,
               types(datetime),
               funcs(func(now), func(resolution)))

// ─── Tests ───────────────────────────────────────────────────────────

TEST_CASE("wit_gen: produces wit_world with one interface, one record, two funcs",
          "[wit_gen]")
{
   auto w = psio::generate_wit<witgen_wall_clock>(
      "wasi", "clocks", "0.2.3");

   CHECK(w.package == "wasi:clocks@0.2.3");
   CHECK(w.name == "witgen-wall-clock");

   REQUIRE(w.exports.size() == 1);
   const auto& iface = w.exports[0];
   CHECK(iface.name == "witgen-wall-clock");

   // datetime record was registered.
   REQUIRE(w.types.size() == 1);
   const auto& td = w.types[0];
   CHECK(td.name == "datetime");
   CHECK(td.kind == static_cast<std::uint8_t>(psio::wit_type_kind::record_));
   REQUIRE(td.fields.size() == 2);
   CHECK(td.fields[0].name == "seconds");
   CHECK(td.fields[0].type_idx == psio::wit_prim_idx(psio::wit_prim::u64));
   CHECK(td.fields[1].name == "nanoseconds");
   CHECK(td.fields[1].type_idx == psio::wit_prim_idx(psio::wit_prim::u32));

   // Two functions, both -> datetime, both 0-arg.
   REQUIRE(w.funcs.size() == 2);
   CHECK(w.funcs[0].name == "now");
   CHECK(w.funcs[0].params.empty());
   REQUIRE(w.funcs[0].results.size() == 1);
   CHECK(w.funcs[0].results[0].type_idx == 0);  // datetime
   CHECK(w.funcs[1].name == "resolution");
}

TEST_CASE("wit_gen: emits standards-compliant WIT text", "[wit_gen]")
{
   auto text = psio::generate_wit_text<witgen_wall_clock>(
      "wasi", "clocks", "0.2.3");

   const std::string expected =
      "package wasi:clocks@0.2.3;\n"
      "\n"
      "interface witgen-wall-clock {\n"
      "  record datetime {\n"
      "    seconds: u64,\n"
      "    nanoseconds: u32,\n"
      "  }\n"
      "\n"
      "  now: func() -> datetime;\n"
      "  resolution: func() -> datetime;\n"
      "}\n"
      "\n"
      "world witgen-wall-clock {\n"
      "  export witgen-wall-clock;\n"
      "}\n";

   CHECK(text == expected);
}

// ─── Parameter passing + kebab-case + multiple funcs ─────────────────

struct witgen_monotonic
{
   static std::uint64_t now();
   static std::uint64_t resolution();
   static std::uint64_t subscribe_instant(std::uint64_t when);
};

PSIO_INTERFACE(witgen_monotonic,
               types(),
               funcs(func(now),
                     func(resolution),
                     func(subscribe_instant, when)))

TEST_CASE("wit_gen: param names are captured + kebab-cased", "[wit_gen]")
{
   auto w = psio::generate_wit<witgen_monotonic>("wasi", "clocks", "0.2.3");

   REQUIRE(w.funcs.size() == 3);
   CHECK(w.funcs[2].name == "subscribe-instant");
   REQUIRE(w.funcs[2].params.size() == 1);
   CHECK(w.funcs[2].params[0].name == "when");
   CHECK(w.funcs[2].params[0].type_idx ==
         psio::wit_prim_idx(psio::wit_prim::u64));
}

TEST_CASE("wit_gen: snake_case identifiers convert to kebab in WIT text",
          "[wit_gen]")
{
   auto text = psio::generate_wit_text<witgen_monotonic>(
      "wasi", "clocks", "0.2.3");

   CHECK(text.find("subscribe-instant: func(when: u64)") != std::string::npos);
}

// ─── std::string / std::vector / std::optional pass-through ──────────

namespace test_witgen
{
   struct blob
   {
      std::string                     name;
      std::vector<std::uint8_t>       payload;
      std::optional<std::uint32_t>    version;
   };
   PSIO_REFLECT(blob, name, payload, version)
}  // namespace test_witgen
using test_witgen::blob;

struct witgen_blobs
{
   static blob fetch(std::string key);
};

PSIO_INTERFACE(witgen_blobs,
               types(blob),
               funcs(func(fetch, key)))

TEST_CASE("wit_gen: list/option/string types render correctly", "[wit_gen]")
{
   auto text = psio::generate_wit_text<witgen_blobs>(
      "test", "blobs", "1.0.0");

   CHECK(text.find("name: string,") != std::string::npos);
   CHECK(text.find("payload: list<u8>,") != std::string::npos);
   CHECK(text.find("version: option<u32>,") != std::string::npos);
   CHECK(text.find("fetch: func(key: string) -> blob;") != std::string::npos);
}
