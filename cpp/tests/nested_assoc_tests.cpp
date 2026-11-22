// psio::nested<T> with maps and sets: see nested_assoc_cases.hpp for the
// cases shared with the exception-disabled build. This file adds seeded
// mutation of the readers' input.
#include "nested_assoc_cases.hpp"

#include <catch.hpp>

#include <random>

using namespace nested_assoc_cases;

TEST_CASE("nested x map / set: every checked entry point, without a throw", "[nested][assoc]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = nested_assoc_cases::run());
   CHECK(failed == 0);
}

TEST_CASE("nested x map / set: equal values under distinct keys", "[nested][assoc]")
{
   // Equal nested VALUES are fine (only keys are unique), in any container.
   const std::unordered_map<std::uint8_t, NU32> same{{2, NU32::from(5)}, {1, NU32::from(5)}};
   CHECK(psio::encode(psio::pssz{}, same) ==
         pssz_of(std::vector<std::tuple<std::uint8_t, Octets>>{
            {1, octets(pssz_of(std::uint32_t{5}))}, {2, octets(pssz_of(std::uint32_t{5}))}}));
}

// Checked readers of a frame holding nested values inside maps and sets
// never throw, abort or read out of bounds, and the typed and schema-driven
// validators agree on every mutated frame.
TEST_CASE("nested x map / set: checked readers on mutated frames", "[nested][assoc][fuzz]")
{
   namespace rt = psio::pssz_ir;
   const auto   schema = psio::SchemaBuilder{}.insert<Ledger>("Ledger").build();
   const Ledger ledger = sample_ledger();
   const Bytes  frame  = pssz_of(ledger);
   const Bytes  key    = key_of(ledger);
   std::mt19937 rng(0x6e657374u);

   std::size_t accepted = 0;
   auto        probe    = [&](const Bytes& bytes) {
      const std::span<const char> span{bytes};
      bool                        typed_unknown = false, dynamic_unknown = false;
      psio::codec_status          typed, dynamic;
      REQUIRE_NOTHROW(typed = psio::validate_pssz_compatible<Ledger>(psio::pssz{}, span,
                                                                    typed_unknown));
      REQUIRE_NOTHROW(dynamic = rt::validate_pssz(schema, "Ledger", span, &dynamic_unknown));
      INFO("typed: " << (typed.ok() ? "ok" : typed.error().what)
                     << "; schema-driven: " << (dynamic.ok() ? "ok" : dynamic.error().what));
      CHECK(typed.ok() == dynamic.ok());
      if (typed.ok() && dynamic.ok())
      {
         CHECK(typed_unknown == dynamic_unknown);
         ++accepted;
      }
      REQUIRE_NOTHROW((void)psio::validate_strict<Ledger>(psio::pssz{}, span));
      Ledger out;
      bool   unknown = false;
      REQUIRE_NOTHROW((void)psio::decode_pssz_checked(psio::pssz{}, span, out, unknown));
      rt::Value value;
      REQUIRE_NOTHROW((void)rt::decode_auto(schema, "Ledger", span, value, unknown,
                                            nullptr, rt::nested_bytes::opaque));
      std::string text;
      REQUIRE_NOTHROW((void)rt::pssz_to_json(schema, "Ledger", psio::json{}, span, text));
   };
   probe(frame);
   CHECK(accepted == 1);
   // Every single-byte change, every truncation, and random multi-byte noise.
   for (std::size_t i = 0; i < frame.size(); ++i)
      for (int delta : {1, 0x80, 0xff})
      {
         Bytes mutated = frame;
         mutated[i]    = static_cast<char>(static_cast<unsigned char>(mutated[i]) ^ delta);
         probe(mutated);
      }
   for (std::size_t n = 0; n < frame.size(); ++n)
      probe(Bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(n)));
   for (int round = 0; round < 2000; ++round)
   {
      Bytes mutated = frame;
      for (int k = 1 + static_cast<int>(rng() % 3); k > 0; --k)
         mutated[rng() % mutated.size()] = static_cast<char>(rng());
      probe(mutated);
   }

   // Keys.
   for (std::size_t i = 0; i < key.size(); ++i)
      for (int delta : {1, 0x80, 0xff})
      {
         Bytes mutated = key;
         mutated[i]    = static_cast<char>(static_cast<unsigned char>(mutated[i]) ^ delta);
         const std::span<const char> span{mutated};
         psio::codec_status          status;
         REQUIRE_NOTHROW(status = psio::validate<Ledger>(psio::key{}, span));
         REQUIRE_NOTHROW((void)psio::validate_strict<Ledger>(psio::key{}, span));
         // What validates is the canonical key of what it decodes to.
         if (status.ok())
            CHECK(key_of(psio::decode<Ledger>(psio::key{}, span)) == mutated);
      }
   for (std::size_t n = 0; n < key.size(); ++n)
      CHECK(!psio::validate<Ledger>(
                psio::key{},
                std::span<const char>{key.data(), n})
                .ok());
}
