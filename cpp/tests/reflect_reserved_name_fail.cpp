// Must not compile: a member named like one of the proxy's own members.
#include <psio/reflect.hpp>
struct clash
{
   int psio_get_proxy;
};
PSIO_REFLECT(clash, psio_get_proxy)
static_assert(psio::reflect<clash>::is_reflected);
