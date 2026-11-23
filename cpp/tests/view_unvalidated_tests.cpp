// Views over bytes nobody validated, and views of nothing.
//
// A read view is specified for validated bytes, but what the PSSZ / SSZ /
// canonical-ABI views do elsewhere is relied on and is pinned here:
//   * a default-constructed view, and `at(i)` past the end, read as zero /
//     empty;
//   * on truncated or corrupted bytes, navigation never hands out a span
//     outside the buffer (so nothing reads out of bounds).
// The PSSZ record view works its header out once per view (view.hpp
// "Per-record state"); these cases hold with and without that
// (PSIO_VIEW_NO_RECORD_STATE).

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{
   struct UvProfile
   {
      std::uint64_t id = 0;
      std::string   name;
      std::string   email;
      std::uint32_t age      = 0;
      bool          verified = false;
   };
   PSIO_REFLECT(UvProfile, id, name, email, age, verified)

   struct UvItem
   {
      std::string   product;
      std::uint32_t qty   = 0;
      double        price = 0;
   };
   PSIO_REFLECT(UvItem, product, qty, price)

   struct UvPoint
   {
      std::int32_t x = 0, y = 0;
   };
   PSIO_REFLECT(UvPoint, x, y, definitionWillNotChange())

   struct UvOrder
   {
      std::uint64_t        id = 0;
      UvProfile            customer;
      std::vector<UvItem>  items;
      UvPoint              at;
      double               total = 0;
      std::string          note;
   };
   PSIO_REFLECT(UvOrder, id, customer, items, at, total, note)

   // The same with trailing optionals (PSSZ prunes absent ones).
   struct UvOrderOpt
   {
      std::uint64_t                id = 0;
      UvProfile                    customer;
      std::vector<UvItem>          items;
      std::optional<std::uint32_t> score;
      std::optional<std::string>   memo;
   };
   PSIO_REFLECT(UvOrderOpt, id, customer, items, score, memo)

   template <typename T>
   T sample()
   {
      T o;
      o.id       = 7;
      o.customer = {9, "Alice", "alice@example.org", 37, true};
      o.items    = {{"a", 1, 1.5}, {"bb", 2, 2.5}, {"ccc", 3, 3.5}};
      return o;
   }

   // Every span a view hands out lies in `whole`, or is empty, or is the
   // one-byte canonical None the PSSZ view returns for a pruned optional.
   struct bounds
   {
      std::span<const char> whole;
      void                  check(std::span<const char> s) const
      {
         if (s.empty() || s.size() == 1)
            return;
         const bool inside = s.data() >= whole.data() &&
                             s.data() + s.size() <= whole.data() + whole.size();
         CHECK(inside);
      }
   };

   template <typename V>
   void walk_profile(const bounds& b, const V& v)
   {
      b.check(v._psio_data());
      (void)v.id();
      b.check(v.name()._psio_data());
      b.check(v.email()._psio_data());
      (void)v.age();
      (void)v.verified();
   }
   template <typename V>
   void walk_order(const bounds& b, const V& v)
   {
      b.check(v._psio_data());
      (void)v.id();
      walk_profile(b, v.customer());
      auto items = v.items();
      b.check(items._psio_data());
      const std::size_t n = items.size();
      // A corrupted count must not turn into unbounded work or reads: a
      // list never reports more elements than its bytes can hold, and
      // every index below size() is read inside them.
      CHECK(n <= b.whole.size());
      for (std::size_t i = 0; i < n; ++i)
      {
         auto it = items[i];
         b.check(it._psio_data());
         b.check(it.product()._psio_data());
         (void)it.qty();
         (void)it.price();
      }
      (void)items.at(n).qty();
      (void)items.at(n + 1000).product().view_();
   }

   template <typename View, typename Bytes>
   void truncations_and_corruptions(const Bytes& good)
   {
      const std::vector<char> bytes(good.begin(), good.end());
      // Every prefix.
      for (std::size_t n = 0; n <= bytes.size(); ++n)
      {
         const std::vector<char>     cut(bytes.begin(), bytes.begin() + n);
         const std::span<const char> s{cut.data(), cut.size()};
         walk_order(bounds{s}, View{s});
      }
      // Every byte set to 0x00, 0xff, and incremented.
      for (std::size_t i = 0; i < bytes.size(); ++i)
         for (int mode = 0; mode < 3; ++mode)
         {
            std::vector<char> bad = bytes;
            bad[i] = mode == 0 ? char(0) : mode == 1 ? char(0xff) : char(bad[i] + 1);
            const std::span<const char> s{bad.data(), bad.size()};
            walk_order(bounds{s}, View{s});
         }
   }
}  // namespace

TEST_CASE("view: a default-constructed record view reads as zero / empty", "[view][unvalidated]")
{
   const auto check = [](const auto& v) {
      CHECK(v.id() == 0);
      CHECK(v.customer().id() == 0);
      CHECK(v.customer().name().view_().empty());
      CHECK(v.customer().age() == 0);
      CHECK_FALSE(v.customer().verified());
      CHECK(v.items().size() == 0);
      CHECK(v.items().empty());
      CHECK(v.items().at(0).qty() == 0);
      CHECK(v.items().at(5).product().view_().empty());
   };
   check(psio::pssz_view<UvOrder>{});
   check(psio::ssz_view<UvOrder>{});
   check(psio::wit_view<UvOrder>{});
   check(psio::pssz_view<UvOrder>{std::span<const char>{}});

   psio::pssz_view<UvOrderOpt> o{};
   CHECK_FALSE(o.score().has_value());
   CHECK_FALSE(o.memo().has_value());
   CHECK(o.score().value_or(4u) == 4u);
}

TEST_CASE("view: at(i) past the end reads as zero / empty", "[view][unvalidated]")
{
   const auto order = sample<UvOrder>();
   const auto check = [&](const auto& v) {
      REQUIRE(v.items().size() == 3);
      CHECK(v.items().at(2).qty() == 3);
      CHECK(v.items().at(3).qty() == 0);
      CHECK(v.items().at(3).product().view_().empty());
      CHECK(v.items().at(1u << 30).price() == 0);
   };
   const auto p = psio::to_pssz(order);
   const auto s = psio::to_ssz(order);
   const auto w = psio::to_wit(order);
   check(psio::pssz_view<UvOrder>{std::span<const char>{p}});
   check(psio::ssz_view<UvOrder>{std::span<const char>{s}});
   check(psio::wit_view<UvOrder>{std::span<const char>{w}});
}

namespace
{
   struct UvShapes
   {
      std::uint32_t                           id = 0;
      std::array<UvPoint, 3>                  corners{};
      std::vector<std::uint32_t>              numbers;
      std::optional<std::array<UvPoint, 3>>   maybe_corners;
      std::vector<std::string>                words;
   };
   PSIO_REFLECT(UvShapes, id, corners, numbers, maybe_corners, words)
}  // namespace

TEST_CASE("view: fixed-size elements the bytes do not hold read as zero / empty",
          "[view][unvalidated]")
{
   // An array always has N elements; a view of nothing has N empty ones.
   psio::pssz_view<UvShapes> none{};
   CHECK(none.corners().at(0).x() == 0);
   CHECK(none.corners().at(2).y() == 0);
   CHECK(none.corners()[1].x() == 0);
   CHECK(psio::bytes(none.corners()[2]).empty());
   CHECK(psio::pssz_view<std::array<UvPoint, 3>>{std::span<const char>{}}[2].y() == 0);
   // An array whose span is short (here: 12 bytes for three 8-byte points).
   const std::vector<char>                    twelve(12, char(1));
   const psio::pssz_view<std::array<UvPoint, 3>> cut{std::span<const char>{twelve}};
   CHECK(cut[0].x() == 0x01010101);
   CHECK(psio::bytes(cut[1]).empty());
   CHECK(cut[1].x() == 0);
   CHECK(cut.at(2).y() == 0);

   // operator[] past the end of a list of fixed-size elements.
   UvShapes value;
   value.numbers = {1, 2, 3};
   value.words   = {"a", "b"};
   const auto                 p = psio::to_pssz(value);
   const auto                 s = psio::to_ssz(value.numbers);
   psio::pssz_view<UvShapes>  v{std::span<const char>{p}};
   CHECK(v.numbers()[2] == 3u);
   CHECK(v.numbers()[3] == 0u);
   CHECK(v.numbers()[1000] == 0u);
   CHECK(psio::bytes(v.numbers()[1000]).empty());
   CHECK(v.numbers()[~std::size_t{0} / 4] == 0u);
   psio::ssz_view<std::vector<std::uint32_t>> sv{std::span<const char>{s}};
   CHECK(sv[2] == 3u);
   CHECK(sv[3] == 0u);
   CHECK(psio::bytes(sv[1u << 20]).empty());
}

TEST_CASE("view: a list never reports more elements than its bytes hold", "[view][unvalidated]")
{
   // The first offset of a list of variable-size elements is its count
   // times the offset width. One nobody validated, pointing past the list,
   // must not become a count whose offsets lie outside the bytes.
   const std::vector<std::string> words{"alpha", "beta", "gamma"};
   {
      const auto        good = psio::to_pssz(words);
      std::vector<char> bad(good.begin(), good.end());
      const std::size_t width = psio::auto_pssz_width_v<std::vector<std::string>>;
      for (std::size_t i = 0; i < width; ++i)
         bad[i] = char(0xff);
      psio::pssz_view<std::vector<std::string>> v{std::span<const char>{bad}};
      CHECK(v.size() == 0);
      CHECK(v.empty());
      CHECK(v.begin() == v.end());
      CHECK(v.at(0).view_().empty());
   }
   {
      const auto        good = psio::to_ssz(words);
      std::vector<char> bad(good.begin(), good.end());
      bad[0] = bad[1] = bad[2] = char(0xff);
      psio::ssz_view<std::vector<std::string>> v{std::span<const char>{bad}};
      CHECK(v.size() == 0);
      CHECK(v.at(0).view_().empty());
   }
}

TEST_CASE("view: a valid record reads the same through every member", "[view][unvalidated]")
{
   auto order  = sample<UvOrderOpt>();
   order.score = 11;
   for (int with_memo = 0; with_memo < 2; ++with_memo)
   {
      if (with_memo)
         order.memo = "m";
      const auto                  p = psio::to_pssz(order);
      psio::pssz_view<UvOrderOpt> v{std::span<const char>{p}};
      CHECK(v.id() == 7);
      CHECK(v.customer().name() == "Alice");
      CHECK(v.customer().email() == "alice@example.org");
      CHECK(v.customer().age() == 37);
      CHECK(v.customer().verified());
      CHECK(v.items()[2].product() == "ccc");
      CHECK(v.score().value_or(0u) == 11u);
      CHECK(v.memo().has_value() == (with_memo == 1));
   }
   // Every trailing optional pruned: the header announces a shorter fixed
   // region than this type knows.
   const auto                  pruned = psio::to_pssz(sample<UvOrderOpt>());
   psio::pssz_view<UvOrderOpt> v{std::span<const char>{pruned}};
   CHECK(v.items().size() == 3);
   CHECK_FALSE(v.score().has_value());
   CHECK_FALSE(v.memo().has_value());
}

TEST_CASE("view: truncated or corrupted PSSZ never yields a span outside the bytes",
          "[view][unvalidated]")
{
   truncations_and_corruptions<psio::pssz_view<UvOrder>>(psio::to_pssz(sample<UvOrder>()));
}

TEST_CASE("view: truncated or corrupted SSZ never yields a span outside the bytes",
          "[view][unvalidated]")
{
   truncations_and_corruptions<psio::ssz_view<UvOrder>>(psio::to_ssz(sample<UvOrder>()));
}

TEST_CASE("view: truncated or corrupted canonical ABI never yields a span outside the bytes",
          "[view][unvalidated]")
{
   truncations_and_corruptions<psio::wit_view<UvOrder>>(psio::to_wit(sample<UvOrder>()));
}
