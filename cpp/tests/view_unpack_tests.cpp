// From a view to its value: `v.unpack()` on every kind of view,
// `psio::value_of(x)` for code that holds a view or a value, and
// `psio::from_pssz<T>(view)` / `psio::decode<T>(psio::pssz{}, view)`.
//
// The cases under a cap are the ones the spelled-out form
// `psio::from_pssz<T>(psio::trusted{psio::bytes(sub_view)})` gets wrong: the
// bytes of a member of a capped record are not a standalone encoding of the
// member's type.

#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <catch.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace unpk
{
   struct Act
   {
      std::uint64_t sender = 0;
      std::string   name;
      friend bool   operator==(const Act&, const Act&) = default;
   };
   PSIO_REFLECT(Act, sender, name)

   struct Inner
   {
      std::uint32_t a = 0;
      std::string   s;
      friend bool   operator==(const Inner&, const Inner&) = default;
   };
   PSIO_REFLECT(Inner, a, s)

   enum class Color : std::uint16_t
   {
      red,
      green = 7
   };

   struct Account
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Account&, const Account&) = default;
   };
   PSIO_REFLECT(Account, value, transparent())

   struct Fixed
   {
      std::uint32_t a = 0;
      std::uint16_t b = 0;
      friend bool   operator==(const Fixed&, const Fixed&) = default;
   };
   PSIO_REFLECT(Fixed, a, b, definitionWillNotChange())

   using Var = std::variant<std::uint32_t, std::string, Inner>;

   struct Rec
   {
      std::uint64_t                         id = 0;
      std::string                           name;
      std::vector<Inner>                    items;
      std::optional<Inner>                  opt;
      std::vector<char>                     raw;
      Var                                   var;
      std::optional<std::uint32_t>          num;
      std::vector<std::uint32_t>            nums;
      std::tuple<std::uint16_t, std::string> tup;
      std::pair<std::uint8_t, Inner>        pr;
      std::map<std::string, std::uint32_t>  map;
      std::set<std::uint16_t>               set;
      std::array<std::string, 2>            arr;
      psio::nested<Inner>                   nested;
      Color                                 color = Color::red;
      Account                               account;
      Fixed                                 fixed;
      std::chrono::seconds                  when{0};
      std::optional<std::string>            last;
      friend bool operator==(const Rec&, const Rec&) = default;
   };
   PSIO_REFLECT(Rec, id, name, items, opt, raw, var, num, nums, tup, pr, map, set, arr, nested,
                color, account, fixed, when, last)

   inline Rec sample()
   {
      return Rec{7,
                 "nm",
                 {{1, "a"}, {2, "b"}},
                 Inner{3, "c"},
                 {'x', 'y'},
                 std::string("vv"),
                 5u,
                 {9, 8},
                 {std::uint16_t{4}, "tu"},
                 {std::uint8_t{6}, Inner{10, "pr"}},
                 {{"k1", 1}, {"k2", 2}},
                 {3, 1, 2},
                 {"a0", "a1"},
                 psio::nested<Inner>::from(Inner{11, "ne"}),
                 Color::green,
                 Account{99},
                 Fixed{12, 13},
                 std::chrono::seconds{77},
                 std::nullopt};
   }

   // A record with a cap: the offsets of what it holds are narrower than
   // those of the same types standing alone.
   struct Row
   {
      std::uint32_t                           id = 0;
      std::vector<std::string>                tags;
      std::vector<std::vector<std::string>>   groups;
      std::optional<std::vector<std::string>> maybe;
      std::tuple<std::string, std::vector<std::string>> tup;
      Inner                                   inner;
      std::map<std::string, std::vector<std::string>> index;
      friend bool operator==(const Row&, const Row&) = default;
   };
   PSIO_REFLECT(Row, id, tags, groups, maybe, tup, inner, index, maxDynamicData(1024))

   // A member bounded by its own annotations.
   struct Bounded
   {
      std::uint32_t            id = 0;
      std::vector<std::string> names;
      std::vector<Inner>       inners;
      friend bool operator==(const Bounded&, const Bounded&) = default;
   };
   PSIO_REFLECT(Bounded, id, attr(names, max<4> | element(max<16>)), attr(inners, max<3>))

   // What a generic reader of a record's members looks like: one spelling
   // for members a view returns by value and members it returns as views.
   template <typename V>
   auto all_values(const V& v)
   {
      return std::tuple{psio::value_of(v.template get<0>()), psio::value_of(v.template get<1>()),
                        psio::value_of(v.template get<2>())};
   }
}  // namespace unpk

using namespace unpk;

TEST_CASE("unpack: record, string and nested record (probes g2_unpack, g26)", "[view][unpack]")
{
   auto                 b = psio::to_pssz(Act{7, "x"});
   psio::pssz_view<Act> v{b};
   Act                  a = v.unpack();
   REQUIRE(a == Act{7, "x"});
   std::string s = v.get<1>().unpack();
   REQUIRE(s == "x");
   REQUIRE(v.name().unpack() == "x");
   STATIC_REQUIRE(std::is_same_v<decltype(v.unpack()), Act>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.name().unpack()), std::string>);

   // The const spelling of the view unpacks to the same (non-const) value.
   psio::view<const Act, psio::pssz> cv{b};
   STATIC_REQUIRE(std::is_same_v<decltype(cv.unpack()), Act>);
   REQUIRE(cv.unpack() == a);
}

TEST_CASE("unpack: every kind of member view returns the member's value", "[view][unpack]")
{
   const Rec            r = sample();
   auto                 b = psio::to_pssz(r);
   psio::pssz_view<Rec> v{b};

   REQUIRE(v.unpack() == r);
   REQUIRE(v.name().unpack() == r.name);
   REQUIRE(v.items().unpack() == r.items);
   REQUIRE(v.items()[1].unpack() == r.items[1]);
   REQUIRE(v.opt().unpack() == r.opt);
   REQUIRE((*v.opt()).unpack() == *r.opt);
   REQUIRE(v.raw().unpack() == r.raw);
   REQUIRE(v.var().unpack() == r.var);
   REQUIRE(v.num().unpack() == r.num);
   REQUIRE((*v.num()).unpack() == 5u);
   REQUIRE(v.nums().unpack() == r.nums);
   REQUIRE(v.nums()[1].unpack() == 8u);
   REQUIRE(v.tup().unpack() == r.tup);
   REQUIRE(v.pr().unpack() == r.pr);
   REQUIRE(v.pr().second().unpack() == r.pr.second);
   REQUIRE(v.map().unpack() == r.map);
   REQUIRE(v.map()[0].unpack() == std::tuple{std::string("k1"), std::uint32_t{1}});
   REQUIRE(v.set().unpack() == r.set);
   REQUIRE(v.arr().unpack() == r.arr);
   REQUIRE(v.nested().unpack() == r.nested);
   REQUIRE(v.nested()->unpack() == Inner{11, "ne"});
   REQUIRE(v.color().unpack() == Color::green);
   REQUIRE(v.account().unpack() == Account{99});
   REQUIRE(v.fixed().unpack() == r.fixed);
   REQUIRE(v.when().unpack() == std::chrono::seconds{77});
   REQUIRE(v.last().unpack() == std::nullopt);
   // The active alternative of a variant.
   bool visited = false;
   v.var().visit([&](auto alt) {
      if constexpr (std::is_same_v<decltype(alt.unpack()), std::string>)
      {
         visited = alt.unpack() == "vv";
      }
   });
   REQUIRE(visited);

   STATIC_REQUIRE(std::is_same_v<decltype(v.items().unpack()), std::vector<Inner>>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.opt().unpack()), std::optional<Inner>>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.var().unpack()), Var>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.account().unpack()), Account>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.color().unpack()), Color>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.map().unpack()), std::map<std::string, std::uint32_t>>);
}

TEST_CASE("unpack: an absent trailing optional the writer pruned", "[view][unpack]")
{
   struct Unused
   {
   };
   Rec r  = sample();
   r.last = std::nullopt;
   auto                 b = psio::to_pssz(r);
   psio::pssz_view<Rec> v{b};
   REQUIRE_FALSE(v.last().has_value());
   REQUIRE(v.last().unpack() == std::nullopt);
   r.last = "set";
   auto                 b2 = psio::to_pssz(r);
   psio::pssz_view<Rec> v2{b2};
   REQUIRE(v2.last().unpack() == std::optional<std::string>{"set"});
   (void)sizeof(Unused);
}

TEST_CASE("value_of: one spelling for a view and for a value (probe g2c)", "[view][unpack]")
{
   auto                 b = psio::to_pssz(Act{7, "x"});
   psio::pssz_view<Act> v{b};
   // get<0>() is a std::uint64_t, get<1>() a view: generic code treats both alike.
   auto sender = psio::value_of(v.get<0>());
   auto name   = psio::value_of(v.get<1>());
   STATIC_REQUIRE(std::is_same_v<decltype(sender), std::uint64_t>);
   STATIC_REQUIRE(std::is_same_v<decltype(name), std::string>);
   REQUIRE(sender == 7);
   REQUIRE(name == "x");
   REQUIRE(psio::value_of(v) == Act{7, "x"});
   // A value is returned as it is (copied / moved, never a reference).
   std::string owned = "o";
   STATIC_REQUIRE(std::is_same_v<decltype(psio::value_of(owned)), std::string>);
   STATIC_REQUIRE(std::is_same_v<decltype(psio::value_of(std::as_const(owned))), std::string>);
   REQUIRE(psio::value_of(owned) == "o");
   REQUIRE(psio::value_of(5) == 5);

   const Rec            r  = sample();
   auto                 rb = psio::to_pssz(r);
   psio::pssz_view<Rec> rv{rb};
   auto [id, nm, items] = all_values(rv);
   REQUIRE(id == r.id);
   REQUIRE(nm == r.name);
   REQUIRE(items == r.items);

   // A mutable view: the value it designates.
   std::vector<char>            w = b;
   psio::pssz_mutable_view<Act> mv{w};
   REQUIRE(psio::value_of(mv) == Act{7, "x"});
   REQUIRE(psio::value_of(mv.sender()) == 7);
   REQUIRE(mv.unpack() == Act{7, "x"});
}

TEST_CASE("from_pssz / decode take a view (probes g2b, g2d)", "[view][unpack]")
{
   auto                 b = psio::to_pssz(Act{7, "x"});
   psio::pssz_view<Act> v{b};
   Act                  a = psio::decode<Act>(psio::pssz{}, v);
   Act                  c = psio::from_pssz<Act>(v);
   REQUIRE(a == Act{7, "x"});
   REQUIRE(c == a);
   REQUIRE(psio::from_pssz<std::string>(v.name()) == "x");
   REQUIRE(psio::decode<std::string>(psio::pssz{}, v.name()) == "x");
   std::string_view sv = v.get<1>();   // the characters, borrowed
   REQUIRE(sv == "x");
   REQUIRE(v.get<1>() == "x");
}

TEST_CASE("unpack under a cap: member bytes are not standalone encodings "
          "(probe g2_subview_decode_capped)",
          "[view][unpack][cap]")
{
   Row r{1,
         {"aa", "bbb"},
         {{"g1", "g22"}, {}, {"g3"}},
         std::vector<std::string>{"m"},
         {"t", {"u", "vv"}},
         Inner{5, "in"},
         {{"k", {"v1", "v2"}}}};
   auto                 b = psio::to_pssz(r);
   psio::pssz_view<Row> v{b};

   // What makes the spelled-out form wrong: the member's bytes differ from
   // the standalone encoding of its type, and do not even validate as it.
   auto tags = v.tags();
   REQUIRE(psio::bytes(tags).size() != psio::to_pssz(r.tags).size());
   REQUIRE_FALSE(psio::validate_pssz<std::vector<std::string>>(psio::bytes(tags)).ok());

   // unpack() knows the position's widths.
   REQUIRE(tags.unpack() == r.tags);
   REQUIRE(tags[1].unpack() == "bbb");
   REQUIRE(psio::value_of(tags) == r.tags);
   REQUIRE(psio::from_pssz<std::vector<std::string>>(tags) == r.tags);
   REQUIRE(psio::decode<std::vector<std::string>>(psio::pssz{}, tags) == r.tags);
   REQUIRE(v.groups().unpack() == r.groups);
   REQUIRE(v.groups()[0].unpack() == r.groups[0]);
   REQUIRE(v.groups()[1].unpack().empty());
   REQUIRE(v.maybe().unpack() == r.maybe);
   REQUIRE((*v.maybe()).unpack() == *r.maybe);
   REQUIRE(v.tup().unpack() == r.tup);
   REQUIRE(v.tup().get<1>().unpack() == std::get<1>(r.tup));
   REQUIRE(v.inner().unpack() == r.inner);
   REQUIRE(v.index().unpack() == r.index);
   REQUIRE(v.index()[0].get<1>().unpack() == r.index.at("k"));
   REQUIRE(v.unpack() == r);

   // An absent optional under the cap.
   Row r2   = r;
   r2.maybe = std::nullopt;
   auto                 b2 = psio::to_pssz(r2);
   psio::pssz_view<Row> v2{b2};
   REQUIRE(v2.maybe().unpack() == std::nullopt);
   REQUIRE(v2.unpack() == r2);
}

TEST_CASE("unpack under member annotations (bounded lists)", "[view][unpack][cap]")
{
   Bounded                  r{3, {"one", "two", "three"}, {{1, "a"}, {2, "bb"}}};
   auto                     b = psio::to_pssz(r);
   psio::pssz_view<Bounded> v{b};
   auto                     names = v.names();
   // A list bounded by max<4> of strings bounded by max<16>: 1-byte offsets.
   REQUIRE(psio::bytes(names).size() != psio::to_pssz(r.names).size());
   REQUIRE(names.unpack() == r.names);
   REQUIRE(names[2].unpack() == "three");
   REQUIRE(v.inners().unpack() == r.inners);
   REQUIRE(v.inners()[1].unpack() == r.inners[1]);
   REQUIRE(v.unpack() == r);
}

TEST_CASE("unpack: explicit-width views", "[view][unpack]")
{
   const Rec r = sample();
   for (int width : {1, 2, 4})
   {
      (void)width;
   }
   auto b2 = psio::encode(psio::pssz_<2>{}, r);
   psio::view<Rec, psio::pssz_<2>> v2{b2};
   REQUIRE(v2.unpack() == r);
   REQUIRE(v2.items().unpack() == r.items);
   REQUIRE(v2.map().unpack() == r.map);
   auto b4 = psio::encode(psio::pssz_<4>{}, r);
   psio::view<Rec, psio::pssz_<4>> v4{b4};
   REQUIRE(v4.unpack() == r);
   REQUIRE(v4.tup().unpack() == r.tup);
}
