// The status-returning entry points with exceptions and RTTI disabled: a
// failure that was a throw is an abort here, so reaching the end is the test.
#include "assoc_cases.hpp"
#include "checked_never_throws.hpp"

#include <cstdio>

int main()
{
   const int failed = never_throws::run_checked_cases();
   if (failed)
      std::fprintf(stderr, "checked case %d did not report its status\n", failed);
   // Pairs, maps and sets: order and uniqueness failures are statuses too.
   const int assoc_failed = assoc_cases::run_assoc_cases();
   if (assoc_failed)
      std::fprintf(stderr, "assoc case %d did not report its status\n", assoc_failed);
   return failed ? failed : assoc_failed;
}
