#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <iostream>
#include <optional>
#include <string>

struct V1 { std::string name; };
PSIO_REFLECT(V1,name)
struct V2String { std::string name; std::string extra; };
PSIO_REFLECT(V2String,name,extra)
struct V2Fixed { std::string name; std::uint32_t extra; };
PSIO_REFLECT(V2Fixed,name,extra)
struct V2Optional { std::string name; std::optional<std::string> extra; };
PSIO_REFLECT(V2Optional,name,extra)
std::string hex(std::span<const char> bytes) {
    constexpr char digits[]="0123456789abcdef"; std::string result;
    for(unsigned char b:bytes){result+=digits[b>>4];result+=digits[b&15];}return result;
}
template<class T> bool valid(std::span<const char> bytes) {
    return psio::validate<T>(psio::pssz32{},bytes).ok();
}
template<class T> void emit(const char* name,const T& value) {
    const auto bytes=psio::encode(psio::pssz32{},value);
    const std::span<const char> span{bytes};
    // Every writer retains name's slot, and its offset lies in this known-valid
    // writer buffer. Reading the V1 raw-string suffix is in bounds here, even
    // when its semantic boundary is wrong. Never invoke a larger reader's
    // unchecked decoder after failed validation.
    const auto old=psio::decode<V1>(psio::pssz32{},span);
    std::cout<<"{\"case\":\""<<name<<"\",\"format\":\"maintained-cpp-pssz32\",\"hex\":\""<<hex(span)
        <<"\",\"size\":"<<bytes.size()<<",\"unchecked_v1_name_hex\":\""<<hex(old.name)
        <<"\",\"validate_v1\":"<<(valid<V1>(span)?"true":"false")
        <<",\"validate_v2_string\":"<<(valid<V2String>(span)?"true":"false")
        <<",\"validate_v2_fixed\":"<<(valid<V2Fixed>(span)?"true":"false")
        <<",\"validate_v2_optional\":"<<(valid<V2Optional>(span)?"true":"false")<<"}\n";
}
int main(){
    emit("v1",V1{"a"});
    emit("v2_string",V2String{"a","b"});
    emit("v2_string_default",V2String{"a",""});
    emit("v2_fixed",V2Fixed{"a",7});
    emit("v2_fixed_ambiguous",V2Fixed{"ab",9});
    emit("v2_fixed_default",V2Fixed{"a",0});
    emit("v2_optional_some",V2Optional{"a",std::string{"b"}});
    emit("v2_optional_none",V2Optional{"a",std::nullopt});
}
