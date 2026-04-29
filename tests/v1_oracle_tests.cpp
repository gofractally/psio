// v1 ↔ psio byte-oracle tests.
//
// Pattern ported from libraries/psio2/cpp/tests/v1_oracle_tests.cpp:
// define parallel v1 (psio1::) and v3 (psio::) types with matching
// shapes, then assert byte-identical encodings across every format.
// Exposes wire-format divergences that per-format unit tests miss —
// in particular, this file covers reflected RECORDS, which the
// existing v1_parity_<fmt>_tests.cpp files historically did not.
//
// Record types:
//   - OraclePoint       — all fields fixed; DWNC.
//   - OracleRecord      — mixed fixed + variable (std::string,
//                         std::vector<u16>, std::optional<u32>);
//                         non-DWNC. The interesting case for pssz's
//                         fracpack-inherited u16 `fixed_size` header.
//   - OracleFlatRecord  — variable fields but no std::optional;
//                         non-DWNC.
//
// Formats exercised: ssz, pssz (pssz8/16/32 + auto), frac32/frac16,
// bin, borsh, bincode, key, avro, json. Each oracle helper asserts
// byte-equality and round-trips through both v1 and v3.

// clang-format off
// Order-sensitive: v1's from_key.hpp depends on key_detail helpers that
// are declared in to_key.hpp. Keep to_key.hpp before from_key.hpp.
#include <psio1/fracpack.hpp>
#include <psio1/from_avro.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/from_borsh.hpp>
#include <psio1/to_key.hpp>
#include <psio1/from_key.hpp>
#include <psio1/from_pssz.hpp>
#include <psio1/from_ssz.hpp>
#include <psio1/to_avro.hpp>
#include <psio1/to_bin.hpp>
#include <psio1/to_bincode.hpp>
#include <psio1/to_borsh.hpp>
#include <psio1/to_pssz.hpp>
#include <psio1/to_ssz.hpp>
// clang-format on

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/frac.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ── Shape A: DWNC all-fixed ───────────────────────────────────────────
struct V1Point
{
   std::int32_t x = 0;
   std::int32_t y = 0;
   friend bool operator==(const V1Point&, const V1Point&) = default;
};
PSIO1_REFLECT(V1Point, definitionWillNotChange(), x, y)

struct V3Point
{
   std::int32_t x = 0;
   std::int32_t y = 0;
   friend bool operator==(const V3Point&, const V3Point&) = default;
};
PSIO_REFLECT(V3Point, x, y)
PSIO_TYPE_ATTRS(V3Point, psio::definition_will_not_change{})

// ── Shape B: non-DWNC with variable fields (the pssz header exerciser) ─
struct V1Record
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const V1Record&, const V1Record&) = default;
};
PSIO1_REFLECT(V1Record, id, label, values, score)

struct V3Record
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const V3Record&, const V3Record&) = default;
};
PSIO_REFLECT(V3Record, id, label, values, score)

// ── Shape C: non-DWNC variable, no optional ───────────────────────────
struct V1FlatRecord
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const V1FlatRecord&, const V1FlatRecord&) = default;
};
PSIO1_REFLECT(V1FlatRecord, id, label, values)

struct V3FlatRecord
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const V3FlatRecord&, const V3FlatRecord&) = default;
};
PSIO_REFLECT(V3FlatRecord, id, label, values)

// ── Helpers ────────────────────────────────────────────────────────────
namespace
{
   template <typename Bytes>
   std::vector<char> as_chars(const Bytes& bytes)
   {
      return {bytes.begin(), bytes.end()};
   }

   std::span<const char> cview(const std::vector<char>& bytes)
   {
      return {bytes.data(), bytes.size()};
   }

   void require_same_bytes(std::string_view         label,
                           const std::vector<char>& v1,
                           const std::vector<char>& v3)
   {
      INFO(label);
      REQUIRE(v1 == v3);
   }

   V1Point      v1_point() { return {.x = -42, .y = 77}; }
   V3Point      v3_point() { return {.x = -42, .y = 77}; }
   V1Record     v1_record()
   {
      return {.id = 7, .label = "oracle", .values = {1, 2, 65535}, .score = 99};
   }
   V3Record     v3_record()
   {
      return {.id = 7, .label = "oracle", .values = {1, 2, 65535}, .score = 99};
   }
   V1FlatRecord v1_flat_record()
   {
      return {.id = 9, .label = "flat-cap", .values = {3, 5, 8, 13}};
   }
   V3FlatRecord v3_flat_record()
   {
      return {.id = 9, .label = "flat-cap", .values = {3, 5, 8, 13}};
   }

   // ── Oracle helpers ────────────────────────────────────────────────
   template <typename V1, typename V3>
   void require_ssz_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::convert_to_ssz(v1));
      auto b3 = as_chars(psio::encode(psio::ssz{}, v3));
      require_same_bytes("ssz", b1, b3);
      REQUIRE(psio1::convert_from_ssz<V1>(b1) == v1);
      REQUIRE(psio::decode<V3>(psio::ssz{}, cview(b3)) == v3);
   }

   template <typename V1, typename V3>
   void require_pssz_oracle(const V1& v1, const V3& v3)
   {
      auto b1_16 = as_chars(
         psio1::convert_to_pssz<psio1::frac_format_pssz16>(v1));
      auto b3_16 = as_chars(psio::encode(psio::pssz16{}, v3));
      require_same_bytes("pssz16", b1_16, b3_16);

      auto b1_32 = as_chars(
         psio1::convert_to_pssz<psio1::frac_format_pssz32>(v1));
      auto b3_32 = as_chars(psio::encode(psio::pssz32{}, v3));
      require_same_bytes("pssz32", b1_32, b3_32);

      // Auto-width: v1 uses auto_pssz_format_t<V1>, v3 uses the
      // `pssz` tag (its auto-W selection).
      auto b1_auto = as_chars(
         psio1::convert_to_pssz<psio1::auto_pssz_format_t<V1>>(v1));
      auto b3_auto = as_chars(psio::encode(psio::pssz{}, v3));
      require_same_bytes("pssz auto", b1_auto, b3_auto);
   }

   template <typename V1, typename V3>
   void require_frac_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::to_frac(v1));
      auto b3 = as_chars(psio::encode(psio::frac32{}, v3));
      require_same_bytes("frac", b1, b3);

      auto b1_16 = as_chars(psio1::to_frac16(v1));
      auto b3_16 = as_chars(psio::encode(psio::frac16{}, v3));
      require_same_bytes("frac16", b1_16, b3_16);
   }

   template <typename V1, typename V3>
   void require_bin_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::convert_to_bin(v1));
      auto b3 = as_chars(psio::encode(psio::bin{}, v3));
      require_same_bytes("bin", b1, b3);
      REQUIRE(psio1::convert_from_bin<V1>(b1) == v1);
      REQUIRE(psio::decode<V3>(psio::bin{}, cview(b3)) == v3);
   }

   template <typename V1, typename V3>
   void require_borsh_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::convert_to_borsh(v1));
      auto b3 = as_chars(psio::encode(psio::borsh{}, v3));
      require_same_bytes("borsh", b1, b3);
      REQUIRE(psio1::convert_from_borsh<V1>(b1) == v1);
      REQUIRE(psio::decode<V3>(psio::borsh{}, cview(b3)) == v3);
   }

   template <typename V1, typename V3>
   void require_bincode_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::convert_to_bincode(v1));
      auto b3 = as_chars(psio::encode(psio::bincode{}, v3));
      require_same_bytes("bincode", b1, b3);
      REQUIRE(psio1::convert_from_bincode<V1>(b1) == v1);
      REQUIRE(psio::decode<V3>(psio::bincode{}, cview(b3)) == v3);
   }

   template <typename V1, typename V3>
   void require_key_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::convert_to_key(v1));
      auto b3 = as_chars(psio::encode(psio::key{}, v3));
      require_same_bytes("key", b1, b3);
   }

   template <typename V1, typename V3>
   void require_avro_oracle(const V1& v1, const V3& v3)
   {
      auto b1 = as_chars(psio1::convert_to_avro(v1));
      auto b3 = as_chars(psio::encode(psio::avro{}, v3));
      require_same_bytes("avro", b1, b3);
      REQUIRE(psio1::convert_from_avro<V1>(b1) == v1);
      REQUIRE(psio::decode<V3>(psio::avro{}, cview(b3)) == v3);
   }
}  // namespace

// ── Test cases ─────────────────────────────────────────────────────────
TEST_CASE("v1-oracle: DWNC all-fixed record (Point)",
          "[oracle][dwnc]")
{
   auto v1 = v1_point();
   auto v3 = v3_point();
   require_ssz_oracle(v1, v3);
   require_pssz_oracle(v1, v3);
   require_frac_oracle(v1, v3);
   require_bin_oracle(v1, v3);
   require_borsh_oracle(v1, v3);
   require_bincode_oracle(v1, v3);
   require_key_oracle(v1, v3);
   require_avro_oracle(v1, v3);
}

TEST_CASE("v1-oracle: non-DWNC variable record (Record)",
          "[oracle][variable]")
{
   auto v1 = v1_record();
   auto v3 = v3_record();
   require_ssz_oracle(v1, v3);
   require_pssz_oracle(v1, v3);
   require_frac_oracle(v1, v3);
   require_bin_oracle(v1, v3);
   require_borsh_oracle(v1, v3);
   require_bincode_oracle(v1, v3);
   require_key_oracle(v1, v3);
   require_avro_oracle(v1, v3);
}

TEST_CASE("v1-oracle: non-DWNC variable record, no optional (FlatRecord)",
          "[oracle][flat]")
{
   auto v1 = v1_flat_record();
   auto v3 = v3_flat_record();
   require_ssz_oracle(v1, v3);
   require_pssz_oracle(v1, v3);
   require_frac_oracle(v1, v3);
   require_bin_oracle(v1, v3);
   require_borsh_oracle(v1, v3);
   require_bincode_oracle(v1, v3);
   require_key_oracle(v1, v3);
   require_avro_oracle(v1, v3);
}
