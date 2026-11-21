// Include this header first, without relying on other format headers to supply
// the standard containers used by its transitive DWNC annotation machinery.
#include <psio/annotate.hpp>

static_assert(psio::all_dwnc_v<std::vector<std::variant<int, bool>>>);
