// Shared Catch2 main for every psio test executable.
// The Catch2 v2 single-header lives at ../../psio/external/Catch2/catch.hpp.
// Define CATCH_CONFIG_MAIN exactly once per test binary; all other TUs
// just include <catch.hpp>.

#define CATCH_CONFIG_MAIN
#include <catch.hpp>
