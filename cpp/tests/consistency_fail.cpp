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

namespace cons_fail
{
   // maxFields(N) is the most fields the record (parameters the method) has.
   struct Fields
   {
      std::uint32_t a = 0, b = 0, c = 0;
   };
#if defined(CONS_FAIL_FIELDS_OVER_CAP)
   PSIO_REFLECT(Fields, a, b, c, maxFields(2))
#elif defined(CONS_FAIL_FIELDS_CAP_OVER_255)
   PSIO_REFLECT(Fields, a, b, c, maxFields(256))
#else
   PSIO_REFLECT(Fields, a, b, c, maxFields(3))
#endif
   inline const auto fields_bytes = psio::to_pssz(Fields{});

   struct Calls
   {
      void put(std::uint32_t id, std::string text);
#if defined(CONS_FAIL_PARAMS_OVER_CAP)
      PSIO_REFLECT(Calls, method(put, id, text, maxFields(1)))
#else
      PSIO_REFLECT(Calls, method(put, id, text, maxFields(2)))
#endif
   };
   inline const auto calls_bytes =
      psio::to_pssz(psio::reflect<Calls>::method_arg_tuple<0>{std::tuple{std::uint32_t{1}, std::string{}}});
}  // namespace cons_fail

namespace cons_fail
{
   // A transparent member takes only what makes it a bounded type.
   struct Memo
   {
      std::string text;
#if defined(CONS_FAIL_TRANSPARENT_EXACT)
      PSIO_REFLECT(Memo, transparent(), attr(text, exact<8>))
#elif defined(CONS_FAIL_TRANSPARENT_MIN)
      PSIO_REFLECT(Memo, transparent(), attr(text, min<1> | max<8>))
#elif defined(CONS_FAIL_TRANSPARENT_SORTED)
      PSIO_REFLECT(Memo, transparent(), attr(text, utf8 | json_string))
#else
      PSIO_REFLECT(Memo, transparent(), attr(text, utf8 | max<80>))
#endif
   };
   inline const auto memo_bytes = psio::to_pssz(Memo{"hi"});
   struct Code
   {
      std::uint64_t value = 0;
#if defined(CONS_FAIL_TRANSPARENT_MAX_ON_INT)
      PSIO_REFLECT(Code, transparent(), attr(value, max<8>))
#else
      PSIO_REFLECT(Code, transparent(), value)
#endif
   };
   inline const auto code_bytes = psio::to_pssz(Code{});
}  // namespace cons_fail

int main() {}
