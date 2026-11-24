// A record with 255 members — the most a record may have — through the
// codecs, the views and the Schema IR.
//
// The point is the compiler, not the bytes: a fold expression over the
// members nests one expression per member, and clang before 21 stops at a
// nesting depth of 256 (-fbracket-depth). This target is compiled with
// that limit stated (tests/CMakeLists.txt), so a member walk in the library
// that would not fit at 255 members fails here on every clang.

#include <psio/bin.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>
#include <psio/ssz.hpp>

#include <catch.hpp>

#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{
#define PSIO_TEST_WIDE_MEMBER(z, n, unused) std::uint16_t m##n = n;
#define PSIO_TEST_WIDE_TEXT(z, n, unused) std::string s##n;
   // 255 fixed-size members.
   struct Wide
   {
      BOOST_PP_REPEAT(255, PSIO_TEST_WIDE_MEMBER, ~)
      friend bool operator==(const Wide&, const Wide&) = default;
   };
   PSIO_REFLECT(Wide, BOOST_PP_ENUM_PARAMS(255, m))
   // 255 variable-size members.
   struct WideText
   {
      BOOST_PP_REPEAT(255, PSIO_TEST_WIDE_TEXT, ~)
      friend bool operator==(const WideText&, const WideText&) = default;
   };
   PSIO_REFLECT(WideText, BOOST_PP_ENUM_PARAMS(255, s))
#undef PSIO_TEST_WIDE_MEMBER
#undef PSIO_TEST_WIDE_TEXT

   static_assert(psio::reflect<Wide>::member_count == 255);
   static_assert(psio::reflect<WideText>::member_count == 255);

   template <typename Fmt, typename T>
   void round_trip(const T& value)
   {
      const auto                  bytes = psio::encode(Fmt{}, value);
      const std::span<const char> in{bytes.data(), bytes.size()};
      REQUIRE(psio::validate<T>(Fmt{}, in).ok());
      REQUIRE(psio::validate_strict<T>(Fmt{}, in).ok());
      REQUIRE(psio::decode<T>(Fmt{}, in) == value);
      REQUIRE(psio::size_of(Fmt{}, value) == bytes.size());
   }
}  // namespace

TEST_CASE("a record of 255 members round-trips in every codec", "[wide-record]")
{
   Wide w;
   w.m0   = 7;
   w.m254 = 65535;
   WideText t;
   t.s0   = "first";
   t.s127 = "middle";
   t.s254 = "last";

   round_trip<psio::pssz>(w);
   round_trip<psio::pssz>(t);
   round_trip<psio::experimental::frac32>(w);
   round_trip<psio::experimental::frac32>(t);
   round_trip<psio::bin>(w);
   round_trip<psio::bin>(t);
   round_trip<psio::ssz>(w);
   round_trip<psio::ssz>(t);
   round_trip<psio::key>(w);
   round_trip<psio::key>(t);
   round_trip<psio::json>(w);
   round_trip<psio::json>(t);
}

TEST_CASE("a record of 255 members has views and a schema", "[wide-record]")
{
   Wide w;
   w.m254 = 9;
   WideText t;
   t.s254 = "last";

   const auto            wb = psio::to_pssz(w);
   psio::pssz_view<Wide> wv{wb};
   REQUIRE(wv.m0() == 0);
   REQUIRE(wv.m254() == 9);
   REQUIRE(wv.template get<200>() == 200);
   REQUIRE(wv.unpack() == w);
   REQUIRE(wv == w);

   const auto                tb = psio::to_pssz(t);
   psio::pssz_view<WideText> tv{tb};
   REQUIRE(tv.s254() == "last");
   REQUIRE(tv.s0().empty());
   REQUIRE(tv.unpack() == t);

   auto                            mb = psio::to_pssz(w);
   psio::mutable_view<Wide, psio::pssz> mv{std::span<char>{mb}};
   REQUIRE(mv.m254().set(11));
   REQUIRE(psio::from_pssz<Wide>(mb).m254 == 11);

   psio::SchemaBuilder b;
   b.insert<Wide>("Wide");
   b.insert<WideText>("WideText");
   const psio::Schema schema = std::move(b).build();
   const auto         text   = psio::encode(psio::json{}, schema);
   REQUIRE(std::string_view{text.data(), text.size()}.find("\"m254\"") != std::string_view::npos);
   REQUIRE(std::string_view{text.data(), text.size()}.find("\"s254\"") != std::string_view::npos);
}
