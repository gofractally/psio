#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <iomanip>
#include <iostream>

struct Small { std::string label; };
PSIO_REFLECT(Small, label)
PSIO_FIELD_ATTRS(Small, label, psio::length_bound{.max=16})
struct Parent { Small small; std::string tail; };
PSIO_REFLECT(Parent, small, tail)

void hex(std::span<const char> value)
{
   for (unsigned char c : value)
      std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
   std::cout << std::dec;
}
int main()
{
   static_assert(psio::auto_pssz_width_v<Small> == 1);
   static_assert(psio::auto_pssz_width_v<Parent> == 4);
   auto standalone = psio::encode(psio::pssz{}, Small{"hi"});
   auto parent = psio::encode(psio::pssz{}, Parent{{"hi"}, "tail"});
   psio::view<Parent,psio::pssz,psio::storage::const_borrow> root{parent};
   auto child = root.get<0>();
   std::cout << "Small width=" << psio::auto_pssz_width_v<Small> << " Parent width=" << psio::auto_pssz_width_v<Parent> << '\n';
   std::cout << "standalone=";hex(standalone);
   std::cout << " nested=";hex(psio::bytes(child));std::cout << '\n';
   std::cout << "nested validates as canonical Small=" << psio::validate<Small>(psio::pssz{},psio::bytes(child)).ok() << '\n';
   std::cout << "parent self roundtrip=" << (psio::decode<Parent>(psio::pssz{},parent).small.label=="hi") << '\n';
   return psio::bytes(child).size()==standalone.size() && std::equal(standalone.begin(),standalone.end(),psio::bytes(child).begin()) ? 0 : 1;
}
