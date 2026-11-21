#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <iostream>
#include <stdexcept>
#include <vector>

// A no-argument action, such as Nop::nop(), has a zero-field action-data record.
namespace action_data {
   struct Nop {};
   PSIO_REFLECT(Nop)
   struct StableNop {};
   PSIO_REFLECT(StableNop)
}
PSIO_TYPE_ATTRS(action_data::StableNop, psio::definition_will_not_change{})

template <typename T, typename F>
void check_empty_action()
{
   using R = psio::reflect<T>;
   static_assert(R::is_reflected && R::member_count == 0);
   constexpr std::size_t width = [] {
      if constexpr (requires { F::offset_width; }) return F::offset_width;
      else return psio::auto_pssz_width_v<T>;
   }();
   const std::vector<char> expected(psio::is_dwnc_v<T> ? 0 : width, 0);
#ifndef PSIO_EMPTY_VIEW_ONLY
   const auto encoded = psio::encode(F{}, T{});
   if (encoded != expected || !psio::validate<T>(F{}, encoded).ok())
      throw std::runtime_error("empty action encoding or validation failed");
   const auto decoded = psio::decode<T>(F{}, encoded);
   if (psio::encode(F{}, decoded) != encoded)
      throw std::runtime_error("empty action decode changed bytes");
#endif
#ifndef PSIO_EMPTY_CODEC_ONLY
   psio::buffer<T, F> owned{expected};
   const auto view = psio::as_view(owned);
   if (psio::bytes(view).data() != psio::bytes(owned).data() ||
       psio::size(view) != expected.size())
      throw std::runtime_error("empty action view lost its borrowed bytes");
#ifdef PSIO_EMPTY_INVALID_INDEX
   (void)view.template get<0>(); // this compile-failure probe must remain invalid
#endif
#endif
   T value;
   unsigned visited = 0;
   R::for_each_field(value, [&](auto&&...) { ++visited; });
   if (visited || R::index_of("argument") || R::index_of_field_number(1))
      throw std::runtime_error("empty action exposed a field");
}

template <typename F>
void check_format()
{
   check_empty_action<action_data::Nop, F>();
   check_empty_action<action_data::StableNop, F>();
}

int main()
{
   check_format<psio::pssz8>();
   check_format<psio::pssz16>();
   check_format<psio::pssz32>();
   check_format<psio::pssz>();
   std::cout << "Empty action records: 2 layouts x 4 existing formats passed\n";
}
