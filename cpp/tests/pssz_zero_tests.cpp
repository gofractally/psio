#include <psio/pssz_view.hpp>
#include <catch.hpp>
#include <array>
#include <optional>
#include <vector>

struct ZeroUnit {};
PSIO_REFLECT(ZeroUnit)
PSIO_TYPE_ATTRS(ZeroUnit, psio::definition_will_not_change{})
struct ZeroEnvelope { std::optional<ZeroUnit> unit; std::uint32_t sequence; std::array<ZeroUnit,3> padding; };
PSIO_REFLECT(ZeroEnvelope, unit, sequence, padding)
struct EmptyExtensible {};
PSIO_REFLECT(EmptyExtensible)

template<typename T,typename F>
auto zero_view(const std::vector<char>& bytes)
{
   return psio::view<T,F,psio::storage::const_borrow>{bytes};
}
template<typename T,typename F>
void zero_roundtrip(const T& value)
{
   const auto wire=psio::encode(F{},value);
   REQUIRE(psio::size_of(F{},value)==wire.size());
   REQUIRE(psio::validate<T>(F{},wire).ok());
   REQUIRE(psio::validate_strict<T>(F{},wire).ok());
   REQUIRE_NOTHROW(psio::validate_or_throw<T>(F{},wire));
   REQUIRE(psio::encode(F{},psio::decode<T>(F{},wire))==wire);
}

TEMPLATE_TEST_CASE("zero-size fixed optional uses existing presence selector",
                   "[pssz][zero][optional]",psio::pssz8,psio::pssz16,psio::pssz32,psio::pssz)
{
   using T=std::optional<ZeroUnit>;
   STATIC_REQUIRE(psio::detail::pssz_impl::is_fixed_v<ZeroUnit>);
   STATIC_REQUIRE(psio::detail::pssz_impl::fixed_size_of<ZeroUnit>()==0);
   const T none;
   const T some{ZeroUnit{}};
   const auto absent=psio::encode(TestType{},none);
   const auto present=psio::encode(TestType{},some);
   REQUIRE(absent==std::vector<char>{0});
   REQUIRE(present==std::vector<char>{1});
   zero_roundtrip<T,TestType>(none);
   zero_roundtrip<T,TestType>(some);
   REQUIRE_FALSE(psio::decode<T>(TestType{},absent).has_value());
   REQUIRE(psio::decode<T>(TestType{},present).has_value());
   REQUIRE_FALSE(zero_view<T,TestType>(absent).has_value());
   const auto view=zero_view<T,TestType>(present);
   REQUIRE(view.has_value());
   REQUIRE(psio::bytes(*view).empty());
   // Nonzero fixed payloads keep their previous length-based presence bytes.
   REQUIRE(psio::encode(TestType{},std::optional<std::uint8_t>{})==std::vector<char>{});
   REQUIRE(psio::encode(TestType{},std::optional<std::uint8_t>{0})==std::vector<char>{0});
   zero_roundtrip<std::optional<EmptyExtensible>,TestType>(EmptyExtensible{});
}

TEMPLATE_TEST_CASE("zero-size presence works in records and existing containers",
                   "[pssz][zero][optional]",psio::pssz8,psio::pssz16,psio::pssz32,psio::pssz)
{
   ZeroEnvelope value{ZeroUnit{},42,{}};
   zero_roundtrip<ZeroEnvelope,TestType>(value);
   auto wire=psio::encode(TestType{},value);
   REQUIRE(psio::decode<ZeroEnvelope>(TestType{},wire).unit.has_value());
   // Explicit record views are not part of the older explicit-profile surface.
   if constexpr(std::is_same_v<TestType,psio::pssz>)
   {
      auto view=zero_view<ZeroEnvelope,TestType>(wire);
      REQUIRE(view.template get<0>().has_value());
      REQUIRE(view.template get<1>()==42);
      REQUIRE(view.template get<2>().size()==3);
   }
   std::vector<std::optional<ZeroUnit>> entries{std::nullopt,ZeroUnit{},std::nullopt,ZeroUnit{}};
   zero_roundtrip<decltype(entries),TestType>(entries);
   auto encoded=psio::encode(TestType{},entries);
   auto back=psio::decode<decltype(entries)>(TestType{},encoded);
   REQUIRE(back.size()==4);
   REQUIRE_FALSE(back[0].has_value());
   REQUIRE(back[1].has_value());
   REQUIRE_FALSE(back[2].has_value());
   REQUIRE(back[3].has_value());
   auto view=zero_view<decltype(entries),TestType>(encoded);
   REQUIRE(view.size()==4);
   REQUIRE(view[1].has_value());
   REQUIRE_FALSE(view[2].has_value());
   zero_roundtrip<std::optional<std::array<ZeroUnit,3>>,TestType>(std::array<ZeroUnit,3>{});
   const auto array=psio::encode(TestType{},std::optional<std::array<ZeroUnit,3>>{std::array<ZeroUnit,3>{}});
   REQUIRE((zero_view<std::optional<std::array<ZeroUnit,3>>,TestType>(array).has_value()));
}

TEMPLATE_TEST_CASE("malformed zero-size optional payloads reject without reading missing bytes",
                   "[pssz][zero][optional][malformed]",psio::pssz8,psio::pssz16,psio::pssz32,psio::pssz)
{
   using T=std::optional<ZeroUnit>;
   for(const auto& wire:std::vector<std::vector<char>>{{},{2},{char(255)},{0,0},{1,0},{0,1},{1,1}})
   {
      REQUIRE_FALSE(psio::validate<T>(TestType{},wire).ok());
      REQUIRE_FALSE(psio::validate_strict<T>(TestType{},wire).ok());
      REQUIRE_THROWS_AS(psio::validate_or_throw<T>(TestType{},wire),psio::codec_exception);
      REQUIRE_THROWS_AS(psio::decode<T>(TestType{},wire),psio::codec_exception);
   }
}
