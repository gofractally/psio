// Declarations the consistency rules refuse at compile time. Each case is
// compiled alone (-DCONS_FAIL_...) and must fail with its message; with no
// case defined the file compiles (the control).
#include <psio/annotate.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cons_fail
{
   struct Bounds
   {
      std::string name;
   };
#if defined(CONS_FAIL_MAX_TWICE)
   PSIO_REFLECT(Bounds, attr(name, max<8> | max<16>))
#elif defined(CONS_FAIL_MIN_ABOVE_MAX)
   PSIO_REFLECT(Bounds, attr(name, min<9> | max<8>))
#else
   PSIO_REFLECT(Bounds, attr(name, min<2> | max<8>))
#endif
   inline const auto bounds_bytes = psio::to_pssz(Bounds{"abc"});
}  // namespace cons_fail

int main() {}
