// MUST NOT COMPILE: two members of one type have the same key under the
// caller-supplied hash. ctest runs the compiler on this file and requires
// the diagnostic of the static_assert in name_lookup.hpp.

#include <psio/reflect.hpp>

#include <cstdint>
#include <string_view>

struct Colliding
{
   int ab = 0;
   int cd = 0;
};
PSIO_REFLECT(Colliding, ab, cd)

struct LengthHash
{
   constexpr std::uint64_t operator()(std::string_view s) const noexcept { return s.size(); }
};

int main()
{
   return psio::reflect<Colliding>::index_of_hash<LengthHash>(2).has_value() ? 0 : 1;
}
