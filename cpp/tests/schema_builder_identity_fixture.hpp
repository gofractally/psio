#pragma once
#include <psio/schema_builder.hpp>

namespace builder_identity_fixture
{
   struct Node
   {
      std::uint32_t value=0;
      std::vector<Node> children;
   };
   PSIO_REFLECT(Node,value,children)
   using Nested=std::tuple<std::uint32_t,std::vector<std::optional<std::string>>>;
   psio::schema_types::AnyType insert_from_other_translation_unit(psio::SchemaBuilder&);
}
