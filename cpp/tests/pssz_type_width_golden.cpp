// Reference emitter for the unchanged explicit-width API. Compile against the
// published552ca headers to regenerate the pinned golden file.
#include "pssz_type_width_fixture.hpp"
#include <iomanip>
#include <iostream>
#include <sstream>

bool first = true;
template<typename F,typename T>
void emit(const char* name,const T& value)
{
   auto bytes=psio::encode(F{},value);
   if(!psio::validate<T>(F{},bytes).ok()) throw std::runtime_error("invalid reference bytes");
   if(psio::encode(F{},psio::decode<T>(F{},bytes))!=bytes) throw std::runtime_error("reference roundtrip failed");
   std::ostringstream hex;
   for(unsigned char byte:bytes) hex<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
   std::cout<<(first?"":",\n")<<"\""<<name<<"-"<<F::offset_width<<"\":\""<<hex.str()<<"\"";
   first=false;
}
template<typename F>
void emit_format()
{
   auto value=width_sample();
   emit<F>("relay",value);
   emit<F>("vector",value.entries);
   emit<F>("array",value.pair);
   emit<F>("some",value.latest);
   emit<F>("none",std::optional<WidthCheckpoint>{});
   emit<F>("variant-record",value.proof);
   emit<F>("variant-number",std::variant<WidthCheckpoint,std::uint32_t>{std::in_place_index<1>,42});
   emit<F>("narrow-parent",WidthNarrowEnvelope{{42},17});
}
int main()
{
   std::cout<<"{\n";
   emit_format<psio::pssz8>();
   emit_format<psio::pssz16>();
   emit_format<psio::pssz32>();
   std::cout<<"\n}\n";
}
