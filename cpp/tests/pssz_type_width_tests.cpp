#include "pssz_type_width_fixture.hpp"
#include <psio/pssz_view.hpp>
#include <catch.hpp>
#include "pssz_type_width_explicit_goldens.hpp"

template<typename T, typename F=psio::pssz>
auto width_view(const std::vector<char>& bytes)
{
   return psio::view<T,F,psio::storage::const_borrow>{bytes};
}
std::vector<char> hex_bytes(std::string_view hex)
{
   std::vector<char> out;
   const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
   for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
      out.push_back(static_cast<char>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
   return out;
}
bool equal_bytes(std::span<const char> a, std::span<const char> b)
{
   return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin());
}
std::string bytes_hex(std::span<const char> bytes)
{
   constexpr char digits[]="0123456789abcdef";
   std::string result;
   for(unsigned char byte:bytes) { result+=digits[byte>>4];result+=digits[byte&15]; }
   return result;
}
template<typename F,typename T>
void check_explicit(const char* name,const T& value)
{
   const auto key=std::string(name)+"-"+std::to_string(F::offset_width);
   auto entry=std::find_if(width_explicit_goldens.begin(),width_explicit_goldens.end(),
                           [&](auto item){return item.first==key;});
   REQUIRE(entry!=width_explicit_goldens.end());
   auto wire=psio::encode(F{},value);
   REQUIRE(bytes_hex(wire)==entry->second);
   REQUIRE(psio::size_of(F{},value)==wire.size());
   REQUIRE(psio::validate<T>(F{},wire).ok());
   REQUIRE_NOTHROW(psio::validate_or_throw<T>(F{},wire));
   REQUIRE(psio::encode(F{},psio::decode<T>(F{},wire))==wire);
}
TEMPLATE_TEST_CASE("existing explicit PSSZ width profiles preserve published552ca bytes",
                   "[pssz][type-width][legacy]",psio::pssz8,psio::pssz16,psio::pssz32)
{
   auto value=width_sample();
   check_explicit<TestType>("relay",value);
   check_explicit<TestType>("vector",value.entries);
   check_explicit<TestType>("array",value.pair);
   check_explicit<TestType>("some",value.latest);
   check_explicit<TestType>("none",std::optional<WidthCheckpoint>{});
   check_explicit<TestType>("variant-record",value.proof);
   check_explicit<TestType>("variant-number",std::variant<WidthCheckpoint,std::uint32_t>{std::in_place_index<1>,42});
   check_explicit<TestType>("narrow-parent",WidthNarrowEnvelope{{42},17});
}

TEST_CASE("automatic PSSZ keeps canonical child bytes inside wider records", "[pssz][type-width]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthCheckpoint> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthRelayBatch> == 4);
   auto value=width_sample();
   auto wire=psio::encode(psio::pssz{},value);
   REQUIRE(wire.size()==psio::size_of(psio::pssz{},value));
   REQUIRE(psio::validate<WidthRelayBatch>(psio::pssz{},wire).ok());
   REQUIRE_NOTHROW(psio::validate_or_throw<WidthRelayBatch>(psio::pssz{},wire));
   auto root=width_view<WidthRelayBatch>(wire);
   auto child=root.get<0>();
   REQUIRE(equal_bytes(psio::bytes(child),psio::encode(psio::pssz{},value.checkpoint)));
   REQUIRE(child.get<0>()==7);
   REQUIRE(child.get<1>().view_()=="main");
   auto entries=root.get<1>();
   REQUIRE(equal_bytes(psio::bytes(entries),psio::encode(psio::pssz{},value.entries)));
   REQUIRE(entries.size()==2);
   REQUIRE(entries[1].get<0>()==9);
   REQUIRE(entries[1].get<1>().view_()=="two");
   auto latest=root.get<2>();
   REQUIRE(latest.has_value());
   REQUIRE((*latest).get<0>()==10);
   REQUIRE(equal_bytes(psio::bytes(*latest),psio::encode(psio::pssz{},*value.latest)));
   auto pair=root.get<3>();
   REQUIRE(pair.size()==2);
   REQUIRE(pair[1].get<1>().view_()=="right");
   REQUIRE(equal_bytes(psio::bytes(pair),psio::encode(psio::pssz{},value.pair)));
   unsigned height_sum=0;
   for(auto element:pair) height_sum+=element.get<0>();
   REQUIRE(height_sum==23);
   REQUIRE(root.get<4>().visit([](const auto& active)->unsigned {
      using T=typename std::remove_cvref_t<decltype(active)>::element_type;
      if constexpr (std::is_same_v<T,WidthCheckpoint>) return active.template get<0>();
      else return active.get();
   })==13);
   auto back=psio::decode<WidthRelayBatch>(psio::pssz{},wire);
   REQUIRE(back.checkpoint.label=="main");
   REQUIRE(back.entries[1].label=="two");
   REQUIRE(back.latest->label=="last");
   REQUIRE(back.pair[1].label=="right");
   REQUIRE(std::get<WidthCheckpoint>(back.proof).label=="proof");
   REQUIRE(psio::encode(psio::pssz{},back)==wire);
   REQUIRE(psio::validate_strict<WidthRelayBatch>(psio::pssz{},wire).ok());
   value.latest.reset();
   auto absent=psio::encode(psio::pssz{},value);
   REQUIRE(psio::validate<WidthRelayBatch>(psio::pssz{},absent).ok());
   REQUIRE_FALSE(psio::decode<WidthRelayBatch>(psio::pssz{},absent).latest.has_value());
   REQUIRE_FALSE(width_view<WidthRelayBatch>(absent).get<2>().has_value());
}

struct WidthNestedContainers
{
   std::vector<std::vector<WidthCheckpoint>> nested;
   std::vector<std::optional<WidthCheckpoint>> optional;
};
PSIO_REFLECT(WidthNestedContainers,nested,optional,maxDynamicData(200))
TEST_CASE("nested containers retain their own tables and canonical record payloads", "[pssz][type-width]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthNestedContainers> == 1);
   STATIC_REQUIRE((psio::auto_pssz_width_v<std::vector<WidthCheckpoint>> == 4));
   WidthNestedContainers value{{{{1,"a"}},{{2,"b"}}},{WidthCheckpoint{3,"c"},std::nullopt}};
   auto wire=psio::encode(psio::pssz{},value);
   REQUIRE(psio::validate<WidthNestedContainers>(psio::pssz{},wire).ok());
   REQUIRE(psio::decode<WidthNestedContainers>(psio::pssz{},wire).nested[1][0].label=="b");
   auto root=width_view<WidthNestedContainers>(wire);
   // Beneath the record's cap of 200 the lists have one-byte tables
   // (docs/pssz-spec.md 3.6): table 02 0a of the two rows, each row its own
   // table 01 and one checkpoint (one-byte offsets: its own cap). On their
   // own the same lists are unbounded and have four-byte tables.
   REQUIRE(equal_bytes(psio::bytes(root.get<0>()),
                       hex_bytes("020a" "01" "050100000005" "61" "01" "050200000005" "62")));
   REQUIRE_FALSE(equal_bytes(psio::bytes(root.get<0>()),psio::encode(psio::pssz{},value.nested)));
   REQUIRE(root.get<0>()[1][0].get<0>()==2);
   REQUIRE(root.get<1>()[0].has_value());
   REQUIRE((*root.get<1>()[0]).get<1>().view_()=="c");
   REQUIRE_FALSE(root.get<1>()[1].has_value());
}

TEST_CASE("automatic widths do not depend on current payload lengths", "[pssz][type-width]")
{
   for(unsigned n=0;n<128;++n)
   {
      auto value=width_sample();
      value.checkpoint.label=std::string(n%17,'x');
      value.entries.resize(n%5,WidthCheckpoint{42,"item"});
      if(n%2) value.latest.reset();
      if(n%3) value.proof=std::uint32_t{n};
      value.context=std::string(n,'c');
      auto wire=psio::encode(psio::pssz{},value);
      REQUIRE(psio::validate<WidthRelayBatch>(psio::pssz{},wire).ok());
      REQUIRE(psio::encode(psio::pssz{},psio::decode<WidthRelayBatch>(psio::pssz{},wire))==wire);
      REQUIRE(equal_bytes(psio::bytes(width_view<WidthRelayBatch>(wire).get<0>()),psio::encode(psio::pssz{},value.checkpoint)));
      REQUIRE(width_view<WidthRelayBatch>(wire).get<0>().get<1>().view_()==value.checkpoint.label);
   }
}

TEST_CASE("automatic PSSZ keeps a wide child in a narrow parent", "[pssz][type-width]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthLargeBudget> == 4);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthNarrowEnvelope> == 1);
   WidthNarrowEnvelope value{{42},17};
   auto wire=psio::encode(psio::pssz{},value);
   REQUIRE(psio::validate<WidthNarrowEnvelope>(psio::pssz{},wire).ok());
   REQUIRE(equal_bytes(psio::bytes(width_view<WidthNarrowEnvelope>(wire).get<0>()),
                       psio::encode(psio::pssz{},value.child)));
   REQUIRE(width_view<WidthNarrowEnvelope>(wire).get<0>().get<0>()==42);
   REQUIRE(psio::decode<WidthNarrowEnvelope>(psio::pssz{},wire).sequence==17);
}

TEST_CASE("declared budgets reserve child growth and inference counts wire overhead", "[pssz][type-width]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthReserveParent> == 4);
   STATIC_REQUIRE(psio::max_encoded_size<WidthReserveParent>() == std::optional<std::size_t>{1000008});
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthGrowV1> == 2);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthGrowV2> == 2);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthGrowParentV1> == 2);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthGrowParentV2> == 2);
   STATIC_REQUIRE(psio::max_encoded_size<WidthGrowParentV1>() == psio::max_encoded_size<WidthGrowParentV2>());
   // The header and slots count at the type's own width (pssz-spec 3.2 fit):
   // 1 + 251, 1 + 252, 1 + 254 = 255, then 2 + 255.
   STATIC_REQUIRE(psio::max_encoded_size<WidthThreshold251>() == std::optional<std::size_t>{252});
   STATIC_REQUIRE(psio::max_encoded_size<WidthThreshold252>() == std::optional<std::size_t>{253});
   STATIC_REQUIRE(psio::max_encoded_size<WidthThreshold254>() == std::optional<std::size_t>{255});
   STATIC_REQUIRE(psio::max_encoded_size<WidthThreshold255>() == std::optional<std::size_t>{257});
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthThreshold251> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthThreshold252> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthThreshold254> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthThreshold255> == 2);
   // Four checkpoints of at most 64 bytes: 4 * (1 + 64) = 260 does not fit a
   // one-byte table, 4 * (2 + 64) = 264 does a two-byte one. The record that
   // holds the list adds its own header and slot: 2 + 2 + 264.
   STATIC_REQUIRE(psio::max_encoded_size<WidthBoundedBatch>() == std::optional<std::size_t>{268});
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthBoundedBatch> == 2);
   STATIC_REQUIRE((psio::max_encoded_size<std::array<WidthCheckpoint,4>>() == std::optional<std::size_t>{264}));
   STATIC_REQUIRE((psio::auto_pssz_width_v<std::array<WidthCheckpoint,4>> == 2));
   STATIC_REQUIRE_FALSE(psio::max_encoded_size<WidthOverflowParent>().has_value());
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthOverflowParent> == 4);
   STATIC_REQUIRE_FALSE((psio::max_encoded_size<std::array<WidthHugeBudget,2>>().has_value()));
   for(auto payload:{std::string{},std::string(16,'x')})
   {
      WidthBoundedBatch value{{{1,payload},{2,payload},{3,payload},{4,payload}}};
      auto wire=psio::encode(psio::pssz{},value);
      REQUIRE(wire.size()==psio::size_of(psio::pssz{},value));
      REQUIRE(psio::validate<WidthBoundedBatch>(psio::pssz{},wire).ok());
      REQUIRE(psio::decode<WidthBoundedBatch>(psio::pssz{},wire).entries[3].label==payload);
      REQUIRE(width_view<WidthBoundedBatch>(wire).get<0>()[3].get<1>().view_()==payload);
   }
}

TEST_CASE("automatic PSSZ rejects overflow without widening or partially appending", "[pssz][type-width]")
{
   WidthCappedPair oversized{std::string(260,'x'),"last"};
   std::vector<char> destination={'p','r','e'};
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{},oversized,destination),psio::codec_exception);
   REQUIRE((destination==std::vector<char>{'p','r','e'}));
   REQUIRE_THROWS_AS(psio::size_of(psio::pssz{},oversized),psio::codec_exception);
   WidthStringPair explicit_value{oversized.first,oversized.second};
   REQUIRE_THROWS_AS(psio::encode(psio::pssz8{},explicit_value),psio::codec_exception);
   auto wide=psio::encode(psio::pssz16{},explicit_value);
   REQUIRE(psio::validate<WidthStringPair>(psio::pssz16{},wide).ok());
   REQUIRE(psio::decode<WidthStringPair>(psio::pssz16{},wide).first==explicit_value.first);
   auto parent=width_sample();
   parent.checkpoint.label=std::string(100,'x');
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{},parent),psio::codec_exception);
   REQUIRE_THROWS_AS(psio::size_of(psio::pssz{},parent),psio::codec_exception);
}

struct WidthMediumPair { std::string first; std::string second; };
PSIO_REFLECT(WidthMediumPair,first,second,maxDynamicData(65535))
struct WidthLargeFixedRegion { std::array<std::uint8_t,65536> payload; };
PSIO_REFLECT(WidthLargeFixedRegion,payload)
struct WidthOnlyTail { std::string payload; };
PSIO_REFLECT(WidthOnlyTail,payload)
TEST_CASE("static width overflow rejects tables and headers at both narrow thresholds", "[pssz][type-width]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthMediumPair> == 2);
   WidthMediumPair medium{std::string(65536,'x'),"tail"};
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{},medium),psio::codec_exception);
   REQUIRE_THROWS_AS(psio::size_of(psio::pssz{},medium),psio::codec_exception);
   std::vector<std::string> table(256);
   REQUIRE_THROWS_AS(psio::encode(psio::pssz8{},table),psio::codec_exception);
   REQUIRE_THROWS_AS(psio::size_of(psio::pssz8{},table),psio::codec_exception);
   WidthLargeFixedRegion fixed{};
   REQUIRE_THROWS_AS(psio::encode(psio::pssz16{},fixed),psio::codec_exception);
   REQUIRE_THROWS_AS(psio::size_of(psio::pssz16{},fixed),psio::codec_exception);
   // Explicit profiles constrain stored offsets, not an unstored final end.
   WidthOnlyTail tail{std::string(512,'x')};
   auto wire=psio::encode(psio::pssz8{},tail);
   REQUIRE(psio::validate<WidthOnlyTail>(psio::pssz8{},wire).ok());
   REQUIRE(psio::decode<WidthOnlyTail>(psio::pssz8{},wire).payload==tail.payload);
}

struct WidthArrayEnvelope
{
   std::array<WidthCheckpoint,4> records;
   std::array<std::uint8_t,32> hash;
   std::string context;
};
PSIO_REFLECT(WidthArrayEnvelope,records,hash,context)
TEST_CASE("fixed-count array views use their own type width and preserve raw fixed bytes", "[pssz][type-width]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<WidthArrayEnvelope> == 4);
   STATIC_REQUIRE((psio::auto_pssz_width_v<std::array<WidthCheckpoint,4>> == 2));
   WidthArrayEnvelope value{{{{1,"a"},{2,"b"},{3,"c"},{4,"d"}}},{},"context"};
   for(std::size_t i=0;i<value.hash.size();++i) value.hash[i]=i;
   auto wire=psio::encode(psio::pssz{},value);
   REQUIRE(psio::validate<WidthArrayEnvelope>(psio::pssz{},wire).ok());
   REQUIRE_NOTHROW(psio::validate_or_throw<WidthArrayEnvelope>(psio::pssz{},wire));
   auto view=width_view<WidthArrayEnvelope>(wire);
   REQUIRE(equal_bytes(psio::bytes(view.get<0>()),psio::encode(psio::pssz{},value.records)));
   REQUIRE(view.get<0>()[3].get<1>().view_()=="d");
   REQUIRE(view.get<1>().size()==32);
   REQUIRE(view.get<1>()[31].get()==31);
   REQUIRE(psio::decode<WidthArrayEnvelope>(psio::pssz{},wire).hash==value.hash);
   REQUIRE(psio::encode(psio::pssz{},psio::decode<WidthArrayEnvelope>(psio::pssz{},wire))==wire);
}

TEST_CASE("nested fixed budgets cannot be skipped by memcpy fast paths", "[pssz][type-width]")
{
   WidthBadFixedParent parent{{1},"tail"};
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{},parent),psio::codec_exception);
   std::vector<WidthBadFixed> nonempty{{1}};
   REQUIRE_THROWS_AS(psio::encode(psio::pssz{},nonempty),psio::codec_exception);
   std::vector<char> invalid(8,0);
   REQUIRE_FALSE(psio::validate<std::vector<WidthBadFixed>>(psio::pssz{},invalid).ok());
   REQUIRE_THROWS_AS(psio::decode<std::vector<WidthBadFixed>>(psio::pssz{},invalid),psio::codec_exception);
   std::vector<WidthBadFixed> empty;
   auto wire=psio::encode(psio::pssz{},empty);
   REQUIRE(wire.empty());
   REQUIRE(psio::validate<std::vector<WidthBadFixed>>(psio::pssz{},wire).ok());
   REQUIRE(psio::decode<std::vector<WidthBadFixed>>(psio::pssz{},wire).empty());
}

TEST_CASE("nested automatic validation and decode enforce the child's budget", "[pssz][type-width]")
{
   auto wire=psio::encode(psio::pssz{},width_sample());
   auto child=width_view<WidthRelayBatch>(wire).get<0>();
   const auto insert=std::size_t(psio::bytes(child).data()-wire.data())+psio::size(child);
   wire.insert(wire.begin()+insert,100,'x');
   // All six relay fields are variable: shift the later root4-byte offsets.
   for(std::size_t i=1;i<6;++i)
   {
      std::uint32_t offset;
      std::memcpy(&offset,wire.data()+4+i*4,4);
      offset+=100;
      std::memcpy(wire.data()+4+i*4,&offset,4);
   }
   REQUIRE_FALSE(psio::validate<WidthRelayBatch>(psio::pssz{},wire).ok());
   REQUIRE_FALSE(psio::validate_strict<WidthRelayBatch>(psio::pssz{},wire).ok());
   REQUIRE_THROWS_AS(psio::validate_or_throw<WidthRelayBatch>(psio::pssz{},wire),psio::codec_exception);
   REQUIRE_THROWS_AS(psio::decode<WidthRelayBatch>(psio::pssz{},wire),psio::codec_exception);
}
