// Writes psio::emit_protobuf of the reflected types of protobuf_shapes.hpp
// to the file named on the command line. The build runs `protoc` on it and
// compiles protobuf_interop_tests.cpp against the generated code.

#include <psio/emit_protobuf.hpp>

#include <cstdio>
#include <fstream>

#include "protobuf_shapes.hpp"

int main(int argc, char** argv)
{
   if (argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.proto>\n", argv[0]);
      return 2;
   }
   std::ofstream out(argv[1], std::ios::binary);
   try
   {
      out << psio::emit_protobuf(protobuf_shapes_schema());
   }
   catch (const std::exception& e)
   {
      std::fprintf(stderr, "%s\n", e.what());
      return 1;
   }
   out.close();
   if (!out)
   {
      std::fprintf(stderr, "cannot write %s\n", argv[1]);
      return 1;
   }
   return 0;
}
