// Must not compile: one attribute declared twice for the same field or type.
// Each ANN_FAIL_* macro selects one conflict (cpp/tests/CMakeLists.txt).

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <string>

namespace app::model
{
   struct Rec
   {
      std::uint64_t id = 0;
      std::string   name;
#if defined(ANN_FAIL_TYPE_KEYWORD_TWICE)
      PSIO_REFLECT(Rec, id, name, maxDynamicData(64), maxDynamicData(128))
#elif defined(ANN_FAIL_REFLECT_AND_TYPE_ATTRS)
      PSIO_REFLECT(Rec, id, name, maxDynamicData(64))
#else
      PSIO_REFLECT(Rec, id, attr(name, max<8>))
#endif
   };

#if defined(ANN_FAIL_REFLECT_AND_FIELD_ATTRS)
   // attr(name, max<8>) above and a second length bound here.
   PSIO_FIELD_ATTRS(Rec, name, psio::length_bound{.max = 16})
   inline constexpr auto use = psio::annotate<&Rec::name>;
#elif defined(ANN_FAIL_REFLECT_AND_TYPE_ATTRS)
   PSIO_TYPE_ATTRS(Rec, psio::max_dynamic_data_spec{128})
   inline constexpr auto use = psio::max_dynamic_data_v<Rec>;
#elif defined(ANN_FAIL_FIELD_ATTRS_TWICE)
   PSIO_FIELD_ATTRS(Rec, id, psio::json_string)
   PSIO_FIELD_ATTRS(Rec, id, psio::json_number)
#elif defined(ANN_FAIL_TYPE_ATTRS_TWICE)
   PSIO_TYPE_ATTRS(Rec, psio::definition_will_not_change{})
   PSIO_TYPE_ATTRS(Rec, psio::definition_will_not_change{})
#elif defined(ANN_FAIL_TYPE_KEYWORD_TWICE)
   inline constexpr auto use = psio::max_dynamic_data_v<Rec>;
#elif defined(ANN_FAIL_FIELD_ATTRS_AFTER_USE)
   inline constexpr bool early = psio::has_spec_v<
      psio::json_int_spec, std::remove_cvref_t<decltype(psio::annotate<&Rec::id>)>>;
   PSIO_FIELD_ATTRS(Rec, id, psio::json_string)
#elif defined(ANN_FAIL_TYPE_ATTRS_AFTER_USE)
   inline constexpr bool early = psio::is_dwnc_v<Rec>;
   PSIO_TYPE_ATTRS(Rec, psio::definition_will_not_change{})
#endif
}  // namespace app::model

int main() {}
