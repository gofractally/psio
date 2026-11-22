// psio::nested<T> with exceptions and RTTI disabled (how a WebAssembly guest
// builds it): every checked entry point reports through its status, so a
// failure that was a throw would be an abort here. Reaching the end is the
// test.
#include "nested_assoc_cases.hpp"
#include "nested_cases.hpp"

#include <cstdio>

int main()
{
   const int failed = nested_cases::run();
   if (failed)
      std::fprintf(stderr, "%d nested checks did not hold\n", failed);
   // Nested values in maps and sets, and a map inside nested bytes.
   const int assoc_failed = nested_assoc_cases::run();
   if (assoc_failed)
      std::fprintf(stderr, "%d nested map / set checks did not hold\n", assoc_failed);
   return failed || assoc_failed ? 1 : 0;
}
