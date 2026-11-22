#include "schema_builder_identity_fixture.hpp"

int main()
{
   using namespace builder_identity_fixture;
   psio::SchemaBuilder builder;
   auto first=insert_from_other_translation_unit(builder);
   const auto before=builder.build();
   auto repeated=builder.insert<Nested>();
   builder.insert<Node>();
   const auto after=std::move(builder).build();
   if(first!=repeated||before!=after)return 1;
   constexpr std::array<std::string_view,8> names{"Node","@1","@2","@3","@4","@5","@6","@7"};
   if(after.types.size()!=names.size())return 4;
   for(std::size_t i=0;i<names.size();++i)if(after.types[i].name!=names[i])return 5;
   if(std::get<psio::schema_types::Type>(first.value).type!="@3")return 6;
   const auto* node=after.get("Node");
   if(!node)return 2;
   const auto& object=std::get<psio::schema_types::Object>(node->value);
   const auto& list=std::get<psio::schema_types::List>(object.members[1].type->resolve(after)->value);
   if(list.type->resolve(after)!=node)return 3;
   return 0;
}
