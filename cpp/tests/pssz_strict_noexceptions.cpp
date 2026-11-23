// pssz_strict_cases.hpp with exceptions and RTTI disabled
// (PSIO_EXCEPTIONS_ENABLED=0): psio::validate_strict returns the same
// statuses a native build returns, for records with adapters, invariants,
// transparent types, bounds and nested values.

#include "pssz_strict_cases.hpp"

#if defined(__cpp_exceptions)
#error "this file is built with -fno-exceptions"
#endif

int main()
{
   const int line = strict_cases::run();
   if (line == 0) std::puts("pssz-strict-noexceptions=PASS");
   return line == 0 ? 0 : 1;
}
