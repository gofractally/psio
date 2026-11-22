// owned_error_cases.hpp with exceptions and RTTI disabled.
#include "owned_error_cases.hpp"

int main()
{
   const int failed = owned_error_cases::run();
   if (failed)
      std::fprintf(stderr, "%d owned-error checks did not hold\n", failed);
   return failed ? 1 : 0;
}
