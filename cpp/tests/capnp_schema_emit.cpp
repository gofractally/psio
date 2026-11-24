// Writes psio::emit_capnp of the reflected types of capnp_shapes.hpp to the
// file named on the command line. The build runs `capnp compile` on it and
// compiles capnp_schema_interop_tests.cpp against the generated code.

#include <psio/emit_capnp.hpp>

#include <cstdio>
#include <fstream>

#include "capnp_shapes.hpp"

int main(int argc, char** argv)
{
   if (argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.capnp>\n", argv[0]);
      return 2;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << psio::emit_capnp(capnp_shapes_schema());
   out.close();
   if (!out)
   {
      std::fprintf(stderr, "cannot write %s\n", argv[1]);
      return 1;
   }
   return 0;
}
