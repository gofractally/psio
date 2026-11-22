// Shared PSSZ ABI inputs. Build against the exact headers being qualified.
// Define PSIO_ABI_HAS_TUPLES once the C++ std::tuple implementation is present.
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/dynamic_pssz.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/schema.hpp>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

struct AbiEmpty {};
PSIO_REFLECT(AbiEmpty)
struct AbiUnit {};
PSIO_REFLECT(AbiUnit)
PSIO_TYPE_ATTRS(AbiUnit, psio::definition_will_not_change{})
struct AbiFixed { std::uint32_t id; };
PSIO_REFLECT(AbiFixed, id, definitionWillNotChange())
struct AbiScalars { bool flag; std::uint32_t id; };
PSIO_REFLECT(AbiScalars, flag, id)
struct AbiNested {
    AbiScalars child;
    std::optional<AbiScalars> optional;
    std::vector<std::string> labels;
};
PSIO_REFLECT(AbiNested, child, optional, labels)
struct AbiNamed { std::uint16_t code; std::string text; };
PSIO_REFLECT(AbiNamed, code, text)
using AbiVariant = std::variant<AbiUnit, std::uint32_t, AbiNamed>;

std::string hex(const std::vector<char>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
template <class F, class T>
void emit(const char* name, const char* type, const T& value, const char* json) {
    auto bytes = psio::encode(F{}, value);
    std::cout << "{\"profile\":\"pssz" << F::offset_width * 8
              << "\",\"name\":\"" << name << "\",\"type\":\"" << type
              << "\",\"value\":" << json << ",\"hex\":\"" << hex(bytes) << "\"}\n";
}
template <class F> void fixtures() {
#ifdef PSIO_ABI_HAS_EMPTY_RECORDS
    emit<F>("empty_record", "AbiEmpty", AbiEmpty{}, "{}");
    emit<F>("unit_record", "AbiUnit", AbiUnit{}, "{}");
#endif
    emit<F>("fixed_record", "AbiFixed", AbiFixed{7}, "{\"id\":7}");
    emit<F>("scalar_record", "AbiScalars", AbiScalars{true,42}, "{\"flag\":true,\"id\":42}");
    emit<F>("optional_record_none", "Option<AbiScalars>", std::optional<AbiScalars>{}, "null");
    emit<F>("optional_record_some", "Option<AbiScalars>", std::optional<AbiScalars>{AbiScalars{true,42}}, "{\"flag\":true,\"id\":42}");
    emit<F>("nested_record", "AbiNested", AbiNested{{true,42},AbiScalars{false,7},{"a","bc"}}, "{\"child\":{\"flag\":true,\"id\":42},\"optional\":{\"flag\":false,\"id\":7},\"labels\":[\"a\",\"bc\"]}");
#ifdef PSIO_ABI_HAS_EMPTY_RECORDS
    emit<F>("variant_unit", "AbiVariant", AbiVariant{std::in_place_index<0>}, "{\"Unit\":null}");
    emit<F>("variant_scalar", "AbiVariant", AbiVariant{std::in_place_index<1>,42u}, "{\"Scalar\":42}");
    emit<F>("variant_named", "AbiVariant", AbiVariant{std::in_place_index<2>,AbiNamed{3,"hi"}}, "{\"Named\":{\"code\":3,\"text\":\"hi\"}}");
#endif
#ifdef PSIO_ABI_HAS_TUPLES
    emit<F>("zero_argument_tuple", "()", std::tuple<>{}, "[]");
    emit<F>("positional_tuple", "(u32,String)", std::tuple<std::uint32_t,std::string>{7,"hi"}, "[7,\"hi\"]");
    using TupleVariant = std::variant<std::uint32_t,std::tuple<std::uint16_t,std::string>>;
    emit<F>("variant_tuple", "TupleVariant", TupleVariant{std::in_place_index<1>,std::tuple<std::uint16_t,std::string>{3,"hi"}}, "{\"Pair\":[3,\"hi\"]}");
#endif
}
int main() {
    fixtures<psio::pssz8>(); fixtures<psio::pssz16>(); fixtures<psio::pssz32>();
}
