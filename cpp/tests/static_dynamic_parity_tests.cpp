// Static ↔ dynamic byte-identity harness (§8 criterion #6).
//
// For every (format, fixture) pair: the static path
// `encode(fmt, T_value)` must produce byte-identical output to the
// dynamic path `encode_dynamic(fmt, reflect<T>::schema(), dyn_value)`.
// The dynamic-encode-equals-static-encode invariant is what lets
// gateway code drive the same wire without a compile-time type.

#include <psio/bin.hpp>
#include <psio/dynamic_bin.hpp>
#include <psio/dynamic_frac.hpp>
#include <psio/dynamic_json.hpp>
#include <psio/dynamic_pssz.hpp>
#include <psio/dynamic_ssz.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/schema.hpp>
#include <psio/ssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sdp {

   struct Point
   {
      std::int32_t x;
      std::int32_t y;
   };
   PSIO_REFLECT(Point, x, y)

   struct Blob
   {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO_REFLECT(Blob, version, payload, note)

}  // namespace sdp

// psio::schema_of<T>() is the static → dynamic bridge.

// ── Binary format cases ────────────────────────────────────────────────

#define SDP_BINARY_CASE(FmtName, FmtValue, FixtureName, FixtureValue)   \
   TEST_CASE("static↔dynamic byte-identity [" #FmtName "]: "            \
             #FixtureName,                                              \
             "[static-dynamic][" #FmtName "]")                          \
   {                                                                     \
      auto v = FixtureValue;                                             \
      auto bytes_static  = psio::encode(FmtValue, v);                   \
      auto sc            = psio::schema_of<decltype(v)>();              \
      auto dv            = psio::to_dynamic(v);                         \
      auto bytes_dynamic = psio::encode_dynamic(FmtValue, sc, dv);      \
      REQUIRE(bytes_static == bytes_dynamic);                            \
   }

#define SDP_ALL_FMTS(FixtureName, FixtureValue)                         \
   SDP_BINARY_CASE(ssz,    ::psio::ssz{},    FixtureName, FixtureValue)\
   SDP_BINARY_CASE(pssz32, ::psio::pssz32{}, FixtureName, FixtureValue)\
   SDP_BINARY_CASE(frac32, ::psio::frac32{}, FixtureName, FixtureValue)\
   SDP_BINARY_CASE(bin,    ::psio::bin{},    FixtureName, FixtureValue)\
   SDP_BINARY_CASE(json,   ::psio::json{},   FixtureName, FixtureValue)

SDP_ALL_FMTS(Point_fixed, (sdp::Point{3, -7}))
SDP_ALL_FMTS(Blob_variable, (sdp::Blob{42, {1, 2, 3}, "hi"}))
SDP_ALL_FMTS(Blob_empty, (sdp::Blob{0, {}, ""}))

#undef SDP_ALL_FMTS
#undef SDP_BINARY_CASE
