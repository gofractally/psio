#include "../tests/pssz_optional_fixtures.hpp"
#include <iostream>
#include <iomanip>
using namespace optional_fixture;
template<typename F,typename T> void emit(const char* name,const T& value) {
 const bool invalid=std::string_view(name)=="node_depth_65_reject";
 auto bytes=[&] { if(invalid) return invalid_depth_bytes<F::offset_width>(32);return psio::encode(F{},value); }();
 if(invalid) {auto status=psio::detail::pssz_impl::check_encode_depth<F::offset_width>(value);if(status.ok()) std::abort();}
 auto status=psio::validate<T>(F{},bytes);
 std::cout<<"{\"name\":\""<<name<<"\",\"profile\":\"pssz"<<F::offset_width*8<<"\",\"expected_accept\":"<<(status.ok()?"true":"false")<<",\"hex\":\"";
 for(unsigned char c:bytes) std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);
 std::cout<<std::dec<<"\"}\n";
}
template<typename F> void profile() {
 emit<F>("base",Base{"a"});emit<F>("extended_some",Extended{"a","b"});emit<F>("extended_none",Extended{"a",std::nullopt});
 emit<F>("twice_empty_middle",Twice{"a",std::nullopt,"b"});emit<F>("twice_pruned_last",Twice{"a",9,std::nullopt});
 emit<F>("odd_base",OddBase{true,"a"});emit<F>("odd_extended_some",OddExtended{true,"a","b"});emit<F>("odd_extended_none",OddExtended{true,"a",std::nullopt});
 emit<F>("option_record_none",std::optional<Base>{});emit<F>("option_record_some",std::optional<Base>{Base{"a"}});
 emit<F>("option_unit_none",std::optional<Unit>{});emit<F>("option_unit_some",std::optional<Unit>{Unit{}});
 using N=std::optional<std::optional<std::string>>;
 emit<F>("nested_option_none",N{});emit<F>("nested_option_some_none",N{std::in_place,std::nullopt});emit<F>("nested_option_some_empty",N{std::in_place,std::string{}});
 emit<F>("zero_argument_tuple",std::tuple<>{});emit<F>("positional_tuple",std::tuple<std::uint32_t,std::string>{7,"hi"});
 emit<F>("nested_record_vector",NewOuter{{"a","b"},{{"c","d"}}});
 emit<F>("node_depth_63",node(31));emit<F>("node_depth_64",std::optional<DepthNode>{node(31)});emit<F>("node_depth_65_reject",node(32));
}
int main() {profile<psio::pssz8>();profile<psio::pssz16>();profile<psio::pssz32>();}
