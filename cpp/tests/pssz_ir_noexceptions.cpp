#include <psio/pssz_ir.hpp>

int main()
{
   namespace ir = psio::schema_types;
   namespace runtime = psio::pssz_ir;
   ir::Schema schema;
   ir::AnyType root = ir::Object{{ir::Member{"value", ir::AnyType{ir::Int{32, false}}, {}}}, {}};
   runtime::Value value = runtime::Record{{"value", runtime::Unsigned{7}}}, decoded;
   std::vector<char> bytes;
   if (!runtime::encode<4>(schema, root, value, bytes).ok())
      return 1;
   if (!runtime::decode_exact<4>(schema, root, bytes, decoded).ok() || decoded != value)
      return 2;
   const auto previous = bytes;
   if (runtime::encode<4>(schema, ir::Int{8, false}, runtime::Unsigned{256}, bytes).ok())
      return 3;
   if (bytes != previous)
      return 4;
   return 0;
}
