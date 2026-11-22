// Compile-fail cases for annotations applied to the wrong shape. Each
// AT_FAIL_* macro selects one; the build must fail with the message named
// in tests/CMakeLists.txt. With no macro defined the file compiles.

#include <psio/helpers.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

struct Control
{
   std::vector<std::string>             tags;
   std::map<std::string, std::uint32_t> m;
   std::tuple<std::string, int>         t;
   PSIO_REFLECT(Control, attr(tags, max<8> | element(utf8 | max<16>)),
                attr(m, max<4> | keys(max<16>)), attr(t, at<0>(max<8>)))
};
static_assert(psio::auto_pssz_width_v<Control> == 2);

#if defined(AT_FAIL_ELEMENT_ON_STRING)
struct Bad
{
   std::string s;
   PSIO_REFLECT(Bad, attr(s, element(max<4>)))
};
#elif defined(AT_FAIL_ELEMENT_ON_MAP)
struct Bad
{
   std::map<std::string, int> s;
   PSIO_REFLECT(Bad, attr(s, element(max<4>)))
};
#elif defined(AT_FAIL_KEYS_ON_VECTOR)
struct Bad
{
   std::vector<std::string> s;
   PSIO_REFLECT(Bad, attr(s, keys(max<4>)))
};
#elif defined(AT_FAIL_VALUES_ON_VECTOR)
struct Bad
{
   std::vector<std::string> s;
   PSIO_REFLECT(Bad, attr(s, values(max<4>)))
};
#elif defined(AT_FAIL_AT_ON_VECTOR)
struct Bad
{
   std::vector<std::string> s;
   PSIO_REFLECT(Bad, attr(s, at<0>(max<4>)))
};
#elif defined(AT_FAIL_AT_OUT_OF_RANGE)
struct Bad
{
   std::tuple<std::string, int> s;
   PSIO_REFLECT(Bad, attr(s, at<2>(max<4>)))
};
#elif defined(AT_FAIL_AT_VARIANT_OUT_OF_RANGE)
struct Bad
{
   std::variant<std::string, int> s;
   PSIO_REFLECT(Bad, attr(s, at<5>(max<4>)))
};
#elif defined(AT_FAIL_MAX_ON_INT_ELEMENT)
struct Bad
{
   std::vector<std::uint32_t> s;
   PSIO_REFLECT(Bad, attr(s, element(max<4>)))
};
#elif defined(AT_FAIL_UTF8_ON_LIST_ELEMENT)
struct Bad
{
   std::vector<std::vector<std::uint8_t>> s;
   PSIO_REFLECT(Bad, attr(s, element(utf8)))
};
#elif defined(AT_FAIL_NESTED_EXACT)
struct Bad
{
   std::vector<std::string> s;
   PSIO_REFLECT(Bad, attr(s, element(exact<4>)))
};
#elif defined(AT_FAIL_DEEP)
struct Bad
{
   std::vector<std::vector<std::uint32_t>> s;
   PSIO_REFLECT(Bad, attr(s, max<2> | element(max<2> | element(max<2>))))
};
#endif

#if defined(AT_FAIL_ELEMENT_ON_STRING) || defined(AT_FAIL_ELEMENT_ON_MAP) || \
   defined(AT_FAIL_KEYS_ON_VECTOR) || defined(AT_FAIL_VALUES_ON_VECTOR) || \
   defined(AT_FAIL_AT_ON_VECTOR) || defined(AT_FAIL_AT_OUT_OF_RANGE) || \
   defined(AT_FAIL_AT_VARIANT_OUT_OF_RANGE) || defined(AT_FAIL_MAX_ON_INT_ELEMENT) || \
   defined(AT_FAIL_UTF8_ON_LIST_ELEMENT) || defined(AT_FAIL_NESTED_EXACT) || \
   defined(AT_FAIL_DEEP)
// Any use of the type's codec instantiates the member's annotations.
static_assert(psio::auto_pssz_width_v<Bad> != 0);
#endif

// A view addresses a member whose annotations leave every nested width as
// its type gives it (a list of bounded strings: the list's table is 4 wide
// either way); one whose annotations narrow a nested value is refused.
inline std::size_t view_tags(std::span<const char> bytes)
{
   psio::view<Control, psio::pssz> v{bytes};
   return v.get<0>().size();
}
#if defined(AT_FAIL_VIEW_NARROWED)
inline std::size_t view_map(std::span<const char> bytes)
{
   psio::view<Control, psio::pssz> v{bytes};
   return v.get<1>().size();  // map entries: a pair whose bounded key narrows it
}
#endif

int main() {}
