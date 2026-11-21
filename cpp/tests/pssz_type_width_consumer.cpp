#include "pssz_type_width_fixture.hpp"
#include <psio/pssz_view.hpp>
#include <cstdio>

template<typename T>
void print_width(const char* name, bool last = false)
{
   constexpr auto budget = psio::max_dynamic_data_v<T>;
   constexpr auto bound = psio::max_encoded_size<T>();
   std::printf("\"%s\":{\"width\":%zu,\"declared_budget\":", name, psio::auto_pssz_width_v<T>);
   if constexpr (budget) std::printf("%zu", *budget);
   else std::printf("null");
   std::printf(",\"inferred_bound\":");
   if constexpr (bound) std::printf("%zu", *bound);
   else std::printf("null");
   std::printf("}%s\n", last ? "" : ",");
}

int main()
{
   auto value = width_sample();
   const auto wire = psio::encode(psio::pssz{}, value);
   if (!psio::validate<WidthRelayBatch>(psio::pssz{}, wire).ok()) return 1;
   psio::validate_or_throw<WidthRelayBatch>(psio::pssz{}, wire);
   const auto decoded = psio::decode<WidthRelayBatch>(psio::pssz{}, wire);
   if (psio::encode(psio::pssz{}, decoded) != wire) return 2;
   psio::view<WidthRelayBatch, psio::pssz, psio::storage::const_borrow> root{wire};
   if (root.get<1>()[1].get<1>().view_() != "two" ||
       (*root.get<2>()).get<0>() != 10 || root.get<3>()[1].get<0>() != 12) return 3;
   std::printf("{\n");
   print_width<WidthCheckpoint>("WidthCheckpoint");
   print_width<WidthRelayBatch>("WidthRelayBatch");
   print_width<std::vector<WidthCheckpoint>>("vector<WidthCheckpoint>");
   print_width<std::array<WidthCheckpoint, 2>>("array<WidthCheckpoint,2>");
   print_width<std::array<WidthCheckpoint, 4>>("array<WidthCheckpoint,4>");
   print_width<WidthLargeBudget>("WidthLargeBudget");
   print_width<WidthNarrowEnvelope>("WidthNarrowEnvelope");
   print_width<WidthReserveParent>("WidthReserveParent");
   print_width<WidthGrowV1>("WidthGrowV1");
   print_width<WidthGrowV2>("WidthGrowV2");
   print_width<WidthGrowParentV1>("WidthGrowParentV1");
   print_width<WidthGrowParentV2>("WidthGrowParentV2");
   print_width<WidthThreshold251>("WidthThreshold251");
   print_width<WidthThreshold252>("WidthThreshold252");
   print_width<WidthBoundedBatch>("WidthBoundedBatch");
   print_width<WidthOverflowParent>("WidthOverflowParent", true);
   std::printf("}\n");
}
