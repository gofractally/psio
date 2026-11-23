// MUST NOT COMPILE (except with no VC_FAIL_* macro, the control).
//
// A view converts implicitly to its value only when the value is trivially
// copyable and of fixed encoded size, and to a borrowed span / string_view
// only for a byte sequence. Everything else is written: unpack().

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct Point
{
   std::int32_t x = 0;
   std::int32_t y = 0;
};
PSIO_REFLECT(Point, definitionWillNotChange(), x, y)
// Extensible: it has a header and may grow.
struct Open
{
   std::int32_t x = 0;
};
PSIO_REFLECT(Open, x)
// definitionWillNotChange, but with a variable-size member.
struct Named
{
   std::uint32_t id = 0;
   std::string   name;
};
PSIO_REFLECT(Named, definitionWillNotChange(), id, name)
// Fixed size, but not trivially copyable.
struct Counted
{
   std::uint32_t v = 0;
   Counted()       = default;
   Counted(const Counted& o) : v(o.v) {}
   Counted& operator=(const Counted&) = default;
};
PSIO_REFLECT(Counted, definitionWillNotChange(), v)

struct Row
{
   Point                        point;
   Open                         open;
   Named                        named;
   Counted                      counted;
   std::string                  text;
   std::vector<std::uint32_t>   numbers;
   std::vector<Point>           points;
   std::optional<std::uint32_t> maybe;
   std::array<std::uint32_t, 2> pair{};
   std::vector<unsigned char>   octets;
};
PSIO_REFLECT(Row, point, open, named, counted, text, numbers, points, maybe, pair, octets)

int main()
{
   auto                 bytes = psio::to_pssz(Row{});
   psio::pssz_view<Row> v{bytes};
   Point                          ok  = v.point();
   std::array<std::uint32_t, 2>   ok2 = v.pair();
   std::span<const unsigned char> ok3 = v.octets();
#if defined(VC_FAIL_EXTENSIBLE_RECORD)
   Open bad = v.open();
#elif defined(VC_FAIL_VARIABLE_RECORD)
   Named bad = v.named();
#elif defined(VC_FAIL_NOT_TRIVIAL)
   Counted bad = v.counted();
#elif defined(VC_FAIL_STRING)
   std::string bad = v.text();
#elif defined(VC_FAIL_VECTOR)
   std::vector<std::uint32_t> bad = v.numbers();
#elif defined(VC_FAIL_RECORD_VECTOR)
   std::vector<Point> bad = v.points();
#elif defined(VC_FAIL_OPTIONAL)
   std::optional<std::uint32_t> bad = v.maybe();
#elif defined(VC_FAIL_WIDE_SPAN)
   std::span<const std::uint32_t> bad = v.numbers();
#elif defined(VC_FAIL_WIDE_ARRAY_SPAN)
   std::span<const std::uint32_t> bad = v.pair();
#elif defined(VC_FAIL_OTHER_BYTE_SPAN)
   std::span<const char> bad = v.octets();
#elif defined(VC_FAIL_ROOT)
   Row bad = v;
#endif
   return int(ok.x + ok2[0] + ok3.size());
}
