// A tuple has at most 255 elements (docs/pssz-spec.md 8.4): a pssz codec
// call on a longer one must not compile, and the diagnostic says why. With
// no case selected the file compiles: 255 elements are fine.
#include <psio/pssz.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

template <std::size_t>
using Element = std::uint8_t;

template <typename Sequence>
struct tuple_of;
template <std::size_t... Is>
struct tuple_of<std::index_sequence<Is...>>
{
   using type = std::tuple<Element<Is>...>;
};
template <std::size_t N>
using TupleOf = typename tuple_of<std::make_index_sequence<N>>::type;

int main()
{
   const std::vector<char> ok = psio::encode(psio::pssz{}, TupleOf<255>{});
   (void)psio::SchemaBuilder{}.insert<TupleOf<255>>("T").build();
#if defined(TUPLE_LIMIT_FAIL_ENCODE)
   const std::vector<char> bytes = psio::encode(psio::pssz{}, TupleOf<256>{});
   (void)bytes;
#elif defined(TUPLE_LIMIT_FAIL_DECODE)
   (void)psio::decode<TupleOf<256>>(psio::pssz{}, std::span<const char>(ok));
#elif defined(TUPLE_LIMIT_FAIL_WIDTH)
   static_assert(psio::auto_pssz_width_v<TupleOf<256>> == 2);
#elif defined(TUPLE_LIMIT_FAIL_SCHEMA)
   (void)psio::SchemaBuilder{}.insert<TupleOf<256>>("T").build();
#endif
   return ok.size() == 256 ? 0 : 1;
}
