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

namespace cons_fail
{
   // A custom type cannot take the id of one of PSIO's own types.
   struct Stamp
   {
      std::int64_t seconds = 0;
   };
   PSIO_REFLECT(Stamp, definitionWillNotChange(), seconds)
   inline psio::codec_status stamp_ok(const Stamp&) noexcept { return psio::codec_ok(); }
   struct duration   // the reflected name is the default id
   {
      std::int64_t ticks = 0;
   };
   PSIO_REFLECT(duration, definitionWillNotChange(), ticks)
   inline psio::codec_status duration_ok(const duration&) noexcept { return psio::codec_ok(); }
}  // namespace cons_fail
#if defined(CONS_FAIL_RESERVED_INVARIANT_ID)
PSIO_INVARIANT(cons_fail::Stamp, cons_fail::stamp_ok, "time-point-seconds")
#else
PSIO_INVARIANT(cons_fail::Stamp, cons_fail::stamp_ok, "Stamp")
#endif
#if defined(CONS_FAIL_RESERVED_REFLECTED_NAME)
PSIO_INVARIANT(cons_fail::duration, cons_fail::duration_ok)
#else
PSIO_INVARIANT(cons_fail::duration, cons_fail::duration_ok, "Span")
#endif
namespace cons_fail
{
   inline const auto stamp_schema    = psio::SchemaBuilder{}.insert<Stamp>();
   inline const auto duration_schema = psio::SchemaBuilder{}.insert<duration>();
}  // namespace cons_fail

int main() {}
