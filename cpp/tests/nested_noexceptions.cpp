// psio::nested<T> with exceptions and RTTI disabled (how a WebAssembly guest
// builds it): every checked entry point reports through its status, so a
// failure that was a throw would be an abort here. Reaching the end is the
// test.
#include "nested_cases.hpp"

#include <cstdio>

int main()
{
   const int failed = nested_cases::run();
   if (failed)
      std::fprintf(stderr, "%d nested checks did not hold\n", failed);
   return failed ? 1 : 0;
}
