// MUST NOT COMPILE (except with no BORROW_FAIL_* macro, the control).
//
// Decoding into a type that only refers to a value compiles exactly for the
// shapes that can borrow from the bytes; and a record that borrows is not
// decoded from a temporary buffer.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

struct Ok
{
   std::string_view      name;
   std::span<const char> body;
};
PSIO_REFLECT(Ok, name, body)

struct WideSpan
{
   std::span<const std::uint32_t> numbers;  // the wire is not aligned for these
};
PSIO_REFLECT(WideSpan, numbers)
struct StringSpan
{
   std::span<const std::string> names;  // variable-size elements
};
PSIO_REFLECT(StringSpan, names)
struct ViewList
{
   std::vector<std::string_view> names;  // would need an owning vector of borrows
};
PSIO_REFLECT(ViewList, names)
struct Ref
{
   std::reference_wrapper<const std::string> name;
};
PSIO_REFLECT(Ref, name)
struct Inner
{
   std::vector<std::string> tags;
};
PSIO_REFLECT(Inner, tags)
struct NarrowedView
{
   // Under the record's cap the list has narrower offsets than a
   // standalone std::vector<std::string>: not viewable in place.
   psio::pssz_view<std::vector<std::string>> tags;
   PSIO_REFLECT(NarrowedView, tags, maxDynamicData(200))
};

int main()
{
   std::vector<char> bytes = psio::to_pssz(std::tuple{std::string("n"), std::vector<char>{1}});
   auto              ok    = psio::from_pssz<Ok>(std::span<const char>{bytes});
#if defined(BORROW_FAIL_WIDE_SPAN)
   auto bad = psio::from_pssz<WideSpan>(std::span<const char>{bytes});
#elif defined(BORROW_FAIL_STRING_SPAN)
   auto bad = psio::from_pssz<StringSpan>(std::span<const char>{bytes});
#elif defined(BORROW_FAIL_VIEW_LIST)
   auto bad = psio::from_pssz<ViewList>(std::span<const char>{bytes});
#elif defined(BORROW_FAIL_REFERENCE)
   auto bad = psio::from_pssz<std::tuple<const std::string&>>(std::span<const char>{bytes});
#elif defined(BORROW_FAIL_NARROWED_VIEW)
   auto bad = psio::from_pssz<NarrowedView>(std::span<const char>{bytes});
#elif defined(BORROW_FAIL_TEMPORARY)
   auto bad = psio::from_pssz<Ok>(psio::to_pssz(std::tuple{std::string("n"), std::vector<char>{1}}));
#endif
   return int(ok.name.size());
}
