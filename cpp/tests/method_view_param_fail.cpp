// MUST NOT COMPILE (except with no MVP_FAIL_* macro, the control).
//
// A method parameter declared as psio::pssz_view<X> is handed the argument's
// bytes as a view. That needs them to be a standalone encoding of X: when
// the method's cap (or the parameter's annotations) narrows the offset
// widths inside the argument, they are not, and psio::invoke_method_pssz
// refuses at compile time instead of reading wrong offsets.
// Decoding INTO a type that only refers to a value is refused too.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

struct Item
{
   std::uint32_t a = 0;
   std::string   s;
};
PSIO_REFLECT(Item, a, s)

struct Svc
{
   // Fine: a record keeps its own widths under any cap.
   void record(psio::pssz_view<Item> item, std::uint32_t n) {}
   // Not viewable: the cap narrows the list's offset table.
   void list(psio::pssz_view<std::vector<std::string>> tags, std::uint32_t n) {}
   PSIO_REFLECT(Svc,
                method(record, item, n, maxDynamicData(1024)),
                method(list, tags, n, maxDynamicData(1024)))
};

int main()
{
   Svc  svc;
   auto ok = psio::to_pssz(psio::reflect<Svc>::method_arg_tuple<0>{Item{1, "a"}, 2u});
   psio::invoke_method_pssz<Svc, 0>(svc, ok);
#if defined(MVP_FAIL_NARROWED_VIEW_PARAM)
   auto bad = psio::to_pssz(
      psio::reflect<Svc>::method_arg_tuple<1>{std::vector<std::string>{"t"}, 2u});
   psio::invoke_method_pssz<Svc, 1>(svc, bad);
#elif defined(MVP_FAIL_DECODE_INTO_VIEW)
   auto v = psio::from_pssz<std::tuple<psio::pssz_view<Item>, std::uint32_t>>(ok);
#endif
   return 0;
}
