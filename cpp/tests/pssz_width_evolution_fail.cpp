// MUST NOT COMPILE (except with no WE_FAIL_* macro, the control).
//
// psio::require_pssz_evolves_from<Old, New>() says why a pair does not
// evolve. When the automatic offset width of the type moved — an unbounded
// optional appended to a type that has no cap — it says so and names the
// remedy, maxDynamicData.

#include <psio/pssz.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

struct Open
{
   std::int32_t socket = 0;
};
PSIO_REFLECT(Open, socket)
struct OpenGrown
{
   std::int32_t                             socket = 0;
   std::optional<std::vector<std::uint64_t>> prev;
};
PSIO_REFLECT(OpenGrown, socket, prev)
struct Row
{
   std::int32_t socket = 0;
   PSIO_REFLECT(Row, socket, maxDynamicData(65536))
};
struct RowGrown
{
   std::int32_t                             socket = 0;
   std::optional<std::vector<std::uint64_t>> prev;
   PSIO_REFLECT(RowGrown, socket, prev, maxDynamicData(65536))
};
struct RowRequired
{
   std::int32_t  socket = 0;
   std::uint64_t owner  = 0;
   PSIO_REFLECT(RowRequired, socket, owner, maxDynamicData(65536))
};

int main()
{
   psio::require_pssz_evolves_from<Row, RowGrown>();
#if defined(WE_FAIL_RECORD_WIDTH)
   psio::require_pssz_evolves_from<Open, OpenGrown>();
#elif defined(WE_FAIL_TUPLE_WIDTH)
   psio::require_pssz_evolves_from<std::tuple<std::int32_t>,
                                   std::tuple<std::int32_t, std::optional<std::string>>>();
#elif defined(WE_FAIL_OTHER_REASON)
   psio::require_pssz_evolves_from<Row, RowRequired>();
#endif
   return 0;
}
