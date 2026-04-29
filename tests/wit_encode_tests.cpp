// Tests for psio/wit_encode.hpp — Component Model binary encoder.
//
// Focus: structural smoke tests on the binary output (header bytes,
// custom section layout, presence of expected strings) and byte-parity
// with the v1 encoder fed an equivalent wit_world.  v1 is the reference
// — it has been validated against wasm-tools — so identical bytes from
// v3 means we round-trip the same way.

#include <psio/wit_encode.hpp>

#include <psio1/wit_encode.hpp>
#include <psio1/wit_gen.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ─── Sample shape (matches v1's wit_world layout) ────────────────────

namespace test_witenc
{
   struct datetime
   {
      std::uint64_t seconds     = 0;
      std::uint32_t nanoseconds = 0;
   };
   PSIO_REFLECT(datetime, seconds, nanoseconds)
}  // namespace test_witenc
using test_witenc::datetime;

struct witenc_wall_clock
{
   static datetime now();
   static datetime resolution();
};

PSIO_PACKAGE(witenc_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(witenc_clocks)

PSIO_INTERFACE(witenc_wall_clock,
               types(datetime),
               funcs(func(now), func(resolution)))

// ─── Tests ───────────────────────────────────────────────────────────

TEST_CASE("wit_encode: produces a Component header", "[wit_encode]")
{
   auto bytes = psio::generate_wit_binary<witenc_wall_clock>(
      "wasi", "clocks", "0.2.3", "imports");

   REQUIRE(bytes.size() >= 8);
   // \0asm\r\0\1\0
   CHECK(bytes[0] == 0x00);
   CHECK(bytes[1] == 0x61);
   CHECK(bytes[2] == 0x73);
   CHECK(bytes[3] == 0x6d);
   CHECK(bytes[4] == 0x0d);
   CHECK(bytes[5] == 0x00);
   CHECK(bytes[6] == 0x01);
   CHECK(bytes[7] == 0x00);
}

namespace
{
   bool contains(const std::vector<std::uint8_t>& bytes, std::string_view needle)
   {
      if (needle.empty() || bytes.size() < needle.size())
         return false;
      for (std::size_t i = 0; i + needle.size() <= bytes.size(); ++i)
      {
         bool ok = true;
         for (std::size_t j = 0; j < needle.size(); ++j)
            if (bytes[i + j] != static_cast<std::uint8_t>(needle[j]))
            {
               ok = false;
               break;
            }
         if (ok)
            return true;
      }
      return false;
   }
}  // namespace

TEST_CASE("wit_encode: carries the standard custom-section signatures",
          "[wit_encode]")
{
   auto bytes = psio::generate_wit_binary<witenc_wall_clock>(
      "wasi", "clocks", "0.2.3", "imports");

   CHECK(contains(bytes, "wit-component-encoding"));
   CHECK(contains(bytes, "producers"));
   CHECK(contains(bytes, "processed-by"));
   CHECK(contains(bytes, "psio-wit-gen"));

   // Field names + interface name appear in the type section payload.
   CHECK(contains(bytes, "seconds"));
   CHECK(contains(bytes, "nanoseconds"));
   CHECK(contains(bytes, "datetime"));
   CHECK(contains(bytes, "now"));
   CHECK(contains(bytes, "resolution"));
   CHECK(contains(bytes, "wasi:clocks/witenc-wall-clock@0.2.3"));
   CHECK(contains(bytes, "imports"));
}

// ─── Byte-parity vs v1 encoder ───────────────────────────────────────
//
// v1's encoder consumes a wit_world identically; build the same world
// by hand on both sides and compare encoded bytes.  This proves the
// v3 encoder hasn't drifted — without dragging v1's runtime
// generate_wit through reflect::member_functions (which v3 lacks).

namespace
{
   psio::wit_world make_v3_world()
   {
      psio::wit_world w;
      w.package = "wasi:clocks@0.2.3";
      w.name    = "imports";

      // datetime record.
      psio::wit_type_def dt;
      dt.name = "datetime";
      dt.kind = static_cast<std::uint8_t>(psio::wit_type_kind::record_);
      dt.fields.push_back({"seconds",
                           psio::wit_prim_idx(psio::wit_prim::u64), {}});
      dt.fields.push_back({"nanoseconds",
                           psio::wit_prim_idx(psio::wit_prim::u32), {}});
      w.types.push_back(std::move(dt));

      // Two funcs, both () -> datetime (type idx 0).
      psio::wit_func fnow;
      fnow.name = "now";
      fnow.results.push_back({"", 0, {}});
      psio::wit_func fres;
      fres.name = "resolution";
      fres.results.push_back({"", 0, {}});
      w.funcs.push_back(std::move(fnow));
      w.funcs.push_back(std::move(fres));

      // One export interface owning both types and funcs.
      psio::wit_interface iface;
      iface.name      = "witenc-wall-clock";
      iface.type_idxs = {0};
      iface.func_idxs = {0, 1};
      w.exports.push_back(std::move(iface));
      return w;
   }

   psio1::wit_world make_v1_world()
   {
      psio1::wit_world w;
      w.package = "wasi:clocks@0.2.3";
      w.name    = "imports";

      psio1::wit_type_def dt;
      dt.name = "datetime";
      dt.kind = static_cast<std::uint8_t>(psio1::wit_type_kind::record_);
      dt.fields.push_back({"seconds",
                           psio1::wit_prim_idx(psio1::wit_prim::u64), {}});
      dt.fields.push_back({"nanoseconds",
                           psio1::wit_prim_idx(psio1::wit_prim::u32), {}});
      w.types.push_back(std::move(dt));

      psio1::wit_func fnow;
      fnow.name = "now";
      fnow.results.push_back({"", 0, {}});
      psio1::wit_func fres;
      fres.name = "resolution";
      fres.results.push_back({"", 0, {}});
      w.funcs.push_back(std::move(fnow));
      w.funcs.push_back(std::move(fres));

      psio1::wit_interface iface;
      iface.name      = "witenc-wall-clock";
      iface.type_idxs = {0};
      iface.func_idxs = {0, 1};
      w.exports.push_back(std::move(iface));
      return w;
   }
}  // namespace

TEST_CASE("wit_encode: byte-identical to v1 encoder for the same world",
          "[wit_encode]")
{
   auto v3 = psio::encode_wit_binary(make_v3_world());
   auto v1 = psio1::encode_wit_binary(make_v1_world());

   CHECK(v3.size() == v1.size());
   CHECK(v3 == v1);
}
