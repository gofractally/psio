// Cross-format integration test for type-level caps
// (`maxFields(N)` / `maxDynamicData(N)`).
//
// Goal: every format that consumes the cap rejects an oversized buffer
// at validate time and / or throws on encode excess.  This test asserts
// the contract is uniform across the format catalog so a future format
// can plug in by following the same pattern.

#include <psio/annotate.hpp>
#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/bson.hpp>
#include <psio/capnp.hpp>
#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/flatbuf.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/msgpack.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/wit.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Capped record used for every format's validate-reject check.  At
// file scope (not in an anonymous namespace) because PSIO_REFLECT cats
// the type name into a helper struct identifier that must be visible
// across translation units.
struct CapRec
{
   std::uint64_t              id   = 0;
   std::vector<std::uint8_t>  blob;
};
PSIO_REFLECT(CapRec, id, blob,
             definitionWillNotChange(),
             maxDynamicData(64))

namespace
{
   // Helper: assert that validate<T>(fmt, oversized) returns an error.
   template <typename Fmt>
   void expect_validate_rejects_oversized(Fmt fmt,
                                          std::string_view fmt_name)
   {
      // 128 bytes > 64-byte cap regardless of structural validity.
      std::vector<char> oversized(128, 0);
      auto st = psio::validate<CapRec>(
         fmt, std::span<const char>{oversized});
      INFO("format = " << fmt_name);
      REQUIRE(!st.ok());
   }
}

TEST_CASE("caps: every format's validate rejects oversized input",
          "[caps][parity][validate]")
{
   expect_validate_rejects_oversized(psio::pssz{},     "pssz");
   expect_validate_rejects_oversized(psio::frac32{},   "frac32");
   expect_validate_rejects_oversized(psio::bin{},      "bin");
   expect_validate_rejects_oversized(psio::borsh{},    "borsh");
   expect_validate_rejects_oversized(psio::bincode{},  "bincode");
   expect_validate_rejects_oversized(psio::ssz{},      "ssz");
   expect_validate_rejects_oversized(psio::avro{},     "avro");
   expect_validate_rejects_oversized(psio::msgpack{},  "msgpack");
   expect_validate_rejects_oversized(psio::protobuf{}, "protobuf");
   expect_validate_rejects_oversized(psio::wit{},      "wit");
   expect_validate_rejects_oversized(psio::json{},     "json");
   expect_validate_rejects_oversized(psio::bson{},     "bson");
   expect_validate_rejects_oversized(psio::capnp{},    "capnp");
   expect_validate_rejects_oversized(psio::flatbuf{},  "flatbuf");
}

TEST_CASE("caps: pssz encode throws when total exceeds cap",
          "[caps][parity][encode]")
{
   // Build a value whose encoded size (vec<u8>(256) + u64) clearly
   // exceeds the 64-byte cap, regardless of per-format header overhead.
   CapRec big{42, std::vector<std::uint8_t>(256, 0xAB)};

   // pssz has a verified encode-throw hook; frac32 doesn't support
   // vector<u8> records in its current encode_value matrix, so we skip
   // it here.  When frac's record-encoding matrix expands, add it.
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{}, big),
                     psio::codec_exception);
}
