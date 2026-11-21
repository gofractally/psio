#include <psio/pssz.hpp>
#include <array>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

struct Unit {};
PSIO_REFLECT(Unit)
PSIO_TYPE_ATTRS(Unit, psio::definition_will_not_change{})

int main(int argc, char** argv)
{
   const auto mode = argc > 1 ? std::string_view{argv[1]} : "optional";
   const std::array<char,1> malformed{'x'};
   if(mode=="optional")
   {
      const auto none=psio::encode(psio::pssz{},std::optional<Unit>{});
      const auto some=psio::encode(psio::pssz{},std::optional<Unit>{Unit{}});
      const auto back=psio::decode<std::optional<Unit>>(psio::pssz{},some);
      std::cout<<"none_size="<<none.size()<<" some_size="<<some.size()<<" distinct="<<(none!=some)<<" decoded_some="<<back.has_value()<<'\n';
      return none!=some && back.has_value() ? 0 : 1;
   }
   if(mode=="vector-encode")
   {
      try {
         const auto wire=psio::encode(psio::pssz{},std::vector<Unit>(3));
         const auto back=psio::decode<std::vector<Unit>>(psio::pssz{},wire);
         std::cout<<"wire_size="<<wire.size()<<" decoded_count="<<back.size()<<'\n';
         return 1;
      } catch(const psio::codec_exception&) { return 0; }
   }
   if(mode=="vector-validate")
      return psio::validate<std::vector<Unit>>(psio::pssz{},malformed).ok() ? 1 : 0;
   if(mode=="vector-decode")
   {
      try { (void)psio::decode<std::vector<Unit>>(psio::pssz{},malformed); return 1; }
      catch(const psio::codec_exception&) { return 0; }
   }
   if(mode=="array-trailing")
      return psio::validate<std::array<Unit,3>>(psio::pssz{},malformed).ok() ? 1 : 0;
   return 2;
}
