// psio::capnp against the official library, both directions, on the same
// schema (tests/capnp/interop.capnp): bytes written by psio are read by
// libcapnp; bytes written by libcapnp are validated, viewed and decoded by
// psio. Built only where CapnProto is found.

#include <psio/capnp.hpp>
#include <psio/capnp_view.hpp>

#include "interop.capnp.h"
#include <capnp/message.h>
#include <capnp/serialize.h>

#include <catch.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
   struct PProfile
   {
      std::uint64_t id = 0;
      std::string   name;
      std::string   email;
      std::uint32_t age      = 0;
      bool          verified = false;
      friend bool   operator==(const PProfile&, const PProfile&) = default;
   };
   PSIO_REFLECT(PProfile, id, name, email, age, verified)

   struct PItem
   {
      std::string   product;
      std::uint32_t qty        = 0;
      double        unit_price = 0;
      friend bool   operator==(const PItem&, const PItem&) = default;
   };
   PSIO_REFLECT(PItem, product, qty, unit_price)

   struct POrder
   {
      std::uint64_t       id = 0;
      PProfile           customer;
      std::vector<PItem> items;
      double              total = 0;
      std::string         note;
      friend bool         operator==(const POrder&, const POrder&) = default;
   };
   PSIO_REFLECT(POrder, id, customer, items, total, note)

   struct PMixed
   {
      bool         flag  = false;
      std::int8_t  small = 0;
      std::int16_t level = 7;
      float        ratio = 1.5f;
      bool         on    = true;
      std::variant<std::monostate, std::uint32_t, std::string, PProfile, double> payment;
      std::optional<std::uint32_t>           score;
      std::optional<std::string>             memo;
      std::vector<bool>                      bits;
      std::vector<std::uint8_t>              blob;
      std::vector<std::string>               names;
      std::vector<std::vector<std::int16_t>> grid;
      std::vector<PProfile>                 people;
      std::uint64_t                          wide = 0;
      friend bool operator==(const PMixed&, const PMixed&) = default;
   };
   PSIO_REFLECT(PMixed, flag, small, level, ratio, on, payment, score, memo, bits, blob, names,
                grid, people, wide)

   POrder sample_order()
   {
      POrder o;
      o.id       = 0x1122334455667788ull;
      o.customer = {77, "Alice Example", "alice@example.org", 37, true};
      for (int i = 0; i < 5; ++i)
         o.items.push_back({"product-" + std::to_string(i), std::uint32_t(i + 1), 1.5 * i});
      o.total = 1234.5;
      o.note  = "leave at the door";
      return o;
   }

   // The flat array libcapnp reads must be word-aligned.
   kj::Array<capnp::word> words_of(std::span<const char> bytes)
   {
      auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
      std::memcpy(words.begin(), bytes.data(), bytes.size());
      return words;
   }
   std::vector<char> bytes_of(capnp::MessageBuilder& mb)
   {
      auto flat = capnp::messageToFlatArray(mb);
      auto b    = flat.asBytes();
      return {reinterpret_cast<const char*>(b.begin()), reinterpret_cast<const char*>(b.end())};
   }
   std::string str(capnp::Text::Reader t) { return {t.begin(), t.size()}; }

}  // namespace

static void put(IoProfile::Builder b, const PProfile& p)
{
   b.setId(p.id);
   b.setName(p.name);
   b.setEmail(p.email);
   b.setAge(p.age);
   b.setVerified(p.verified);
}
static PProfile get(IoProfile::Reader r)
{
   return {r.getId(), str(r.getName()), str(r.getEmail()), r.getAge(), r.getVerified()};
}

static std::vector<char> official_order(const POrder& o)
{
   capnp::MallocMessageBuilder mb;
   auto                        b = mb.initRoot<IoOrder>();
   b.setId(o.id);
   put(b.initCustomer(), o.customer);
   auto items = b.initItems(static_cast<unsigned>(o.items.size()));
   for (unsigned i = 0; i < o.items.size(); ++i)
   {
      items[i].setProduct(o.items[i].product);
      items[i].setQty(o.items[i].qty);
      items[i].setUnitPrice(o.items[i].unit_price);
   }
   b.setTotal(o.total);
   b.setNote(o.note);
   return bytes_of(mb);
}

TEST_CASE("capnp interop: an order is byte-identical to libcapnp's", "[capnp][interop]")
{
   const auto o      = sample_order();
   const auto theirs = official_order(o);
   const auto ours   = psio::to_capnp(o);
   REQUIRE(ours.size() == theirs.size());
   CHECK(std::memcmp(ours.data(), theirs.data(), ours.size()) == 0);
   CHECK(psio::capnp_size(o) == theirs.size());
}

TEST_CASE("capnp interop: libcapnp reads what psio writes", "[capnp][interop]")
{
   const auto o     = sample_order();
   const auto bytes = psio::to_capnp(o);
   auto       words = words_of(bytes);
   capnp::FlatArrayMessageReader reader(words);
   auto                          r = reader.getRoot<IoOrder>();
   CHECK(r.getId() == o.id);
   CHECK(get(r.getCustomer()) == o.customer);
   REQUIRE(r.getItems().size() == o.items.size());
   for (unsigned i = 0; i < o.items.size(); ++i)
   {
      CHECK(str(r.getItems()[i].getProduct()) == o.items[i].product);
      CHECK(r.getItems()[i].getQty() == o.items[i].qty);
      CHECK(r.getItems()[i].getUnitPrice() == o.items[i].unit_price);
   }
   CHECK(r.getTotal() == o.total);
   CHECK(str(r.getNote()) == o.note);
}

TEST_CASE("capnp interop: psio validates, views and decodes what libcapnp writes",
          "[capnp][interop]")
{
   const auto o     = sample_order();
   const auto bytes = official_order(o);
   REQUIRE(psio::validate_capnp<POrder>(std::span<const char>{bytes}).ok());
   psio::capnp_view<POrder> v{std::span<const char>{bytes}};
   CHECK(v.id() == o.id);
   CHECK(v.customer().name() == "Alice Example");
   CHECK(v.customer().verified());
   CHECK(v.items().size() == 5);
   CHECK(v.items()[4].product() == "product-4");
   CHECK(v.items()[4].unit_price() == 6.0);
   CHECK(v.note() == "leave at the door");
   CHECK(v.unpack() == o);
   CHECK(psio::from_capnp<POrder>(std::span<const char>{bytes}) == o);
}

static PMixed sample_mixed(int which)
{
   PMixed m;
   m.flag  = true;
   m.small = -5;
   m.level = which == 0 ? 7 : -300;  // 7 is the default: wire holds 0
   m.ratio = 2.25f;
   m.on    = which != 1;
   switch (which % 5)
   {
      case 0: break;
      case 1: m.payment.emplace<1>(4242); break;
      case 2: m.payment.emplace<2>("card-1234"); break;
      case 3: m.payment.emplace<3>(PProfile{3, "Carol", "c@x", 30, true}); break;
      case 4: m.payment.emplace<4>(99.5); break;
   }
   if (which & 1)
      m.score = 17;
   if (which & 2)
      m.memo = "memo";
   m.bits   = {true, false, true, true, false, false, false, false, true};
   m.blob   = {0, 1, 2, 250, 255};
   m.names  = {"one", "", "three"};
   m.grid   = {{1, -2}, {}, {3}};
   m.people = {{1, "a", "b", 2, false}, {2, "cc", "dd", 4, true}};
   m.wide   = 0xfedcba9876543210ull;
   return m;
}

static void check_official(IoMixed::Reader r, const PMixed& m)
{
   CHECK(r.getFlag() == m.flag);
   CHECK(r.getSmall() == m.small);
   CHECK(r.getLevel() == m.level);
   CHECK(r.getRatio() == m.ratio);
   CHECK(r.getOn() == m.on);
   auto p = r.getPayment();
   REQUIRE(static_cast<std::size_t>(p.which()) == m.payment.index());
   switch (p.which())
   {
      case IoMixed::Payment::NONE: break;
      case IoMixed::Payment::CASH: CHECK(p.getCash() == std::get<1>(m.payment)); break;
      case IoMixed::Payment::CARD: CHECK(str(p.getCard()) == std::get<2>(m.payment)); break;
      case IoMixed::Payment::OWNER: CHECK(get(p.getOwner()) == std::get<3>(m.payment)); break;
      case IoMixed::Payment::AMOUNT: CHECK(p.getAmount() == std::get<4>(m.payment)); break;
   }
   CHECK(r.getScore().isSome() == m.score.has_value());
   if (m.score)
      CHECK(r.getScore().getSome() == *m.score);
   CHECK(r.hasMemo() == m.memo.has_value());
   if (m.memo)
      CHECK(str(r.getMemo()) == *m.memo);
   REQUIRE(r.getBits().size() == m.bits.size());
   for (unsigned i = 0; i < m.bits.size(); ++i)
      CHECK(r.getBits()[i] == m.bits[i]);
   REQUIRE(r.getBlob().size() == m.blob.size());
   CHECK(std::memcmp(r.getBlob().begin(), m.blob.data(), m.blob.size()) == 0);
   REQUIRE(r.getNames().size() == m.names.size());
   for (unsigned i = 0; i < m.names.size(); ++i)
      CHECK(str(r.getNames()[i]) == m.names[i]);
   REQUIRE(r.getGrid().size() == m.grid.size());
   for (unsigned i = 0; i < m.grid.size(); ++i)
   {
      REQUIRE(r.getGrid()[i].size() == m.grid[i].size());
      for (unsigned j = 0; j < m.grid[i].size(); ++j)
         CHECK(r.getGrid()[i][j] == m.grid[i][j]);
   }
   REQUIRE(r.getPeople().size() == m.people.size());
   for (unsigned i = 0; i < m.people.size(); ++i)
      CHECK(get(r.getPeople()[i]) == m.people[i]);
   CHECK(r.getWide() == m.wide);
}

static std::vector<char> official_mixed(const PMixed& m, bool many_segments)
{
   // A first segment of one word forces every object into later segments,
   // so the message is all far pointers.
   capnp::MallocMessageBuilder mb(many_segments ? 1 : 1024,
                                  many_segments ? capnp::AllocationStrategy::FIXED_SIZE
                                                : capnp::AllocationStrategy::GROW_HEURISTICALLY);
   auto b = mb.initRoot<IoMixed>();
   b.setFlag(m.flag);
   b.setSmall(m.small);
   b.setLevel(m.level);
   b.setRatio(m.ratio);
   b.setOn(m.on);
   auto p = b.getPayment();
   switch (m.payment.index())
   {
      case 0: p.setNone(); break;
      case 1: p.setCash(std::get<1>(m.payment)); break;
      case 2: p.setCard(std::get<2>(m.payment)); break;
      case 3: put(p.initOwner(), std::get<3>(m.payment)); break;
      case 4: p.setAmount(std::get<4>(m.payment)); break;
   }
   if (m.score)
      b.getScore().setSome(*m.score);
   if (m.memo)
      b.setMemo(*m.memo);
   auto bits = b.initBits(static_cast<unsigned>(m.bits.size()));
   for (unsigned i = 0; i < m.bits.size(); ++i)
      bits.set(i, m.bits[i]);
   b.setBlob(kj::arrayPtr(reinterpret_cast<const kj::byte*>(m.blob.data()), m.blob.size()));
   auto names = b.initNames(static_cast<unsigned>(m.names.size()));
   for (unsigned i = 0; i < m.names.size(); ++i)
      names.set(i, m.names[i]);
   auto grid = b.initGrid(static_cast<unsigned>(m.grid.size()));
   for (unsigned i = 0; i < m.grid.size(); ++i)
   {
      auto row = grid.init(i, static_cast<unsigned>(m.grid[i].size()));
      for (unsigned j = 0; j < m.grid[i].size(); ++j)
         row.set(j, m.grid[i][j]);
   }
   auto people = b.initPeople(static_cast<unsigned>(m.people.size()));
   for (unsigned i = 0; i < m.people.size(); ++i)
      put(people[i], m.people[i]);
   b.setWide(m.wide);
   return bytes_of(mb);
}

TEST_CASE("capnp interop: unions, optionals, defaults, bit lists — psio to libcapnp",
          "[capnp][interop]")
{
   for (int which = 0; which < 10; ++which)
   {
      const auto m     = sample_mixed(which);
      const auto bytes = psio::to_capnp(m);
      auto       words = words_of(bytes);
      capnp::FlatArrayMessageReader reader(words);
      check_official(reader.getRoot<IoMixed>(), m);
   }
}

TEST_CASE("capnp interop: unions, optionals, defaults, bit lists — libcapnp to psio",
          "[capnp][interop]")
{
   for (int segments = 0; segments < 2; ++segments)
      for (int which = 0; which < 10; ++which)
      {
         auto m = sample_mixed(which);
         // libcapnp cannot tell an empty list in a list from a null one.
         const auto bytes = official_mixed(m, segments == 1);
         if (segments == 1)
            REQUIRE(psio::detail::capnp_impl::load32(
                       reinterpret_cast<const std::uint8_t*>(bytes.data())) > 0);
         const auto st = psio::validate_capnp<PMixed>(std::span<const char>{bytes});
         INFO((st.ok() ? std::string() : std::string(st.error().what)));
         REQUIRE(st.ok());
         psio::capnp_view<PMixed> v{std::span<const char>{bytes}};
         CHECK(v.level() == m.level);
         CHECK(v.on() == m.on);
         CHECK(v.payment().index() == m.payment.index());
         CHECK(v.score().has_value() == m.score.has_value());
         CHECK(v.memo().has_value() == m.memo.has_value());
         CHECK(v.bits().size() == m.bits.size());
         CHECK(v.people()[1].email() == "dd");
         CHECK(v.grid()[0][1] == -2);
         CHECK(v.names()[2] == "three");
         CHECK(v.wide() == m.wide);
         CHECK(v.unpack() == m);
         CHECK(psio::from_capnp<PMixed>(std::span<const char>{bytes}) == m);

         // Every truncation is rejected, never read out of bounds.
         for (std::size_t n = 0; n < bytes.size(); n += 3)
            CHECK_FALSE(
               psio::validate_capnp<PMixed>(std::span<const char>{bytes.data(), n}).ok());
      }
}

TEST_CASE("capnp interop: an edited document is a message libcapnp reads", "[capnp][interop]")
{
   auto                        m = sample_mixed(3);
   psio::capnp_document<PMixed> doc{m};
   REQUIRE(doc.edit().memo().set(std::optional<std::string>{"a longer memo than there was"}));
   REQUIRE(doc.edit().names().set(std::vector<std::string>{"x", "yy", "zzz", "wwww"}));
   REQUIRE(doc.edit().people().set(std::vector<PProfile>{{9, "nine", "9@x", 9, true}}));
   REQUIRE(doc.edit().level().set(1234));
   m.memo   = "a longer memo than there was";
   m.names  = {"x", "yy", "zzz", "wwww"};
   m.people = {{9, "nine", "9@x", 9, true}};
   m.level  = 1234;
   REQUIRE(psio::validate_capnp<PMixed>(doc.bytes()).ok());
   CHECK(doc.unpack() == m);
   {
      auto                          words = words_of(doc.bytes());
      capnp::FlatArrayMessageReader reader(words);
      check_official(reader.getRoot<IoMixed>(), m);
   }
   const auto grown = doc.bytes().size();
   doc.compact();
   CHECK(doc.bytes().size() < grown);
   CHECK(doc.unpack() == m);
   const auto fresh = psio::to_capnp(m);
   CHECK(std::vector<char>(doc.bytes().begin(), doc.bytes().end()) == fresh);
}
