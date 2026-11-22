// Phase 7 — pSSZ format tag family (pssz8 / pssz16 / pssz32).
//
// Same shape coverage as ssz_tests.cpp but verifies:
//   1. All three widths round-trip primitives, arrays, vectors, strings,
//      optionals, and reflected records.
//   2. Narrower offsets produce smaller wire output.
//   3. Each width's codec agrees with itself on its own output.

#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

struct PssEmpty {};
PSIO_REFLECT(PssEmpty)
struct PssUnit {};
PSIO_REFLECT(PssUnit)
PSIO_TYPE_ATTRS(PssUnit, psio::definition_will_not_change{})

TEMPLATE_TEST_CASE("pssz positional tuples use extensible record framing",
                   "[pssz][tuple]", psio::pssz8, psio::pssz16, psio::pssz32)
{
   using F = TestType;
   auto empty = psio::encode(F{}, std::tuple<>{});
   REQUIRE(empty == std::vector<char>(F::offset_width, 0));
   REQUIRE(psio::validate<std::tuple<>>(F{}, empty).ok());
   REQUIRE((psio::decode<std::tuple<>>(F{}, empty) == std::tuple<>{}));

   using Arguments = std::tuple<std::uint32_t, std::string>;
   const Arguments arguments{7, "hi"};
   auto bytes = psio::encode(F{}, arguments);
   REQUIRE(psio::validate<Arguments>(F{}, bytes).ok());
   REQUIRE((psio::decode<Arguments>(F{}, bytes) == arguments));
   using Choice = std::variant<std::uint32_t, Arguments>;
   const Choice choice{std::in_place_index<1>, arguments};
   auto variant = psio::encode(F{}, choice);
   REQUIRE(psio::validate<Choice>(F{}, variant).ok());
   REQUIRE((psio::decode<Choice>(F{}, variant) == choice));
}

TEMPLATE_TEST_CASE("pssz encodes actual zero-field records across widths",
                   "[pssz][empty]", psio::pssz8, psio::pssz16, psio::pssz32)
{
   using F = TestType;
   auto empty = psio::encode(F{}, PssEmpty{});
   REQUIRE(empty == std::vector<char>(F::offset_width, 0));
   REQUIRE(psio::validate<PssEmpty>(F{}, empty).ok());
   psio::decode<PssEmpty>(F{}, empty);
   auto unit = psio::encode(F{}, PssUnit{});
   REQUIRE(unit.empty());
   REQUIRE(psio::validate<PssUnit>(F{}, unit).ok());
   psio::decode<PssUnit>(F{}, unit);
}

// These fixtures opt into definitionWillNotChange(): the pssz wire
// layout's u{W} fixed_size header is reserved for extensible types,
// and the size-equality assertions below ("4 + 1 + 3", "8", etc.)
// match the DWNC layout where the header is elided. Non-DWNC pssz
// records carry a u{W} header (W=1/2/4 for pssz8/16/32). Tests that
// exercise the header path live under the [pssz][caps] section.
struct PssPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(PssPoint, x, y, definitionWillNotChange())

struct PssLabelled
{
   std::int32_t id;
   std::string  name;
};
PSIO_REFLECT(PssLabelled, id, name, definitionWillNotChange())

struct PssPacket
{
   std::uint16_t             version;
   std::vector<std::int32_t> payload;
};
PSIO_REFLECT(PssPacket, version, payload, definitionWillNotChange())

TEMPLATE_TEST_CASE("pssz round-trips primitives across widths",
                   "[pssz][primitive]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   auto b  = psio::encode(F{}, std::uint32_t{0xDEADBEEF});
   REQUIRE(b.size() == 4);
   auto v =
      psio::decode<std::uint32_t>(F{}, std::span<const char>{b});
   REQUIRE(v == 0xDEADBEEF);
}

TEMPLATE_TEST_CASE("pssz round-trips std::string across widths",
                   "[pssz][string]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   auto b = psio::encode(F{}, std::string{"hi"});
   auto v = psio::decode<std::string>(F{}, std::span<const char>{b});
   REQUIRE(v == "hi");
}

TEMPLATE_TEST_CASE("pssz round-trips fixed records across widths",
                   "[pssz][record]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   PssPoint p{2, 5};
   auto     b = psio::encode(F{}, p);
   REQUIRE(b.size() == 8);
   auto v = psio::decode<PssPoint>(F{}, std::span<const char>{b});
   REQUIRE(v.x == 2);
   REQUIRE(v.y == 5);
}

TEST_CASE("pssz8 record-with-variable-field uses 1-byte offsets",
          "[pssz][record][variable]")
{
   PssLabelled l{7, "abc"};
   auto        b8  = psio::encode(psio::pssz8{}, l);
   auto        b16 = psio::encode(psio::pssz16{}, l);
   auto        b32 = psio::encode(psio::pssz32{}, l);

   // Fixed id(4) + offset (1/2/4) + tail "abc" (3).
   REQUIRE(b8.size() == 4 + 1 + 3);
   REQUIRE(b16.size() == 4 + 2 + 3);
   REQUIRE(b32.size() == 4 + 4 + 3);
}

TEMPLATE_TEST_CASE("pssz round-trips records with variable fields",
                   "[pssz][record][variable]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   PssLabelled l{42, "alice"};
   auto        b = psio::encode(F{}, l);
   auto        v = psio::decode<PssLabelled>(F{}, std::span<const char>{b});
   REQUIRE(v.id == 42);
   REQUIRE(v.name == "alice");
}

TEMPLATE_TEST_CASE("pssz round-trips vectors of fixed elements",
                   "[pssz][vector]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   std::vector<std::uint32_t> v{1, 2, 3};
   auto                       b = psio::encode(F{}, v);
   REQUIRE(b.size() == 12);
   auto back = psio::decode<std::vector<std::uint32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back == v);
}

TEMPLATE_TEST_CASE("pssz round-trips records with a vector field",
                   "[pssz][record][vector]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   PssPacket p{9, {100, 200, 300}};
   auto      b    = psio::encode(F{}, p);
   auto      back = psio::decode<PssPacket>(F{}, std::span<const char>{b});
   REQUIRE(back.version == 9);
   REQUIRE(back.payload == std::vector<std::int32_t>{100, 200, 300});
}

TEMPLATE_TEST_CASE("pssz round-trips std::optional across widths",
                   "[pssz][optional]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   std::optional<std::uint32_t> some = 123;
   auto                         b    = psio::encode(F{}, some);
   REQUIRE(b.size() == 4);
   auto back = psio::decode<std::optional<std::uint32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back.has_value());
   REQUIRE(*back == 123);

   std::optional<std::uint32_t> none;
   auto                         b2 = psio::encode(F{}, none);
   REQUIRE(b2.size() == 0);
}

TEMPLATE_TEST_CASE("pssz size_of agrees with encode output size",
                   "[pssz][size_of]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   PssLabelled l{1, "hello"};
   auto        b = psio::encode(F{}, l);
   REQUIRE(psio::size_of(F{}, l) == b.size());
}

TEMPLATE_TEST_CASE("pssz validate accepts well-formed buffers",
                   "[pssz][validate]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   auto b  = psio::encode(F{}, std::uint32_t{0xCAFEBABE});
   auto s = psio::validate<std::uint32_t>(F{}, std::span<const char>{b});
   REQUIRE(s.ok());
}

TEMPLATE_TEST_CASE("pssz validate rejects truncated primitives",
                   "[pssz][validate]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   char tiny[2]{};
   auto s =
      psio::validate<std::uint32_t>(F{}, std::span<const char>{tiny, 2});
   REQUIRE(!s.ok());
   REQUIRE(s.error().format_name == "pssz");
}

TEMPLATE_TEST_CASE("pssz scoped sugar matches generic CPO",
                   "[pssz][format_tag_base]", psio::pssz8, psio::pssz16,
                   psio::pssz32)
{
   using F = TestType;
   PssPoint p{8, -4};
   auto     a = psio::encode(F{}, p);
   auto     b = F::encode(p);
   REQUIRE(a == b);

   auto v  = psio::decode<PssPoint>(F{}, std::span<const char>{a});
   auto v2 = F::template decode<PssPoint>(std::span<const char>{a});
   REQUIRE(v.x == v2.x);
   REQUIRE(v.y == v2.y);
}

// ── maxDynamicData encode-time enforcement ───────────────────────────────

struct CappedRecord
{
   std::uint64_t            id;
   std::vector<std::uint8_t> blob;
};
PSIO_REFLECT(CappedRecord, id, blob,
             definitionWillNotChange(),
             maxDynamicData(64))

TEST_CASE("pssz encode throws when total exceeds maxDynamicData cap",
          "[pssz][caps]")
{
   CappedRecord ok{42, std::vector<std::uint8_t>(8, 0xAB)};  // small blob
   REQUIRE_NOTHROW(psio::encode(psio::pssz{}, ok));

   CappedRecord big{42, std::vector<std::uint8_t>(256, 0xAB)};  // > cap
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{}, big), psio::codec_exception);
}

TEST_CASE("pssz validate rejects buffer exceeding maxDynamicData cap",
          "[pssz][caps]")
{
   // Wire bytes engineered larger than the cap (bytes.size() > 64 cap).
   std::vector<char> oversized(128, 0);
   auto              st = psio::validate<CappedRecord>(
      psio::pssz{}, std::span<const char>{oversized});
   REQUIRE(!st.ok());
   REQUIRE(st.error().format_name == "pssz");

   REQUIRE_THROWS_AS(
      psio::validate_or_throw<CappedRecord>(
         psio::pssz{}, std::span<const char>{oversized}),
      psio::codec_exception);
}

// ── Record field evolution (the probe's V1/V2 pair) ─────────────────────────
//
// A field appended after first publication is optional<T>: it owns an offset
// slot, and readers honor the header's fixed-region size instead of assuming
// their own schema's. See pssz_evolution_matrix_tests.cpp for the full matrix.
struct PssEvoV1
{
   std::uint32_t              a;
   std::optional<std::string> b;
   friend bool operator==(const PssEvoV1&, const PssEvoV1&) = default;
};
PSIO_REFLECT(PssEvoV1, a, b)
struct PssEvoV2
{
   std::uint32_t                a;
   std::optional<std::string>   b;
   std::optional<std::uint32_t> c;
   friend bool operator==(const PssEvoV2&, const PssEvoV2&) = default;
};
PSIO_REFLECT(PssEvoV2, a, b, c)

TEMPLATE_TEST_CASE("pssz records evolve through trailing optional fields",
                   "[pssz][evolution]", psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz)
{
   using F = TestType;
   // Both revisions are unbounded, so the automatic profile uses four bytes.
   STATIC_REQUIRE(psio::auto_pssz_width_v<PssEvoV1> == 4);
   STATIC_REQUIRE(psio::auto_pssz_width_v<PssEvoV2> == 4);
   const std::size_t W = std::is_same_v<F, psio::pssz> ? 4 : [] {
      if constexpr (requires { F::offset_width; }) return std::size_t{F::offset_width};
      else return std::size_t{4};
   }();
   auto slot = [&](std::vector<char>& out, std::size_t value) {
      for (std::size_t i = 0; i < W; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
   };

   const PssEvoV1 v1{7, std::string("hi")};
   const auto     v1_bytes = psio::encode(F{}, v1);
   // header | a | offset(b) | Some("hi")
   std::vector<char> expect_v1;
   slot(expect_v1, 4 + W);
   expect_v1.insert(expect_v1.end(), {7, 0, 0, 0});
   slot(expect_v1, 4 + W);
   expect_v1.insert(expect_v1.end(), {1, 'h', 'i'});
   REQUIRE(v1_bytes == expect_v1);
   if (W == 4)
      REQUIRE(v1_bytes == std::vector<char>{8, 0, 0, 0, 7, 0, 0, 0, 8, 0, 0, 0, 1, 'h', 'i'});

   // Unset suffix is pruned: V2{7,hi,none} is byte-identical to V1{7,hi}.
   REQUIRE(psio::encode(F{}, PssEvoV2{7, std::string("hi"), std::nullopt}) == v1_bytes);
   // Every trailing None is pruned, leaving only the required prefix.
   std::vector<char> expect_prefix;
   slot(expect_prefix, 4);
   expect_prefix.insert(expect_prefix.end(), {7, 0, 0, 0});
   REQUIRE(psio::encode(F{}, PssEvoV2{7, std::nullopt, std::nullopt}) == expect_prefix);
   REQUIRE(psio::encode(F{}, PssEvoV1{7, std::nullopt}) == expect_prefix);

   // Old data -> new reader.
   REQUIRE(psio::validate<PssEvoV2>(F{}, v1_bytes).ok());
   REQUIRE(psio::validate_strict<PssEvoV2>(F{}, v1_bytes).ok());
   REQUIRE((psio::decode<PssEvoV2>(F{}, v1_bytes) ==
            PssEvoV2{7, std::string("hi"), std::nullopt}));
   REQUIRE((psio::decode<PssEvoV2>(F{}, expect_prefix) ==
            PssEvoV2{7, std::nullopt, std::nullopt}));

   // New data -> old reader: the unknown appended field is ignored, and it
   // still delimits `b`.
   const PssEvoV2 v2{7, std::string("hi"), 9};
   const auto     v2_bytes = psio::encode(F{}, v2);
   std::vector<char> expect_v2;
   slot(expect_v2, 4 + 2 * W);
   expect_v2.insert(expect_v2.end(), {7, 0, 0, 0});
   slot(expect_v2, 4 + 2 * W);
   slot(expect_v2, 4 + 2 * W + 3);
   expect_v2.insert(expect_v2.end(), {1, 'h', 'i', 9, 0, 0, 0});
   REQUIRE(v2_bytes == expect_v2);
   REQUIRE((psio::decode<PssEvoV2>(F{}, v2_bytes) == v2));
   REQUIRE(psio::validate<PssEvoV1>(F{}, v2_bytes).ok());
   REQUIRE((psio::decode<PssEvoV1>(F{}, v2_bytes) == v1));
   bool unknown = false;
   REQUIRE(psio::validate_pssz_compatible<PssEvoV1>(F{}, v2_bytes, unknown).ok());
   REQUIRE(unknown);
   REQUIRE_FALSE(psio::validate_strict<PssEvoV1>(F{}, v2_bytes).ok());
   REQUIRE(psio::validate_pssz_compatible<PssEvoV2>(F{}, v1_bytes, unknown).ok());
   REQUIRE_FALSE(unknown);

   // Pruning is mandatory: a stored trailing None is rejected.
   std::vector<char> unpruned;
   slot(unpruned, 4 + 2 * W);
   unpruned.insert(unpruned.end(), {7, 0, 0, 0});
   slot(unpruned, 4 + 2 * W);
   slot(unpruned, 4 + 2 * W + 3);
   unpruned.insert(unpruned.end(), {1, 'h', 'i'});
   REQUIRE_FALSE(psio::validate<PssEvoV2>(F{}, unpruned).ok());
   REQUIRE_FALSE(psio::validate_strict<PssEvoV2>(F{}, unpruned).ok());
   REQUIRE_THROWS_AS(psio::validate_or_throw<PssEvoV2>(F{}, unpruned), psio::codec_exception);
   REQUIRE_THROWS_AS(psio::decode<PssEvoV2>(F{}, unpruned), psio::codec_exception);
}
