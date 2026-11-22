// Must not compile: uses of psio::nested<T> that have no meaning, each with
// a message naming the reason (see CMakeLists.txt for the expected
// diagnostics).
#include <psio/graphql.hpp>
#include <psio/nested.hpp>
#include <psio/ssz.hpp>

#include <cstdint>
#include <string>

struct Inner
{
   std::uint32_t id = 0;
   std::string   name;
};
PSIO_REFLECT(Inner, id, name)

#if defined(NESTED_FAIL_CV)
// The bytes are the encoding of a value type.
psio::nested<const Inner> bad;
#elif defined(NESTED_FAIL_GRAPHQL_DEFERRED)
// A deferred nested value is opaque bytes to an enclosing reader: GraphQL
// cannot resolve it as its inner type.
struct Query
{
   int                                                     ok = 0;
   psio::nested<Inner, psio::nested_validation::deferred> later;
};
PSIO_REFLECT(Query, ok, later)
using api = psio::graphql::schema<Query>;
auto use  = &api::sdl;
#elif defined(NESTED_FAIL_GRAPHQL_ARGUMENT)
// A nested value is an output-only GraphQL shape.
struct Query
{
   int ok = 0;
   int take(psio::nested<Inner> packed) const { return static_cast<int>(packed.size()); }
};
PSIO_REFLECT(Query, ok, method(take, packed))
using api = psio::graphql::schema<Query>;
auto use  = &api::sdl;
#elif defined(NESTED_FAIL_OTHER_FORMAT)
// Outside the core formats a nested value stays an unsupported type.
auto bytes = psio::encode(psio::ssz{}, psio::nested<Inner>{});
#endif

int main() {}
