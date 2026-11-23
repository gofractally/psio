// MUST NOT COMPILE (except with no VT_FAIL_* macro, the control).
//
// Trust is in the type of a view and is only obtained explicitly
// (view.hpp "Safe and trusted views"): a trusted view is built from
// psio::trusted{bytes}, never from a span, a vector or another mutable
// source, and never from a safe view.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct Inner
{
   std::uint32_t n = 0;
   std::string   text;
};
PSIO_REFLECT(Inner, n, text)
struct Row
{
   std::uint64_t      key = 0;
   Inner              inner;
   std::vector<Inner> list;
};
PSIO_REFLECT(Row, key, inner, list)

std::uint32_t read_trusted(psio::pssz_trusted_view<Inner> v) { return v.n(); }
std::uint32_t read_safe(psio::pssz_view<Inner> v) { return v.n(); }

int main()
{
   std::vector<char>           bytes = psio::to_pssz(Row{});
   const std::span<const char> span{bytes};
   std::span<char>             writable{bytes};

   // The control: what does compile.
   psio::pssz_view<Row>         safe{span};
   psio::pssz_trusted_view<Row> trusted{psio::trusted{span}};
   psio::pssz_trusted_view<Row> from_vector{psio::trusted{bytes}};
   psio::pssz_view<Row>         widened = trusted;          // trusted -> safe
   std::uint32_t                total   = read_safe(trusted.inner()) + read_trusted(trusted.inner());
   (void)from_vector;
   (void)widened;
   (void)writable;

#if defined(VT_FAIL_SAFE_TO_TRUSTED)
   psio::pssz_trusted_view<Row> bad = safe;
   (void)bad;
#elif defined(VT_FAIL_SAFE_TO_TRUSTED_EXPLICIT)
   psio::pssz_trusted_view<Row> bad{safe};
   (void)bad;
#elif defined(VT_FAIL_SAFE_SUBVIEW_TO_TRUSTED)
   total += read_trusted(safe.inner());
#elif defined(VT_FAIL_SAFE_ELEMENT_TO_TRUSTED)
   psio::pssz_trusted_view<Inner> bad = safe.list()[0];
   (void)bad;
#elif defined(VT_FAIL_BARE_SPAN)
   psio::pssz_trusted_view<Row> bad{span};
   (void)bad;
#elif defined(VT_FAIL_BARE_SPAN_ASSIGN)
   psio::pssz_trusted_view<Row> bad = span;
   (void)bad;
#elif defined(VT_FAIL_VECTOR)
   psio::pssz_trusted_view<Row> bad{bytes};
   (void)bad;
#elif defined(VT_FAIL_MUTABLE_SPAN)
   psio::pssz_trusted_view<Row> bad{writable};
   (void)bad;
#elif defined(VT_FAIL_SPAN_AND_ROOT)
   psio::pssz_trusted_view<Row> bad{span, span};
   (void)bad;
#elif defined(VT_FAIL_SUBVIEW_FROM_SPAN)
   psio::pssz_trusted_view<std::string> bad{span};
   (void)bad;
#elif defined(VT_FAIL_SSZ_BARE_SPAN)
   psio::ssz_trusted_view<Row> bad{span};
   (void)bad;
#elif defined(VT_FAIL_IMPLICIT_TAG)
   // psio::trusted is itself explicit: a span does not become one silently.
   psio::trusted bad = span;
   (void)bad;
#elif defined(VT_FAIL_SAFE_FROM_TAG)
   // The safe view's name never yields a trusted view.
   psio::pssz_view<Row> bad{psio::trusted{span}};
   (void)bad;
#endif
   return static_cast<int>(total + safe.key());
}
