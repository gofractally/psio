#pragma once
// The Schema IR of the types of flatbuf_shapes.hpp, as the schema builder
// makes it from their reflection: what fbs_schema_emit.cpp writes as `.fbs`
// text for flatc (flatbuf_schema_interop_tests.cpp exchanges buffers with
// the code generated from it) and fbs_schema_tests.cpp compares layouts of.

#include <psio/schema_builder.hpp>

#include "flatbuf_shapes.hpp"

inline psio::Schema flatbuf_shapes_schema()
{
   psio::SchemaBuilder b;
   b.insert<Order>("Order");
   b.insert<Tree>("Tree");
   b.insert<Scalars>("Scalars");
   b.insert<Defaults>("Defaults");
   b.insert<Renumbered>("Renumbered");
   b.insert<WithDeprecated>("WithDeprecated");
   b.insert<Sparse>("Sparse");
   b.insert<Point>("Point");
   b.insert<Numbers>("Numbers");
   b.insert<Basket>("Basket");
   b.insert<Longs>("Longs");
   auto s         = b.build();
   s.package.name = "fbe";
   s.package.attributes.push_back({"fbs_file_identifier", "ORDR"});
   s.package.attributes.push_back({"fbs_root_type", "Order"});
   return s;
}
