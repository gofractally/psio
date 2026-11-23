// Named accessors on record views: every reflected member of T is a member
// function of psio::view<T, Fmt> and of psio::mutable_view<T, Fmt>, by its
// name. (The view derives from reflect<T>::proxy<...>; `get<N>()` is the
// same access by index.)
//
// The "psiserve" cases are the view tests of the in-tree psio the named
// readers come from, adapted to PSSZ.

#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace named
{
   struct Act
   {
      std::uint64_t sender = 0;
      std::string   name;
   };
   PSIO_REFLECT(Act, sender, name)

   struct Hdr
   {
      std::uint32_t num = 0;
      std::string   producer;
   };
   PSIO_REFLECT(Hdr, num, producer)

   struct Blk
   {
      Hdr                header;
      std::vector<Hdr>   prev;
      std::optional<Hdr> next;
   };
   PSIO_REFLECT(Blk, header, prev, next)

   enum class Kind : std::uint8_t
   {
      plain,
      fancy
   };

   // A transparent wrapper: on the wire, and through a view, it is its member.
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
   };
   PSIO_REFLECT(Fixed, a, b, definitionWillNotChange())

   struct Mixed
   {
      Account                                  owner;
      Kind                                     kind = Kind::plain;
      Fixed                                    fixed;
      std::variant<std::uint32_t, std::string> choice;
      psio::nested<Act>                        inner;
      std::tuple<std::uint16_t, std::string>   pair;
      std::uint64_t                            total = 0;
   };
   PSIO_REFLECT(Mixed, owner, kind, fixed, choice, inner, pair, total)

   // A record under a cap, and a member whose annotations narrow its widths:
   // the member views are psio::pssz_at<...> views.
   struct Capped
   {
      std::uint32_t            id = 0;
      std::vector<std::string> tags;
      Act                      act;
   };
   PSIO_REFLECT(Capped, id, tags, act, maxDynamicData(1024))

   // Members named like the view's own members: the view's win; the member
   // is read by index.
   struct Clash
   {
      std::uint32_t get    = 0;
      std::string   unpack;
      std::uint32_t size   = 0;
      std::string   data;
      std::uint16_t index  = 0;
      std::uint16_t set    = 0;
      std::uint8_t  empty  = 0;
      std::uint8_t  value  = 0;
      std::uint8_t  bytes  = 0;
   };
   PSIO_REFLECT(Clash, get, unpack, size, data, index, set, empty, value, bytes)

   class Private
   {
      std::uint32_t secret_ = 7;
      std::string   label_  = "p";

    public:
      PSIO_REFLECT(Private, secret_, label_)
   };

   struct Svc
   {
      std::uint32_t counter = 0;
      void          bump(std::uint32_t by) { counter += by; }
      PSIO_REFLECT(Svc, counter, method(bump, by))
   };

   struct Point
   {
      std::uint32_t x = 0;
      std::uint32_t y = 0;
   };
   PSIO_REFLECT(Point, x, y)
}  // namespace named

using namespace named;

TEST_CASE("record view: named accessors (probes g1_named, g24)", "[view][named]")
{
   auto                 b = psio::to_pssz(Act{7, "x"});
   psio::pssz_view<Act> v{b};
   REQUIRE(v.sender() == 7);
   REQUIRE(v.name().view_() == "x");
   // The same as by index, with the same types.
   STATIC_REQUIRE(std::is_same_v<decltype(v.sender()), std::uint64_t>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.sender()), decltype(v.get<0>())>);
   STATIC_REQUIRE(std::is_same_v<decltype(v.name()), decltype(v.get<1>())>);
   REQUIRE(v.get<0>() == 7);

   // psio::view<const T, Fmt> is the same view.
   psio::view<const Act, psio::pssz> cv{b};
   REQUIRE(cv.sender() == 7);
   REQUIRE(cv.name().view_() == "x");
   psio::pssz_view<const Act> hv{b};
   REQUIRE(hv.sender() == 7);
}

TEST_CASE("record view: chains through nested records, lists and optionals", "[view][named]")
{
   auto                 b = psio::to_pssz(Blk{{7, "p"}, {{1, "a"}, {2, "b"}}, Hdr{9, "n"}});
   psio::pssz_view<Blk> blk{b};
   REQUIRE(blk.header().num() == 7);
   REQUIRE(blk.header().producer().view_() == "p");
   REQUIRE(blk.prev().size() == 2);
   REQUIRE(blk.prev()[0].num() == 1);
   REQUIRE(blk.prev()[1].producer().view_() == "b");
   REQUIRE(blk.next().has_value());
   REQUIRE((*blk.next()).num() == 9);
   REQUIRE(blk.next()->producer().view_() == "n");
   std::uint32_t sum = 0;
   for (auto h : blk.prev())
      sum += h.num();
   REQUIRE(sum == 3);
}

TEST_CASE("record view: transparent, enum, DWNC, variant, nested and tuple members",
          "[view][named]")
{
   Mixed m{Account{42}, Kind::fancy, Fixed{5, 6}, std::string("alt"),
           psio::nested<Act>::from(Act{3, "in"}), {std::uint16_t{8}, "t"}, 99};
   auto                   b = psio::to_pssz(m);
   psio::pssz_view<Mixed> v{b};
   // A transparent member's view is its member's view: a scalar here.
   REQUIRE(v.owner().get() == 42);
   REQUIRE(v.owner().unpack() == Account{42});
   REQUIRE(v.kind().get() == Kind::fancy);
   REQUIRE(v.fixed().a() == 5);
   REQUIRE(v.fixed().b() == 6);
   REQUIRE(v.choice().index() == 1);
   REQUIRE(v.inner()->sender() == 3);
   REQUIRE((*v.inner()).name().view_() == "in");
   REQUIRE(v.pair().get<0>() == 8);
   REQUIRE(v.pair().get<1>().view_() == "t");
   REQUIRE(v.total() == 99);
}

TEST_CASE("record view: named accessors under a cap (pssz_at member views)", "[view][named]")
{
   Capped                  c{1, {"aa", "bbb"}, Act{4, "who"}};
   auto                    b = psio::to_pssz(c);
   psio::pssz_view<Capped> v{b};
   REQUIRE(v.id() == 1);
   auto tags = v.tags();
   // The member's widths are narrowed by the cap: not a plain pssz view.
   STATIC_REQUIRE(!std::is_same_v<decltype(tags)::format_type, psio::pssz>);
   REQUIRE(tags.size() == 2);
   REQUIRE(tags[1].view_() == "bbb");
   REQUIRE(v.act().sender() == 4);
   REQUIRE(v.act().name().view_() == "who");
}

TEST_CASE("record view: the view's own names win over a member of the same name",
          "[view][named]")
{
   Clash                  c{1, "u", 3, "d", 5, 6, 7, 8, 9};
   auto                   b = psio::to_pssz(c);
   psio::pssz_view<Clash> v{b};
   // get<N>() and unpack() are the view's.
   REQUIRE(v.get<0>() == 1);
   REQUIRE(v.get<1>().view_() == "u");
   REQUIRE(v.unpack().unpack == "u");
   // Everything else is the member's accessor: a record view has no
   // size() / data() / index() / set() / empty() / value() / bytes() of
   // its own.
   REQUIRE(v.size() == 3);
   REQUIRE(v.data().view_() == "d");
   REQUIRE(v.index() == 5);
   REQUIRE(v.set() == 6);
   REQUIRE(v.empty() == 7);
   REQUIRE(v.value() == 8);
   REQUIRE(v.bytes() == 9);
   // The index of a member the view's own name hides.
   constexpr auto i = *psio::reflect<Clash>::index_of("unpack");
   REQUIRE(v.get<i>().view_() == "u");

   std::vector<char>              w = b;
   psio::pssz_mutable_view<Clash> mv{w};
   REQUIRE(mv.get<0>().get() == 1);
   // On the mutable view `get`, `set` and `unpack` are the view's; the
   // members of those names are addressed by index.
   REQUIRE(mv.get<5>().get() == 6);
   REQUIRE(mv.size().get() == 3);
   REQUIRE(mv.index().get() == 5);
   REQUIRE(mv.empty().get() == 7);
   REQUIRE(mv.value().get() == 8);
}

TEST_CASE("record view: private members reflected in-class, and a type with methods",
          "[view][named]")
{
   auto                     b = psio::to_pssz(Private{});
   psio::pssz_view<Private> v{b};
   REQUIRE(v.secret_() == 7);
   REQUIRE(v.label_().view_() == "p");

   auto                 sb = psio::to_pssz(Svc{3});
   psio::pssz_view<Svc> sv{sb};
   REQUIRE(sv.counter() == 3);
}

TEST_CASE("record view: explicit-width and ssz views have the same names", "[view][named]")
{
   auto bw = psio::encode(psio::pssz_<2>{}, Act{7, "x"});
   psio::view<Act, psio::pssz_<2>> vw{bw};
   REQUIRE(vw.sender() == 7);
   REQUIRE(vw.name().view_() == "x");

   auto                  sb = psio::to_ssz(Point{3, 4});
   psio::ssz_view<Point> sv{sb};
   REQUIRE(sv.x() == 3);
   REQUIRE(sv.y() == 4);
}

TEST_CASE("mutable view: named sub-views and setters (probes g4_mut, g4_mut_gap)",
          "[mutable_view][named]")
{
   struct Local
   {
   };
   std::vector<char>            b = psio::to_pssz(Fixed{1, 2});
   psio::pssz_mutable_view<Fixed> w{b};
   REQUIRE(w.a().get() == 1);
   REQUIRE(w.a().set(42u));
   REQUIRE(psio::from_pssz<Fixed>(b).a == 42);
   w.b() = std::uint16_t{9};   // assignment syntax
   w.a() = 43;                 // a literal converts to the member's type
   REQUIRE(psio::from_pssz<Fixed>(b).a == 43);
   REQUIRE(psio::from_pssz<Fixed>(b).b == 9);
   w.get<0>() = 44u;
   REQUIRE(w.a().get() == 44);
   REQUIRE(w.a() == 44u);
   REQUIRE(w.unpack().a == 44);

   // Through nested records, lists, optionals and a transparent member.
   Mixed m{Account{42}, Kind::fancy, Fixed{5, 6}, std::uint32_t{1},
           psio::nested<Act>::from(Act{3, "in"}), {std::uint16_t{8}, "t"}, 99};
   std::vector<char>              mb = psio::to_pssz(m);
   psio::pssz_mutable_view<Mixed> mw{mb};
   mw.fixed().a() = 50;
   mw.owner() = Account{77};
   REQUIRE(mw.owner().set(Account{78}));
   mw.kind() = Kind::plain;
   mw.total() = 100;
   mw.inner()->sender() = 4;
   mw.pair().get<0>() = std::uint16_t{9};
   REQUIRE(mw.fixed().set(Fixed{51, 7}));
   auto back = psio::from_pssz<Mixed>(mb);
   REQUIRE(back.fixed.a == 51);
   REQUIRE(back.fixed.b == 7);
   REQUIRE(back.owner == Account{78});
   REQUIRE(back.kind == Kind::plain);
   REQUIRE(back.total == 100);
   REQUIRE(back.inner.decode().sender == 4);
   REQUIRE(std::get<0>(back.pair) == 9);
   // The frame is still the canonical encoding of the modified value.
   REQUIRE(mb == psio::to_pssz(back));

   std::vector<char>            bb = psio::to_pssz(Blk{{7, "p"}, {{1, "a"}}, Hdr{9, "n"}});
   psio::pssz_mutable_view<Blk> bw{bb};
   bw.header().num() = 8;
   bw.prev()[0].num() = 2;
   (*bw.next()).num() = 10;
   auto blk = psio::from_pssz<Blk>(bb);
   REQUIRE(blk.header.num == 8);
   REQUIRE(blk.prev[0].num == 2);
   REQUIRE(blk.next->num == 10);

   // The read view over the same bytes has the same names.
   REQUIRE(psio::as_view(bw).header().num() == 8);
   (void)sizeof(Local);
}

#if defined(__cpp_exceptions)
TEST_CASE("mutable view: assignment to an absent position fails loudly",
          "[mutable_view][named]")
{
   std::vector<char>            bb = psio::to_pssz(Blk{{7, "p"}, {}, std::nullopt});
   psio::pssz_mutable_view<Blk> bw{bb};
   const auto                   before = bb;
   // set() reports; `=` does not write nothing silently.
   REQUIRE_FALSE(bw.prev().at(3).num().set(1u));
   REQUIRE_THROWS_AS(bw.prev().at(3).num() = 1u, psio::codec_exception);
   REQUIRE(bb == before);
}
#endif

TEST_CASE("pjson typed view: named accessors", "[view][named][pjson]")
{
   struct Unused
   {
   };
   Act  a{7, "x"};
   auto bytes = psio::from_struct(a);
   psio::pjson_view raw{bytes.data(), bytes.size()};
   auto             v = psio::view<Act, psio::pjson_format>::from_pjson(raw);
   REQUIRE(v.sender() == 7);
   REQUIRE(v.name() == "x");
   REQUIRE(v.get<0>() == 7);
   REQUIRE(v.to_struct().name == "x");
   (void)sizeof(Unused);
}

// ── The psiserve view tests, adapted to PSSZ ────────────────────────────────

namespace psv
{
   struct VPoint
   {
      std::int32_t x;
      std::int32_t y;
   };
   PSIO_REFLECT(VPoint, definitionWillNotChange(), x, y)

   struct VCustomer
   {
      std::uint64_t id;
      std::string   name;
      std::string   email;
   };
   PSIO_REFLECT(VCustomer, id, name, email)

   struct VLineItem
   {
      std::string   product;
      std::uint32_t qty;
      double        unit_price;
   };
   PSIO_REFLECT(VLineItem, product, qty, unit_price)

   struct VOrder
   {
      std::uint64_t              id;
      VCustomer                  customer;
      std::vector<VLineItem>     items;
      double                     total;
      std::optional<std::string> note;
   };
   PSIO_REFLECT(VOrder, id, customer, items, total, note)

   struct VTagged
   {
      std::uint32_t          id;
      std::set<std::int32_t> tags;
   };
   PSIO_REFLECT(VTagged, id, tags)

   struct VConfig
   {
      std::uint32_t                        version;
      std::map<std::string, std::uint32_t> limits;
   };
   PSIO_REFLECT(VConfig, version, limits)
}  // namespace psv

using namespace psv;

TEST_CASE("psiserve: fixed-size struct", "[view][named][psiserve]")
{
   auto                    packed = psio::to_pssz(VPoint{42, -7});
   psio::pssz_view<VPoint> v{packed};
   REQUIRE(v.x() == 42);
   REQUIRE(v.y() == -7);
}

TEST_CASE("psiserve: struct with strings", "[view][named][psiserve]")
{
   auto packed = psio::to_pssz(VCustomer{99, "Alice", "alice@test.com"});
   psio::pssz_view<VCustomer> v{packed};
   REQUIRE(v.id() == 99);
   REQUIRE(v.name() == "Alice");
   REQUIRE(v.email() == "alice@test.com");
   std::string_view name = v.name();
   REQUIRE(name == "Alice");
}

TEST_CASE("psiserve: nested struct with vector", "[view][named][psiserve]")
{
   VOrder o{1, {10, "Bob", "bob@x.com"}, {{"A", 2, 5.0}, {"B", 1, 10.0}}, 20.0, "rush"};
   auto                    packed = psio::to_pssz(o);
   psio::pssz_view<VOrder> v{packed};

   REQUIRE(v.id() == 1);
   REQUIRE(v.total() == 20.0);
   REQUIRE(v.customer().name() == "Bob");

   auto items = v.items();
   REQUIRE(items.size() == 2);
   REQUIRE(items[0].product() == "A");
   REQUIRE(items[0].qty() == 2);
   REQUIRE(items[1].product() == "B");
   REQUIRE(items[1].unit_price() == 10.0);

   int count = 0;
   for (auto item : v.items())
   {
      REQUIRE(item.qty() == (count == 0 ? 2u : 1u));
      ++count;
   }
   REQUIRE(count == 2);
}

TEST_CASE("psiserve: optional present and absent", "[view][named][psiserve]")
{
   VOrder with_note{1, {1, "A", "a@a"}, {}, 0, "hello"};
   VOrder no_note{2, {2, "B", "b@b"}, {}, 0, std::nullopt};
   auto   p1 = psio::to_pssz(with_note);
   auto   p2 = psio::to_pssz(no_note);
   psio::pssz_view<VOrder> v1{p1};
   psio::pssz_view<VOrder> v2{p2};

   REQUIRE(v1.note().has_value());
   REQUIRE(v1.note() == "hello");
   REQUIRE(*v1.note() == "hello");
   REQUIRE_FALSE(v2.note().has_value());
   REQUIRE(v2.note() == std::nullopt);
   REQUIRE(v2.note().value_or("none") == "none");
}

TEST_CASE("psiserve: set view contains and iteration", "[view][named][psiserve]")
{
   VTagged                  t{7, {10, 20, 30, 40, 50}};
   auto                     packed = psio::to_pssz(t);
   psio::pssz_view<VTagged> v{packed};

   REQUIRE(v.id() == 7);
   auto tags = v.tags();
   REQUIRE(tags.size() == 5);
   REQUIRE(tags.contains(10));
   REQUIRE(tags.contains(30));
   REQUIRE(tags.contains(50));
   REQUIRE(!tags.contains(15));
   REQUIRE(tags.find(40) == 3);
   REQUIRE(tags.find(41) == tags.size());
   REQUIRE(tags.lower_bound(25) == 2);

   std::vector<std::int32_t> keys;
   for (auto k : tags)
      keys.push_back(k);
   REQUIRE(keys == std::vector<std::int32_t>{10, 20, 30, 40, 50});
}

TEST_CASE("psiserve: map view lookup and iteration", "[view][named][psiserve]")
{
   VConfig                  cfg{1, {{"alpha", 10}, {"beta", 20}, {"gamma", 30}}};
   auto                     packed = psio::to_pssz(cfg);
   psio::pssz_view<VConfig> v{packed};

   REQUIRE(v.version() == 1);
   auto lim = v.limits();
   REQUIRE(lim.size() == 3);
   REQUIRE(lim.contains("beta"));
   REQUIRE(!lim.contains("delta"));
   REQUIRE(lim.value_or("alpha", 0u) == 10u);
   REQUIRE(lim.value_or("gamma", 0u) == 30u);
   REQUIRE(lim.value_or("missing", 999u) == 999u);
   REQUIRE(lim.at("beta") == 20u);
   REQUIRE_FALSE(lim.at("delta").has_value());

   // Structured binding iteration.
   std::vector<std::pair<std::string, std::uint32_t>> entries;
   for (auto [k, val] : lim)
      entries.push_back({std::string(k.view_()), val});
   REQUIRE(entries.size() == 3);
   REQUIRE(entries[0].first == "alpha");
   REQUIRE(entries[0].second == 10);
   REQUIRE(entries[2].first == "gamma");
   REQUIRE(entries[2].second == 30);
}

TEST_CASE("psiserve: iterator concepts", "[view][named][psiserve]")
{
   using vec_iter = psio::pssz_view<std::vector<VLineItem>>::iterator;
   using set_iter = psio::pssz_view<std::set<std::int32_t>>::iterator;
   using map_iter = psio::pssz_view<std::map<std::string, std::uint32_t>>::iterator;
   STATIC_REQUIRE(std::random_access_iterator<vec_iter>);
   STATIC_REQUIRE(std::random_access_iterator<set_iter>);
   STATIC_REQUIRE(std::random_access_iterator<map_iter>);
}

TEST_CASE("psiserve: view aliases are the generic view", "[view][named][psiserve]")
{
   STATIC_REQUIRE(std::is_same_v<psio::pssz_view<VOrder>, psio::view<VOrder, psio::pssz>>);
   STATIC_REQUIRE(std::is_same_v<psio::pssz_view<const VOrder>, psio::view<VOrder, psio::pssz>>);
}
