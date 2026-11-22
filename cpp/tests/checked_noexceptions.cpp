// The status-returning entry points with exceptions and RTTI disabled: a
// failure that was a throw is an abort here, so reaching the end is the test.
#include "checked_never_throws.hpp"

#include <cstdio>

int main()
{
   const int failed = never_throws::run_checked_cases();
   if (failed)
      std::fprintf(stderr, "checked case %d did not report its status\n", failed);
   return failed;
}
