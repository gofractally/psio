// Must not compile: a name listed as a field and as a method.
#include <psio/reflect.hpp>
struct both
{
   int value;
};
PSIO_REFLECT(both, value, method(value))
static_assert(psio::reflect<both>::is_reflected);
