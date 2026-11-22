// <psio/reflect.hpp> alone is enough for every keyword PSIO_REFLECT accepts:
// plain members, attr(field, spec), method(...), definitionWillNotChange(),
// maxFields(N), maxDynamicData(N), transparent(), invariant(fn) — in a class
// body and at namespace scope. Nothing else from psio is included here, and
// the standard headers are named explicitly (the object is compiled with
// libc++'s transitive includes removed).
#include <psio/reflect.hpp>

#include <cstdint>
#include <string>

namespace reflect_only
{
   struct Plain
   {
      std::uint32_t a;
      std::string   b;
   };
   PSIO_REFLECT(Plain, a, b)

   struct Frozen
   {
      std::uint32_t x;
      std::uint32_t y;
   };
   PSIO_REFLECT(Frozen, definitionWillNotChange(), x, y)

   struct Capped
   {
      std::uint64_t id;
      std::string   owner;
      PSIO_REFLECT(Capped, attr(id, json_string), attr(owner, max<32>), maxFields(4),
                   maxDynamicData(64))
   };

   struct Wrapper
   {
      std::uint64_t value;
      PSIO_REFLECT(Wrapper, value, transparent())
   };

   struct Even
   {
      std::uint32_t n;
      static psio::codec_status check(const Even& e)
      {
         return e.n % 2 == 0 ? psio::codec_ok() : psio::codec_fail("odd", 0, "test");
      }
      PSIO_REFLECT(Even, n, invariant(check))
   };

   struct Service
   {
      std::uint32_t total;
      void          add(std::uint32_t amount);
      std::uint32_t get() const;
      PSIO_REFLECT(Service, total, method(add, amount), method(get))
   };

   struct Everything
   {
      std::uint32_t n;
      static psio::codec_status ok(const Everything&) { return psio::codec_ok(); }
      PSIO_REFLECT(Everything, attr(n, json_string), definitionWillNotChange(), maxFields(1),
                   maxDynamicData(8), invariant(ok))
   };

   enum class Color : std::uint8_t { red, green };
   PSIO_REFLECT_ENUM(Color, red, green)
}  // namespace reflect_only

static_assert(psio::Reflected<reflect_only::Plain>);
static_assert(psio::reflect<reflect_only::Frozen>::member_count == 2);
static_assert(psio::reflect<reflect_only::Capped>::member_count == 2);
static_assert(psio::reflect<reflect_only::Wrapper>::member_count == 1);
static_assert(psio::reflect<reflect_only::Even>::member_count == 1);
static_assert(psio::Reflected<reflect_only::Service>);
static_assert(psio::Reflected<reflect_only::Everything>);
static_assert(psio::ReflectedEnum<reflect_only::Color>);
