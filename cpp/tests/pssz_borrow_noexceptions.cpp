// pssz_borrow_cases.hpp with exceptions and RTTI disabled
// (PSIO_EXCEPTIONS_ENABLED=0).

#include "pssz_borrow_cases.hpp"

#if defined(__cpp_exceptions)
#error "this file is built with -fno-exceptions"
#endif

int main()
{
   const int line = borrow_cases::run();
   if (line == 0) std::puts("pssz-borrow-noexceptions=PASS");
   return line == 0 ? 0 : 1;
}
