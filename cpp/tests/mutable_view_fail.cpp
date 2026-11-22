// MUST NOT COMPILE (except with no MV_FAIL_* macro, the control): each case
// asks psio::mutable_view for an in-place mutation that cannot preserve the
// frame (docs/pssz-spec.md §17). ctest runs the compiler on this file once
// per case and requires the corresponding static_assert text.

#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct FailPoint
{
   std::int32_t x = 0;
   std::int32_t y = 0;
};
PSIO_REFLECT(FailPoint, x, y, definitionWillNotChange())

// Stands for a type with a semantic invariant: low <= high.
struct FailRange
{
   std::uint32_t low  = 0;
   std::uint32_t high = 0;
};
PSIO_REFLECT(FailRange, low, high, definitionWillNotChange())
template <>
inline constexpr bool psio::detail::mutation_blocked_v<FailRange> = true;

struct FailHolder
{
   std::uint32_t id = 0;
   FailRange     range;
};
PSIO_REFLECT(FailHolder, id, range, definitionWillNotChange())

struct FailRecord
{
   std::uint32_t                            id = 0;
   std::string                              name;
   std::vector<std::uint32_t>               numbers;
   std::optional<std::uint32_t>             maybe;
   std::variant<std::uint32_t, std::string> choice;
   psio::bitlist<16>                        bits;
   FailPoint                                point;
   FailRange                                range;
   FailHolder                               holder;
   std::uint32_t                            shown = 0;
   std::vector<std::uint32_t>               sorted;
   std::string                              text;
   std::vector<std::array<std::uint8_t, 2>> keyed;
   std::pair<std::uint16_t, std::uint16_t>  limits;
};
PSIO_REFLECT(FailRecord, id, name, numbers, maybe, choice, bits, point, range, holder, shown,
             sorted, text, keyed, limits)
// Position specs constrain content below the member, as sorted does.
PSIO_FIELD_ATTRS(FailRecord, keyed, psio::element(psio::sorted))
PSIO_FIELD_ATTRS(FailRecord, limits, psio::at<0>(psio::unique_keys))
PSIO_FIELD_ATTRS(FailRecord, sorted, psio::sorted_spec{})
PSIO_FIELD_ATTRS(FailRecord, text, psio::utf8_spec{})
template <>
inline constexpr auto psio::annotate<&FailRecord::shown> = std::tuple{psio::as<psio::hex_tag>};

// Not a PSSZ format tag.
struct FailFormat
{
};

struct FailOuter
{
   FailRecord record;
};
PSIO_REFLECT(FailOuter, record)

int main()
{
   std::vector<char> bytes;
   psio::mutable_view<FailRecord, psio::pssz> mv{bytes};
   // The control: what the same record does allow.
   bool ok = mv.get<0>().set(1) && mv.get<6>().set(FailPoint{1, 2}) &&
             mv.get<6>().get<0>().set(1) && mv.get<2>()[0].set(1) && (*mv.get<3>()).set(1) &&
             psio::size(mv.get<1>()) == 0;
#if defined(MV_FAIL_STRING)
   ok = mv.get<1>().set("longer");
#elif defined(MV_FAIL_VECTOR)
   ok = mv.get<2>().set({1, 2, 3});
#elif defined(MV_FAIL_OPTIONAL)
   ok = mv.get<3>().set(std::nullopt);
#elif defined(MV_FAIL_VARIANT)
   ok = mv.get<4>().set(std::string{"other alternative"});
#elif defined(MV_FAIL_BITLIST)
   ok = mv.get<5>().set(psio::bitlist<16>{});
#elif defined(MV_FAIL_EXTENSIBLE_RECORD)
   ok = psio::mutable_view<FailOuter, psio::pssz>{bytes}.get<0>().set(FailRecord{});
#elif defined(MV_FAIL_BLOCKED_TYPE)
   // A member of a blocked type: no leaf beneath it may be written.
   ok = mv.get<7>().get<0>().set(5);
#elif defined(MV_FAIL_BLOCKED_ROOT)
   ok = psio::mutable_view<FailRange, psio::pssz>{bytes}.get<1>().set(5);
#elif defined(MV_FAIL_BLOCKED_NESTED)
   // Overwriting a whole fixed-size value that contains a blocked type.
   ok = mv.get<8>().set(FailHolder{});
#elif defined(MV_FAIL_AS_OVERRIDE)
   ok = mv.get<9>().set(5);
#elif defined(MV_FAIL_SORTED_MEMBER)
   // An element write could break the member's declared order.
   ok = mv.get<10>()[0].set(5);
#elif defined(MV_FAIL_VALIDATED_MEMBER)
   ok = psio::size(mv.get<11>()) != 0;
#elif defined(MV_FAIL_ELEMENT_MEMBER)
   ok = psio::size(mv.get<12>()) != 0;
#elif defined(MV_FAIL_AT_MEMBER)
   ok = mv.get<13>().get<0>().set(5);
#elif defined(MV_FAIL_FORMAT)
   ok = psio::mutable_view<FailPoint, FailFormat>{bytes}.set(FailPoint{});
#endif
   return ok ? 0 : 1;
}
