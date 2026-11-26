#include "pssz_optional_fixtures.hpp"
#include <catch.hpp>

#include "validated_write.hpp"
using namespace optional_fixture;

template<typename T,typename F> auto checked_view(F format,std::span<const char> bytes,bool& unknown) {
 std::optional<psio::view<T,F,psio::storage::const_borrow>> view;
 auto status=psio::validated_pssz_view<T>(format,bytes,view,unknown);
 REQUIRE(status.ok());REQUIRE(view.has_value());return *view;
}

TEMPLATE_TEST_CASE("optional suffixes preserve old payload boundaries without extra framing", "[pssz][optional-suffix]", psio::pssz8, psio::pssz16, psio::pssz32) {
 using F=TestType;constexpr auto W=F::offset_width;
 auto base=psio::encode(F{},Base{"a"});
 REQUIRE(psio::encode(F{},Extended{"a",std::nullopt})==base);
 bool unknown=false; Extended upgraded{"stale","stale"};
 REQUIRE(psio::decode_pssz_checked(F{},base,upgraded,unknown).ok());
 REQUIRE_FALSE(unknown);REQUIRE(upgraded.name=="a");REQUIRE_FALSE(upgraded.extra);
 auto present=psio::encode(F{},Extended{"a","b"});
 REQUIRE(present.size()==3*W+3); // header + two slots + 'a' + Some('b')
 Base old; REQUIRE(psio::decode_pssz_checked(F{},present,old,unknown).ok());
 REQUIRE(unknown); REQUIRE(old.name=="a");
 REQUIRE_FALSE(psio::validate_strict<Base>(F{},present).ok());
 auto old_view=checked_view<Base>(F{},present,unknown);
 REQUIRE(unknown);REQUIRE(old_view.template get<0>().view_()=="a");
 auto missing_view=checked_view<Extended>(F{},base,unknown);
 REQUIRE_FALSE(missing_view.template get<1>().has_value());
 REQUIRE(psio::bytes(missing_view.template get<1>()).size()==1);
 REQUIRE(psio::bytes(missing_view.template get<1>())[0]==0);
 auto twice=psio::encode(F{},Twice{"a",std::nullopt,"b"});
 REQUIRE(psio::decode_pssz_checked(F{},twice,old,unknown).ok());
 REQUIRE(unknown);REQUIRE(old.name=="a");
 auto twice_view=checked_view<Twice>(F{},twice,unknown);
 REQUIRE_FALSE(twice_view.template get<1>().has_value());
 REQUIRE(twice_view.template get<2>().has_value());
 REQUIRE(twice_view.template get<2>().get().view_()=="b");
 auto odd=psio::encode(F{},OddExtended{true,"a","b"});
 OddBase odd_old; REQUIRE(psio::decode_pssz_checked(F{},odd,odd_old,unknown).ok());
 REQUIRE(unknown);REQUIRE(odd_old.flag);REQUIRE(odd_old.name=="a");
 REQUIRE(checked_view<OddBase>(F{},odd,unknown).template get<1>().view_()=="a");
 auto nested=psio::encode(F{},NewOuter{{"a","b"},{{"c","d"}}});
 OldOuter nested_old;REQUIRE(psio::decode_pssz_checked(F{},nested,nested_old,unknown).ok());
 REQUIRE(unknown);REQUIRE(nested_old.inner.name=="a");REQUIRE(nested_old.values[0].name=="c");
 auto nested_view=checked_view<OldOuter>(F{},nested,unknown);
 REQUIRE(nested_view.template get<0>().template get<0>().view_()=="a");
 REQUIRE(nested_view.template get<1>()[0].template get<0>().view_()=="c");
}

TEMPLATE_TEST_CASE("optional minimum-size rule preserves present empty values", "[pssz][optional-suffix]", psio::pssz8, psio::pssz16, psio::pssz32) {
 using F=TestType;bool unknown=false;
 auto none=psio::encode(F{},std::optional<Base>{});REQUIRE(none.empty());
 auto record=psio::encode(F{},std::optional<Base>{Base{"a"}});
 REQUIRE(record==psio::encode(F{},Base{"a"}));
 auto none_unit=psio::encode(F{},std::optional<Unit>{});REQUIRE(none_unit==std::vector<char>{0});
 auto some_unit=psio::encode(F{},std::optional<Unit>{Unit{}});REQUIRE(some_unit==std::vector<char>{1});
 REQUIRE_FALSE(checked_view<std::optional<Unit>>(F{},none_unit,unknown).has_value());
 REQUIRE(checked_view<std::optional<Unit>>(F{},some_unit,unknown).has_value());
 using Nested=std::optional<std::optional<std::string>>;
 auto outer_none=psio::encode(F{},Nested{});REQUIRE(outer_none.empty());
 auto inner_none=psio::encode(F{},Nested{std::in_place,std::nullopt});REQUIRE(inner_none==std::vector<char>{0});
 auto inner_empty=psio::encode(F{},Nested{std::in_place,std::string{}});REQUIRE(inner_empty==std::vector<char>{1});
 REQUIRE(checked_view<Nested>(F{},inner_none,unknown).has_value());
 REQUIRE_FALSE(checked_view<Nested>(F{},inner_none,unknown).get().has_value());
 REQUIRE(checked_view<Nested>(F{},inner_empty,unknown).get().has_value());
 using Args=std::tuple<std::uint32_t,std::string>;
 auto args=psio::encode(F{},Args{7,"hi"});auto view=checked_view<Args>(F{},args,unknown);
 REQUIRE(view.template get<0>()==7);REQUIRE(view.template get<1>().view_()=="hi");
}

TEMPLATE_TEST_CASE("checked readers enforce node depth and malformed payloads consistently", "[pssz][optional-suffix]", psio::pssz8, psio::pssz16, psio::pssz32) {
 using F=TestType;bool unknown=false;
 auto depth63=psio::encode(F{},node(31));REQUIRE(psio::validate<DepthNode>(F{},depth63).ok());
 auto depth64=psio::encode(F{},std::optional<DepthNode>{node(31)});
 REQUIRE(psio::validate<std::optional<DepthNode>>(F{},depth64).ok());
 auto depth65=invalid_depth_bytes<F::offset_width>(32);
 REQUIRE_FALSE(psio::validate<DepthNode>(F{},depth65).ok());
 REQUIRE_THROWS_AS(psio::encode(F{},node(32)),psio::codec_exception);
 REQUIRE_FALSE(psio::validate<bool>(F{},std::vector<char>{2}).ok());
 REQUIRE_FALSE(psio::validate<bool>(F{},std::vector<char>{0,0}).ok());
 REQUIRE_FALSE(psio::validate<std::vector<bool>>(F{},std::vector<char>{0,2}).ok());
 using Bools=std::array<std::array<bool,1>,1>;
 REQUIRE_FALSE(psio::validate<Bools>(F{},std::vector<char>{2}).ok());
 REQUIRE_FALSE(psio::validate<std::vector<Unit>>(F{},std::vector<char>{1}).ok());
 REQUIRE_THROWS_AS(psio::encode(F{},std::vector<Unit>(1)),psio::codec_exception);
 using Units=std::array<Unit,2>;
 REQUIRE(psio::validate<Units>(F{},std::vector<char>{}).ok());
 REQUIRE_FALSE(psio::validate<std::vector<std::string>>(F{},std::vector<char>(F::offset_width,0)).ok());
 // A plain String is Vector(u8) carrying raw bytes: default validation passes
 // them through, strict validation does too (a raw string is any bytes), and the utf8_string
 // wrapper carries the constraint in its type (rejected in both modes).
 const std::vector<char> not_utf8{char(0xc0),char(0x80)};
 REQUIRE(psio::validate<std::string>(F{},not_utf8).ok());
 REQUIRE(psio::decode<std::string>(F{},not_utf8).size()==2);
 REQUIRE(psio::validate_strict<std::string>(F{},not_utf8).ok());
 REQUIRE_FALSE(psio::validate<psio::utf8_string<8>>(F{},not_utf8).ok());
 REQUIRE_FALSE(psio::validate_strict<psio::utf8_string<8>>(F{},not_utf8).ok());
 REQUIRE_THROWS_AS(psio_test::decode_validated<psio::utf8_string<8>>(F{},not_utf8),psio::codec_exception);
 REQUIRE_THROWS_AS(psio_test::encode_validated(F{}, psio::utf8_string<8>{std::string(not_utf8.begin(),not_utf8.end())}),psio::codec_exception);
 const auto good_utf8=psio::encode(F{},psio::utf8_string<8>{std::string("h\xc3\xa9")});
 REQUIRE(psio::validate<psio::utf8_string<8>>(F{},good_utf8).ok());
 REQUIRE(psio::validate_strict<psio::utf8_string<8>>(F{},good_utf8).ok());
 REQUIRE(psio::decode<psio::utf8_string<8>>(F{},good_utf8).storage()=="h\xc3\xa9");
 REQUIRE_FALSE(psio::validate<psio::utf8_string<2>>(F{},good_utf8).ok());
 REQUIRE(psio::validate_strict<std::string>(F{},good_utf8).ok());
 REQUIRE_FALSE(psio::validate<psio::bitvector<1>>(F{},std::vector<char>{2}).ok());
 REQUIRE_FALSE(psio::validate<psio::bitlist<16>>(F{},std::vector<char>{1,0}).ok());
 auto bytes=psio::encode(F{},OddExtended{true,"a","b"});
 for(std::size_t length=0;length<=bytes.size();++length) {
  std::span<const char> input(bytes.data(),length);OddBase value;bool decode_unknown=false,view_unknown=false;
  auto status=psio::decode_pssz_checked(F{},input,value,decode_unknown);
  std::optional<psio::view<OddBase,F,psio::storage::const_borrow>> view;
  auto viewed=psio::validated_pssz_view<OddBase>(F{},input,view,view_unknown);
  REQUIRE(status.ok()==viewed.ok());if(status.ok()) { REQUIRE(decode_unknown==view_unknown); REQUIRE(value.name==view->template get<1>().view_()); }
 }
}

TEST_CASE("fixed buffer bounds reject overflow before pointer arithmetic", "[stream][bounds]") {
 char buffer[4]{};psio::fixed_buf_stream stream(buffer,sizeof(buffer));
 REQUIRE_THROWS_AS(stream.write(buffer,std::numeric_limits<std::size_t>::max()),psio::codec_exception);
 REQUIRE_THROWS_AS(stream.rewrite(std::numeric_limits<std::size_t>::max(),buffer,1),psio::codec_exception);
 REQUIRE_THROWS_AS(stream.rewrite(3,buffer,2),psio::codec_exception);
 REQUIRE_THROWS_AS(stream.skip(std::numeric_limits<std::int32_t>::min()),psio::codec_exception);
 REQUIRE_THROWS_AS(stream.skip(std::numeric_limits<std::int32_t>::max()),psio::codec_exception);
 stream.write("abcd",4);REQUIRE(stream.remaining()==0);stream.skip(-4);REQUIRE(stream.written()==0);
 psio::fixed_buf_stream empty(nullptr,0);empty.write(nullptr,0);REQUIRE(empty.written()==0);
 psio::fast_buf_stream fast_empty(nullptr,0);fast_empty.write(nullptr,0);REQUIRE(fast_empty.written()==0);
 REQUIRE_THROWS_AS(psio::encode(psio::pssz8{},std::vector<std::string>{std::string(256,'a'),"b"}),psio::codec_exception);
}
