// An untagged variant whose alternatives are written as the same kind of
// JSON value cannot be read back: it must not compile.
#include <psio/json.hpp>

#include <variant>

using Ambiguous = std::variant<int, double>;  // both are numbers
namespace psio
{
   constexpr bool psio_is_untagged(const Ambiguous*) { return true; }
}

int main()
{
   return static_cast<int>(psio::to_ijson(Ambiguous{1}).size());
}
