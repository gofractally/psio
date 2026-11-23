// MUST NOT COMPILE (except with no PA_FAIL_* macro, the control).
//
// A value that does not pack as the type asked for is refused at compile
// time, by a constraint that names both types (psio::packs_as<U, T>,
// psio::pssz_source_of<S, T>).

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct Trace
{
   std::uint32_t code = 0;
   std::string   error;
};
PSIO_REFLECT(Trace, code, error)
struct Rec
{
   std::uint64_t id = 0;
   Trace         trace;
};
PSIO_REFLECT(Rec, id, trace)
struct RecView
{
   std::uint64_t          id = 0;
   psio::pssz_view<Trace> trace;
};
PSIO_REFLECT(RecView, id, trace)
struct Wrong
{
   std::string   id;
   std::uint32_t n = 0;
};
PSIO_REFLECT(Wrong, id, n)

struct Svc
{
   void put(Rec row) {}
   PSIO_REFLECT(Svc, method(put, row))
};

// psibase's Table<T>::put(const U&).
template <typename T, typename U>
   requires psio::packs_as<U, T>
std::vector<char> put(const U& row)
{
   return psio::to_pssz(row);
}

int main()
{
   auto    tb = psio::to_pssz(Trace{1, "e"});
   RecView rv{1, psio::pssz_view<Trace>{tb}};
   auto    a = put<Rec>(rv);
   auto    b = psio::to_pssz_as<Rec>(rv);
   auto    c = psio::method_args_to_pssz<Svc, 0>(rv);
#if defined(PA_FAIL_PUT)
   auto bad = put<Rec>(Wrong{"x", 1});
#elif defined(PA_FAIL_TO_PSSZ_AS)
   auto bad = psio::to_pssz_as<Rec>(Wrong{"x", 1});
#elif defined(PA_FAIL_SIZE_AS)
   auto bad = psio::pssz_size_as<Rec>(std::uint64_t{1});
#elif defined(PA_FAIL_METHOD_ARG)
   auto bad = psio::method_args_to_pssz<Svc, 0>(Wrong{"x", 1});
#endif
   return int(a.size() + b.size() + c.size());
}
