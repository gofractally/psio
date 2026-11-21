#include <psio/pssz_ir.hpp>
#include <psio/pssz.hpp>

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
   psio::bounded<unsigned,2> bounded;
   bounded.push_back(1);bounded.push_back(2);
   psio::utf8_string<4> string{std::string("text")};
   psio::bitlist<8> bits;bits.push_back(true);bits.push_back(false);
   auto packed=psio::encode(psio::pssz32{},bits);
   psio::bitlist<8> unpacked;bool unknown=false;
   if(!psio::decode_pssz_checked(psio::pssz32{},packed,unpacked,unknown).ok())return 5;
   if(unknown||unpacked.size()!=2||!unpacked.test(0)||unpacked.test(1))return 6;
   if(bounded.size()!=2||string.size()!=4)return 7;
   return 0;
}
