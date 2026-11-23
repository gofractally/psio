// The service-shaped schema (graphql_service_fixture.hpp) with exceptions
// and RTTI disabled, as a WebAssembly guest builds it: key ranges, pages,
// byte budgets, flattened members and argument groups answer as they do in
// graphql_service_tests.cpp, and nothing aborts.

#include "graphql_service_checks.hpp"

#include <cstdio>

int main()
{
   const std::vector<std::string> failures = gqls::service_failures();
   for (const std::string& f : failures)
      std::printf("FAIL %s\n", f.c_str());
   if (!failures.empty())
      return 1;
   std::puts("graphql service noexceptions: ok");
   return 0;
}
