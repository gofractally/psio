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
static_assert(psio::auto_pssz_width_v<Control> == 1);

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

// A view addresses every member, also one whose annotations narrow an offset
// width in it (a bounded list's own table, a map entry with a bounded key, a
// tuple with a bounded element): its view carries the annotations. The one
// exception is a variant whose alternatives are narrowed by at<I>(...).
inline std::size_t view_tags(std::span<const char> bytes)
{
   psio::view<Control, psio::pssz> v{bytes};
   return v.get<0>().size();
}
inline std::size_t view_map(std::span<const char> bytes)
{
   psio::view<Control, psio::pssz> v{bytes};
   return v.get<1>().size() + v.get<2>().get<0>().view_().size();
}
#if defined(AT_FAIL_VIEW_NARROWED)
struct Narrowed
{
   std::variant<std::uint32_t, std::vector<std::string>> v;
   PSIO_REFLECT(Narrowed, attr(v, at<1>(max<2> | element(max<16>))))
};
inline std::size_t view_variant(std::span<const char> bytes)
{
   psio::view<Narrowed, psio::pssz> v{bytes};
   return v.get<0>().index();
}
#endif
