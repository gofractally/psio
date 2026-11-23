// Keys from values that do not own their data, and psio::raw_key (probe
// g10_keyview): std::string_view / C strings as a string, views and
// reference wrappers as the value they refer to, and bytes that already
// are a key spliced into a tuple verbatim.

#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <catch.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ksrc
{
   struct Row
   {
      std::uint32_t id = 0;
      std::string   name;
   };
   PSIO_REFLECT(Row, id, name)

   struct RowKey
   {
      psio::raw_key    table;   // a stored prefix
      std::uint32_t    id = 0;
      std::string_view name;
   };
   PSIO_REFLECT(RowKey, table, id, name)
}  // namespace ksrc

using namespace ksrc;

TEST_CASE("to_key of string_view and C strings is the string's key", "[key][source]")
{
   REQUIRE(psio::to_key(std::string_view{"abc"}) == psio::to_key(std::string("abc")));
   REQUIRE(psio::to_key("abc") == psio::to_key(std::string("abc")));
   const char* cs = "abc";
   REQUIRE(psio::to_key(cs) == psio::to_key(std::string("abc")));
   // Embedded NULs are escaped, as in a std::string.
   REQUIRE(psio::to_key(std::string_view{"a\0b", 3}) == psio::to_key(std::string("a\0b", 3)));
   REQUIRE(psio::to_key(std::string_view{}) == psio::to_key(std::string()));
   REQUIRE(psio::key_size(std::string_view{"abc"}) == 5);
   // As tuple elements, with appending and the checked encoder.
   REQUIRE(psio::to_key(std::tuple{std::uint16_t{1}, std::string_view{"k"}}) ==
           psio::to_key(std::tuple{std::uint16_t{1}, std::string("k")}));
   std::vector<char> out{char(7)};
   psio::to_key(std::string_view{"k"}, out);
   REQUIRE(out.size() == 1 + psio::key_size(std::string("k")));
   std::vector<char> checked;
   REQUIRE(psio::encode_key_checked(std::string_view{"k"}, checked).ok());
   REQUIRE(checked == psio::encode(psio::key{}, std::string("k")));
   // Order is the string's.
   REQUIRE(psio::to_key(std::string_view{"a"}) < psio::to_key(std::string_view{"b"}));
}

TEST_CASE("to_key of references and views is the key of the value", "[key][source]")
{
   Row                  r{5, "n"};
   auto                 rb = psio::to_pssz(r);
   psio::pssz_view<Row> rv{rb};
   REQUIRE(psio::to_key(rv) == psio::to_key(r));
   REQUIRE(psio::to_key(rv.name()) == psio::to_key(std::string("n")));
   REQUIRE(psio::to_key(std::tuple{std::cref(r), rv.name()}) ==
           psio::to_key(std::tuple{r, std::string("n")}));
   std::uint32_t a = 1;
   std::string   s = "x";
   REQUIRE(psio::to_key(std::tie(a, s)) == psio::to_key(std::tuple{a, s}));
   // A reference to a fixed-size key is still a fixed-size key.
   STATIC_REQUIRE(std::is_same_v<decltype(psio::to_key(std::cref(a))), std::array<char, 4>>);
   REQUIRE(psio::to_key(std::cref(a)) == psio::to_key(a));
}

TEST_CASE("raw_key splices bytes that already are a key (probe g10_keyview)", "[key][raw_key]")
{
   const std::vector<char> prefix =
      psio::encode(psio::key{}, std::tuple{std::uint64_t{7}, std::uint16_t{1}});
   const auto want = psio::to_key(
      std::tuple{std::uint64_t{7}, std::uint16_t{1}, std::uint32_t{5}, std::string("n")});

   auto k = psio::to_key(std::tuple{psio::raw_key{prefix}, std::uint32_t{5}, std::string_view{"n"}});
   REQUIRE(k == want);
   // The raw bytes are not escaped or framed: 00 and FF pass through.
   const std::vector<char> odd{0, char(0xff), 0, 1};
   auto                    ko = psio::to_key(std::tuple{psio::raw_key{odd}, std::uint8_t{9}});
   REQUIRE(ko == std::vector<char>{0, char(0xff), 0, 1, 9});
   REQUIRE(psio::to_key(psio::raw_key{odd}) == odd);
   REQUIRE(psio::to_key(psio::raw_key{}).empty());
   REQUIRE(psio::key_size(std::tuple{psio::raw_key{prefix}, std::uint32_t{5}}) ==
           prefix.size() + 4);
   // A key holding a raw key is never of fixed size.
   STATIC_REQUIRE(std::is_same_v<decltype(psio::to_key(std::tuple{psio::raw_key{}, 5u})),
                                 std::vector<char>>);

   // The same by appending to a copy of the prefix.
   std::vector<char> appended = prefix;
   psio::to_key(std::tuple{std::uint32_t{5}, std::string_view{"n"}}, appended);
   REQUIRE(appended == want);

   // As a record member.
   REQUIRE(psio::to_key(RowKey{psio::raw_key{prefix}, 5, "n"}) == want);

   // The prefix helpers honour it.
   using Key = std::tuple<psio::raw_key, std::uint32_t, std::string>;
   auto p    = psio::key_prefix<Key>(psio::raw_key{prefix}, 5);
   REQUIRE(p.size() == prefix.size() + 4);
   REQUIRE(std::equal(p.begin(), p.end(), want.begin()));
   REQUIRE(psio::key_prefix<Key>(psio::raw_key{prefix}) == prefix);
   REQUIRE(psio::key_prefix<Key>(psio::raw_key{prefix}, 5, "n") == want);
   auto bound = psio::key_prefix_upper_bound(p);
   REQUIRE(bound.has_value());
   REQUIRE(p < *bound);
   REQUIRE(want < *bound);

   // The checked encoder and a sizing stream.
   std::vector<char> checked;
   REQUIRE(psio::encode_key_checked(std::tuple{psio::raw_key{prefix}, std::string_view{"z"}},
                                    checked)
              .ok());
   REQUIRE(checked.size() == prefix.size() + 3);
   psio::size_stream ss;
   psio::to_key(std::tuple{psio::raw_key{prefix}, std::uint32_t{5}}, ss);
   REQUIRE(ss.size == prefix.size() + 4);
}
