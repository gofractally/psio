// Writes psio::emit_fbs of the reflected types of flatbuf_shapes.hpp to the
// file named on the command line. The build runs `flatc` on it and compiles
// flatbuf_schema_interop_tests.cpp against the generated code.

#include <psio/emit_fbs.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>

#include "flatbuf_schema_shapes.hpp"

int main(int argc, char** argv)
{
   if (argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.fbs>\n", argv[0]);
      return 2;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << psio::emit_fbs(flatbuf_shapes_schema());
   out.close();
   if (!out)
   {
      std::fprintf(stderr, "cannot write %s\n", argv[1]);
      return 1;
   }
   return 0;
}
