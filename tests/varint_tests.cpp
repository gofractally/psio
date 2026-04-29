// libraries/psio/cpp/tests/varint_tests.cpp — varint correctness.
//
// Exercises every encode/decode pair for round-trip, every fast::
// path against the scalar:: reference, and the canonicity rules
// where each algorithm has them.
//
// The fast paths are scalar in this commit (NEON + BMI2 land
// separately), so the parity assertions are tautologically true today
// — they exist to lock the harness in place so the next commit, which
// substitutes a divergent fast path, gets diff-free coverage.

#include <psio/varint/varint.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace v = psio::varint;

namespace {

   // Deterministic PRNG so failures are reproducible. seed_seq fixed.
   std::mt19937_64 rng(0xc0ffee'd00d'5eedULL);

   std::vector<std::uint64_t> sample_u64(std::size_t n)
   {
      std::vector<std::uint64_t> out;
      out.reserve(n);
      // Mixture: small / medium / large / spec-edge.
      std::uniform_int_distribution<int> bucket(0, 3);
      for (std::size_t i = 0; i < n; ++i)
      {
         std::uint64_t v;
         switch (bucket(rng))
         {
            case 0: v = rng() & 0x1F; break;                    // [0, 32)
            case 1: v = rng() & 0xFFFF; break;                  // [0, 64k)
            case 2: v = rng() & 0xFFFFFFFFu; break;             // [0, 4G)
            default: v = rng(); break;                          // full u64
         }
         out.push_back(v);
      }
      // Force a few exact boundary values into the corpus.
      out.insert(out.end(),
                 {0ULL, 31ULL, 32ULL, (1ULL << 13) - 1, 1ULL << 13,
                  (1ULL << 53) - 1, 1ULL << 53, ~0ULL});
      return out;
   }

}  // namespace

// ───────────────────────────────────────────────────────────────────
// LEB128 — uleb / sleb / zigzag round-trips and parity.
// ───────────────────────────────────────────────────────────────────

TEST_CASE("leb128 uleb32 round-trips", "[varint][leb128]")
{
   std::array<std::uint8_t, v::leb128::max_bytes_u32> buf{};
   for (std::uint64_t base : sample_u64(4096))
   {
      const auto v32 = static_cast<std::uint32_t>(base);
      const auto n   = v::leb128::scalar::encode_u32(buf.data(), v32);
      REQUIRE(n >= 1);
      REQUIRE(n <= v::leb128::max_bytes_u32);
      REQUIRE(n == v::leb128::scalar::size_u32(v32));
      const auto r = v::leb128::scalar::decode_u32(buf.data(), n);
      REQUIRE(r.ok);
      REQUIRE(r.len == n);
      REQUIRE(r.value == v32);
   }
}

TEST_CASE("leb128 uleb64 round-trips", "[varint][leb128]")
{
   std::array<std::uint8_t, v::leb128::max_bytes_u64> buf{};
   for (std::uint64_t value : sample_u64(4096))
   {
      const auto n = v::leb128::scalar::encode_u64(buf.data(), value);
      REQUIRE(n >= 1);
      REQUIRE(n <= v::leb128::max_bytes_u64);
      REQUIRE(n == v::leb128::scalar::size_u64(value));
      const auto r = v::leb128::scalar::decode_u64(buf.data(), n);
      REQUIRE(r.ok);
      REQUIRE(r.len == n);
      REQUIRE(r.value == value);
   }
}

TEST_CASE("leb128 sleb32/64 round-trips", "[varint][leb128]")
{
   std::array<std::uint8_t, v::leb128::max_bytes_i64> buf{};
   for (std::uint64_t base : sample_u64(4096))
   {
      const auto i32 = static_cast<std::int32_t>(base);
      const auto n32 = v::leb128::scalar::encode_i32(buf.data(), i32);
      const auto r32 = v::leb128::scalar::decode_i32(buf.data(), n32);
      REQUIRE(r32.ok);
      REQUIRE(r32.len == n32);
      REQUIRE(r32.value == i32);

      const auto i64 = static_cast<std::int64_t>(base);
      const auto n64 = v::leb128::scalar::encode_i64(buf.data(), i64);
      const auto r64 = v::leb128::scalar::decode_i64(buf.data(), n64);
      REQUIRE(r64.ok);
      REQUIRE(r64.len == n64);
      REQUIRE(r64.value == i64);
   }
}

TEST_CASE("leb128 zigzag64 round-trips", "[varint][leb128]")
{
   std::array<std::uint8_t, v::leb128::max_bytes_i64> buf{};
   for (std::uint64_t base : sample_u64(4096))
   {
      const auto v64 = static_cast<std::int64_t>(base);
      const auto n   = v::leb128::scalar::encode_zigzag64(buf.data(), v64);
      const auto r   = v::leb128::scalar::decode_zigzag64(buf.data(), n);
      REQUIRE(r.ok);
      REQUIRE(r.len == n);
      REQUIRE(r.value == v64);
   }
}

TEST_CASE("leb128 fast == scalar parity", "[varint][leb128][parity]")
{
   std::array<std::uint8_t, v::leb128::max_bytes_u64> sbuf{};
   std::array<std::uint8_t, v::leb128::max_bytes_u64> fbuf{};
   for (std::uint64_t value : sample_u64(8192))
   {
      const auto sn = v::leb128::scalar::encode_u64(sbuf.data(), value);
      const auto fn = v::leb128::fast::encode_u64(fbuf.data(), value);
      REQUIRE(sn == fn);
      REQUIRE(std::equal(sbuf.begin(), sbuf.begin() + sn, fbuf.begin()));

      const auto sr = v::leb128::scalar::decode_u64(sbuf.data(), sn);
      const auto fr = v::leb128::fast::decode_u64(sbuf.data(), sn);
      REQUIRE(sr.ok == fr.ok);
      REQUIRE(sr.len == fr.len);
      REQUIRE(sr.value == fr.value);
   }
}

TEST_CASE("leb128 truncated input rejected", "[varint][leb128]")
{
   const std::uint8_t buf[] = {0x80, 0x80};  // continuation, no terminator
   REQUIRE_FALSE(v::leb128::scalar::decode_u32(buf, 2).ok);
   REQUIRE_FALSE(v::leb128::scalar::decode_u64(buf, 2).ok);
}

TEST_CASE("leb128 oversize rejected", "[varint][leb128]")
{
   // Six continuation bytes followed by a final — too many for u32.
   const std::uint8_t buf[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
   REQUIRE_FALSE(v::leb128::scalar::decode_u32(buf, sizeof buf).ok);
}

// ───────────────────────────────────────────────────────────────────
// prefix3 — round-trip + canonicity.
// ───────────────────────────────────────────────────────────────────

TEST_CASE("prefix3 round-trips full u64", "[varint][prefix3]")
{
   std::array<std::uint8_t, v::prefix3::max_bytes_u64> buf{};
   for (std::uint64_t value : sample_u64(8192))
   {
      const auto n = v::prefix3::scalar::encode_u64(buf.data(), value);
      REQUIRE(n == v::prefix3::scalar::size_u64(value));
      const auto r = v::prefix3::scalar::decode_u64(buf.data(), n);
      REQUIRE(r.ok);
      REQUIRE(r.len == n);
      REQUIRE(r.value == value);
   }
}

TEST_CASE("prefix3 length table matches spec", "[varint][prefix3]")
{
   REQUIRE(v::prefix3::scalar::size_u64(0) == 1);
   REQUIRE(v::prefix3::scalar::size_u64((1ULL << 5) - 1) == 1);
   REQUIRE(v::prefix3::scalar::size_u64(1ULL << 5) == 2);
   REQUIRE(v::prefix3::scalar::size_u64((1ULL << 13) - 1) == 2);
   REQUIRE(v::prefix3::scalar::size_u64(1ULL << 13) == 3);
   REQUIRE(v::prefix3::scalar::size_u64((1ULL << 53) - 1) == 7);
   REQUIRE(v::prefix3::scalar::size_u64(1ULL << 53) == 9);  // skips 8
   REQUIRE(v::prefix3::scalar::size_u64(~0ULL) == 9);
}

TEST_CASE("prefix3 strict rejects non-canonical", "[varint][prefix3]")
{
   // 2-byte form encoding value 0 (which canonical form would put in
   // 1 byte).
   const std::uint8_t buf[] = {0x20 /* code=001, low5=0 */, 0x00};
   const auto         loose = v::prefix3::scalar::decode_u64(buf, 2);
   REQUIRE(loose.ok);
   REQUIRE(loose.value == 0);
   const auto strict = v::prefix3::scalar::decode_u64_strict(buf, 2);
   REQUIRE_FALSE(strict.ok);
}

TEST_CASE("prefix3 strict rejects escape with reserved bits set",
          "[varint][prefix3]")
{
   std::uint8_t buf[9] = {0xE1};  // code=111, low5 = 1 (illegal)
   for (int i = 1; i < 9; ++i) buf[i] = 0;
   REQUIRE_FALSE(v::prefix3::scalar::decode_u64_strict(buf, 9).ok);
}

// ───────────────────────────────────────────────────────────────────
// prefix2 — round-trip both flavours, BE matches QUIC test vectors.
// ───────────────────────────────────────────────────────────────────

TEST_CASE("prefix2 BE round-trips up to max_value", "[varint][prefix2]")
{
   std::array<std::uint8_t, v::prefix2::max_bytes_u64> buf{};
   for (std::uint64_t value : sample_u64(4096))
   {
      const auto v62 = value & v::prefix2::max_value;
      const auto n   = v::prefix2::be::scalar::encode_u64(buf.data(), v62);
      REQUIRE(n >= 1);
      REQUIRE(n == v::prefix2::size_u64(v62));
      const auto r = v::prefix2::be::scalar::decode_u64(buf.data(), n);
      REQUIRE(r.ok);
      REQUIRE(r.len == n);
      REQUIRE(r.value == v62);
   }
}

TEST_CASE("prefix2 LE round-trips up to max_value", "[varint][prefix2]")
{
   std::array<std::uint8_t, v::prefix2::max_bytes_u64> buf{};
   for (std::uint64_t value : sample_u64(4096))
   {
      const auto v62 = value & v::prefix2::max_value;
      const auto n   = v::prefix2::le::scalar::encode_u64(buf.data(), v62);
      REQUIRE(n == v::prefix2::size_u64(v62));
      const auto r = v::prefix2::le::scalar::decode_u64(buf.data(), n);
      REQUIRE(r.ok);
      REQUIRE(r.len == n);
      REQUIRE(r.value == v62);
   }
}

TEST_CASE("prefix2 BE matches RFC 9000 § A.1 vectors", "[varint][prefix2]")
{
   // From RFC 9000 Appendix A.1.
   struct vec { std::uint64_t value; std::vector<std::uint8_t> wire; };
   const vec vecs[] = {
      {151'288'809'941'952'652ULL,
       {0xc2, 0x19, 0x7c, 0x5e, 0xff, 0x14, 0xe8, 0x8c}},
      {494'878'333ULL,         {0x9d, 0x7f, 0x3e, 0x7d}},
      {15'293ULL,              {0x7b, 0xbd}},
      {37ULL,                  {0x25}},
   };
   for (const auto& v : vecs)
   {
      std::array<std::uint8_t, 8> buf{};
      const auto                  n =
         psio::varint::prefix2::be::scalar::encode_u64(buf.data(), v.value);
      REQUIRE(n == v.wire.size());
      REQUIRE(std::equal(v.wire.begin(), v.wire.end(), buf.begin()));

      const auto r =
         psio::varint::prefix2::be::scalar::decode_u64(v.wire.data(),
                                                       v.wire.size());
      REQUIRE(r.ok);
      REQUIRE(r.len == v.wire.size());
      REQUIRE(r.value == v.value);
   }
}

TEST_CASE("prefix2 BE fast == scalar parity", "[varint][prefix2][parity]")
{
   std::array<std::uint8_t, 16> buf{};  // padded so fast-path 8-byte load is safe
   for (std::uint64_t value : sample_u64(8192))
   {
      const auto v62 = value & v::prefix2::max_value;
      const auto n   = v::prefix2::be::scalar::encode_u64(buf.data(), v62);

      const auto sr = v::prefix2::be::scalar::decode_u64(buf.data(), 16);
      const auto fr = v::prefix2::be::fast::decode_u64(buf.data(), 16);
      REQUIRE(sr.ok);
      REQUIRE(fr.ok);
      REQUIRE(sr.len == n);
      REQUIRE(fr.len == n);
      REQUIRE(sr.value == fr.value);
   }
}

TEST_CASE("prefix2 LE fast == scalar parity", "[varint][prefix2][parity]")
{
   std::array<std::uint8_t, 16> buf{};
   for (std::uint64_t value : sample_u64(8192))
   {
      const auto v62 = value & v::prefix2::max_value;
      const auto n   = v::prefix2::le::scalar::encode_u64(buf.data(), v62);

      const auto sr = v::prefix2::le::scalar::decode_u64(buf.data(), 16);
      const auto fr = v::prefix2::le::fast::decode_u64(buf.data(), 16);
      REQUIRE(sr.ok);
      REQUIRE(fr.ok);
      REQUIRE(sr.len == n);
      REQUIRE(fr.len == n);
      REQUIRE(sr.value == fr.value);
   }
}

TEST_CASE("prefix2 rejects values >= 2^62", "[varint][prefix2]")
{
   std::array<std::uint8_t, 8> buf{};
   REQUIRE(v::prefix2::be::scalar::encode_u64(buf.data(), 1ULL << 62) == 0);
   REQUIRE(v::prefix2::le::scalar::encode_u64(buf.data(), 1ULL << 62) == 0);
}
