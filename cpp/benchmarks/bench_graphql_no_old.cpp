// Stand-in for bench_graphql_old.cpp when the old engine's sources are not
// available: bench_graphql then reports the new engine only.
#include "graphql_shapes.hpp"

namespace gqlbench
{
   bool        old_available() { return false; }
   void*       old_root(const Data&) { return nullptr; }
   std::string old_run(void*, std::string_view) { return {}; }
}  // namespace gqlbench
