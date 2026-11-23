// psio::capnp — layout against the official compiler's (fixture), codec
// round trips, typed validation, views, in-place writes, document edits.
// Interop with the official library is capnp_interop_tests.cpp.

#include <psio/capnp.hpp>
#include <psio/capnp_view.hpp>

#include <catch.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "capnp_layout_fixture.hpp"

namespace
{
   using namespace psio::detail::capnp_impl;

   std::string show(slot s)
   {
      if (s.kind == slot_kind::none)
         return "void";
      if (s.kind == slot_kind::pointer)
         return "ptr[" + std::to_string(s.offset) + "]";
      return "bits[" + std::to_string(s.offset << s.lg) + "," +
             std::to_string((s.offset + 1) << s.lg) + ")";
   }
   template <typename T>
   std::string slots_of()
   {
      using L = psio::capnp::layout<T>;
      std::string out;
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (([&] {
             if (Is)
                out += ";";
             out += show(L::field(Is));
             const std::size_t n = union_alts<typename L::template member<Is>>::count;
             for (std::size_t a = 0; a < n; ++a)
                out += " " + show(L::alt(Is, a));
          }()),
          ...);
      }(std::make_index_sequence<L::members>{});
      return out;
   }

   struct Profile
   {
      std::uint64_t id = 0;
      std::string   name;
      std::string   email;
      std::uint32_t age      = 0;
      bool          verified = false;
      friend bool   operator==(const Profile&, const Profile&) = default;
   };
   PSIO_REFLECT(Profile, id, name, email, age, verified)

   enum class Color : std::uint16_t { red, green, blue };

   struct Everything
   {
      bool          flag  = false;
      std::int16_t  level = 7;
      double        ratio = 0.5;
      Color         color = Color::green;
      std::variant<std::monostate, std::uint32_t, std::string, Profile, double> pay;
      std::variant<std::uint32_t, std::uint32_t>                                same;
      std::optional<std::uint32_t>            score;
      std::optional<std::string>              memo;
      std::optional<Profile>                  boss;
      std::vector<bool>                       bits;
      std::vector<std::uint8_t>               blob;
      std::vector<std::string>                names;
      std::vector<std::vector<std::int16_t>>  grid;
      std::vector<Profile>                    people;
      std::array<std::uint32_t, 3>            triple{};
      Profile                                 owner;
      friend bool operator==(const Everything&, const Everything&) = default;
   };
   PSIO_REFLECT(Everything, flag, level, ratio, color, pay, same, score, memo, boss, bits, blob,
                names, grid, people, triple, owner)

   Everything sample(int which)
   {
      Everything e;
      e.flag  = which & 1;
      e.level = static_cast<std::int16_t>(which == 0 ? 7 : -which);
      e.ratio = 0.25 * which;
      e.color = static_cast<Color>(which % 3);
      switch (which % 5)
      {
         case 1: e.pay.emplace<1>(4242); break;
         case 2: e.pay.emplace<2>("card"); break;
         case 3: e.pay.emplace<3>(Profile{3, "Carol", "c@x", 30, true}); break;
         case 4: e.pay.emplace<4>(99.5); break;
         default: break;
      }
      e.same.emplace<1>(77);
      if (which & 1)
         e.score = 17;
      if (which & 2)
         e.memo = which & 4 ? "" : "memo";
      if (which & 4)
         e.boss = Profile{8, "Boss", "b@x", 60, true};
      e.bits   = {true, false, true, true, false, false, false, false, true};
      e.blob   = {0, 1, 2, 250, 255};
      e.names  = {"one", "", "three"};
      e.grid   = {{1, -2}, {}, {3}};
      e.people = {{1, "a", "b", 2, false}, {2, "cc", "dd", 4, true}};
      e.triple = {1, 2, 3};
      e.owner  = {5, "own", "o@x", 50, true};
      return e;
   }

   // Older and newer versions of one record.
   struct V1
   {
      std::uint32_t a = 0;
      std::string   s;
   };
   PSIO_REFLECT(V1, a, s)
   struct V2
   {
      std::uint32_t            a = 0;
      std::string              s;
      std::uint64_t            b = 9;  // default 9
      std::vector<std::string> more;
      bool                     c = true;
      friend bool              operator==(const V2&, const V2&) = default;
   };
   PSIO_REFLECT(V2, a, s, b, more, c)
}  // namespace

TEST_CASE("capnp layout: equal to the official compiler's on generated records", "[capnp][layout]")
{
   using namespace capnp_layout_fixture;
#define X(NAME, DW, PTRS, SLOTS)                                    \
   {                                                                \
      INFO(#NAME);                                                  \
      CHECK(psio::capnp::layout<NAME>::data_words == DW);           \
      CHECK(psio::capnp::layout<NAME>::ptr_count == PTRS);          \
      CHECK(slots_of<NAME>() == SLOTS);                             \
   }
   CAPNP_LAYOUT_FIXTURE(X)
#undef X
}

TEST_CASE("capnp layout: union members share space as capnp compile lays them out",
          "[capnp][layout]")
{
   struct U
   {
      std::variant<std::uint32_t, std::uint32_t> u;
   };
   // struct U { u :union { a @0 :UInt32; b @1 :UInt32; } }  → 8 bytes, 0 ptrs:
   // both members at bits [0, 32), tag at bits [32, 48).
   using L = psio::capnp::layout<Everything>;
   (void)sizeof(U);
   CHECK(L::alt(5, 0).offset == L::alt(5, 1).offset);
   CHECK(L::alt(5, 0).lg == 5);
}

TEST_CASE("capnp: round trip, size, validation", "[capnp]")
{
   for (int which = 0; which < 16; ++which)
   {
      const auto e     = sample(which);
      const auto bytes = psio::to_capnp(e);
      INFO(which);
      CHECK(bytes.size() % 8 == 0);
      CHECK(psio::capnp_size(e) == bytes.size());
      REQUIRE(psio::validate_capnp<Everything>(std::span<const char>{bytes}).ok());
      CHECK(psio::from_capnp<Everything>(std::span<const char>{bytes}) == e);
      CHECK(psio::capnp_message_size(std::span<const char>{bytes}) == bytes.size());
      std::vector<char> sink{'x'};
      psio::to_capnp(e, sink);
      CHECK(sink == bytes);
   }
   const Everything none{};
   const auto       bytes = psio::to_capnp(none);
   CHECK(psio::from_capnp<Everything>(std::span<const char>{bytes}) == none);
}

TEST_CASE("capnp: validation rejects what a view could not read safely", "[capnp][validate]")
{
   const auto e     = sample(7);
   const auto bytes = psio::to_capnp(e);
   const auto ok    = [](const std::vector<char>& b) {
      return psio::validate_capnp<Everything>(std::span<const char>{b}).ok();
   };
   REQUIRE(ok(bytes));
   // Every truncation.
   for (std::size_t n = 0; n < bytes.size(); ++n)
      CHECK_FALSE(ok(std::vector<char>(bytes.begin(), bytes.begin() + n)));
   // Trailing bytes.
   {
      auto longer = bytes;
      longer.resize(longer.size() + 8);
      CHECK_FALSE(ok(longer));
   }
   // Every single-byte corruption either is rejected or leaves a message
   // that views and decodes without leaving the buffer (checked by reading
   // everything; a decode must also re-encode).
   std::size_t accepted = 0, rejected = 0;
   for (std::size_t i = 0; i < bytes.size(); ++i)
      for (int mode = 0; mode < 3; ++mode)
      {
         auto bad = bytes;
         bad[i]   = mode == 0 ? char(0) : mode == 1 ? char(0xff) : char(bad[i] + 1);
         if (!ok(bad))
         {
            ++rejected;
            continue;
         }
         ++accepted;
         const auto v = psio::from_capnp<Everything>(psio::trusted{std::span<const char>{bad}});
         (void)psio::to_capnp(v);
         psio::capnp_view<Everything> view{std::span<const char>{bad}};
         std::size_t                  n = view.names().size();
         for (std::size_t k = 0; k < n; ++k)
            (void)view.names()[k].view_().size();
         for (auto p : view.people())
            (void)(p.name().view_().size() + p.email().view_().size());
      }
   CHECK(rejected > 100);
   CHECK(accepted > 100);

   // Limits.
   CHECK_FALSE(psio::validate_capnp<Everything>(std::span<const char>{bytes},
                                               psio::capnp::limits{.traversal_words = 4})
                  .ok());
   CHECK_FALSE(
      psio::validate_capnp<Everything>(std::span<const char>{bytes}, psio::capnp::limits{.nesting = 1})
         .ok());
}

TEST_CASE("capnp view: named readers, lists, unions, optionals, unpack", "[capnp][view]")
{
   const auto e     = sample(7);  // pay = card, score, memo, boss
   const auto bytes = psio::to_capnp(e);
   psio::capnp_view<Everything> v{std::span<const char>{bytes}};
   CHECK(v.flag());
   CHECK(v.level() == -7);
   CHECK(v.ratio() == 1.75);
   CHECK(v.color() == Color::green);
   CHECK(v.get<1>() == -7);
   CHECK(v.pay().index() == 2);
   CHECK(v.pay().holds_alternative<std::string>());
   CHECK(v.pay().get_if<2>()->view_() == "card");
   CHECK_FALSE(v.pay().get_if<1>().has_value());
   CHECK(v.same().index() == 1);
   CHECK(*v.same().get_if<1>() == 77);
   CHECK(v.score().has_value());
   CHECK(*v.score() == 17);
   CHECK(v.score().value_or(0u) == 17);
   REQUIRE(v.memo().has_value());
   CHECK(v.memo()->view_().empty());  // Some(""): a non-null pointer
   CHECK(v.memo().unpack() == std::optional<std::string>{""});
   REQUIRE(v.boss().has_value());
   CHECK(v.boss()->name() == "Boss");
   CHECK(v.bits().size() == 9);
   CHECK(v.bits()[0]);
   CHECK_FALSE(v.bits()[1]);
   CHECK(v.bits()[8]);
   CHECK(v.blob().size() == 5);
   CHECK(std::span<const char>(v.blob()).size() == 5);
   CHECK(v.names().size() == 3);
   CHECK(v.names()[0] == "one");
   CHECK(v.names()[1] == "");
   CHECK(v.grid()[0][1] == -2);
   CHECK(v.grid()[1].empty());
   CHECK(v.people().size() == 2);
   CHECK(v.people()[1].email() == "dd");
   CHECK(v.people().back().age() == 4);
   CHECK(v.people().at(9).age() == 0);
   std::uint32_t ages = 0;
   for (auto p : v.people())
      ages += p.age();
   CHECK(ages == 6);
   CHECK(v.triple()[2] == 3);
   CHECK(v.owner().name() == "own");
   CHECK(v.owner().unpack() == e.owner);
   CHECK(v.people().unpack() == e.people);
   CHECK(v.pay().unpack() == e.pay);
   CHECK(v.unpack() == e);
   CHECK(psio::value_of(v.owner()) == e.owner);
   CHECK(v.owner() == e.owner);
   const std::size_t alt = v.pay().visit([](auto a) -> std::size_t { return sizeof(a); });
   CHECK(alt > 0);

   // A default view reads the defaults (member initialisers).
   psio::capnp_view<Everything> none{};
   CHECK(none.level() == 7);
   CHECK(none.ratio() == 0.5);
   CHECK(none.color() == Color::green);
   CHECK(none.pay().index() == 0);
   CHECK_FALSE(none.score().has_value());
   CHECK_FALSE(none.memo().has_value());
   CHECK(none.people().empty());
   CHECK(none.owner().name().view_().empty());
   CHECK(none.owner().age() == 0);
}

TEST_CASE("capnp view: a struct from an older or newer schema", "[capnp][view][evolution]")
{
   // Old writer, new reader: members beyond the writer's sections read as
   // their defaults, never out of bounds.
   const auto old_bytes = psio::to_capnp(V1{5, "five"});
   REQUIRE(psio::validate_capnp<V2>(std::span<const char>{old_bytes}).ok());
   psio::capnp_view<V2> v{std::span<const char>{old_bytes}};
   CHECK(v.a() == 5);
   CHECK(v.s() == "five");
   CHECK(v.b() == 9);
   CHECK(v.c());
   CHECK(v.more().empty());
   CHECK(psio::from_capnp<V2>(std::span<const char>{old_bytes}) == V2{5, "five", 9, {}, true});

   // New writer, old reader: the extra words are ignored.
   const auto new_bytes = psio::to_capnp(V2{6, "six", 1, {"x"}, false});
   REQUIRE(psio::validate_capnp<V1>(std::span<const char>{new_bytes}).ok());
   psio::capnp_view<V1> w{std::span<const char>{new_bytes}};
   CHECK(w.a() == 6);
   CHECK(w.s() == "six");

   // In place: a member the old writer has no room for cannot be written.
   auto                          copy = old_bytes;
   psio::capnp_mutable_view<V2> m{copy};
   CHECK(m.a().set(8));
   CHECK_FALSE(m.b().set(1));
   // (c is a bit of the first data word, which the old writer does have.)
   CHECK(psio::capnp_view<V2>{std::span<const char>{copy}}.a() == 8);
   // A document made from it: compact() re-encodes at this schema's size.
   psio::capnp_document<V2> doc{copy};
   CHECK_FALSE(doc.edit().more().set(std::vector<std::string>{"m"}));
   doc.compact();
   CHECK(doc.edit().more().set(std::vector<std::string>{"m"}));
   CHECK(doc.edit().b().set(1));
   CHECK(doc.unpack() == V2{8, "five", 1, {"m"}, true});
}

TEST_CASE("capnp: in-place scalar writes and size-changing edits keep the message valid",
          "[capnp][edit]")
{
   auto e     = sample(5);
   auto bytes = psio::to_capnp(e);
   psio::capnp_mutable_view<Everything> m{bytes};
   CHECK(m.level().get() == -5);
   REQUIRE(m.level().set(7));  // back to the default: the wire holds zero
   REQUIRE(m.flag().set(false));
   REQUIRE(m.ratio().set(8.5));
   REQUIRE(m.owner().age().set(51));
   REQUIRE(m.owner().verified().set(false));
   e.level          = 7;
   e.flag           = false;
   e.ratio          = 8.5;
   e.owner.age      = 51;
   e.owner.verified = false;
   REQUIRE(psio::validate_capnp<Everything>(std::span<const char>{bytes}).ok());
   CHECK(psio::from_capnp<Everything>(std::span<const char>{bytes}) == e);
   CHECK(bytes == psio::to_capnp(e));  // exactly the encoding of the new value

   psio::capnp_document<Everything> doc{bytes};
   const auto step = [&] {
      REQUIRE(psio::validate_capnp<Everything>(doc.bytes()).ok());
      CHECK(doc.unpack() == e);
   };
   REQUIRE(doc.edit().owner().name().set("a much longer owner name than before"));
   e.owner.name = "a much longer owner name than before";
   step();
   REQUIRE(doc.edit().owner().name().set("x"));  // shrink
   e.owner.name = "x";
   step();
   REQUIRE(doc.edit().memo().set(std::optional<std::string>{"hello"}));  // absent → present
   e.memo = "hello";
   step();
   REQUIRE(doc.edit().memo().set(std::nullopt));  // present → absent
   e.memo.reset();
   step();
   REQUIRE(doc.edit().boss().set(std::optional<Profile>{}));
   e.boss.reset();
   step();
   REQUIRE(doc.edit().people().set(std::vector<Profile>{{9, "z", "y", 1, true}}));
   e.people = {{9, "z", "y", 1, true}};
   step();
   REQUIRE(doc.edit().grid().set(std::vector<std::vector<std::int16_t>>{{5, 6, 7}}));
   e.grid = {{5, 6, 7}};
   step();
   REQUIRE(doc.edit().owner().set(Profile{1, "new", "n@x", 2, true}));  // a record as a whole
   e.owner = {1, "new", "n@x", 2, true};
   step();
   REQUIRE(doc.edit().owner().email().set("after@replace"));  // into the replaced record
   e.owner.email = "after@replace";
   step();
   const auto grown = doc.bytes().size();
   doc.compact();
   CHECK(doc.bytes().size() < grown);
   step();
   CHECK(std::vector<char>(doc.bytes().begin(), doc.bytes().end()) == psio::to_capnp(e));
}
