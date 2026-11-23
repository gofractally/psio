// Must not compile: schemas psio::graphql rejects, each with a message that
// names the reason (see CMakeLists.txt for the expected diagnostics).
#include <psio/graphql.hpp>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace a
{
   struct Thing
   {
      int x = 0;
   };
   PSIO_REFLECT(Thing, x)
}  // namespace a
namespace b
{
   struct Thing
   {
      int y = 0;
   };
   PSIO_REFLECT(Thing, y)
}  // namespace b

struct ThingInput
{
   int z = 0;
};
PSIO_REFLECT(ThingInput, z)

struct Empty
{
   int hidden_member = 0;
};
PSIO_REFLECT(Empty, attr(hidden_member, psio::graphql::hidden))

// A class template with a non-type argument cannot be taken apart: its
// instantiations share the reflected name unless type_name<T> names them.
// (Two that have the same fields would be one GraphQL type.)
template <bool Wide>
struct Sized
{
   std::conditional_t<Wide, std::string, int> v{};
   PSIO_REFLECT(Sized, v)
};
// A template argument that has no GraphQL name.
struct Tag
{
};
template <typename T>
struct Tagged
{
   int v = 0;
   PSIO_REFLECT(Tagged, v)
};
struct Part
{
   int ok   = 0;
   int more = 0;
   PSIO_REFLECT(Part, ok, more)
};
struct Query
{
   int ok = 0;
#if defined(GQL_FAIL_TYPE_NAME)
   a::Thing one;
   b::Thing two;
   PSIO_REFLECT(Query, ok, one, two)
#elif defined(GQL_FAIL_INPUT_NAME)
   // The input object of a::Thing is named ThingInput too.
   ThingInput out;
   int        take(a::Thing thing) const { return thing.x; }
   PSIO_REFLECT(Query, ok, out, method(take, thing))
#elif defined(GQL_FAIL_MEMBER_NAME)
   int other = 0;
   PSIO_REFLECT(Query, ok, attr(other, psio::graphql::name_as{"ok"}))
#elif defined(GQL_FAIL_UNMAPPED)
   // (A std::map is a List of its entries; a type that is not reflected has
   // no mapping.)
   struct Unreflected
   {
      int x = 0;
   };
   Unreflected table;
   PSIO_REFLECT(Query, ok, table)
#elif defined(GQL_FAIL_UNION)
   std::variant<int, std::string> either;
   PSIO_REFLECT(Query, ok, either)
#elif defined(GQL_FAIL_PARAM_NAMES)
   int add(int a, int b) const { return a + b; }
   PSIO_REFLECT(Query, ok, method(add))
#elif defined(GQL_FAIL_ARGUMENT)
   int count(std::map<std::string, int> table) const { return static_cast<int>(table.size()); }
   PSIO_REFLECT(Query, ok, method(count, table))
#elif defined(GQL_FAIL_OPTIONAL)
   std::optional<std::optional<int>> twice;
   PSIO_REFLECT(Query, ok, twice)
#elif defined(GQL_FAIL_ID)
   a::Thing thing;
   PSIO_REFLECT(Query, ok, attr(thing, psio::graphql::id))
#elif defined(GQL_FAIL_EMPTY)
   Empty empty;
   PSIO_REFLECT(Query, ok, empty)
#elif defined(GQL_FAIL_TEMPLATE_NAME)
   Sized<false> one;
   Sized<true>  two;
   PSIO_REFLECT(Query, ok, one, two)
#elif defined(GQL_FAIL_TEMPLATE_ARG)
   Tagged<Tag> tagged;
   PSIO_REFLECT(Query, ok, tagged)
#elif defined(GQL_FAIL_FLATTEN_NAME)
   // Part has a member named `ok` too.
   Part part;
   PSIO_REFLECT(Query, ok, attr(part, psio::graphql::flatten))
#elif defined(GQL_FAIL_FLATTEN_SCALAR)
   std::optional<Part> part;
   PSIO_REFLECT(Query, ok, attr(part, psio::graphql::flatten))
#elif defined(GQL_FAIL_ARGUMENT_NAME)
   int rows(int first, psio::graphql::page_args page) const { return first + page.first.value_or(0); }
   PSIO_REFLECT(Query, ok, method(rows, first, page))
#else
   PSIO_REFLECT(Query, ok)
#endif
};

int main()
{
   return static_cast<int>(psio::graphql::schema<Query>::sdl().size());
}
