// psio::pssz_ir::equivalent / evolves_from with exceptions and RTTI off: a
// wasm guest compares the schema a service publishes with the installed one.
#include <psio/pssz_ir_compare.hpp>
#include <psio/schema_builder.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace v1
{
   struct Row
   {
      std::uint64_t id = 0;
      std::string   owner;
      PSIO_REFLECT(Row, id, owner, maxDynamicData(1000))
   };
   struct Svc
   {
      void put(std::uint64_t id, std::string owner);
   };
   PSIO_REFLECT(Svc, method(put, id, owner, maxDynamicData(1000)))
   PSIO_INTERFACE(Svc, name("svc"))
}
namespace v2
{
   struct Row
   {
      std::uint64_t                id = 0;
      std::string                  owner;
      std::optional<std::uint32_t> flags;
      PSIO_REFLECT(Row, id, owner, flags, maxDynamicData(1000))
   };
   struct Svc
   {
      void put(std::uint64_t id, std::string owner, std::optional<std::uint32_t> flags);
   };
   PSIO_REFLECT(Svc, method(put, id, owner, flags, maxDynamicData(1000)))
   PSIO_INTERFACE(Svc, name("svc"))
}

int main()
{
   namespace ir = psio::pssz_ir;
   const auto older = psio::SchemaBuilder{}.insert<v1::Row>("Row").insert_interface<v1::Svc>().build();
   const auto newer = psio::SchemaBuilder{}.insert<v2::Row>("Row").insert_interface<v2::Svc>().build();
   if (!ir::evolves_from(older, "Row", newer, "Row"))
      return 1;
   if (ir::equivalent(older, "Row", newer, "Row"))
      return 2;
   const auto back = ir::evolves_from(newer, "Row", older, "Row");
   if (back.ok || back.code != "fields-removed" || back.path != "(root)" || back.reason.empty())
      return 3;
   if (back.status().ok() || back.status().error().what.find("fields were removed") == std::string_view::npos)
      return 4;
   if (!ir::evolves_from(older, newer) || !ir::equivalent(older, older))
      return 5;
   const auto whole = ir::evolves_from(newer, older);
   if (whole.ok || whole.path != "svc/put(args)")
      return 6;
   // A malformed schema is a result, not an abort.
   const auto missing = ir::equivalent(older, "Row", newer, "NoSuchType");
   if (missing.ok || missing.code != "malformed-schema")
      return 7;
   std::puts("pssz_ir_compare noexceptions ok");
   return 0;
}
