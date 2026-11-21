#include "schema_builder_identity_fixture.hpp"

psio::schema_types::AnyType builder_identity_fixture::insert_from_other_translation_unit(psio::SchemaBuilder& builder)
{
   builder.insert<Node>();
   return builder.insert<Nested>();
}
