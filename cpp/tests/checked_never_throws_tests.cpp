// The same cases with exceptions enabled: nothing may be thrown.
#include "checked_never_throws.hpp"

#include <catch.hpp>

TEST_CASE("status-returning entry points never throw", "[checked]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = never_throws::run_checked_cases());
   CHECK(failed == 0);
}

TEST_CASE("the plain encoder still fails fast on the same values", "[checked]")
{
   using namespace never_throws;
   const std::string big(200, 'x');
   Outer             bad;
   bad.inner.text = big;
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, bad), psio::codec_exception);
   CHECK_THROWS_AS(psio::size_of(psio::pssz{}, bad), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, Wide{std::string(70000, 'a'), "b"}),
                   psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, Inner{big}), psio::codec_exception);
}
