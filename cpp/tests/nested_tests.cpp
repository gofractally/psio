// psio::nested<T>: see nested_cases.hpp for the cases shared with the
// exception-disabled build. This file adds what needs exceptions or Catch:
// the fail-fast paths and seeded mutation of every reader's input.
#include "nested_cases.hpp"

#include <catch.hpp>

TEST_CASE("nested: every checked entry point, without a throw", "[nested]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = nested_cases::run());
   CHECK(failed == 0);
}
