// MUST NOT COMPILE (except with no MV_FAIL_* macro, the control): each case
// asks psio::mutable_view for a write inside a set, a map key or a nested
// value that cannot preserve the frame (docs/pssz-spec.md §17). ctest runs
// the compiler on this file once per case and requires the corresponding
// message.

#include <psio/invariant.hpp>
#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

struct FailBalance
{
   std::uint32_t amount = 0;
   bool          frozen = false;
};
PSIO_REFLECT(FailBalance, amount, frozen, definitionWillNotChange())

// Stands for a type with a semantic invariant: low <= high.
struct FailRange
{
   std::uint32_t low  = 0;
   std::uint32_t high = 0;
};
PSIO_REFLECT(FailRange, low, high, definitionWillNotChange())
template <>
inline constexpr bool psio::detail::mutation_blocked_v<FailRange> = true;

// Types with a real invariant (invariant.hpp): blocked without a
// mutation_blocked_v specialization.
struct FailSpan
{
   std::uint32_t lo = 0;
   std::uint32_t hi = 0;
   static psio::codec_status check(const FailSpan& s) noexcept
   {
      return s.lo <= s.hi ? psio::codec_ok() : psio::codec_fail("FailSpan: lo > hi", 0, "FailSpan");
   }
   PSIO_REFLECT(FailSpan, definitionWillNotChange(), invariant(check), lo, hi)
};
struct FailEven
{
   std::uint32_t value = 0;
   static psio::codec_status check(const FailEven& e) noexcept
   {
      return e.value % 2 == 0 ? psio::codec_ok() : psio::codec_fail("FailEven: odd", 0, "FailEven");
   }
   PSIO_REFLECT(FailEven, transparent(), invariant(check), value)
};
// A transparent wrapper without an invariant is its member.
struct FailCount
{
   std::uint32_t value = 0;
   PSIO_REFLECT(FailCount, transparent(), value)
};
struct FailPairOfSpans
{
   FailSpan      span;
   std::uint32_t tag = 0;
   PSIO_REFLECT(FailPairOfSpans, definitionWillNotChange(), span, tag)
};
struct FailGuarded
{
   FailSpan                                  span;
   FailEven                                  even;
   FailCount                                 count;
   std::map<std::string, FailSpan>           spans;
   psio::nested<FailSpan>                    packed;
   FailPairOfSpans                           holder;
   std::vector<FailEven>                     evens;
   std::map<std::string, FailCount>          counts;
   PSIO_REFLECT(FailGuarded, span, even, count, spans, packed, holder, evens, counts)
};

struct FailBook
{
   std::set<std::uint32_t>                                          ids;
   std::map<std::string, std::uint32_t>                             totals;
   std::map<std::uint32_t, std::set<std::uint8_t>>                  groups;
   psio::nested<FailBalance>                                        packed;
   psio::nested<FailBalance, psio::nested_validation::deferred>     later;
   psio::nested<FailRange>                                          range;
   psio::nested<std::unordered_set<std::uint16_t>>                  packed_set;
   std::map<std::uint8_t, FailRange>                                ranges;
   std::map<std::string, std::string>                               names;
};
PSIO_REFLECT(FailBook, ids, totals, groups, packed, later, range, packed_set, ranges, names)

int main()
{
   std::vector<char> bytes;
   psio::mutable_view<FailBook, psio::pssz> mv{bytes};
   // The control: what the same record does allow.
   bool ok = mv.get<1>()[0].value().set(1) && mv.get<1>()[0].key().view_().empty() &&
             mv.get<2>().size() == 0 && mv.get<3>().get().set(FailBalance{}) &&
             (*mv.get<3>()).get<0>().set(1) && mv.get<7>().empty() &&
             psio::size(mv.get<8>()[0].value()) == 0;
   psio::mutable_view<FailGuarded, psio::pssz> guarded{bytes};
   ok = ok && guarded.get<2>().set(1) && guarded.get<3>().empty() &&
        guarded.get<5>().get<1>().set(1) && guarded.get<7>()[0].value().set(1) &&
        psio::size(guarded.get<4>()) == 0;
#if defined(MV_FAIL_SET)
   // An element write could break the order of the set.
   ok = mv.get<0>().size() == 0;
#elif defined(MV_FAIL_SET_ROOT)
   ok = psio::size(psio::mutable_view<std::set<std::uint32_t>, psio::pssz>{bytes}) == 0;
#elif defined(MV_FAIL_SET_VALUE)
   // A set reached as the mapped value of a map.
   ok = mv.get<2>()[0].value().size() == 0;
#elif defined(MV_FAIL_SET_NESTED)
   // A set inside nested bytes.
   ok = mv.get<6>().get().size() == 0;
#elif defined(MV_FAIL_MAP_KEY)
   // A key is handed out as a read view: it has no `set`.
   ok = mv.get<8>()[0].key().set("other");
#elif defined(MV_FAIL_MAP_ENTRY)
   // An entry is not the tuple view: its key member is not addressable.
   ok = mv.get<1>()[0].get<0>().set(1);
#elif defined(MV_FAIL_MAP_WHOLE)
   ok = mv.get<1>().set({{"a", 1}});
#elif defined(MV_FAIL_MAP_VALUE_VARIABLE)
   ok = mv.get<8>()[0].value().set("longer");
#elif defined(MV_FAIL_MAP_VALUE_BLOCKED)
   ok = mv.get<7>()[0].value().get<0>().set(1);
#elif defined(MV_FAIL_NESTED_WHOLE)
   // The byte count of the inner encoding cannot change in place.
   ok = mv.get<3>().set(psio::nested<FailBalance>{});
#elif defined(MV_FAIL_NESTED_DEFERRED)
   // Nobody has checked that these bytes are a FailBalance.
   ok = mv.get<4>().get().set(FailBalance{});
#elif defined(MV_FAIL_NESTED_BLOCKED)
   ok = mv.get<5>().get().get<0>().set(1);
#elif defined(MV_FAIL_INVARIANT_LEAF)
   // A leaf of a type with an invariant: lo <= hi would not be re-checked.
   ok = guarded.get<0>().get<0>().set(1);
#elif defined(MV_FAIL_INVARIANT_WHOLE_TYPE)
   // Nor the whole value: `set` writes what it is given, unchecked.
   ok = guarded.get<0>().set(FailSpan{});
#elif defined(MV_FAIL_INVARIANT_TRANSPARENT)
   ok = guarded.get<1>().set(2);
#elif defined(MV_FAIL_INVARIANT_MAP_VALUE)
   ok = guarded.get<3>()[0].value().get<1>().set(1);
#elif defined(MV_FAIL_INVARIANT_NESTED)
   ok = guarded.get<4>().get().get<1>().set(1);
#elif defined(MV_FAIL_INVARIANT_LIST_ELEMENT)
   ok = guarded.get<6>()[0].set(2);
#elif defined(MV_FAIL_INVARIANT_CONTAINED)
   // A fixed-size value that contains a type with an invariant.
   ok = guarded.get<5>().set(FailPairOfSpans{});
#endif
   return ok ? 0 : 1;
}
