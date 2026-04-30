// Phase 0 placeholder. Proves the build system works end-to-end —
// library links, test runner compiles, catch2 main() is found, the
// test is registered with ctest. Replaced by real tests in Phase 1.

#include <psio/psio.hpp>

#include <catch.hpp>

TEST_CASE("psio scaffold compiles and links", "[scaffold]")
{
   // Reaching this point means the CMake target, include path, Boost
   // link, and Catch2 plumbing all worked. Real functionality tests
   // live in phase-specific test files (phase 1+).
   REQUIRE(true);
}
