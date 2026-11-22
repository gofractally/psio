// Actual C++ schema_ir.hpp wire fixtures; Rust mirrors these same typed values.
#include <psio/pssz.hpp>
#include <psio/schema_ir.hpp>
#include <iostream>
#include <string>
#include <vector>
using namespace psio::schema_types;

std::string hex(const std::vector<char>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char b : bytes) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
template <class T> void emit(const char* name, const T& value) {
    std::cout << "{\"profile\":\"pssz32\",\"name\":\"" << name
              << "\",\"hex\":\"" << hex(psio::encode(psio::pssz32{},value)) << "\"}\n";
}
int main() {
    auto integer = [](unsigned bits) { return AnyType{Int{bits,false}}; };
    auto member = [&](std::string name) { return Member{std::move(name),Box<AnyType>{integer(32)},{Attribute{"visible",std::nullopt}}}; };
    emit("Struct",AnyType{Struct{{member("x")},{Attribute{"definitionWillNotChange",std::string{}}}}});
    emit("Object",AnyType{Object{{member("x")},{Attribute{"doc",std::string{"example"}}}}});
    emit("Array",AnyType{Array{Box<AnyType>{integer(8)},3}});
    emit("List",AnyType{List{Box<AnyType>{integer(32)}}});
    emit("BoundedList",AnyType{BoundedList{Box<AnyType>{integer(16)},3}});
    emit("Option",AnyType{Option{Box<AnyType>{integer(64)}}});
    emit("Variant",AnyType{Variant{{member("Left"),member("Right")},{}}});
    emit("Tuple",AnyType{Tuple{{Box<AnyType>{integer(16)},Box<AnyType>{integer(32)}}}});
    emit("Int",integer(32));
    emit("Float",AnyType{Float{8,24}});
    emit("FracPack",AnyType{FracPack{Box<AnyType>{integer(8)}}});
    emit("Custom",AnyType{Custom{Box<AnyType>{integer(64)},"account"}});
    emit("Resource",AnyType{Resource{"handle",{Func{"run",{member("value")},std::nullopt,{}}},{Attribute{"capability",std::nullopt}}}});
    emit("Type",AnyType{Type{"named"}});
    Schema schema;
    schema.package = Package{"abi","1",{Attribute{"stable",std::nullopt}}};
    schema.interfaces = {Interface{"api",{"named"},{Func{"zero",{},std::nullopt,{}}},{}}};
    schema.worlds = {World{"world",{UseRef{"base","common"}},{"api"},{}}};
    schema.uses = {Use{"base","common","1",{UseItem{"type",std::string{"alias"}}}}};
    schema.types = {Member{"named",Box<AnyType>{Object{{member("x")},{}}},{Attribute{"doc",std::string{"named type"}}}}};
    emit("Schema",schema);
}
