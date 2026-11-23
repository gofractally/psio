// psio::flatbuf — layout, codec round trips, the verifier, views (safe and
// trusted), in-place writes, file identifier and size prefix.
// Interop with the official library is flatbuf_interop_tests.cpp; arbitrary
// bytes are flatbuf_fuzz_tests.cpp.

#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>

#include <catch.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "flatbuf_shapes.hpp"

namespace
{
   std::uint32_t u32_at(const std::vector<char>& b, std::size_t pos)
   {
      std::uint32_t v;
      std::memcpy(&v, b.data() + pos, 4);
      return v;
   }
   std::uint16_t u16_at(const std::vector<char>& b, std::size_t pos)
   {
      std::uint16_t v;
      std::memcpy(&v, b.data() + pos, 2);
      return v;
   }

   template <typename T>
   void round_trip(const T& value)
   {
      const auto bytes = psio::to_flatbuf(value);
      INFO("size " << bytes.size());
      const auto st = psio::validate_flatbuf<T>(bytes);
      if (!st.ok())
         FAIL("validate: " << st.error().what << " at " << st.error().byte_offset);
      CHECK(psio::from_flatbuf<T>(bytes) == value);
      CHECK(psio::from_flatbuf<T>(psio::trusted{bytes}) == value);
      CHECK(psio::flatbuf_size(value) == bytes.size());
      CHECK(psio::flatbuf_view<T>{bytes}.unpack() == value);
      CHECK(psio::flatbuf_trusted_view<T>{psio::trusted{bytes}}.unpack() == value);
   }
}  // namespace

TEST_CASE("flatbuf: struct layout", "[flatbuf][layout]")
{
   using psio::flatbuf;
   static_assert(flatbuf::layout<Vec3>::is_struct);
   static_assert(flatbuf::layout<Vec3>::size() == 12);
   static_assert(flatbuf::layout<Vec3>::align() == 4);
   static_assert(flatbuf::layout<Vec3>::offset(2) == 8);

   // { u8 a; u64 b; u16 c; [i32:3] d; Vec3 v; bool e; }
   static_assert(flatbuf::layout<Mixed>::is_struct);
   static_assert(flatbuf::layout<Mixed>::offset(0) == 0);
   static_assert(flatbuf::layout<Mixed>::offset(1) == 8);
   static_assert(flatbuf::layout<Mixed>::offset(2) == 16);
   static_assert(flatbuf::layout<Mixed>::offset(3) == 20);
   static_assert(flatbuf::layout<Mixed>::offset(4) == 32);
   static_assert(flatbuf::layout<Mixed>::offset(5) == 44);
   static_assert(flatbuf::layout<Mixed>::size() == 48);
   static_assert(flatbuf::layout<Mixed>::align() == 8);

   // A record without definitionWillNotChange is a table, whatever it holds.
   static_assert(!flatbuf::layout<Point>::is_struct);
   // So is a fixed record that holds a string.
   static_assert(!flatbuf::layout<Profile>::is_struct);
   SUCCEED();
}

TEST_CASE("flatbuf: vtable slots", "[flatbuf][layout]")
{
   using psio::flatbuf;
   // Declaration order; a variant takes two slots.
   static_assert(flatbuf::layout<Order>::slot(0) == 0);   // id
   static_assert(flatbuf::layout<Order>::slot(5) == 5);   // payment (type)
   static_assert(flatbuf::layout<Order>::slot(6) == 7);   // position
   static_assert(flatbuf::layout<Order>::vtable_offset(6) == 18);
   static_assert(flatbuf::layout<Order>::slots() == flatbuf::layout<Order>::members + 2);

   // field<N>: slots in ascending field-number order.
   static_assert(flatbuf::layout<Renumbered>::slot(0) == 2);  // c: field<3>
   static_assert(flatbuf::layout<Renumbered>::slot(1) == 0);  // a: field<1>
   static_assert(flatbuf::layout<Renumbered>::slot(2) == 1);  // b: field<2>
   SUCCEED();
}

TEST_CASE("flatbuf: bytes of a small table", "[flatbuf][codec]")
{
   // table Point { x:int; y:int; }  with x = 1, y = 2, as the official
   // builder writes it: root offset, vtable, table.
   const auto bytes = psio::to_flatbuf(Point{1, 2});
   const unsigned char expect[] = {
      0x0c, 0x00, 0x00, 0x00,  // root: table at 12
      0x08, 0x00,              // vtable: 8 bytes
      0x0c, 0x00,              //   table: 12 bytes
      0x04, 0x00,              //   x at +4
      0x08, 0x00,              //   y at +8
      0x08, 0x00, 0x00, 0x00,  // table: vtable 8 bytes back
      0x01, 0x00, 0x00, 0x00,  //   x
      0x02, 0x00, 0x00, 0x00,  //   y
   };
   REQUIRE(bytes.size() == sizeof(expect));
   CHECK(std::memcmp(bytes.data(), expect, sizeof(expect)) == 0);

   // A scalar equal to its default is not written, and the vtable is
   // trimmed after the last field present.
   const auto only_x = psio::to_flatbuf(Point{7, 0});
   CHECK(only_x.size() == 20);     // root, 2 bytes of padding, vtable (6), table (8)
   CHECK(u16_at(only_x, 6) == 6);  // the vtable holds x only
   const auto none = psio::to_flatbuf(Point{});
   CHECK(none.size() == 12);
   CHECK(psio::from_flatbuf<Point>(none) == Point{});
}

TEST_CASE("flatbuf: round trips", "[flatbuf][codec]")
{
   round_trip(Point{});
   round_trip(Point{-5, 9});
   round_trip(Profile{});
   round_trip(sample_profile());
   round_trip(Order{});
   round_trip(sample_order());

   Order o = sample_order();
   o.payment = Wire{"DE00 1234"};
   o.refund  = Wire{"FR11"};
   o.note    = std::string{};  // engaged, empty: still written
   o.origin.reset();
   o.discount = 0;             // engaged, zero: still written
   round_trip(o);
   {
      const auto bytes = psio::to_flatbuf(o);
      psio::flatbuf_view<Order> v{bytes};
      CHECK(v.note().has_value());
      CHECK(v.note()->empty());
      CHECK(v.discount().has_value());
      CHECK(*v.discount() == 0);
      CHECK_FALSE(v.origin().has_value());
   }
   o.payment = std::monostate{};
   round_trip(o);

   Tree t;
   t.value = 1;
   t.children.resize(3);
   t.children[1].value = 2;
   t.children[1].children.resize(2);
   t.children[1].children[0].name = "leaf";
   round_trip(t);

   round_trip(Scalars{});
   Scalars s{true, -1, 2, -3, 4, -5, 6, -7, 8, 1.5f, -2.25, Color::blue, 'x'};
   round_trip(s);
   // -0.0 is a value of its own (floats are compared by bits).
   Scalars z{};
   z.f = -0.0f;
   z.d = -0.0;
   const auto zb = psio::to_flatbuf(z);
   CHECK(std::signbit(psio::from_flatbuf<Scalars>(zb).f));
   CHECK(std::signbit(psio::from_flatbuf<Scalars>(zb).d));
}

TEST_CASE("flatbuf: defaults are member initialisers", "[flatbuf][codec]")
{
   // struct Defaults { int16 level = 7; bool on = true; Color c = green; double ratio = 0.5; }
   const auto all_default = psio::to_flatbuf(Defaults{});
   CHECK(all_default.size() == 12);  // root offset, empty vtable, table
   CHECK(psio::from_flatbuf<Defaults>(all_default) == Defaults{});
   psio::flatbuf_view<Defaults> v{all_default};
   CHECK(v.level() == 7);
   CHECK(v.on() == true);
   CHECK(v.c() == Color::green);
   CHECK(v.ratio() == 0.5);

   Defaults d;
   d.level = 0;
   d.on    = false;
   const auto bytes = psio::to_flatbuf(d);
   CHECK(bytes.size() > all_default.size());  // 0 and false are written
   CHECK(psio::from_flatbuf<Defaults>(bytes) == d);
   psio::flatbuf_trusted_view<Defaults> tv{psio::trusted{bytes}};
   CHECK(tv.level() == 0);
   CHECK(tv.on() == false);
   CHECK(tv.c() == Color::green);
}

TEST_CASE("flatbuf: deprecated members keep their slot and are not written", "[flatbuf][codec]")
{
   static_assert(psio::flatbuf::layout<WithDeprecated>::slot(2) == 2);
   WithDeprecated w{1, 99, 3};
   const auto     bytes = psio::to_flatbuf(w);
   const auto     back  = psio::from_flatbuf<WithDeprecated>(bytes);
   CHECK(back.a == 1);
   CHECK(back.old == 0);
   CHECK(back.c == 3);
   // The same bytes as a table whose middle field is simply absent.
   CHECK(bytes == psio::to_flatbuf(WithDeprecated{1, 0, 3}));
   psio::flatbuf_view<WithDeprecated> v{bytes};
   CHECK(v.old() == 0);
   CHECK(v.c() == 3);
}

TEST_CASE("flatbuf: views", "[flatbuf][view]")
{
   const Order o     = sample_order();
   const auto  bytes = psio::to_flatbuf(o);

   const auto check = [&](const auto& v) {
      CHECK(v.id() == o.id);
      CHECK(v.total() == o.total);
      CHECK(v.customer().id() == o.customer.id);
      CHECK(std::string_view(v.customer().name()) == o.customer.name);
      CHECK(v.customer().name() == o.customer.name);
      CHECK(v.customer().age() == o.customer.age);
      CHECK(v.customer().verified() == o.customer.verified);
      CHECK(v.customer().unpack() == o.customer);

      REQUIRE(v.items().size() == o.items.size());
      std::size_t i = 0;
      for (auto item : v.items())
      {
         CHECK(item.product() == o.items[i].product);
         CHECK(item.qty() == o.items[i].qty);
         CHECK(item.unit_price() == o.items[i].unit_price);
         ++i;
      }
      CHECK(i == o.items.size());
      CHECK(v.items()[1].unpack() == o.items[1]);
      CHECK(v.items().at(99).qty() == Item{}.qty);  // past the end: defaults
      CHECK(v.items().unpack() == o.items);

      REQUIRE(v.note().has_value());
      CHECK(*v.note() == *o.note);
      CHECK(v.note().value_or("x") == *o.note);
      CHECK(v.note().unpack() == o.note);

      // Union.
      CHECK(v.payment().index() == 1);
      CHECK(v.payment().template holds_alternative<Card>());
      REQUIRE(v.payment().template get_if<Card>().has_value());
      CHECK(v.payment().template get_if<Card>()->number() == "4111");
      CHECK(v.payment().template get_if<Card>()->cvv() == 123);
      CHECK_FALSE(v.payment().template get_if<Wire>().has_value());
      CHECK(v.payment().unpack() == o.payment);
      const int seen = v.payment().visit([](const auto& alt) -> int {
         if constexpr (std::is_same_v<std::remove_cvref_t<decltype(alt)>, std::monostate>)
            return 0;
         else
            return alt.unpack() == typename std::remove_cvref_t<decltype(alt)>::element_type{} ? -1
                                                                                             : 1;
      });
      CHECK(seen == 1);
      CHECK(v.refund().index() == 0);
      CHECK(v.refund().template get_if<0>()->number() == "refund");

      // Structs.
      CHECK(v.position().x() == o.position.x);
      CHECK(v.position().z() == o.position.z);
      const Vec3 pos = v.position();
      CHECK(pos == o.position);
      REQUIRE(v.origin().has_value());
      CHECK(v.origin()->y() == o.origin->y);
      REQUIRE(v.path().size() == o.path.size());
      CHECK(v.path()[1].y() == o.path[1].y);
      CHECK(v.path().unpack() == o.path);
      CHECK(v.mixed().b() == o.mixed.b);
      CHECK(v.mixed().d().size() == 3);
      CHECK(v.mixed().d()[2] == o.mixed.d[2]);
      CHECK(v.mixed().v().y() == o.mixed.v.y);
      CHECK(v.mixed().e() == o.mixed.e);
      CHECK(v.mixed().unpack() == o.mixed);

      // Vectors of strings, scalars, enums, bools; a byte vector.
      REQUIRE(v.tags().size() == o.tags.size());
      CHECK(v.tags()[0] == o.tags[0]);
      CHECK(v.tags().unpack() == o.tags);
      CHECK(v.color() == o.color);
      CHECK(v.colors().unpack() == o.colors);
      CHECK(v.flags().unpack() == o.flags);
      CHECK(v.flags()[0] == true);
      CHECK(v.blob().size() == o.blob.size());
      const std::span<const char> blob = v.blob();
      CHECK(std::memcmp(blob.data(), o.blob.data(), o.blob.size()) == 0);
      CHECK(v.quad().unpack() == o.quad);
      REQUIRE(v.discount().has_value());
      CHECK(*v.discount() == *o.discount);
      CHECK(v.discount().value_or(0) == *o.discount);

      // Sorted vectors: key lookup.
      CHECK(v.labels().size() == o.labels.size());
      CHECK(v.labels().contains("rush"));
      CHECK_FALSE(v.labels().contains("slow"));
      CHECK(v.labels().find("gift") == 0);
      CHECK(v.labels().unpack() == o.labels);
      CHECK(v.ids().contains(40));
      CHECK_FALSE(v.ids().contains(41));
      CHECK(v.ids().lower_bound(41) == 2);
      CHECK(v.ids().unpack() == o.ids);

      CHECK(v.stock().size() == o.stock.size());
      CHECK(v.stock().contains("bolt"));
      CHECK_FALSE(v.stock().contains("zip"));
      REQUIRE(v.stock().at("nut").has_value());
      CHECK(*v.stock().at("nut") == 0);  // a mapped scalar equal to its default
      CHECK(*v.stock().at("bolt") == 17);
      CHECK_FALSE(v.stock().at("zip").has_value());
      CHECK(v.stock().value_or("zip", 5u) == 5);
      CHECK(v.stock().unpack() == o.stock);
      std::size_t n = 0;
      for (auto [key, value] : v.stock())
      {
         CHECK(o.stock.at(std::string(std::string_view(key))) == value);
         ++n;
      }
      CHECK(n == o.stock.size());

      CHECK(v.by_id().contains(0));  // key 0 is not written, and found
      REQUIRE(v.by_id().at(7).has_value());
      CHECK(v.by_id().at(7)->product() == "seven");
      CHECK(v.by_id().value_or(8, Item{}).product.empty());
      CHECK(v.by_id().unpack() == o.by_id);

      CHECK(v.unpack() == o);
      CHECK(v.template get<0>() == o.id);
   };

   SECTION("safe") { check(psio::flatbuf_view<Order>{bytes}); }
   SECTION("trusted") { check(psio::flatbuf_trusted_view<Order>{psio::trusted{bytes}}); }
   SECTION("validated_view")
   {
      auto r = psio::validated_view<Order>(psio::flatbuf{}, bytes);
      REQUIRE(r.ok());
      check(*r);
      CHECK(r->customer().age() == o.customer.age);

      std::vector<char> bad(bytes.begin(), bytes.begin() + 10);
      auto              r2 = psio::validated_view<Order>(psio::flatbuf{}, bad);
      CHECK_FALSE(r2.ok());
      CHECK(r2->id() == 0);  // the empty view
   }
   SECTION("buffer")
   {
      psio::flatbuf_buffer<Order> buf{o};
      CHECK(buf->customer().name() == o.customer.name);
      CHECK(buf.unpack() == o);
   }
}

TEST_CASE("flatbuf: an empty view reads as defaults", "[flatbuf][view]")
{
   const auto check = [](const auto& v) {
      CHECK(v.id() == 0);
      CHECK(v.customer().name().empty());
      CHECK(v.customer().age() == 0);
      CHECK(v.items().size() == 0);
      CHECK(v.items().at(0).qty() == Item{}.qty);
      CHECK_FALSE(v.note().has_value());
      CHECK(v.payment().index() == 0);
      CHECK(v.position().x() == 0);
      CHECK(v.mixed().d().at(0) == 0);
      CHECK(v.stock().size() == 0);
      CHECK_FALSE(v.stock().contains("x"));
      CHECK(v.color() == Color::green);
      CHECK(v.unpack() == Order{});
   };
   check(psio::flatbuf_view<Order>{});
   check(psio::flatbuf_trusted_view<Order>{});
   check(psio::flatbuf_view<Order>{std::span<const char>{}});
   // A table with nothing in it.
   const auto empty = psio::to_flatbuf(Order{});
   check(psio::flatbuf_view<Order>{empty});
   check(psio::flatbuf_trusted_view<Order>{psio::trusted{empty}});
}

TEST_CASE("flatbuf: a union that is NONE", "[flatbuf][view]")
{
   Order o;
   o.payment = std::monostate{};
   const auto bytes = psio::to_flatbuf(o);
   psio::flatbuf_view<Order> v{bytes};
   CHECK(v.payment().index() == 0);
   CHECK(v.payment().holds_alternative<std::monostate>());
   CHECK(v.payment().unpack() == Payment{});
   // `refund` has no std::monostate: its first alternative is always
   // written, and an absent union reads as the default-constructed variant.
   CHECK(v.refund().index() == 0);
   CHECK(v.refund().unpack() == o.refund);
}

TEST_CASE("flatbuf: writing fixed-size fields in place", "[flatbuf][mutable]")
{
   Order o = sample_order();
   auto  bytes = psio::to_flatbuf(o);
   const auto before = bytes.size();

   psio::flatbuf_mutable_view<Order> m{bytes};
   CHECK(m.id().get() == o.id);
   CHECK(m.id().set(77));
   CHECK(m.customer().age().set(38));
   CHECK(m.customer().verified().set(false));
   CHECK(m.total().set(1.25));
   CHECK(m.position().x().set(9.5f));
   CHECK(m.mixed().d()[1].set(-4));
   CHECK(m.mixed().v().z().set(3.0f));
   CHECK(m.items()[0].qty().set(70));
   CHECK_FALSE(m.items()[1].qty().set(70));  // qty == 1, its default: not stored
   CHECK(m.path()[0].y().set(-1.0f));
   CHECK(m.quad()[3].set(9));
   CHECK(m.flags()[1].set(true));
   CHECK(m.colors()[0].set(Color::blue));
   CHECK(m.color().set(Color::red));
   CHECK(m.discount().has_value());
   CHECK(m.discount().set(-9));
   CHECK(m.origin().set(Vec3{7, 8, 9}));
   CHECK(m.payment().index() == 1);  // a union reads; its type cannot change
   // Past the end, and members that are not stored: nothing is written.
   CHECK_FALSE(m.items()[99].qty().set(1));
   CHECK_FALSE(m.quad()[4].set(1));

   o.id                = 77;
   o.customer.age      = 38;
   o.customer.verified = false;
   o.total             = 1.25;
   o.position.x        = 9.5f;
   o.mixed.d[1]        = -4;
   o.mixed.v.z         = 3.0f;
   o.items[0].qty      = 70;
   o.path[0].y         = -1.0f;
   o.quad[3]           = 9;
   o.flags[1]          = true;
   o.colors[0]         = Color::blue;
   o.color             = Color::red;
   o.discount          = -9;
   o.origin            = Vec3{7, 8, 9};

   CHECK(bytes.size() == before);
   REQUIRE(psio::validate_flatbuf<Order>(bytes).ok());
   CHECK(psio::from_flatbuf<Order>(bytes) == o);
   CHECK(m.unpack() == o);
   CHECK(psio::as_view(m).customer().age() == 38);

   // A scalar that equals its default is not in the buffer: set says so and
   // the buffer is unchanged.
   Profile p;
   p.name       = "n";
   auto pb      = psio::to_flatbuf(p);
   const auto c = pb;
   psio::flatbuf_mutable_view<Profile> pm{pb};
   CHECK_FALSE(pm.age().has_value());
   CHECK(pm.age().get() == 0);
   CHECK_FALSE(pm.age().set(5));
   CHECK(pb == c);

   // An empty optional has no bytes either.
   Order e;
   auto  eb = psio::to_flatbuf(e);
   psio::flatbuf_mutable_view<Order> em{eb};
   CHECK_FALSE(em.discount().has_value());
   CHECK_FALSE(em.discount().set(1));
   CHECK_FALSE(em.origin().has_value());
   CHECK_FALSE(em.origin().set(Vec3{1, 2, 3}));
   CHECK(em.position().x().set(4.0f));  // a struct member of a table is always written
   CHECK(psio::from_flatbuf<Order>(eb).position.x == 4.0f);
}

TEST_CASE("flatbuf: the verifier", "[flatbuf][validate]")
{
   const Order o     = sample_order();
   const auto  bytes = psio::to_flatbuf(o);
   REQUIRE(psio::validate_flatbuf<Order>(bytes).ok());

   SECTION("too small, bad root")
   {
      CHECK_FALSE(psio::validate_flatbuf<Order>(std::span<const char>{}).ok());
      CHECK_FALSE(psio::validate_flatbuf<Order>(std::span<const char>{bytes.data(), 3}).ok());
      auto b = bytes;
      std::uint32_t big = 0x7ffffff0;
      std::memcpy(b.data(), &big, 4);
      CHECK_FALSE(psio::validate_flatbuf<Order>(b).ok());
      std::uint32_t zero = 0;
      std::memcpy(b.data(), &zero, 4);
      CHECK_FALSE(psio::validate_flatbuf<Order>(b).ok());
      std::uint32_t odd = u32_at(bytes, 0) + 1;
      std::memcpy(b.data(), &odd, 4);
      CHECK_FALSE(psio::validate_flatbuf<Order>(b).ok());  // misaligned table
   }
   SECTION("every truncation is rejected")
   {
      // The buffer ends with the two bytes that pad its last string;
      // nothing refers to them.
      for (std::size_t n = 0; n < bytes.size() - 2; ++n)
      {
         std::vector<char> cut(bytes.begin(), bytes.begin() + n);
         INFO("length " << n);
         CHECK_FALSE(psio::validate_flatbuf<Order>(cut).ok());
      }
   }
   SECTION("a string must be NUL-terminated")
   {
      const Profile p{1, "abcdefg", "", 0, false};
      auto          b = psio::to_flatbuf(p);
      const auto    at = std::string_view(b.data(), b.size()).find("abcdefg");
      REQUIRE(at != std::string_view::npos);
      b[at + 7] = 'x';
      const auto st = psio::validate_flatbuf<Profile>(b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: string is not NUL-terminated");
   }
   SECTION("a vector length that exceeds the buffer")
   {
      Order s;
      s.blob  = {1, 2, 3, 4, 5, 6, 7, 8};
      auto  b = psio::to_flatbuf(s);
      bool  found = false;
      for (std::size_t i = 0; i + 12 <= b.size() && !found; i += 4)
         if (u32_at(b, i) == 8 && std::memcmp(b.data() + i + 4, s.blob.data(), 8) == 0)
         {
            const std::uint32_t huge = 0x40000000;
            std::memcpy(b.data() + i, &huge, 4);
            found = true;
         }
      REQUIRE(found);
      const auto st = psio::validate_flatbuf<Order>(b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: vector out of bounds");
      // The safe view reads the vector as empty; nothing is read out of bounds.
      CHECK(psio::flatbuf_view<Order>{b}.blob().size() == 0);
   }
   SECTION("unknown union type, and a type without a value")
   {
      Order s;
      s.payment = Card{"1", 2};
      auto b    = psio::to_flatbuf(s);
      psio::flatbuf_mutable_view<Order> m{b};
      // Find the type byte through the vtable.
      const std::size_t root   = u32_at(b, 0);
      std::int32_t      soff;
      std::memcpy(&soff, b.data() + root, 4);
      const std::size_t vt  = root - soff;
      const std::size_t tvo = psio::flatbuf::layout<Order>::vtable_offset(5);
      const std::size_t tat = root + u16_at(b, vt + tvo);
      REQUIRE(b[tat] == 1);
      b[tat] = 9;
      auto st = psio::validate_flatbuf<Order>(b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: unknown union type");
      CHECK(psio::flatbuf_view<Order>{b}.payment().index() == 0);
      b[tat] = 1;
      // Remove the value: zero its vtable entry.
      const std::uint16_t zero = 0;
      std::memcpy(b.data() + vt + tvo + 2, &zero, 2);
      st = psio::validate_flatbuf<Order>(b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: union has a type but no value");
   }
   SECTION("set and map keys must ascend")
   {
      Order s;
      s.ids  = {1, 2, 3};
      auto b = psio::to_flatbuf(s);
      bool found = false;
      for (std::size_t i = 0; i + 16 <= b.size() && !found; i += 4)
         if (u32_at(b, i) == 3 && u32_at(b, i + 4) == 1 && u32_at(b, i + 8) == 2 &&
             u32_at(b, i + 12) == 3)
         {
            const std::uint32_t one = 1;
            std::memcpy(b.data() + i + 12, &one, 4);
            found = true;
         }
      REQUIRE(found);
      const auto st = psio::validate_flatbuf<Order>(b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: set keys are not in ascending order");
   }
   SECTION("limits")
   {
      Tree t;
      Tree* cur = &t;
      for (int i = 0; i < 70; ++i)
      {
         cur->children.resize(1);
         cur = &cur->children[0];
      }
      const auto deep = psio::to_flatbuf(t);
      const auto st   = psio::validate_flatbuf<Tree>(deep);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: tables nested too deeply");
      CHECK(psio::validate_flatbuf<Tree>(deep, psio::flatbuf::limits{.max_depth = 100}).ok());

      Tree wide;
      wide.children.resize(50);
      const auto wb = psio::to_flatbuf(wide);
      CHECK(psio::validate_flatbuf<Tree>(wb).ok());
      const auto st2 = psio::validate_flatbuf<Tree>(wb, psio::flatbuf::limits{.max_tables = 20});
      REQUIRE_FALSE(st2.ok());
      CHECK(st2.error().what == "flatbuf: too many tables");
   }
   SECTION("a field offset outside its table")
   {
      auto b = psio::to_flatbuf(Point{1, 2});
      const std::uint16_t far = 400;
      std::memcpy(b.data() + 8, &far, 2);  // x's vtable entry
      const auto st = psio::validate_flatbuf<Point>(b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "flatbuf: field outside its table");
      CHECK(psio::flatbuf_view<Point>{b}.x() == 0);
      CHECK(psio::flatbuf_view<Point>{b}.y() == 2);
   }
   SECTION("validate_strict checks annotations on the value")
   {
      CHECK(psio::validate_strict<Order>(psio::flatbuf{}, bytes).ok());
   }
}

TEST_CASE("flatbuf: file identifier and size prefix", "[flatbuf][codec]")
{
   const Order o = sample_order();

   const auto with_id = psio::to_flatbuf(o, psio::flatbuf::options{.file_identifier = "ORDR"});
   CHECK(psio::flatbuf_identifier(with_id) == "ORDR");
   CHECK(psio::flatbuf_has_identifier(with_id, "ORDR"));
   CHECK_FALSE(psio::flatbuf_has_identifier(with_id, "NOPE"));
   CHECK(psio::validate_flatbuf<Order>(with_id).ok());
   CHECK(psio::validate_flatbuf<Order>(with_id, psio::flatbuf::options{.file_identifier = "ORDR"})
            .ok());
   CHECK_FALSE(
      psio::validate_flatbuf<Order>(with_id, psio::flatbuf::options{.file_identifier = "NOPE"})
         .ok());
   CHECK(psio::from_flatbuf<Order>(with_id) == o);

   const auto prefixed = psio::to_flatbuf(o, psio::flatbuf::options{.size_prefixed = true});
   CHECK(u32_at(prefixed, 0) == prefixed.size() - 4);
   CHECK(psio::validate_flatbuf<Order>(prefixed, psio::flatbuf::options{.size_prefixed = true})
            .ok());
   const auto inner = psio::flatbuf_size_prefixed(prefixed);
   REQUIRE(inner.size() == prefixed.size() - 4);
   CHECK(psio::flatbuf_view<Order>{inner}.unpack() == o);
   CHECK(psio::from_flatbuf<Order>(psio::trusted{inner}) == o);
   // A stream: the next buffer starts after the first.
   auto stream = prefixed;
   stream.insert(stream.end(), prefixed.begin(), prefixed.end());
   CHECK(psio::flatbuf_size_prefixed(stream).size() == prefixed.size() - 4);
   CHECK(psio::flatbuf_size_prefixed(std::span<const char>{prefixed.data(), 10}).empty());

   const auto both = psio::to_flatbuf(
      o, psio::flatbuf::options{.file_identifier = "ORDR", .size_prefixed = true});
   CHECK(psio::validate_flatbuf<Order>(
            both, psio::flatbuf::options{.file_identifier = "ORDR", .size_prefixed = true})
            .ok());
   CHECK(psio::flatbuf_identifier(psio::flatbuf_size_prefixed(both)) == "ORDR");
}

TEST_CASE("flatbuf: vtables are shared", "[flatbuf][codec]")
{
   Tree t;
   t.children.resize(100);
   for (std::size_t i = 0; i < t.children.size(); ++i)
      t.children[i].value = static_cast<std::int32_t>(i + 1);
   const auto bytes = psio::to_flatbuf(t);
   // 100 tables of { soffset, value } = 8 bytes, 400 bytes of offsets; one
   // vtable for all of them.
   CHECK(bytes.size() < 100 * 8 + 400 + 64);
   round_trip(t);
}

TEST_CASE("flatbuf: more than 1 KiB, and many distinct vtables", "[flatbuf][codec]")
{
   Order o = sample_order();
   o.blob.assign(5000, 0x5a);
   for (int i = 0; i < 300; ++i)
   {
      o.tags.push_back(std::string(static_cast<std::size_t>(i % 17), 'a'));
      o.items.push_back(Item{i % 3 ? "p" : "", static_cast<std::uint32_t>(i % 2), i % 5 ? 1.0 : 0.0});
      o.by_id[static_cast<std::uint32_t>(i * 3)] = Item{"m", static_cast<std::uint32_t>(i), 0};
   }
   round_trip(o);

   // 40 presence patterns: more vtables than the builder keeps inline.
   Sparse s;
   for (int i = 0; i < 40; ++i)
   {
      SparseRow r;
      r.a = (i & 1) ? 1 : 0;
      r.b = (i & 2) ? 1 : 0;
      r.c = (i & 4) ? 1 : 0;
      r.d = (i & 8) ? 1 : 0;
      r.e = (i & 16) ? 1 : 0;
      r.f = (i & 32) ? 1 : 0;
      s.rows.push_back(r);
      s.rows.push_back(r);
   }
   round_trip(s);
}
