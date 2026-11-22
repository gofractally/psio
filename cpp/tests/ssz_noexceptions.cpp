// ssz.hpp without exceptions: the header compiles and its status-returning
// entry points work when PSIO_EXCEPTIONS_ENABLED is 0 or not defined at all
// (the fail-fast paths abort instead of throwing, as in the other format
// headers). Built as an executable with -fno-exceptions, and compiled a
// second time with the macro undefined (tests/CMakeLists.txt).

#include <psio/dynamic_ssz.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>

#include <cstdint>
#include <span>
#include <vector>

struct Leaf
{
   std::uint32_t              id = 0;
   std::vector<std::uint16_t> words;
   PSIO_REFLECT(Leaf, id, words)
};
struct Tree
{
   std::uint64_t     root = 0;
   std::vector<Leaf> leaves;
   PSIO_REFLECT(Tree, root, leaves)
};

int main()
{
   const Tree tree{7, {Leaf{1, {2, 3}}, Leaf{4, {}}}};
   const auto bytes = psio::encode(psio::ssz{}, tree);
   if (!psio::validate<Tree>(psio::ssz{}, std::span<const char>{bytes}).ok())
      return 1;
   if (!psio::validate_strict<Tree>(psio::ssz{}, std::span<const char>{bytes}).ok())
      return 2;
   const auto back = psio::decode<Tree>(psio::ssz{}, std::span<const char>{bytes});
   if (back.root != 7 || back.leaves.size() != 2 || back.leaves[0].words.size() != 2)
      return 3;
   // A truncated frame is a status, not a throw or an abort.
   const std::span<const char> cut{bytes.data(), bytes.size() - 1};
   if (psio::validate<Tree>(psio::ssz{}, cut).ok())
      return 4;
   if (psio::validate_strict<Tree>(psio::ssz{}, cut).ok())
      return 5;
   return 0;
}
