// The read view's surface beyond member access (probes g5, g_misc_view, g2b):
// list views (front / back / reverse / random access), optional views
// (-> / value / value_or), variant views (get_if / holds_alternative),
// comparison of a view with a value and with another view, structured
// bindings on tuple / pair views, and the conversions that borrow.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <catch.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace surf
{
   struct Act
   {
      std::uint64_t sender = 0;
      std::string   name;
      friend auto   operator<=>(const Act&, const Act&) = default;
   };
   PSIO_REFLECT(Act, sender, name)

   // No comparison operators: its views do not compare either.
   struct Opaque
   {
      std::uint32_t x = 0;
   };
   PSIO_REFLECT(Opaque, x)

   enum class Kind : std::uint8_t
   {
      a,
      b
   };

   struct R
   {
      std::vector<std::string>     v;
      std::optional<std::string>   o;
      std::optional<std::uint32_t> n;
      std::optional<Act>           act;
      std::array<std::uint16_t, 3> arr;
      Kind                         kind = Kind::a;
   };
   PSIO_REFLECT(R, v, o, n, act, arr, kind)

   template <typename A, typename B>
   concept compares_with = requires(const A& a, const B& b) {
      { a == b } -> std::same_as<bool>;
   };

   using Args = std::tuple<std::int32_t, std::vector<char>, std::uint32_t>;
   using Var  = std::variant<std::uint32_t, std::string, Act>;
   struct Row
   {
      std::vector<char>                 data;
      Var                               info;
      std::vector<std::uint32_t>        xs;
      std::pair<std::uint16_t, std::string> pr;
      std::vector<Act>                  acts;
   };
   PSIO_REFLECT(Row, data, info, xs, pr, acts)
}  // namespace surf

using namespace surf;

TEST_CASE("list view: front, back, random access and reverse iteration (probe g5)",
          "[view][surface]")
{
   auto               b = psio::to_pssz(R{{"a", "b", "c"}, "x", 3, Act{1, "z"}, {4, 5, 6}, Kind::b});
   psio::pssz_view<R> r{b};
   auto               list = r.v();
   REQUIRE(list.front().view_() == "a");
   REQUIRE(list.back().view_() == "c");
   REQUIRE_FALSE(list.empty());
   REQUIRE(list.end() - list.begin() == 3);
   REQUIRE(list.begin()[1].view_() == "b");
   REQUIRE((list.begin() + 2)->view_() == "c");
   REQUIRE(*(list.end() - 1) == "c");
   REQUIRE(list.begin() < list.end());
   auto it = list.begin();
   it += 2;
   REQUIRE(*it == "c");
   --it;
   REQUIRE(*it == "b");

   std::string reversed;
   for (auto i = list.rbegin(); i != list.rend(); ++i)
      reversed += (*i).view_();
   REQUIRE(reversed == "cba");
   std::string joined;
   for (auto s : list | std::views::reverse)
      joined += s.view_();
   REQUIRE(joined == "cba");

   using list_view = decltype(list);
   STATIC_REQUIRE(std::random_access_iterator<list_view::iterator>);
   STATIC_REQUIRE(std::ranges::random_access_range<list_view>);
   STATIC_REQUIRE(std::ranges::sized_range<list_view>);
   STATIC_REQUIRE(std::ranges::forward_range<list_view>);

   // An iterator keeps the list it walks: no dangling on a temporary.
   auto first = r.v().begin();
   auto last  = r.v().end();
   REQUIRE(std::distance(first, last) == 3);
   REQUIRE(*first == "a");

   // std algorithms.
   REQUIRE(std::ranges::find(list, std::string_view{"b"}) == list.begin() + 1);
   REQUIRE(std::ranges::count_if(list, [](auto s) { return s.size() == 1; }) == 3);

   // An array view.
   auto arr = r.arr();
   REQUIRE(arr.front() == 4);
   REQUIRE(arr.back() == 6);
   REQUIRE(arr.end() - arr.begin() == 3);
   std::uint32_t sum = 0;
   for (auto x : arr)
      sum += x;
   REQUIRE(sum == 15);
   REQUIRE(*arr.rbegin() == 6);

   // An empty list.
   auto               eb = psio::to_pssz(R{});
   psio::pssz_view<R> e{eb};
   REQUIRE(e.v().empty());
   REQUIRE(e.v().begin() == e.v().end());
   REQUIRE(e.v().rbegin() == e.v().rend());
}

TEST_CASE("list view: zip with another range", "[view][surface]")
{
   auto                 b = psio::to_pssz(Row{{}, std::uint32_t{0}, {4, 5, 6}, {}, {}});
   psio::pssz_view<Row> r{b};
   std::vector<int>     other{1, 2, 3};
   std::uint32_t        dot = 0;
   for (auto [x, y] : std::views::zip(r.xs(), other))
      dot += x * static_cast<std::uint32_t>(y);
   REQUIRE(dot == 4 + 10 + 18);
}

TEST_CASE("optional view: ->, value, value_or, bool (probe g5)", "[view][surface]")
{
   auto               b = psio::to_pssz(R{{"a", "b"}, "x", 3, Act{1, "z"}, {}, Kind::a});
   psio::pssz_view<R> r{b};
   REQUIRE(r.o().has_value());
   REQUIRE(bool(r.o()));
   REQUIRE(r.o()->view_() == "x");
   REQUIRE(r.o().value().view_() == "x");
   REQUIRE(r.o().value_or("none") == "x");
   REQUIRE(r.n().value_or(7u) == 3u);
   REQUIRE(r.n().value() == 3u);
   REQUIRE(r.act()->sender() == 1);
   REQUIRE(r.act()->name() == "z");
   REQUIRE(r.act().value().name() == "z");
   REQUIRE(r.act().value_or(Act{9, "d"}) == Act{1, "z"});
   STATIC_REQUIRE(std::is_same_v<decltype(r.n().value_or(7u)), std::uint32_t>);
   STATIC_REQUIRE(std::is_same_v<decltype(r.o().value_or("none")), std::string>);

   auto               eb = psio::to_pssz(R{});
   psio::pssz_view<R> e{eb};
   REQUIRE_FALSE(e.o().has_value());
   REQUIRE_FALSE(bool(e.n()));
   REQUIRE(e.o().value_or("none") == "none");
   REQUIRE(e.n().value_or(7u) == 7u);
   REQUIRE(e.act().value_or(Act{9, "d"}) == Act{9, "d"});
#if defined(__cpp_exceptions)
   // value() of an absent optional fails as std::optional::value() does.
   REQUIRE_THROWS_AS(e.o().value(), psio::codec_exception);
   REQUIRE_THROWS_AS(e.act().value(), psio::codec_exception);
#endif
}

TEST_CASE("variant view: index, get_if, holds_alternative, visit (probe g_misc_view)",
          "[view][surface]")
{
   auto                 b = psio::to_pssz(Row{{'x', 'y'}, std::string("s"), {4, 5}, {7, "p"}, {}});
   psio::pssz_view<Row> rv{b};
   auto                 info = rv.info();
   REQUIRE(info.index() == 1);
   REQUIRE(info.holds_alternative<std::string>());
   REQUIRE_FALSE(info.holds_alternative<std::uint32_t>());
   REQUIRE(holds_alternative<std::string>(info));
   auto alt = get_if<std::string>(info);   // found by ADL, as std::get_if is not
   REQUIRE(alt.has_value());
   REQUIRE(alt->view_() == "s");
   REQUIRE(*alt == "s");
   REQUIRE_FALSE(info.get_if<std::uint32_t>().has_value());
   REQUIRE_FALSE(info.get_if<Act>().has_value());
   REQUIRE(info.get_if<1>().has_value());
   REQUIRE_FALSE(get_if<0>(info).has_value());
   std::string seen;
   info.visit([&](auto a) {
      if constexpr (requires { a.view_(); })
         seen = a.view_();
   });
   REQUIRE(seen == "s");

   auto b2 = psio::to_pssz(Row{{}, Act{3, "act"}, {}, {}, {}});
   psio::pssz_view<Row> rv2{b2};
   REQUIRE(rv2.info().index() == 2);
   REQUIRE(rv2.info().get_if<Act>()->sender() == 3);
   REQUIRE(rv2.info().get_if<Act>()->name() == "act");

   // Comparison of variant views, and with a value.
   REQUIRE(rv.info() == rv.info());
   REQUIRE(rv.info() != rv2.info());
   REQUIRE(rv.info() == Var{std::string("s")});
   REQUIRE(rv2.info() == Var{Act{3, "act"}});
}

TEST_CASE("comparison: a view with a value and with another view (probe g2b)",
          "[view][surface]")
{
   auto                 b = psio::to_pssz(Act{7, "x"});
   auto                 c = psio::to_pssz(Act{7, "y"});
   psio::pssz_view<Act> v{b};
   psio::pssz_view<Act> w{c};

   // Strings: with a literal, a std::string, a std::string_view, a view.
   REQUIRE(v.name() == "x");
   REQUIRE("x" == v.name());
   REQUIRE(v.name() != "y");
   REQUIRE(v.name() == std::string("x"));
   REQUIRE(v.name() == std::string_view{"x"});
   REQUIRE(v.name() != w.name());
   REQUIRE(v.name() == v.name());
   REQUIRE(v.name() < w.name());
   REQUIRE(v.name() < "y");
   REQUIRE((v.name() <=> w.name()) == std::strong_ordering::less);

   // Records: needs the value type's operators.
   REQUIRE(v == Act{7, "x"});
   REQUIRE(Act{7, "x"} == v);
   REQUIRE(v != w);
   REQUIRE(v == v);
   REQUIRE(v < w);
   REQUIRE(v < Act{8, ""});
   // A record without comparison has views without comparison.
   STATIC_REQUIRE(std::equality_comparable<psio::pssz_view<Act>>);
   STATIC_REQUIRE(compares_with<psio::pssz_view<Act>, Act>);
   STATIC_REQUIRE_FALSE(std::equality_comparable<psio::pssz_view<Opaque>>);
   STATIC_REQUIRE_FALSE(compares_with<psio::pssz_view<Opaque>, Opaque>);

   // The const spelling compares too, and with the plain one.
   psio::view<const Act, psio::pssz> cv{b};
   REQUIRE(cv == v);
   REQUIRE(cv == Act{7, "x"});

   // Scalars, enums, lists, optionals, tuples.
   auto               rb = psio::to_pssz(R{{"a", "b"}, "x", 3, Act{1, "z"}, {1, 2, 3}, Kind::b});
   psio::pssz_view<R> r{rb};
   REQUIRE(r.kind() == Kind::b);
   REQUIRE(r.kind() != Kind::a);
   REQUIRE(r.v() == std::vector<std::string>{"a", "b"});
   REQUIRE(r.v() == r.v());
   REQUIRE(r.v()[0] == "a");
   REQUIRE(r.o() == "x");
   REQUIRE(r.o() == std::optional<std::string>{"x"});
   REQUIRE(r.o() != std::nullopt);
   REQUIRE(r.n() == 3u);
   REQUIRE(r.act() == Act{1, "z"});
   REQUIRE(r.arr() == std::array<std::uint16_t, 3>{1, 2, 3});
   REQUIRE(r.arr()[1] == 2);
   REQUIRE(r.arr()[1] < 3);

   auto                  ab = psio::to_pssz(Args{1, {'a'}, 2});
   psio::pssz_view<Args> av{ab};
   REQUIRE(av == Args{1, {'a'}, 2});
   REQUIRE(av.get<1>() == std::vector<char>{'a'});
}

TEST_CASE("structured bindings on tuple and pair views (probe g_misc_view)", "[view][surface]")
{
   auto                  ab = psio::to_pssz(Args{1, {'a', 'b'}, 2});
   psio::pssz_view<Args> av{ab};
   auto [socket, data, flags] = av;
   STATIC_REQUIRE(std::is_same_v<decltype(socket), std::int32_t>);
   REQUIRE(socket == 1);
   REQUIRE(data.size() == 2);
   REQUIRE(flags == 2);
   STATIC_REQUIRE(std::tuple_size_v<psio::pssz_view<Args>> == 3);

   auto                 b = psio::to_pssz(Row{{}, std::uint32_t{0}, {}, {7, "p"}, {}});
   psio::pssz_view<Row> rv{b};
   auto [first, second] = rv.pr();
   REQUIRE(first == 7);
   REQUIRE(second == "p");
   REQUIRE(rv.pr().first() == 7);
   REQUIRE(rv.pr().second() == "p");
}

TEST_CASE("conversions that borrow: string_view, span of bytes, scalars (probes g2b, "
          "g_misc_view)",
          "[view][surface]")
{
   auto                 b = psio::to_pssz(Row{{'x', 'y'}, std::uint32_t{5}, {4, 5}, {7, "p"}, {}});
   psio::pssz_view<Row> rv{b};
   std::span<const char> s = rv.data();          // a byte list is its bytes
   REQUIRE(s.size() == 2);
   REQUIRE(s[1] == 'y');
   REQUIRE(rv.data().data() == s.data());
   std::string_view sv = rv.pr().second();       // a string is its characters
   REQUIRE(sv == "p");
   REQUIRE(sv.data() >= b.data());
   REQUIRE(sv.data() < b.data() + b.size());
   std::uint32_t x = rv.xs()[1];                 // a scalar view is its value
   REQUIRE(x == 5);
   std::uint32_t sum = 0;
   for (std::uint32_t e : rv.xs())
      sum += e;
   REQUIRE(sum == 9);
   REQUIRE(std::accumulate(rv.xs().begin(), rv.xs().end(), std::uint32_t{0}) == 9);

   auto               rb = psio::to_pssz(R{{}, {}, {}, {}, {}, Kind::b});
   psio::pssz_view<R> r{rb};
   Kind               k = r.kind();
   REQUIRE(k == Kind::b);

   // Owned values are always spelled: no implicit allocation.
   STATIC_REQUIRE_FALSE(std::is_convertible_v<psio::pssz_view<Act>, Act>);
   STATIC_REQUIRE_FALSE(std::is_convertible_v<psio::pssz_view<std::string>, std::string>);
   STATIC_REQUIRE(std::is_convertible_v<psio::pssz_view<std::string>, std::string_view>);
   STATIC_REQUIRE(
      std::is_convertible_v<psio::pssz_view<std::vector<char>>, std::span<const char>>);
   STATIC_REQUIRE_FALSE(
      std::is_convertible_v<psio::pssz_view<std::vector<std::uint32_t>>, std::span<const char>>);
   std::string owned{rv.pr().second()};          // explicit construction from the characters
   REQUIRE(owned == "p");
}
