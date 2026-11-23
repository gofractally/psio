// psio::validate_strict over adapters, invariants, transparent and bounded
// types (pssz_strict_cases.hpp); pssz_strict_noexceptions.cpp runs the same
// cases with exceptions and RTTI disabled.

#include <catch.hpp>

#include "pssz_strict_cases.hpp"

TEST_CASE("validate_strict: adapters, invariants, bounds, nested values", "[pssz][strict]")
{
   REQUIRE(strict_cases::run() == 0);
}

#if defined(__cpp_exceptions)
namespace
{
   struct Thrower
   {
      std::uint32_t v = 0;
   };
   PSIO_REFLECT(Thrower, definitionWillNotChange(), v)
   struct ThrowerBin
   {
      static std::size_t packsize(const Thrower&) { return 4; }
      template <typename Sink>
      static void encode(const Thrower& t, Sink& s) { s.write(&t.v, 4); }
      static void encode(const Thrower& t, std::vector<char>& o)
      {
         o.insert(o.end(), reinterpret_cast<const char*>(&t.v),
                  reinterpret_cast<const char*>(&t.v) + 4);
      }
      // Breaks the adapter contract: fails on bytes validate accepted.
      static Thrower decode(std::span<const char>) { throw 1; }
      static psio::codec_status validate(std::span<const char>) noexcept { return psio::codec_ok(); }
      static psio::codec_status validate_strict(std::span<const char>) noexcept
      {
         return psio::codec_ok();
      }
   };
}  // namespace
PSIO_ADAPTER(Thrower, psio::binary_category, ThrowerBin)

TEST_CASE("validate_strict: a throwing adapter is a failed status where exceptions exist",
          "[pssz][strict]")
{
   const std::vector<char> bytes(4, 0);
   auto st = psio::validate_strict<Thrower>(psio::pssz{}, std::span<const char>{bytes});
   REQUIRE_FALSE(st.ok());
   REQUIRE(st.error().what == "pssz: decode failed during validate_strict");
}
#endif
