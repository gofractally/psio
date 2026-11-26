// Safe and trusted views (view.hpp "Safe and trusted views").
//
// `psio::F_view<T>` is the safe view: it compares what it reads from the
// bytes before using it. `psio::F_trusted_view<T>` is the view of bytes the
// caller vouches for: the same templates instantiated for the other storage
// kind, without those compares.
//
//   * the whole read surface exists for both kinds and reads the same values
//     (and hands out the same spans) from valid bytes — every case below is
//     instantiated for both;
//   * trust is only obtained explicitly, propagates to sub-views, and
//     converts one way: trusted -> safe;
//   * a default-constructed trusted view, and `at(i)` past the end, read as
//     zero / empty;
//   * bytes validation accepts are bytes the trusted view reads inside of.
//
// On valid bytes no compare a view makes may fail: every case below reads
// valid bytes through both kinds.

#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace
{
   struct TvProfile
   {
      std::uint64_t id = 0;
      std::string   name;
      std::string   email;
      std::uint32_t age      = 0;
      bool          verified = false;
      friend bool   operator==(const TvProfile&, const TvProfile&) = default;
   };
   PSIO_REFLECT(TvProfile, id, name, email, age, verified)

   struct TvItem
   {
      std::string   product;
      std::uint32_t qty   = 0;
      double        price = 0;
      friend bool   operator==(const TvItem&, const TvItem&) = default;
   };
   PSIO_REFLECT(TvItem, product, qty, price)

   struct TvPoint
   {
      std::int32_t x = 0, y = 0;
      friend bool  operator==(const TvPoint&, const TvPoint&) = default;
      friend auto  operator<=>(const TvPoint&, const TvPoint&) = default;
   };
   PSIO_REFLECT(TvPoint, x, y, definitionWillNotChange())

   // Shapes every format here has: records, lists of fixed and variable
   // elements, strings.
   struct TvOrder
   {
      std::uint64_t              id = 0;
      TvProfile                  customer;
      std::vector<TvItem>        items;
      TvPoint                    at;
      double                     total = 0;
      std::string                note;
      std::vector<std::uint32_t> numbers;
      friend bool                operator==(const TvOrder&, const TvOrder&) = default;
   };
   PSIO_REFLECT(TvOrder, id, customer, items, at, total, note, numbers)

   enum class TvColor : std::uint8_t
   {
      red,
      green,
      blue
   };

   // Everything else the PSSZ view has.
   struct TvWide
   {
      std::uint32_t                             id = 0;
      std::optional<std::uint32_t>              maybe_number;
      std::optional<std::string>                maybe_text;
      std::optional<TvPoint>                    maybe_point;
      std::optional<std::vector<std::uint8_t>>  maybe_list;  // min size 0: a selector byte
      std::variant<std::uint32_t, std::string, TvPoint> choice;
      std::tuple<std::uint32_t, std::string, TvPoint>   triple;
      std::pair<std::string, std::uint16_t>     duo;
      std::map<std::string, std::uint32_t>      limits;
      std::map<std::uint32_t, TvItem>           by_number;
      std::set<std::int32_t>                    tags;
      std::array<TvPoint, 3>                    corners{};
      std::array<std::string, 2>                names;
      std::vector<std::string>                  words;
      std::vector<unsigned char>                octets;
      std::array<std::uint8_t, 4>               quad{};
      TvColor                                   color = TvColor::red;
      psio::nested<TvItem>                      packed;
      std::optional<std::uint32_t>              trailing_a;
      std::optional<std::string>                trailing_b;
      friend bool operator==(const TvWide&, const TvWide&) = default;
   };
   PSIO_REFLECT(TvWide, id, maybe_number, maybe_text, maybe_point, maybe_list, choice, triple, duo,
                limits, by_number, tags, corners, names, words, octets, quad, color, packed,
                trailing_a, trailing_b)

   TvOrder sample_order()
   {
      TvOrder o;
      o.id       = 7;
      o.customer = {9, "Alice", "alice@example.org", 37, true};
      o.items    = {{"a", 1, 1.5}, {"bb", 2, 2.5}, {"ccc", 3, 3.5}};
      o.at       = {-4, 5};
      o.total    = 12.5;
      o.note     = "leave at the door";
      o.numbers  = {10, 20, 30, 40};
      return o;
   }
   TvWide sample_wide()
   {
      TvWide w;
      w.id           = 3;
      w.maybe_number = 11;
      w.maybe_text   = "text";
      w.maybe_point  = TvPoint{1, 2};
      w.maybe_list   = std::vector<std::uint8_t>{};
      w.choice       = std::string("chosen");
      w.triple       = {5, "five", TvPoint{5, 55}};
      w.duo          = {"two", 2};
      w.limits       = {{"alpha", 1}, {"beta", 2}, {"gamma", 3}, {"delta", 4}};
      w.by_number    = {{1, {"one", 1, 1.0}}, {5, {"five", 5, 5.0}}, {9, {"nine", 9, 9.0}}};
      w.tags         = {-7, 0, 3, 100, 1000};
      w.corners      = {TvPoint{1, 1}, TvPoint{2, 2}, TvPoint{3, 3}};
      w.names        = {"first", "second"};
      w.words        = {"", "w", "word"};
      w.octets       = {1, 2, 3};
      w.quad         = {9, 8, 7, 6};
      w.color        = TvColor::blue;
      w.packed       = psio::nested<TvItem>{TvItem{"inner", 4, 4.5}};
      return w;
   }

   // ── The two kinds ─────────────────────────────────────────────────────
   struct safe_kind
   {
      static constexpr psio::storage store = psio::storage::const_borrow;
      template <typename T, typename Fmt>
      static psio::view<T, Fmt, store> make(Fmt, std::span<const char> bytes)
      {
         return psio::view<T, Fmt, store>{bytes};
      }
   };
   struct trusted_kind
   {
      static constexpr psio::storage store = psio::storage::trusted_borrow;
      template <typename T, typename Fmt>
      static psio::view<T, Fmt, store> make(Fmt, std::span<const char> bytes)
      {
         return psio::view<T, Fmt, store>{psio::trusted{bytes}};
      }
   };

   template <typename V>
   constexpr psio::storage kind_of(const V&)
   {
      return V::storage_kind;
   }

   template <typename V>
   void check_order_values(const V& v, const TvOrder& o);

   // `Values`: the format's view has unpack() and, through it, comparison
   // of composite views (PSSZ; the SSZ view navigates only).
   template <bool Values, typename V>
   void check_order(const V& v, const TvOrder& o)
   {
      // Named readers, nested records, scalars, strings.
      CHECK(v.id() == o.id);
      CHECK(v.customer().id() == o.customer.id);
      CHECK(v.customer().name() == o.customer.name);
      CHECK(v.customer().name().view_() == o.customer.name);
      CHECK(std::string_view(v.customer().email()) == o.customer.email);
      CHECK(v.customer().email().size() == o.customer.email.size());
      CHECK(v.customer().age() == o.customer.age);
      CHECK(v.customer().verified() == o.customer.verified);
      CHECK(v.at().x() == o.at.x);
      CHECK(v.at().y() == o.at.y);
      CHECK(v.total() == o.total);
      CHECK(v.note() == o.note);
      CHECK(v.template get<0>() == o.id);
      CHECK(v.template get<5>() == o.note);

      // Lists: size, index, at, front / back, iteration both ways.
      auto items = v.items();
      REQUIRE(items.size() == o.items.size());
      CHECK_FALSE(items.empty());
      for (std::size_t i = 0; i < o.items.size(); ++i)
      {
         CHECK(items[i].product() == o.items[i].product);
         CHECK(items[i].qty() == o.items[i].qty);
         CHECK(items[i].price() == o.items[i].price);
         CHECK(items.at(i).qty() == o.items[i].qty);
      }
      CHECK(items.front().product() == o.items.front().product);
      CHECK(items.back().product() == o.items.back().product);
      std::size_t n = 0;
      for (auto it : items)
      {
         CHECK(it.qty() == o.items[n].qty);
         CHECK(it.product() == o.items[n].product);
         ++n;
      }
      CHECK(n == o.items.size());
      std::uint32_t sum = 0;
      for (auto it = items.rbegin(); it != items.rend(); ++it)
         sum += (*it).qty();
      CHECK(sum == 6);
      CHECK((items.begin() + 2)->qty() == 3);
      CHECK(items.end() - items.begin() == 3);

      auto numbers = v.numbers();
      REQUIRE(numbers.size() == o.numbers.size());
      for (std::size_t i = 0; i < o.numbers.size(); ++i)
         CHECK(numbers[i] == o.numbers[i]);
      std::uint32_t total = 0;
      for (std::uint32_t x : numbers)
         total += x;
      CHECK(total == 100);

      // at(i) past the end: checked in both kinds.
      CHECK(items.at(o.items.size()).qty() == 0);
      CHECK(items.at(o.items.size()).product().view_().empty());
      CHECK(items.at(1u << 30).price() == 0);
      CHECK(numbers.at(4) == 0u);
      CHECK(numbers.at(~std::size_t{0}) == 0u);

      // The kind carries over to every sub-view.
      CHECK(kind_of(v.customer()) == V::storage_kind);
      CHECK(kind_of(v.customer().name()) == V::storage_kind);
      CHECK(kind_of(items) == V::storage_kind);
      CHECK(kind_of(items[1]) == V::storage_kind);
      CHECK(kind_of(items[1].product()) == V::storage_kind);
      CHECK(kind_of(*items.begin()) == V::storage_kind);
      CHECK(kind_of(items.at(0)) == V::storage_kind);
      CHECK(kind_of(numbers[0]) == V::storage_kind);

      CHECK(v.note() == std::string_view("leave at the door"));
      CHECK(v.note() < std::string_view("zzz"));
      CHECK(v.customer().name() == v.customer().name());
      if constexpr (!Values)
         return;
      else
         check_order_values(v, o);
   }

   template <typename V>
   void check_order_values(const V& v, const TvOrder& o)
   {
      auto items   = v.items();
      auto numbers = v.numbers();
      // unpack, comparison with values and with views.
      CHECK(v.unpack() == o);
      CHECK(v.customer().unpack() == o.customer);
      CHECK(items.unpack() == o.items);
      CHECK(items[1].unpack() == o.items[1]);
      CHECK(numbers.unpack() == o.numbers);
      CHECK(psio::value_of(v.customer()) == o.customer);
      CHECK(v == o);
      CHECK(v.customer() == o.customer);
      CHECK(v.customer() == v.customer());
      CHECK(items[0] == o.items[0]);
      CHECK_FALSE(items[0] == o.items[1]);
   }

   template <typename Kind>
   void check_wide(const TvWide& w)
   {
      const auto bytes = psio::to_pssz(w);
      REQUIRE(psio::validate_pssz<TvWide>(bytes).ok());
      const auto v = Kind::template make<TvWide>(psio::pssz{}, std::span<const char>{bytes});
      constexpr auto kind = Kind::store;

      CHECK(v.id() == w.id);

      // Optionals: length-disambiguated and with a selector byte.
      CHECK(v.maybe_number().has_value() == w.maybe_number.has_value());
      CHECK(v.maybe_number().value_or(99u) == w.maybe_number.value_or(99u));
      CHECK(v.maybe_text().has_value() == w.maybe_text.has_value());
      CHECK(v.maybe_text().value_or("none") == w.maybe_text.value_or("none"));
      CHECK(static_cast<bool>(v.maybe_point()) == w.maybe_point.has_value());
      CHECK(v.maybe_list().has_value() == w.maybe_list.has_value());
      if (w.maybe_number)
      {
         CHECK(*v.maybe_number() == *w.maybe_number);
         CHECK(v.maybe_number().get() == *w.maybe_number);
         CHECK(v.maybe_number().value() == *w.maybe_number);
         CHECK(kind_of(*v.maybe_number()) == kind);
      }
      if (w.maybe_text)
      {
         CHECK(*v.maybe_text() == *w.maybe_text);
         CHECK(v.maybe_text()->size() == w.maybe_text->size());
      }
      if (w.maybe_point)
      {
         CHECK(v.maybe_point()->x() == w.maybe_point->x);
         CHECK(v.maybe_point()->y() == w.maybe_point->y);
         CHECK(kind_of(*v.maybe_point()) == kind);
      }
      if (w.maybe_list)
         CHECK((*v.maybe_list()).size() == w.maybe_list->size());
      CHECK(v.maybe_number().unpack() == w.maybe_number);
      CHECK(v.maybe_text().unpack() == w.maybe_text);
      CHECK(v.maybe_list().unpack() == w.maybe_list);
      // Trailing optionals (pruned when absent).
      CHECK(v.trailing_a().has_value() == w.trailing_a.has_value());
      CHECK(v.trailing_b().has_value() == w.trailing_b.has_value());
      CHECK(v.trailing_a().value_or(5u) == w.trailing_a.value_or(5u));
      CHECK(v.trailing_b().value_or("x") == w.trailing_b.value_or("x"));
      CHECK(v.trailing_a().unpack() == w.trailing_a);
      CHECK(v.trailing_b().unpack() == w.trailing_b);

      // Variant.
      CHECK(v.choice().index() == w.choice.index());
      CHECK(v.choice().unpack() == w.choice);
      const auto seen = v.choice().visit([&](auto alt) -> std::size_t {
         CHECK(kind_of(alt) == kind);
         return psio::size(alt);
      });
      (void)seen;
      if (auto* s = std::get_if<std::string>(&w.choice))
      {
         REQUIRE(v.choice().template get_if<std::string>().has_value());
         CHECK(*v.choice().template get_if<std::string>() == *s);
         CHECK(v.choice().template get_if<1>()->view_() == *s);
         CHECK(v.choice().template holds_alternative<std::string>());
         CHECK_FALSE(v.choice().template get_if<TvPoint>().has_value());
         CHECK(kind_of(*v.choice().template get_if<std::string>()) == kind);
      }
      if (auto* p = std::get_if<TvPoint>(&w.choice))
      {
         REQUIRE(v.choice().template get_if<TvPoint>().has_value());
         CHECK(v.choice().template get_if<TvPoint>()->y() == p->y);
      }
      if (auto* u = std::get_if<std::uint32_t>(&w.choice))
      {
         REQUIRE(v.choice().template get_if<0>().has_value());
         CHECK(*v.choice().template get_if<0>() == *u);
      }

      // Tuple, pair, structured bindings.
      CHECK(v.triple().template get<0>() == std::get<0>(w.triple));
      CHECK(v.triple().template get<1>() == std::get<1>(w.triple));
      CHECK(v.triple().template get<2>().y() == std::get<2>(w.triple).y);
      CHECK(v.triple().unpack() == w.triple);
      CHECK(kind_of(v.triple().template get<1>()) == kind);
      {
         auto [number, text, point] = v.triple();
         CHECK(number == std::get<0>(w.triple));
         CHECK(text == std::get<1>(w.triple));
         CHECK(point.x() == std::get<2>(w.triple).x);
      }
      CHECK(v.duo().first() == w.duo.first);
      CHECK(v.duo().second() == w.duo.second);
      CHECK(v.duo().unpack() == w.duo);
      {
         auto [text, number] = v.duo();
         CHECK(text == w.duo.first);
         CHECK(number == w.duo.second);
      }

      // Map / set: the list of entries, and lookup.
      auto limits = v.limits();
      REQUIRE(limits.size() == w.limits.size());
      std::size_t i = 0;
      for (const auto& [key, value] : w.limits)
      {
         CHECK(limits[i].template get<0>() == key);
         CHECK(limits[i].template get<1>() == value);
         CHECK(limits.contains(key));
         CHECK(limits.find(key) == i);
         CHECK(limits.lower_bound(key) == i);
         CHECK(limits.value_or(key, 0u) == value);
         REQUIRE(limits.at(key).has_value());
         CHECK(*limits.at(key) == value);
         ++i;
      }
      CHECK_FALSE(limits.contains("nope"));
      CHECK(limits.find("nope") == limits.size());
      CHECK(limits.value_or("nope", 77u) == 77u);
      CHECK_FALSE(limits.at("nope").has_value());
      CHECK(limits.unpack() == w.limits);

      auto by_number = v.by_number();
      for (const auto& [key, value] : w.by_number)
      {
         REQUIRE(by_number.contains(key));
         auto found = by_number.at(key);
         REQUIRE(found.has_value());
         CHECK(found->product() == value.product);
         CHECK(found->qty() == value.qty);
         CHECK(kind_of(*found) == kind);
         CHECK(by_number.value_or(key, TvItem{}) == value);
      }
      CHECK_FALSE(by_number.contains(4u));
      CHECK(by_number.unpack() == w.by_number);

      auto tags = v.tags();
      REQUIRE(tags.size() == w.tags.size());
      for (auto tag : w.tags)
         CHECK(tags.contains(tag));
      CHECK_FALSE(tags.contains(1));
      CHECK_FALSE(tags.contains(-1000));
      if (w.tags.size() == 5)
         CHECK(tags.lower_bound(1) == 2);
      CHECK(tags.unpack() == w.tags);

      // Arrays of fixed and of variable elements; a list of strings.
      for (std::size_t c = 0; c < w.corners.size(); ++c)
      {
         CHECK(v.corners()[c].x() == w.corners[c].x);
         CHECK(v.corners().at(c).y() == w.corners[c].y);
      }
      CHECK(v.corners().at(3).x() == 0);
      CHECK(v.corners().size() == 3);
      CHECK(v.corners().unpack() == w.corners);
      CHECK(v.names()[0] == w.names[0]);
      CHECK(v.names()[1] == w.names[1]);
      CHECK(v.names().back() == w.names[1]);
      REQUIRE(v.words().size() == w.words.size());
      for (std::size_t c = 0; c < w.words.size(); ++c)
         CHECK(v.words()[c] == w.words[c]);
      CHECK(v.words().at(w.words.size()).view_().empty());
      CHECK(v.words().unpack() == w.words);

      // G50 conversions: a trivially copyable value of fixed size is its
      // value; a byte sequence is what borrows it.
      {
         const TvPoint corner = v.corners()[1];
         CHECK(corner == w.corners[1]);
         const std::array<TvPoint, 3> corners = v.corners();
         CHECK(corners == w.corners);
         const std::string_view first = v.names()[0];
         CHECK(first == w.names[0]);
         const std::span<const unsigned char> octets = v.octets();
         REQUIRE(octets.size() == w.octets.size());
         CHECK(std::equal(octets.begin(), octets.end(), w.octets.begin()));
         const std::span<const std::uint8_t, 4> quad = v.quad();
         CHECK(quad[2] == w.quad[2]);
         const std::span<const std::uint8_t> quad_dynamic = v.quad();
         CHECK(quad_dynamic.size() == 4);
         const std::array<std::uint8_t, 4> quad_value = v.quad();
         CHECK(quad_value == w.quad);
      }
      CHECK(v.color() == w.color);
      CHECK(v.color().get() == w.color);
      CHECK(v.corners()[0] == w.corners[0]);
      CHECK(v.corners()[0] < w.corners[1]);

      // A nested (packed) member: the inner value's view has the kind too
      // (the default nested validation is eager).
      CHECK(v.packed()->product() == w.packed.view().product());
      CHECK(v.packed().get().qty() == 4);
      CHECK(kind_of(v.packed().get()) == kind);

      CHECK(v.unpack() == w);
   }
}  // namespace

TEMPLATE_TEST_CASE("view kinds: named readers, records, lists, strings, unpack, compare (PSSZ)",
                   "[view][trusted]", safe_kind, trusted_kind)
{
   const auto o     = sample_order();
   const auto bytes = psio::to_pssz(o);
   REQUIRE(psio::validate_pssz<TvOrder>(bytes).ok());
   check_order<true>(TestType::template make<TvOrder>(psio::pssz{}, std::span<const char>{bytes}),
                     o);
}

TEMPLATE_TEST_CASE("view kinds: named readers, records, lists, strings (SSZ)",
                   "[view][trusted]", safe_kind, trusted_kind)
{
   const auto o     = sample_order();
   const auto bytes = psio::to_ssz(o);
   REQUIRE(psio::validate_ssz<TvOrder>(bytes).ok());
   check_order<false>(TestType::template make<TvOrder>(psio::ssz{}, std::span<const char>{bytes}),
                      o);
}

TEMPLATE_TEST_CASE("view kinds: named readers, records, lists, strings (canonical ABI)",
                   "[view][trusted]", safe_kind, trusted_kind)
{
   const auto o     = sample_order();
   const auto bytes = psio::to_wit(o);
   REQUIRE(psio::validate_wit<TvOrder>(bytes).ok());
   const auto v = TestType::template make<TvOrder>(psio::wit{}, std::span<const char>{bytes});
   CHECK(v.id() == o.id);
   CHECK(v.customer().id() == o.customer.id);
   CHECK(v.customer().name().view_() == o.customer.name);
   CHECK(v.customer().email().view_() == o.customer.email);
   CHECK(v.customer().age() == o.customer.age);
   CHECK(v.customer().verified() == o.customer.verified);
   CHECK(v.at().x() == o.at.x);
   CHECK(v.at().y() == o.at.y);
   CHECK(v.total() == o.total);
   CHECK(v.note().view_() == o.note);
   auto items = v.items();
   REQUIRE(items.size() == o.items.size());
   std::size_t n = 0;
   for (auto it : items)
   {
      CHECK(it.product().view_() == o.items[n].product);
      CHECK(it.qty() == o.items[n].qty);
      CHECK(it.price() == o.items[n].price);
      CHECK(items[n].qty() == o.items[n].qty);
      CHECK(kind_of(it) == TestType::store);
      CHECK(kind_of(it.product()) == TestType::store);
      ++n;
   }
   CHECK(items.at(3).qty() == 0);
   CHECK(items.at(3).product().view_().empty());
   CHECK(items.at(1u << 30).price() == 0);
   auto numbers = v.numbers();
   REQUIRE(numbers.size() == o.numbers.size());
   for (std::size_t i = 0; i < o.numbers.size(); ++i)
      CHECK(numbers[i] == o.numbers[i]);
   CHECK(numbers.at(4) == 0u);
}

TEMPLATE_TEST_CASE("view kinds: optionals, variants, tuples, pairs, maps, sets, arrays, "
                   "conversions (PSSZ)",
                   "[view][trusted]", safe_kind, trusted_kind)
{
   auto w = sample_wide();
   SECTION("every optional present but the trailing ones") { check_wide<TestType>(w); }
   SECTION("trailing optionals present")
   {
      w.trailing_a = 1;
      check_wide<TestType>(w);
      w.trailing_b = "b";
      check_wide<TestType>(w);
      w.trailing_a.reset();
      check_wide<TestType>(w);
   }
   SECTION("optionals absent, other alternatives")
   {
      w.maybe_number.reset();
      w.maybe_text.reset();
      w.maybe_point.reset();
      w.maybe_list.reset();
      w.choice = TvPoint{8, 9};
      check_wide<TestType>(w);
      w.choice = std::uint32_t{12};
      check_wide<TestType>(w);
   }
   SECTION("empty containers")
   {
      w.limits.clear();
      w.by_number.clear();
      w.tags.clear();
      w.words.clear();
      w.octets.clear();
      w.maybe_list = std::vector<std::uint8_t>{1, 2};
      check_wide<TestType>(w);
   }
}

TEST_CASE("trusted view: the two kinds hand out the same spans on valid bytes", "[view][trusted]")
{
   const auto same = [](std::span<const char> a, std::span<const char> b) {
      return a.size() == b.size() && (a.empty() || a.data() == b.data());
   };
   const auto o = sample_order();
   const auto walk = [&](const auto& s, const auto& t) {
      CHECK(same(psio::bytes(s), psio::bytes(t)));
      CHECK(same(psio::bytes(s.customer()), psio::bytes(t.customer())));
      CHECK(same(psio::bytes(s.customer().name()), psio::bytes(t.customer().name())));
      CHECK(same(psio::bytes(s.customer().email()), psio::bytes(t.customer().email())));
      CHECK(same(psio::bytes(s.items()), psio::bytes(t.items())));
      for (std::size_t i = 0; i < 3; ++i)
      {
         CHECK(same(psio::bytes(s.items()[i]), psio::bytes(t.items()[i])));
         CHECK(same(psio::bytes(s.items()[i].product()), psio::bytes(t.items()[i].product())));
      }
      CHECK(same(psio::bytes(s.at()), psio::bytes(t.at())));
      CHECK(same(psio::bytes(s.note()), psio::bytes(t.note())));
      CHECK(same(psio::bytes(s.numbers()), psio::bytes(t.numbers())));
      CHECK(same(psio::bytes(s.numbers()[3]), psio::bytes(t.numbers()[3])));
   };
   const auto p = psio::to_pssz(o);
   const auto s = psio::to_ssz(o);
   const auto w = psio::to_wit(o);
   walk(psio::pssz_view<TvOrder>{std::span<const char>{p}},
        psio::pssz_trusted_view<TvOrder>{psio::trusted{p}});
   walk(psio::ssz_view<TvOrder>{std::span<const char>{s}},
        psio::ssz_trusted_view<TvOrder>{psio::trusted{s}});
   walk(psio::wit_view<TvOrder>{std::span<const char>{w}},
        psio::wit_trusted_view<TvOrder>{psio::trusted{w}});
}

TEST_CASE("trusted view: the types", "[view][trusted]")
{
   using safe    = psio::pssz_view<TvOrder>;
   using trusted = psio::pssz_trusted_view<TvOrder>;
   static_assert(std::is_same_v<safe, psio::view<TvOrder, psio::pssz, psio::storage::const_borrow>>);
   static_assert(
      std::is_same_v<trusted, psio::view<TvOrder, psio::pssz, psio::storage::trusted_borrow>>);
   static_assert(std::is_same_v<psio::pssz_trusted_view<const TvOrder>, trusted>);
   static_assert(!std::is_same_v<safe, trusted>);
   static_assert(safe::storage_kind == psio::storage::const_borrow);
   static_assert(trusted::storage_kind == psio::storage::trusted_borrow);

   // The safe view's layout is what it was: two spans and one word of
   // per-record state. The trusted view is the same size.
#ifndef PSIO_VIEW_NO_RECORD_STATE
   static_assert(sizeof(safe) == 2 * sizeof(std::span<const char>) + sizeof(std::size_t));
#endif
   static_assert(sizeof(trusted) == sizeof(safe));
   static_assert(std::is_trivially_copyable_v<trusted>);

   // Trust is only obtained explicitly ...
   static_assert(std::is_constructible_v<safe, std::span<const char>>);
   static_assert(std::is_constructible_v<safe, const std::vector<char>&>);
   static_assert(std::is_constructible_v<trusted, psio::trusted>);
   static_assert(!std::is_constructible_v<trusted, std::span<const char>>);
   static_assert(!std::is_constructible_v<trusted, std::span<char>>);
   static_assert(!std::is_constructible_v<trusted, const std::vector<char>&>);
   static_assert(!std::is_constructible_v<trusted, std::vector<char>&>);
   static_assert(!std::is_constructible_v<trusted, std::span<const char>, std::span<const char>>);
   static_assert(!std::is_constructible_v<trusted, std::string_view>);
   static_assert(!std::is_convertible_v<psio::trusted, trusted>);  // explicit
   static_assert(!std::is_constructible_v<safe, psio::trusted>);
   // ... converts one way ...
   static_assert(std::is_convertible_v<trusted, safe>);
   static_assert(!std::is_convertible_v<safe, trusted>);
   static_assert(!std::is_constructible_v<trusted, safe>);
   static_assert(!std::is_assignable_v<trusted&, safe>);
   static_assert(std::is_assignable_v<safe&, trusted>);
   // ... for sub-views too.
   static_assert(std::is_convertible_v<psio::pssz_trusted_view<std::string>,
                                       psio::pssz_view<std::string>>);
   static_assert(!std::is_convertible_v<psio::pssz_view<std::string>,
                                        psio::pssz_trusted_view<std::string>>);
   static_assert(std::is_convertible_v<psio::pssz_trusted_view<std::vector<TvItem>>,
                                       psio::pssz_view<std::vector<TvItem>>>);
   static_assert(!std::is_convertible_v<psio::pssz_view<std::vector<TvItem>>,
                                        psio::pssz_trusted_view<std::vector<TvItem>>>);
   static_assert(std::is_convertible_v<psio::pssz_trusted_view<std::uint32_t>,
                                       psio::pssz_view<std::uint32_t>>);
   static_assert(!std::is_constructible_v<psio::pssz_trusted_view<std::uint32_t>,
                                          std::span<const char>>);
   static_assert(!std::is_constructible_v<psio::pssz_trusted_view<std::string>,
                                          std::span<const char>>);
   static_assert(!std::is_constructible_v<psio::pssz_trusted_view<std::vector<TvItem>>,
                                          std::span<const char>>);
   static_assert(!std::is_constructible_v<psio::pssz_trusted_view<std::optional<TvPoint>>,
                                          std::span<const char>>);
   static_assert(!std::is_constructible_v<psio::pssz_trusted_view<std::map<std::string, int>>,
                                          std::span<const char>>);
   static_assert(!std::is_constructible_v<psio::ssz_trusted_view<TvOrder>, std::span<const char>>);
   static_assert(!std::is_constructible_v<psio::wit_trusted_view<TvOrder>, std::span<const char>>);
   static_assert(!std::is_convertible_v<psio::ssz_view<TvOrder>, psio::ssz_trusted_view<TvOrder>>);
   static_assert(std::is_convertible_v<psio::ssz_trusted_view<TvOrder>, psio::ssz_view<TvOrder>>);
   static_assert(std::is_convertible_v<psio::wit_trusted_view<TvOrder>, psio::wit_view<TvOrder>>);
   // Read-only: a member is returned by value, a sub-view is a view.
   static_assert(std::is_same_v<decltype(std::declval<const trusted&>().id()), std::uint64_t>);
   static_assert(std::is_same_v<decltype(std::declval<const trusted&>().note()),
                                psio::pssz_trusted_view<std::string>>);
   SUCCEED();
}

namespace
{
   std::string_view name_of(psio::pssz_view<TvProfile> v) { return v.name(); }
   std::uint32_t    quantity(psio::pssz_view<TvItem> v) { return v.qty(); }
}  // namespace

TEST_CASE("trusted view: converts implicitly to the safe view of the same bytes",
          "[view][trusted]")
{
   const auto o     = sample_order();
   const auto bytes = psio::to_pssz(o);
   const psio::pssz_trusted_view<TvOrder> t{psio::trusted{bytes}};

   const psio::pssz_view<TvOrder> s = t;
   CHECK(psio::bytes(s).data() == bytes.data());
   CHECK(psio::bytes(s).size() == bytes.size());
   CHECK(s == o);
   CHECK(s.customer().name() == "Alice");

   // Sub-views: record, list, element, string, scalar list.
   CHECK(name_of(t.customer()) == "Alice");
   CHECK(quantity(t.items()[2]) == 3);
   const psio::pssz_view<std::vector<TvItem>> items = t.items();
   CHECK(items.size() == 3);
   CHECK(items[1].product() == "bb");
   const psio::pssz_view<std::string> note = t.note();
   CHECK(note == "leave at the door");
   const psio::pssz_view<TvPoint> at = t.at();
   CHECK(at.y() == 5);
   const psio::pssz_view<const TvProfile> customer = t.customer();
   CHECK(customer.age() == 37);

   // A safe view and a trusted view compare as the values they designate.
   CHECK(s == t);
   CHECK(t.customer() == s.customer());
   CHECK(t.note() == s.note());

   // The other formats.
   const auto sb = psio::to_ssz(o);
   const auto wb = psio::to_wit(o);
   const psio::ssz_view<TvOrder> ss = psio::ssz_trusted_view<TvOrder>{psio::trusted{sb}};
   const psio::wit_view<TvOrder> ws = psio::wit_trusted_view<TvOrder>{psio::trusted{wb}};
   CHECK(ss.items()[1].product() == "bb");
   CHECK(ws.items()[1].product().view_() == "bb");  // (root carried over)
   CHECK(ws.customer().email().view_() == "alice@example.org");

   // PSSZ shapes with their own view classes.
   const auto wide = sample_wide();
   const auto wbytes = psio::to_pssz(wide);
   const psio::pssz_trusted_view<TvWide> tw{psio::trusted{wbytes}};
   const psio::pssz_view<std::map<std::string, std::uint32_t>> limits = tw.limits();
   CHECK(limits.value_or("beta", 0u) == 2);
   const psio::pssz_view<std::set<std::int32_t>> tags = tw.tags();
   CHECK(tags.contains(100));
   const psio::pssz_view<std::tuple<std::uint32_t, std::string, TvPoint>> triple = tw.triple();
   CHECK(triple.get<1>() == "five");
   const psio::pssz_view<std::pair<std::string, std::uint16_t>> duo = tw.duo();
   CHECK(duo.second() == 2);
   const psio::pssz_view<std::optional<std::string>> maybe = tw.maybe_text();
   CHECK(maybe.value_or("") == "text");
   const psio::pssz_view<std::variant<std::uint32_t, std::string, TvPoint>> choice = tw.choice();
   CHECK(choice.index() == 1);
   const psio::pssz_view<std::array<TvPoint, 3>> corners = tw.corners();
   CHECK(corners[2].x() == 3);
   const psio::pssz_view<TvColor> color = tw.color();
   CHECK(color == TvColor::blue);
   const psio::pssz_view<psio::nested<TvItem>> packed = tw.packed();
   CHECK(packed->qty() == 4);
}

TEST_CASE("trusted view: default-constructed reads as zero / empty", "[view][trusted]")
{
   const auto check = [](const auto& v) {
      CHECK(v.id() == 0);
      CHECK(v.customer().id() == 0);
      CHECK(v.customer().name().view_().empty());
      CHECK(v.customer().age() == 0);
      CHECK_FALSE(v.customer().verified());
      CHECK(v.at().x() == 0);
      CHECK(v.total() == 0);
      CHECK(v.note().view_().empty());
      CHECK(v.items().size() == 0);
      CHECK(v.items().empty());
      CHECK(v.items().begin() == v.items().end());
      CHECK(v.items().at(0).qty() == 0);
      CHECK(v.items().at(5).product().view_().empty());
      CHECK(v.numbers().size() == 0);
      CHECK(v.numbers().at(0) == 0u);
   };
   check(psio::pssz_trusted_view<TvOrder>{});
   check(psio::ssz_trusted_view<TvOrder>{});
   check(psio::wit_trusted_view<TvOrder>{});
   // No bytes handed over by the caller are bytes, not a view of nothing:
   // they are not a frame of the record.
   CHECK_THROWS_AS(psio::pssz_trusted_view<TvOrder>{psio::trusted{std::span<const char>{}}}.id(),
                   psio::codec_exception);
   CHECK_THROWS_AS(psio::ssz_trusted_view<TvOrder>{psio::trusted{std::span<const char>{}}}.id(),
                   psio::codec_exception);
   CHECK_THROWS_AS(psio::wit_trusted_view<TvOrder>{psio::trusted{std::span<const char>{}}}.id(),
                   psio::codec_exception);

   psio::pssz_trusted_view<TvWide> w{};
   CHECK(w.id() == 0);
   CHECK_FALSE(w.maybe_number().has_value());
   CHECK_FALSE(w.maybe_text().has_value());
   CHECK_FALSE(w.maybe_list().has_value());
   CHECK_FALSE(w.trailing_a().has_value());
   CHECK(w.maybe_number().value_or(4u) == 4u);
   CHECK(w.choice().index() == 0);
   CHECK(w.triple().get<0>() == 0);
   CHECK(w.triple().get<1>().view_().empty());
   CHECK(w.duo().second() == 0);
   CHECK(w.limits().size() == 0);
   CHECK_FALSE(w.limits().contains("a"));
   CHECK(w.limits().value_or("a", 9u) == 9u);
   CHECK_FALSE(w.tags().contains(1));
   CHECK(w.corners().at(0).x() == 0);
   CHECK(w.corners().at(2).y() == 0);
   CHECK(w.names().at(1).view_().empty());
   CHECK(w.words().size() == 0);
   CHECK(w.color() == TvColor::red);

   CHECK(psio::pssz_trusted_view<std::uint32_t>{} == 0u);
   CHECK(psio::pssz_trusted_view<std::string>{}.view_().empty());
   CHECK(psio::pssz_trusted_view<std::vector<std::string>>{}.size() == 0);
   CHECK_FALSE(psio::pssz_trusted_view<std::optional<std::vector<std::uint8_t>>>{}.has_value());

   // The checked view that did not validate holds one.
   const std::vector<char> junk{1, 2, 3};
   const auto bad = psio::validated_pssz_view<TvOrder>(junk);
   CHECK_FALSE(bad.ok());
   check(*bad);
}

TEST_CASE("trusted view: at(i) is bounds-checked", "[view][trusted]")
{
   const auto o     = sample_order();
   const auto check = [&](const auto& v) {
      REQUIRE(v.items().size() == 3);
      CHECK(v.items().at(2).qty() == 3);
      CHECK(v.items().at(3).qty() == 0);
      CHECK(v.items().at(3).product().view_().empty());
      CHECK(v.items().at(1u << 30).price() == 0);
      CHECK(v.items().at(~std::size_t{0}).qty() == 0);
      CHECK(v.numbers().at(3) == 40u);
      CHECK(v.numbers().at(4) == 0u);
   };
   const auto p = psio::to_pssz(o);
   const auto s = psio::to_ssz(o);
   const auto w = psio::to_wit(o);
   check(psio::pssz_trusted_view<TvOrder>{psio::trusted{p}});
   check(psio::ssz_trusted_view<TvOrder>{psio::trusted{s}});
   check(psio::wit_trusted_view<TvOrder>{psio::trusted{w}});
}

TEST_CASE("trusted view: validate once, then the trusted view", "[view][trusted]")
{
   const auto o     = sample_order();
   const auto bytes = psio::to_pssz(o);

   auto row = psio::validated_pssz_view<TvOrder>(bytes);
   static_assert(std::is_same_v<decltype(row), psio::checked_view<TvOrder, psio::pssz>>);
   static_assert(std::is_same_v<decltype(*row), psio::pssz_trusted_view<TvOrder>>);
   static_assert(std::is_same_v<decltype(row.view()), psio::pssz_trusted_view<TvOrder>>);
   REQUIRE(row.ok());
   REQUIRE(static_cast<bool>(row));
   CHECK(row->customer().name() == "Alice");
   CHECK((*row).items()[1].qty() == 2);
   CHECK(row.value().note() == o.note);
   CHECK(row.view() == o);
   CHECK(psio::bytes(*row).data() == bytes.data());

   // Bytes that are not a TvOrder: a status, no view of them.
   std::vector<char> bad(bytes.begin(), bytes.end());
   bad.resize(bad.size() - 3);
   auto refused = psio::validated_pssz_view<TvOrder>(bad);
   CHECK_FALSE(refused.ok());
   CHECK_FALSE(static_cast<bool>(refused));
   CHECK_FALSE(refused.error().what.empty());
   CHECK(psio::bytes(*refused).empty());
   CHECK(refused->id() == 0);
#if PSIO_EXCEPTIONS_ENABLED
   CHECK_THROWS_AS(refused.value(), psio::codec_exception);
#endif
   psio::codec_status status = std::move(refused).status();
   CHECK_FALSE(status.ok());

   // The generic spelling, for any format with a trusted view.
   const auto sb = psio::to_ssz(o);
   const auto wb = psio::to_wit(o);
   auto sv = psio::validated_view<TvOrder>(psio::ssz{}, sb);
   static_assert(std::is_same_v<decltype(*sv), psio::ssz_trusted_view<TvOrder>>);
   REQUIRE(sv.ok());
   CHECK(sv->items()[2].product() == "ccc");
   auto wv = psio::validated_view<TvOrder>(psio::wit{}, wb);
   static_assert(std::is_same_v<decltype(*wv), psio::wit_trusted_view<TvOrder>>);
   REQUIRE(wv.ok());
   CHECK(wv->items()[2].product().view_() == "ccc");
   const auto pb = psio::encode(psio::pssz8{}, TvPoint{1, 2});
   auto       p8 = psio::validated_view<TvPoint>(psio::pssz8{}, pb);
   REQUIRE(p8.ok());
   CHECK(p8->y() == 2);

   // The out-parameter form takes either kind.
   bool unknown = true;
   std::optional<psio::pssz_trusted_view<TvOrder>> trusted_out;
   REQUIRE(psio::validated_pssz_view(psio::pssz{}, std::span<const char>{bytes}, trusted_out,
                                     unknown)
              .ok());
   REQUIRE(trusted_out.has_value());
   CHECK_FALSE(unknown);
   CHECK(trusted_out->customer().age() == 37);
   std::optional<psio::pssz_view<TvOrder>> safe_out;
   REQUIRE(
      psio::validated_pssz_view(psio::pssz{}, std::span<const char>{bytes}, safe_out, unknown).ok());
   REQUIRE(safe_out.has_value());
   CHECK(safe_out->customer().age() == 37);
   std::optional<psio::pssz_trusted_view<TvOrder>> none;
   CHECK_FALSE(
      psio::validated_pssz_view(psio::pssz{}, std::span<const char>{bad}, none, unknown).ok());
   CHECK_FALSE(none.has_value());
}

TEST_CASE("trusted view: a buffer hands out the trusted view", "[view][trusted]")
{
   const auto o     = sample_order();
   const auto bytes = psio::to_pssz(o);

   const psio::pssz_buffer<TvOrder> owned{std::vector<char>(bytes.begin(), bytes.end())};
   const psio::shared_pssz<TvOrder> shared{std::span<const char>{bytes}};
   static_assert(std::is_same_v<decltype(owned.view()), psio::pssz_trusted_view<TvOrder>>);
   static_assert(std::is_same_v<decltype(*owned), psio::pssz_trusted_view<TvOrder>>);
   static_assert(std::is_same_v<decltype(owned.get()), psio::pssz_trusted_view<TvOrder>>);
   static_assert(std::is_same_v<decltype(shared.view()), psio::pssz_trusted_view<TvOrder>>);
   static_assert(std::is_same_v<decltype(*shared), psio::pssz_trusted_view<TvOrder>>);
   // psio::as_view is the safe view.
   static_assert(std::is_same_v<decltype(psio::as_view(owned)), psio::pssz_view<TvOrder>>);
   static_assert(std::is_same_v<decltype(psio::as_view(shared)), psio::pssz_view<TvOrder>>);

   CHECK(owned->customer().name() == "Alice");
   CHECK((*owned).items()[1].qty() == 2);
   CHECK(owned.get().note() == o.note);
   CHECK(shared->items().size() == 3);
   CHECK(shared.view() == o);
   // Where a safe view is expected, the buffer's view converts.
   CHECK(name_of(owned->customer()) == "Alice");
   const psio::pssz_view<TvOrder> s = shared.view();
   CHECK(s.total() == o.total);

   // A default-constructed buffer holds the encoding of T{}.
   const psio::shared_pssz<TvOrder> empty;
   CHECK(empty->id() == 0);
   CHECK(empty->items().empty());
   CHECK(empty->note().view_().empty());

   // Every way of construction.
   const auto from_value   = psio::pssz_buffer<TvOrder>::from(o);
   const auto from_trusted = psio::shared_pssz<TvOrder>{psio::trusted{bytes}};
   const auto unchecked =
      psio::pssz_buffer<TvOrder>::unchecked(std::vector<char>(bytes.begin(), bytes.end()));
   psio::shared_pssz<TvOrder> checked;
   REQUIRE(psio::shared_pssz<TvOrder>::checked(std::span<const char>{bytes}, checked).ok());
   CHECK(from_value->items()[2].product() == "ccc");
   CHECK(from_trusted->items()[2].product() == "ccc");
   CHECK(unchecked->items()[2].product() == "ccc");
   CHECK(checked->items()[2].product() == "ccc");

   // The other formats with a trusted view.
   const psio::ssz_buffer<TvOrder> sbuf{o};
   static_assert(std::is_same_v<decltype(sbuf.view()), psio::ssz_trusted_view<TvOrder>>);
   CHECK(sbuf->items()[0].product() == "a");
}

namespace
{
   struct TvEnvelope
   {
      std::uint32_t                                             id = 0;
      psio::nested<TvItem>                                      checked;
      psio::nested<TvItem, psio::nested_validation::deferred>  opaque;
   };
   PSIO_REFLECT(TvEnvelope, id, checked, opaque)
}  // namespace

TEST_CASE("trusted view: the inner view of a deferred nested member is the safe view",
          "[view][trusted]")
{
   TvEnvelope e;
   e.id      = 1;
   e.checked = psio::nested<TvItem>{TvItem{"in", 2, 2.5}};
   e.opaque  = psio::nested<TvItem, psio::nested_validation::deferred>{TvItem{"out", 3, 3.5}};
   const auto bytes = psio::to_pssz(e);
   const psio::pssz_trusted_view<TvEnvelope> v{psio::trusted{bytes}};

   static_assert(std::is_same_v<decltype(v.checked().get()), psio::pssz_trusted_view<TvItem>>);
   static_assert(std::is_same_v<decltype(v.opaque().get()), psio::pssz_view<TvItem>>);
   static_assert(std::is_same_v<decltype(*v.opaque()), psio::pssz_view<TvItem>>);
   CHECK(v.checked()->product() == "in");
   CHECK(v.opaque()->product() == "out");
   CHECK(v.opaque()->qty() == 3);

   // Nothing validated the deferred member's bytes; the view of them does
   // not read outside them whatever they are.
   std::vector<char> junk(40, char(0xff));
   TvEnvelope        bad = e;
   bad.opaque = psio::nested<TvItem, psio::nested_validation::deferred>::unchecked(junk);
   const auto bad_bytes = psio::to_pssz(bad);
   REQUIRE(psio::validate_pssz<TvEnvelope>(bad_bytes).ok());
   const auto checked = psio::validated_pssz_view<TvEnvelope>(bad_bytes);
   REQUIRE(checked.ok());
   // ... and fails: they are not a frame of the member's type.
   CHECK_THROWS_AS(checked->opaque().get().qty(), psio::codec_exception);
}

namespace
{
   struct TvService
   {
      std::uint32_t seen = 0;
      std::string   text;
      // The packed arguments are validated before the call (or the caller
      // vouched for them): a parameter may be declared as the trusted view.
      void take(psio::pssz_trusted_view<TvItem> item, std::uint32_t n,
                psio::pssz_view<TvItem> other)
      {
         seen = item.qty() + other.qty() + n;
         text = std::string(item.product().view_()) + "/" + std::string(other.product().view_());
      }
      PSIO_REFLECT(TvService, method(take, item, n, other))
   };
}  // namespace

TEST_CASE("trusted view: a method parameter declared as the trusted view", "[view][trusted]")
{
   using Args = psio::reflect<TvService>::method_arg_tuple<0>;
   static_assert(std::is_same_v<std::tuple_element_t<0, Args::tuple_type>, TvItem>);
   const auto bytes =
      psio::to_pssz(Args{TvItem{"left", 2, 0.5}, std::uint32_t{10}, TvItem{"right", 3, 1.5}});
   TvService service;
   psio::invoke_method_pssz<TvService, 0>(service, std::span<const char>{bytes});  // validates
   CHECK(service.seen == 15);
   CHECK(service.text == "left/right");
   TvService again;
   psio::invoke_method_pssz<TvService, 0>(again, psio::trusted{bytes});
   CHECK(again.seen == 15);
}

// ── Bytes validation accepts are bytes the trusted view reads inside of ───
//
// Every truncation and every single-byte corruption of a valid encoding:
// whenever validation accepts the result, the trusted view must hand out
// only spans inside the bytes, the same ones the safe view hands out. (Under
// no compare a view makes may fail on them: that is a codec_exception.)
namespace
{
   struct agreement
   {
      std::span<const char> whole;
      void                  check(std::span<const char> safe, std::span<const char> trusted) const
      {
         CHECK(safe.size() == trusted.size());
         if (trusted.empty() || trusted.size() == 1)  // (1: the canonical None of a pruned optional)
            return;
         CHECK(safe.data() == trusted.data());
         const bool inside = trusted.data() >= whole.data() &&
                             trusted.data() + trusted.size() <= whole.data() + whole.size();
         CHECK(inside);
      }
   };

   template <typename S, typename T>
   void agree_order(const agreement& a, const S& s, const T& t)
   {
      a.check(psio::bytes(s), psio::bytes(t));
      CHECK(s.id() == t.id());
      a.check(psio::bytes(s.customer()), psio::bytes(t.customer()));
      CHECK(s.customer().id() == t.customer().id());
      a.check(psio::bytes(s.customer().name()), psio::bytes(t.customer().name()));
      a.check(psio::bytes(s.customer().email()), psio::bytes(t.customer().email()));
      CHECK(s.customer().age() == t.customer().age());
      CHECK(s.customer().verified() == t.customer().verified());
      a.check(psio::bytes(s.items()), psio::bytes(t.items()));
      REQUIRE(s.items().size() == t.items().size());
      const std::size_t n = t.items().size();
      for (std::size_t i = 0; i < n; ++i)
      {
         a.check(psio::bytes(s.items()[i]), psio::bytes(t.items()[i]));
         a.check(psio::bytes(s.items()[i].product()), psio::bytes(t.items()[i].product()));
         CHECK(s.items()[i].qty() == t.items()[i].qty());
      }
      CHECK(s.at().x() == t.at().x());
      CHECK(s.at().y() == t.at().y());
      a.check(psio::bytes(s.note()), psio::bytes(t.note()));
      a.check(psio::bytes(s.numbers()), psio::bytes(t.numbers()));
      REQUIRE(s.numbers().size() == t.numbers().size());
      for (std::size_t i = 0; i < t.numbers().size(); ++i)
         CHECK(s.numbers()[i] == t.numbers()[i]);
   }

   template <typename Fmt, typename Bytes>
   std::size_t validated_mutations_agree(Fmt fmt, const Bytes& good)
   {
      const std::vector<char> bytes(good.begin(), good.end());
      std::size_t             accepted = 0;
      const auto              probe    = [&](const std::vector<char>& candidate) {
         const std::span<const char> s{candidate.data(), candidate.size()};
         auto                        checked = psio::validated_view<TvOrder>(fmt, s);
         if (!checked.ok())
            return;
         ++accepted;
         agree_order(agreement{s}, psio::view<TvOrder, Fmt>{s}, *checked);
      };
      for (std::size_t n = 0; n <= bytes.size(); ++n)
         probe(std::vector<char>(bytes.begin(), bytes.begin() + n));
      for (std::size_t i = 0; i < bytes.size(); ++i)
         for (int mode = 0; mode < 4; ++mode)
         {
            std::vector<char> bad = bytes;
            bad[i] = mode == 0   ? char(0)
                     : mode == 1 ? char(0xff)
                     : mode == 2 ? char(bad[i] + 1)
                                 : char(bad[i] - 1);
            probe(bad);
         }
      return accepted;
   }
}  // namespace

TEST_CASE("trusted view: whatever PSSZ validation accepts, the trusted view reads inside of",
          "[view][trusted]")
{
   const auto accepted = validated_mutations_agree(psio::pssz{}, psio::to_pssz(sample_order()));
   CHECK(accepted > 100);  // the corruptions of scalar and string bytes validate
}

TEST_CASE("trusted view: whatever SSZ validation accepts, the trusted view reads inside of",
          "[view][trusted]")
{
   const auto accepted = validated_mutations_agree(psio::ssz{}, psio::to_ssz(sample_order()));
   CHECK(accepted > 100);
}

TEST_CASE("trusted view: whatever canonical-ABI validation accepts, the trusted view reads "
          "inside of",
          "[view][trusted]")
{
   const auto accepted = validated_mutations_agree(psio::wit{}, psio::to_wit(sample_order()));
   CHECK(accepted > 100);
}

namespace
{
   // Every span the PSSZ-only shapes hand out, safe against trusted.
   template <typename S, typename T>
   void agree_wide(const agreement& a, const S& s, const T& t, bool valid = true)
   {
      const auto both = [&](const auto& x, const auto& y) { a.check(psio::bytes(x), psio::bytes(y)); };
      both(s, t);
      CHECK(s.id() == t.id());
      both(s.maybe_number(), t.maybe_number());
      REQUIRE(s.maybe_number().has_value() == t.maybe_number().has_value());
      CHECK(s.maybe_number().value_or(0u) == t.maybe_number().value_or(0u));
      both(s.maybe_text(), t.maybe_text());
      REQUIRE(s.maybe_text().has_value() == t.maybe_text().has_value());
      if (t.maybe_text().has_value())
         both(*s.maybe_text(), *t.maybe_text());
      REQUIRE(s.maybe_point().has_value() == t.maybe_point().has_value());
      if (t.maybe_point().has_value())
      {
         both(*s.maybe_point(), *t.maybe_point());
         CHECK(s.maybe_point()->x() == t.maybe_point()->x());
         CHECK(s.maybe_point()->y() == t.maybe_point()->y());
      }
      both(s.maybe_list(), t.maybe_list());
      REQUIRE(s.maybe_list().has_value() == t.maybe_list().has_value());
      if (t.maybe_list().has_value())
      {
         both(*s.maybe_list(), *t.maybe_list());
         CHECK((*s.maybe_list()).size() == (*t.maybe_list()).size());
      }
      both(s.choice(), t.choice());
      REQUIRE(s.choice().index() == t.choice().index());
      s.choice().visit([&](auto sa) {
         t.choice().visit([&](auto ta) {
            if constexpr (std::is_same_v<typename decltype(sa)::element_type,
                                         typename decltype(ta)::element_type>)
               both(sa, ta);
         });
      });
      if (auto p = t.choice().template get_if<TvPoint>())
         CHECK(p->y() == s.choice().template get_if<TvPoint>()->y());
      both(s.triple(), t.triple());
      CHECK(s.triple().template get<0>() == t.triple().template get<0>());
      both(s.triple().template get<1>(), t.triple().template get<1>());
      both(s.triple().template get<2>(), t.triple().template get<2>());
      CHECK(s.triple().template get<2>().y() == t.triple().template get<2>().y());
      both(s.duo(), t.duo());
      both(s.duo().first(), t.duo().first());
      CHECK(s.duo().second() == t.duo().second());
      both(s.limits(), t.limits());
      REQUIRE(s.limits().size() == t.limits().size());
      for (std::size_t i = 0; i < t.limits().size(); ++i)
      {
         both(s.limits()[i], t.limits()[i]);
         both(s.limits()[i].template get<0>(), t.limits()[i].template get<0>());
         CHECK(s.limits()[i].template get<1>() == t.limits()[i].template get<1>());
      }
      CHECK(s.limits().value_or("beta", 0u) == t.limits().value_or("beta", 0u));
      CHECK(s.limits().contains("zeta") == t.limits().contains("zeta"));
      both(s.by_number(), t.by_number());
      REQUIRE(s.by_number().size() == t.by_number().size());
      for (std::size_t i = 0; i < t.by_number().size(); ++i)
      {
         both(s.by_number()[i], t.by_number()[i]);
         CHECK(s.by_number()[i].template get<0>() == t.by_number()[i].template get<0>());
         both(s.by_number()[i].template get<1>(), t.by_number()[i].template get<1>());
         both(s.by_number()[i].template get<1>().product(),
              t.by_number()[i].template get<1>().product());
         CHECK(s.by_number()[i].template get<1>().qty() == t.by_number()[i].template get<1>().qty());
      }
      CHECK(s.by_number().contains(5u) == t.by_number().contains(5u));
      both(s.tags(), t.tags());
      REQUIRE(s.tags().size() == t.tags().size());
      for (std::size_t i = 0; i < t.tags().size(); ++i)
         CHECK(s.tags()[i] == t.tags()[i]);
      CHECK(s.tags().contains(3) == t.tags().contains(3));
      both(s.corners(), t.corners());
      for (std::size_t i = 0; i < 3; ++i)
      {
         both(s.corners()[i], t.corners()[i]);
         CHECK(s.corners()[i].y() == t.corners()[i].y());
      }
      both(s.names(), t.names());
      both(s.names()[0], t.names()[0]);
      both(s.names()[1], t.names()[1]);
      both(s.words(), t.words());
      REQUIRE(s.words().size() == t.words().size());
      for (std::size_t i = 0; i < t.words().size(); ++i)
         both(s.words()[i], t.words()[i]);
      both(s.octets(), t.octets());
      both(s.quad(), t.quad());
      CHECK(s.color() == t.color());
      both(s.packed(), t.packed());
      both(s.packed().get(), t.packed().get());
      both(s.packed()->product(), t.packed()->product());
      CHECK(s.packed()->qty() == t.packed()->qty());
      both(s.trailing_a(), t.trailing_a());
      REQUIRE(s.trailing_a().has_value() == t.trailing_a().has_value());
      CHECK(s.trailing_a().value_or(0u) == t.trailing_a().value_or(0u));
      both(s.trailing_b(), t.trailing_b());
      REQUIRE(s.trailing_b().has_value() == t.trailing_b().has_value());
      if (t.trailing_b().has_value())
         both(*s.trailing_b(), *t.trailing_b());
      // The trusted decode of what validated.
      if (valid)
         CHECK(s.unpack() == t.unpack());
   }

   std::size_t validated_wide_mutations_agree(const TvWide& value)
   {
      const auto              good = psio::to_pssz(value);
      const std::vector<char> bytes(good.begin(), good.end());
      std::size_t             accepted = 0;
      const auto              probe    = [&](const std::vector<char>& candidate) {
         const std::span<const char> s{candidate.data(), candidate.size()};
         auto                        checked = psio::validated_pssz_view<TvWide>(s);
         if (!checked.ok())
            return;
         ++accepted;
         agree_wide(agreement{s}, psio::pssz_view<TvWide>{s}, *checked);
      };
      for (std::size_t n = 0; n <= bytes.size(); ++n)
         probe(std::vector<char>(bytes.begin(), bytes.begin() + n));
      for (std::size_t i = 0; i < bytes.size(); ++i)
         for (int mode = 0; mode < 4; ++mode)
         {
            std::vector<char> bad = bytes;
            bad[i] = mode == 0   ? char(0)
                     : mode == 1 ? char(0xff)
                     : mode == 2 ? char(bad[i] + 1)
                                 : char(bad[i] - 1);
            probe(bad);
         }
      return accepted;
   }
}  // namespace

TEST_CASE("trusted view: whatever PSSZ validation accepts, the trusted view reads inside of "
          "(optionals, variants, tuples, maps, sets, arrays, nested)",
          "[view][trusted]")
{
   auto w = sample_wide();
   CHECK(validated_wide_mutations_agree(w) > 100);
   w.trailing_a = 5;
   w.trailing_b = "tail";
   w.choice     = TvPoint{3, 4};
   w.maybe_list = std::vector<std::uint8_t>{1, 2, 3};
   CHECK(validated_wide_mutations_agree(w) > 100);
   w.maybe_number.reset();
   w.maybe_text.reset();
   w.maybe_point.reset();
   w.maybe_list.reset();
   w.choice = std::uint32_t{6};
   w.limits.clear();
   w.words.clear();
   CHECK(validated_wide_mutations_agree(w) > 100);
}

// (Every truncation and corruption, validated or not, of the PSSZ-only
// shapes: trusted_structure_tests.cpp — a view of either kind hands out only
// spans inside the bytes, and fails where a member cannot be placed.)

TEST_CASE("trusted view: PSSZ evolution — what the writer did not write reads as absent",
          "[view][trusted]")
{
   // An older writer: the trailing optionals are not in its header at all.
   auto       w     = sample_wide();
   const auto bytes = psio::to_pssz(w);
   const psio::pssz_trusted_view<TvWide> v{psio::trusted{bytes}};
   CHECK_FALSE(v.trailing_a().has_value());
   CHECK_FALSE(v.trailing_b().has_value());
   CHECK(v.trailing_b().value_or("gone") == "gone");
   // The last variable member before them ends where the record ends.
   CHECK(v.packed()->product() == "inner");
}

namespace
{
   struct TvOld
   {
      std::uint32_t id = 0;
      std::string   text;
   };
   PSIO_REFLECT(TvOld, id, text)
   struct TvNew
   {
      std::uint32_t              id = 0;
      std::string                text;
      std::optional<std::string> more;
   };
   PSIO_REFLECT(TvNew, id, text, more)
}  // namespace

// A newer writer: this reader's type lacks the appended optional, whose
// offset slot bounds the last variable member this reader knows.
TEST_CASE("trusted view: PSSZ evolution — unknown appended fields bound the known ones",
          "[view][trusted]")
{
   const auto newer = psio::to_pssz(TvNew{4, "known", std::string("unknown to the old reader")});
   bool       unknown = false;
   std::optional<psio::pssz_trusted_view<TvOld>> old_view;
   REQUIRE(
      psio::validated_pssz_view(psio::pssz{}, std::span<const char>{newer}, old_view, unknown).ok());
   CHECK(unknown);
   REQUIRE(old_view.has_value());
   CHECK(old_view->id() == 4);
   CHECK(old_view->text() == "known");
   const psio::pssz_view<TvOld> safe{std::span<const char>{newer}};
   CHECK(safe.text() == "known");

   const auto older = psio::to_pssz(TvOld{5, "old"});
   const auto new_view = psio::validated_pssz_view<TvNew>(older);
   REQUIRE(new_view.ok());
   CHECK(new_view->id() == 5);
   CHECK(new_view->text() == "old");
   CHECK_FALSE(new_view->more().has_value());
}
