// std::pair, maps and sets as PSIO values (docs/pssz-spec.md "Pairs, maps
// and sets"): a pair is the 2-tuple; a map / set is the list of its entries
// / elements in strictly ascending key order — in pssz, key, json, ijson,
// the Schema IR and the schema-driven codecs.
#include "assoc_cases.hpp"

#include <psio/graphql.hpp>
#include <psio/max_size.hpp>

#include <catch.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <variant>

using namespace assoc_cases;
namespace ir = psio::pssz_ir;
namespace st = psio::schema_types;

namespace
{
   std::string hex(const Bytes& bytes)
   {
      static constexpr char digits[] = "0123456789abcdef";
      std::string           out;
      for (unsigned char c : bytes)
      {
         out.push_back(digits[c >> 4]);
         out.push_back(digits[c & 15]);
      }
      return out;
   }
   template <typename T>
   Bytes pssz(const T& v) { return psio::encode(psio::pssz{}, v); }
   template <typename T>
   Bytes key(const T& v) { return psio::encode(psio::key{}, v); }
   template <typename T>
   std::string json(const T& v) { return psio::encode(psio::json{}, v); }
   template <typename T>
   std::string ijson(const T& v) { return psio::encode(psio::ijson{}, v); }
   template <typename T, typename Fmt = psio::json>
   T from_text(std::string_view text)
   {
      T out{};
      REQUIRE(psio::decode_json_checked(Fmt{}, std::span<const char>{text}, out).ok());
      return out;
   }
   template <typename T>
   psio::Schema schema_of(std::string name)
   {
      psio::SchemaBuilder builder;
      builder.insert<T>(std::move(name));
      return std::move(builder).build();
   }
   const st::AnyType& named(const psio::Schema& schema, std::string_view name)
   {
      for (const auto& type : schema.types)
         if (type.name == name)
            return *type.type;
      FAIL("no type " << name);
      static const st::AnyType none;
      return none;
   }

   enum class Shade : std::uint8_t { dark, light };
   PSIO_REFLECT_ENUM(Shade, dark, light)

   // utf8_string has no operator<: any comparator will do, the codec orders
   // by key bytes.
   struct Utf8Less
   {
      template <typename S>
      bool operator()(const S& a, const S& b) const { return a.storage() < b.storage(); }
   };

   struct Sized
   {
      std::set<std::uint16_t>              small;
      std::map<std::uint8_t, std::uint8_t> pairs;
      PSIO_REFLECT(Sized, attr(small, max<4>), attr(pairs, max<4>))
   };
   struct SizedLists
   {
      std::vector<std::uint16_t>                          small;
      std::vector<std::tuple<std::uint8_t, std::uint8_t>> pairs;
      PSIO_REFLECT(SizedLists, attr(small, max<4>), attr(pairs, max<4>))
   };
   struct OnlySet
   {
      std::set<std::uint16_t> small;
      PSIO_REFLECT(OnlySet, attr(small, max<4>))
   };
   struct OnlyList
   {
      std::vector<std::uint16_t> small;
      PSIO_REFLECT(OnlyList, attr(small, max<4>))
   };
}  // namespace

TEST_CASE("assoc: the checked entry points report through status", "[assoc]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = run_assoc_cases());
   CHECK(failed == 0);
}

TEST_CASE("assoc: a std::pair is the 2-tuple in every format", "[assoc][pair]")
{
   const std::pair<std::uint16_t, std::string>  pair{7, "seven"};
   const std::tuple<std::uint16_t, std::string> tuple{7, "seven"};
   CHECK(pssz(pair) == pssz(tuple));
   CHECK(psio::encode(psio::pssz_<2>{}, pair) == psio::encode(psio::pssz_<2>{}, tuple));
   CHECK(key(pair) == key(tuple));
   CHECK(json(pair) == R"([7,"seven"])");
   CHECK(ijson(pair) == ijson(tuple));
   CHECK(psio::decode<std::remove_cvref_t<decltype(pair)>>(psio::pssz{}, std::span<const char>{pssz(tuple)}) == pair);
   CHECK(psio::decode<std::remove_cvref_t<decltype(pair)>>(psio::key{}, std::span<const char>{key(tuple)}) == pair);
   CHECK(from_text<std::remove_cvref_t<decltype(pair)>>(R"([7,"seven"])") == pair);
   CHECK(psio::validate_strict<std::remove_cvref_t<decltype(pair)>>(psio::pssz{}, std::span<const char>{pssz(tuple)})
            .ok());
   CHECK(!psio::validate_json<std::remove_cvref_t<decltype(pair)>>(psio::json{}, std::span<const char>{"[7]", 3}).ok());

   // Trailing optionals are pruned exactly as a tuple's are.
   const std::pair<std::uint8_t, std::optional<std::string>> pruned{1, std::nullopt};
   CHECK(pssz(pruned) == pssz(std::tuple<std::uint8_t, std::optional<std::string>>{1, std::nullopt}));
   CHECK(psio::decode<std::remove_cvref_t<decltype(pruned)>>(psio::pssz{}, std::span<const char>{pssz(pruned)}) == pruned);

   using P = std::pair<std::uint16_t, std::string>;
   using T = std::tuple<std::uint16_t, std::string>;
   CHECK(std::string_view{psio::get_type_name(static_cast<const P*>(nullptr))} ==
         std::string_view{psio::get_type_name(static_cast<const T*>(nullptr))});
   CHECK(std::string_view{psio::get_type_name(static_cast<const P*>(nullptr))} ==
         "tuple_uint16_string");
   static_assert(psio::auto_pssz_width_v<P> == psio::auto_pssz_width_v<T>);
   static_assert(psio::inferred_bound<P>() == psio::inferred_bound<T>());
   static_assert(psio::pssz_evolves_from<P, T>);
   static_assert(psio::pssz_evolves_from<T, P>);
   static_assert(!psio::pssz_evolves_from<P, std::pair<std::uint32_t, std::string>>);
   static_assert(psio::pssz_evolves_from<
      std::pair<int, int>, std::tuple<int, int, std::optional<int>>>);

   // Schema: Tuple{[A, B]} either way.
   const auto  schema = schema_of<P>("P");
   const auto  twin   = schema_of<T>("T");
   const auto* a      = named(schema, "P").resolve(schema);
   const auto* b      = named(twin, "T").resolve(twin);
   REQUIRE(a);
   REQUIRE(b);
   REQUIRE(std::holds_alternative<st::Tuple>(a->value));
   CHECK(std::get<st::Tuple>(a->value).members.size() == 2);
   CHECK(psio::encode(psio::pssz{}, schema).size() == psio::encode(psio::pssz{}, twin).size());
   std::string text;
   REQUIRE(ir::pssz_to_json(schema, st::Type{"P"}, psio::json{}, std::span<const char>{pssz(pair)},
                            text)
              .ok());
   CHECK(text == R"([7,"seven"])");

   // View: the tuple's, plus first() / second().
   const Bytes                                           bytes = pssz(pair);
   psio::view<P, psio::pssz, psio::storage::const_borrow> view{std::span<const char>{bytes}};
   CHECK(view.first() == 7);
   CHECK(view.second().view_() == "seven");
   CHECK(view.get<0>() == 7);

   // A composite key type.
   CHECK(psio::key_prefix<P>(std::uint16_t{7}) == key(std::uint16_t{7}));
   CHECK(psio::key_prefix<P>(7, "seven") == key(pair));
}

TEST_CASE("assoc: worked byte example", "[assoc][pssz]")
{
   // docs/pssz-spec.md: map<string, u32>{"b": 2, "a": 1}.
   const std::map<std::string, std::uint32_t> m{{"b", 2}, {"a", 1}};
   CHECK(hex(pssz(m)) == "08000000" "15000000"
                         "08000000" "08000000" "01000000" "61"
                         "08000000" "08000000" "02000000" "62");
   CHECK(pssz(m) == pssz(std::vector<std::tuple<std::string, std::uint32_t>>{{"a", 1}, {"b", 2}}));
   CHECK(hex(key(m)) == "01" "610000" "00000001" "01" "620000" "00000002" "00");
   CHECK(json(m) == R"({"a":1,"b":2})");
   // set<u16>{513, 2}: key order is numeric order, not the order of the
   // little-endian wire bytes.
   const std::set<std::uint16_t> s{513, 2};
   CHECK(hex(pssz(s)) == "0200" "0102");
   CHECK(hex(key(s)) == "01" "0002" "01" "0201" "00");
   CHECK(json(s) == "[2,513]");
   // A set of octets takes the octet-list framing in key.
   CHECK(hex(key(std::set<std::uint8_t>{3, 0, 1})) == "0001" "01" "03" "0000");
   CHECK(key(std::set<std::int8_t>{3, 0, -1}) == key(std::vector<std::int8_t>{-1, 0, 3}));
   CHECK(pssz(std::map<int, int>{}).empty());
   CHECK(hex(key(std::map<int, int>{})) == "00");
   CHECK(json(std::map<int, int>{}) == "[]");
   CHECK(json(std::map<std::string, int>{}) == "{}");
   CHECK(json(std::set<int>{}) == "[]");
}

TEST_CASE("assoc: any container, one canonical order", "[assoc][pssz]")
{
   const std::vector<std::tuple<std::string, std::uint32_t>> list{{"a", 1}, {"b", 2}, {"c", 3}};
   const std::map<std::string, std::uint32_t>                ordered{{"c", 3}, {"a", 1}, {"b", 2}};
   const std::map<std::string, std::uint32_t, Descending>    reversed{{"c", 3}, {"a", 1}, {"b", 2}};
   const std::unordered_map<std::string, std::uint32_t>      hashed{{"c", 3}, {"a", 1}, {"b", 2}};
   for (const auto& bytes : {pssz(ordered), pssz(reversed), pssz(hashed)})
      CHECK(bytes == pssz(list));
   for (const auto& bytes : {key(ordered), key(reversed), key(hashed)})
      CHECK(bytes == key(list));
   for (const auto& text : {json(ordered), json(reversed), json(hashed)})
      CHECK(text == R"({"a":1,"b":2,"c":3})");
   CHECK(psio::encode(psio::pssz_<1>{}, hashed) == psio::encode(psio::pssz_<1>{}, list));
   CHECK(psio::size_of(psio::pssz{}, hashed) == pssz(list).size());
   CHECK(psio::decode<std::remove_cvref_t<decltype(reversed)>>(psio::pssz{}, std::span<const char>{pssz(list)}) ==
         reversed);
   CHECK(psio::decode<std::remove_cvref_t<decltype(hashed)>>(psio::key{}, std::span<const char>{key(list)}) == hashed);
   CHECK(from_text<std::remove_cvref_t<decltype(reversed)>>(R"({"b":2,"c":3,"a":1})") == reversed);

   // Signed keys: -1 sorts before 1 although its wire bytes are larger.
   const std::unordered_set<std::int32_t> signed_set{1, -1, 0, std::numeric_limits<int>::min()};
   CHECK(pssz(signed_set) ==
         pssz(std::vector<std::int32_t>{std::numeric_limits<int>::min(), -1, 0, 1}));

   // A record key is ordered by its key bytes, whatever its operator<.
   const std::map<Point, int> points{{Point{2, 0}, 1}, {Point{1, 5}, 2}, {Point{-3, 9}, 3}};
   CHECK(pssz(points) == pssz(std::vector<std::tuple<Point, int>>{
                            {Point{-3, 9}, 3}, {Point{1, 5}, 2}, {Point{2, 0}, 1}}));
   CHECK(psio::decode<std::remove_cvref_t<decltype(points)>>(psio::pssz{}, std::span<const char>{pssz(points)}) ==
         points);

   // Floats: key-spec order. Negative NaN first, -0 and +0 one key, positive
   // NaN last.
   const double nan  = std::numeric_limits<double>::quiet_NaN();
   const double inf  = std::numeric_limits<double>::infinity();
   const std::set<double, BitLess> floats{nan, 1.0, -inf, -nan, inf, 0.0, -1.5};
   const Bytes                     float_bytes = pssz(floats);
   REQUIRE(float_bytes.size() == 7 * 8);
   const auto decoded =
      psio::decode<std::vector<double>>(psio::pssz{}, std::span<const char>{float_bytes});
   CHECK(std::isnan(decoded[0]));
   CHECK(std::signbit(decoded[0]));
   CHECK(std::vector<double>(decoded.begin() + 1, decoded.begin() + 6) ==
         std::vector<double>{-inf, -1.5, 0.0, 1.0, inf});
   CHECK(std::isnan(decoded[6]));
   CHECK(!std::signbit(decoded[6]));
   CHECK(psio::validate<std::set<double, BitLess>>(psio::pssz{}, std::span<const char>{float_bytes})
            .ok());
   CHECK(psio::validate<std::unordered_set<double>>(psio::pssz{},
                                                    std::span<const char>{float_bytes})
            .ok());
}

TEST_CASE("assoc: values with no encoding fail fast in the plain encoders", "[assoc]")
{
   const std::set<double, BitLess> zeros{0.0, -0.0};
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, zeros), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::key{}, zeros), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::json{}, zeros), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, zeros), psio::codec_exception);
   const std::map<double, int, BitLess> entries{{0.0, 1}, {-0.0, 2}};
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, entries), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::key{}, entries), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::json{}, entries), psio::codec_exception);

   // The trusted decode inserts; a container that merges keys the encoding
   // distinguishes would lose entries, which is a failure.
   const Bytes two = pssz(std::vector<std::tuple<std::uint32_t, bool>>{{1, true}, {3, false}});
   using Merging   = std::map<std::uint32_t, bool, ParityLess>;
   CHECK(psio::validate<Merging>(psio::pssz{}, std::span<const char>{two}).ok());
   CHECK_THROWS_AS(psio::decode<Merging>(psio::pssz{}, std::span<const char>{two}),
                   psio::codec_exception);
   const Bytes two_keys = key(std::vector<std::tuple<std::uint32_t, bool>>{{1, true}, {3, false}});
   CHECK(psio::validate<Merging>(psio::key{}, std::span<const char>{two_keys}).ok());
   CHECK_THROWS_AS(psio::decode<Merging>(psio::key{}, std::span<const char>{two_keys}),
                   psio::codec_exception);
   CHECK_THROWS_AS(psio::decode<Merging>(psio::json{}, std::span<const char>{"[[1,true],[3,false]]", 20}),
                   psio::codec_exception);

   // validate_or_throw is the status validator, raised.
   const Bytes unsorted = pssz(std::vector<std::uint32_t>{2, 1});
   CHECK_THROWS_AS(psio::validate_or_throw<std::set<std::uint32_t>>(
                      psio::pssz{}, std::span<const char>{unsorted}),
                   psio::codec_exception);
   CHECK_NOTHROW(psio::validate_or_throw<std::vector<std::uint32_t>>(
      psio::pssz{}, std::span<const char>{unsorted}));
}

TEST_CASE("assoc: which key types iterate in key order", "[assoc]")
{
   using psio::detail::key_order_is_less;
   static_assert(key_order_is_less<std::uint32_t>());
   static_assert(key_order_is_less<std::int64_t>());
   static_assert(key_order_is_less<bool>());
   static_assert(key_order_is_less<Shade>());
   static_assert(key_order_is_less<std::string>());
   static_assert(key_order_is_less<std::chrono::seconds>());
   static_assert(key_order_is_less<std::chrono::sys_time<std::chrono::microseconds>>());
   static_assert(key_order_is_less<std::tuple<std::uint8_t, std::string>>());
   static_assert(key_order_is_less<std::pair<std::int16_t, std::optional<std::string>>>());
   static_assert(key_order_is_less<std::vector<std::array<std::uint16_t, 2>>>());
   static_assert(key_order_is_less<std::variant<std::uint8_t, std::string>>());
   static_assert(!key_order_is_less<double>());
   static_assert(!key_order_is_less<char>());
   static_assert(!key_order_is_less<std::vector<char>>());
   static_assert(!key_order_is_less<psio::uint128>());
   static_assert(!key_order_is_less<Point>());
   static_assert(!key_order_is_less<std::tuple<std::uint8_t, float>>());
   using psio::detail::iterates_in_key_order_v;
   static_assert(iterates_in_key_order_v<std::map<std::string, int>>);
   static_assert(iterates_in_key_order_v<std::set<std::uint8_t, std::less<>>>);
   static_assert(!iterates_in_key_order_v<std::map<std::string, int, Descending>>);
   static_assert(!iterates_in_key_order_v<std::unordered_set<int>>);
   static_assert(!iterates_in_key_order_v<std::set<Point>>);

   // Both paths give one answer: the order of the key bytes.
   std::mt19937 rng(8);
   for (int round = 0; round < 50; ++round)
   {
      std::set<std::tuple<std::int16_t, std::string>>           fast;
      std::unordered_map<std::int16_t, std::vector<std::string>> slow_source;
      for (int i = 0; i < 20; ++i)
         fast.emplace(static_cast<std::int16_t>(rng()),
                      std::string(rng() % 3, static_cast<char>('a' + rng() % 3)));
      std::vector<std::tuple<std::int16_t, std::string>> sorted(fast.begin(), fast.end());
      std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return key(a) < key(b) ||
                   (key(a) != key(b) && std::lexicographical_compare(
                       key(a).begin(), key(a).end(), key(b).begin(), key(b).end(),
                       [](char x, char y) { return static_cast<unsigned char>(x) <
                                                   static_cast<unsigned char>(y); })); });
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
         const Bytes ka = key(a), kb = key(b);
         return std::lexicographical_compare(
            ka.begin(), ka.end(), kb.begin(), kb.end(), [](char x, char y) {
               return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
            });
      });
      CHECK(pssz(fast) == pssz(sorted));
      CHECK(psio::validate<std::remove_cvref_t<decltype(fast)>>(psio::pssz{}, std::span<const char>{pssz(sorted)}).ok());
      std::unordered_set<std::tuple<std::int16_t, std::string>, decltype([](const auto& t) {
                            return std::hash<std::string>{}(std::get<1>(t)) ^
                                   static_cast<std::size_t>(std::get<0>(t));
                         })>
         hashed(fast.begin(), fast.end());
      CHECK(pssz(hashed) == pssz(sorted));
      std::reverse(sorted.begin(), sorted.end());
      if (sorted.size() > 1)
         CHECK(!psio::validate<std::remove_cvref_t<decltype(fast)>>(psio::pssz{}, std::span<const char>{pssz(sorted)})
                   .ok());
   }
}

TEST_CASE("assoc: json object for text keys, pairs otherwise", "[assoc][json]")
{
   // String kinds and text-adapter types are object keys.
   CHECK(json(std::map<psio::utf8_string<4>, bool, Utf8Less>{{psio::utf8_string<4>{"k"}, true}}) ==
         R"({"k":true})");
   CHECK(json(std::map<Label, std::string>{{Label{12}, "x"}, {Label{3}, "y"}}) ==
         R"({"L3":"y","L12":"x"})");
   CHECK(from_text<std::map<Label, std::string>>(R"({"L12":"x","L3":"y"})") ==
         std::map<Label, std::string>{{Label{12}, "x"}, {Label{3}, "y"}});
   // Everything else is an array of [k, v] pairs: integers, enums, time
   // points, bytes, tuples.
   CHECK(json(std::map<std::uint64_t, bool>{{1, true}}) == "[[1,true]]");
   CHECK(ijson(std::map<std::uint64_t, bool>{{9007199254740993ull, true}}) ==
         R"([["9007199254740993",true]])");
   CHECK(json(std::map<Shade, int>{{Shade::light, 1}, {Shade::dark, 0}}) ==
         R"([["dark",0],["light",1]])");
   CHECK(json(std::map<std::chrono::sys_seconds, int>{{std::chrono::sys_seconds{}, 1}}) ==
         R"([["1970-01-01T00:00:00Z",1]])");
   CHECK(ijson(std::map<std::vector<std::uint8_t>, int>{{{1, 255}, 1}}) == R"([["01FF",1]])");
   CHECK(json(std::map<std::pair<int, std::string>, int>{{{1, "a"}, 2}}) == R"([[[1,"a"],2]])");
   // A set of bytes stays an array of numbers under ijson: it is not a
   // byte string.
   CHECK(ijson(std::set<std::uint8_t>{2, 1}) == "[1,2]");
   // Escapes in keys; I-JSON refuses a noncharacter in a key as in any string.
   const std::map<std::string, int> odd{{"a\"b\n", 1}, {"\xEF\xBF\xBE", 2}};
   CHECK(json(odd) == "{\"a\\\"b\\n\":1,\"\xEF\xBF\xBE\":2}");
   std::string out;
   CHECK(!psio::encode_json_checked(psio::ijson{}, odd, out).ok());
   const std::string escaped = "{\"a\\u0022b\\n\":1}";
   CHECK(from_text<std::map<std::string, int>>(escaped) ==
         std::map<std::string, int>{{"a\"b\n", 1}});
   CHECK(!psio::validate_json<std::map<std::string, int>>(
             psio::ijson{}, std::span<const char>{"{\"\xEF\xBF\xBE\":2}", 9})
             .ok());
   // The key's own constraints apply to member names.
   CHECK(text_rejects<std::map<psio::utf8_string<2>, int, Utf8Less>>(R"({"abc":1})", "exceeds its bound"));
   // Readers take any order; writers emit the canonical one.
   using Nested = std::map<std::string, std::map<std::int32_t, std::set<std::string>>>;
   const Nested nested = from_text<Nested>(R"({"z":[[5,["b","a"]],[-5,[]]],"a":[]})");
   CHECK(json(nested) == R"({"a":[],"z":[[-5,[]],[5,["a","b"]]]})");
   // A map is one nesting level, like any container.
   CHECK(text_rejects<std::map<std::string, int>>(R"({"a":1,"a":1})", "duplicate map key"));
   CHECK(text_rejects<std::map<Label, int>>(R"({"L1":1,"L1":2})", "duplicate map key"));
   CHECK(text_rejects<std::unordered_set<std::string>>(R"(["a","a"])", "duplicate set element"));
   // Variant alternatives are named map_K_V / set_K.
   using V = std::variant<std::map<std::string, std::uint32_t>, std::set<std::int32_t>,
                          std::pair<std::uint8_t, bool>>;
   CHECK(ijson(V{std::set<std::int32_t>{3, 1}}) == R"({"set_int32":[1,3]})");
   CHECK(ijson(V{std::map<std::string, std::uint32_t>{{"k", 1}}}) ==
         R"({"map_string_uint32":{"k":1}})");
   CHECK(ijson(V{std::pair<std::uint8_t, bool>{1, true}}) == R"({"tuple_uint8_bool":[1,true]})");
   CHECK(from_text<V, psio::ijson>(R"({"set_int32":[3,1]})") == V{std::set<std::int32_t>{1, 3}});
}

TEST_CASE("assoc: views, widths, bounds, evolution", "[assoc][pssz]")
{
   const Book  book  = sample();
   const Bytes frame = pssz(book);
   psio::view<Book, psio::pssz, psio::storage::const_borrow> view{std::span<const char>{frame}};
   // A map's view is the view of the list of its entries, in key order.
   const auto counts = view.get<0>();
   REQUIRE(counts.size() == 4);
   std::vector<std::pair<std::string, std::uint32_t>> seen;
   for (auto entry : counts)
      seen.emplace_back(std::string(entry.get<0>().view_()), entry.get<1>());
   CHECK(seen == std::vector<std::pair<std::string, std::uint32_t>>(book.counts.begin(),
                                                                   book.counts.end()));
   const auto ids = view.get<2>();
   REQUIRE(ids.size() == 5);
   CHECK(ids[0].get() == -300);
   CHECK(ids[4].get() == 300);
   CHECK(view.get<6>().first() == 7);
   CHECK(view.get<5>()[1].get<1>().size() == 3);  // labelled: L10 -> {0, 3, 255}

   // Widths and bounds follow the list.
   static_assert(psio::auto_pssz_width_v<Sized> == psio::auto_pssz_width_v<SizedLists>);
   static_assert(psio::inferred_bound<Sized>() == psio::inferred_bound<SizedLists>());
   static_assert(psio::inferred_bound<OnlySet>() == psio::inferred_bound<OnlyList>());
   static_assert(psio::inferred_bound<OnlySet>().has_value());
   static_assert(psio::auto_pssz_width_v<OnlySet> == 1);
   static_assert(psio::auto_pssz_width_v<std::map<int, int>> ==
                 psio::auto_pssz_width_v<std::vector<std::tuple<int, int>>>);
   const Sized sized{{4, 3, 2, 1}, {{2, 2}, {1, 1}}};
   CHECK(pssz(sized) == pssz(SizedLists{{1, 2, 3, 4}, {{1, 1}, {2, 2}}}));
   CHECK(pssz(OnlySet{{9, 8}}) == pssz(OnlyList{{8, 9}}));
   Bytes out;
   CHECK(says(psio::encode_pssz_checked(psio::pssz{}, OnlySet{{1, 2, 3, 4, 5}}, out),
              "length_bound"));
   CHECK(says(psio::validate<OnlySet>(
                 psio::pssz{}, std::span<const char>{psio::detail::pssz_impl::
                                                        encode_ignoring_length_bounds(
                                                           psio::pssz{}, OnlyList{{1, 2, 3, 4, 5}})}),
              "length_bound"));
   CHECK(says(psio::validate<OnlySet>(psio::pssz{}, std::span<const char>{pssz(OnlyList{{2, 1}})}),
              "set elements"));

   // One value kind per (K, V), whatever the container; a different key
   // type is a different order; a map is not the plain list of entries.
   static_assert(psio::pssz_evolves_from<std::map<int, int>, std::unordered_map<int, int>>);
   static_assert(psio::pssz_evolves_from<std::set<std::string>, std::unordered_set<std::string>>);
   static_assert(psio::pssz_evolves_from<std::map<int, std::tuple<int>>,
                                         std::map<int, std::tuple<int, std::optional<int>>>>);
   static_assert(!psio::pssz_evolves_from<std::map<int, int>, std::map<unsigned, int>>);
   static_assert(!psio::pssz_evolves_from<std::map<int, int>, std::vector<std::tuple<int, int>>>);
   static_assert(!psio::pssz_evolves_from<std::vector<int>, std::set<int>>);
   static_assert(!psio::pssz_evolves_from<std::set<int>, std::map<int, int>>);

   // An optional map needs a selector (its payload can be empty), as an
   // optional list does.
   CHECK(pssz(std::optional<std::set<int>>{std::set<int>{}}) == Bytes{1});
   CHECK(pssz(std::optional<std::set<int>>{}) == Bytes{0});

   // The key of a map orders maps lexicographically by sorted entries.
   CHECK(key(std::map<int, int>{{0, 0}, {1, 1}}) < key(std::map<int, int>{{1, 1}}));
   CHECK(key(std::set<int>{1}) < key(std::set<int>{1, 2}));
   CHECK(key(std::set<int>{1, 2}) < key(std::set<int>{2}));
}

TEST_CASE("assoc: schema nodes and malformed schemas", "[assoc][schema]")
{
   const auto  schema = schema_of<Book>("Book");
   const auto& book   = std::get<st::Object>(named(schema, "Book").value);
   auto member        = [&](std::string_view name) -> const st::AnyType& {
      for (const auto& m : book.members)
         if (m.name == name)
            return *m.type;
      FAIL("no member " << name);
      return *book.members[0].type;
   };
   // map: Custom{ List{ Tuple{[K, V]} }, "map" }
   {
      const auto* node = member("counts").resolve(schema);
      REQUIRE(node);
      const auto& custom = std::get<st::Custom>(node->value);
      CHECK(custom.id == "map");
      const auto& list  = std::get<st::List>(custom.type->value);
      const auto* entry = list.type->resolve(schema);
      REQUIRE(entry);
      CHECK(std::get<st::Tuple>(entry->value).members.size() == 2);
   }
   // set: Custom{ List{ K }, "set" }
   {
      const auto* node = member("tags").resolve(schema);
      REQUIRE(node);
      const auto& custom = std::get<st::Custom>(node->value);
      CHECK(custom.id == "set");
      CHECK(std::holds_alternative<st::List>(custom.type->value));
   }
   // A member max count: BoundedList in place of List.
   {
      const auto& custom = std::get<st::Custom>(member("limited").value);
      CHECK(custom.id == "map");
      CHECK(std::get<st::BoundedList>(custom.type->value).maxCount == 2);
   }

   auto refused = [](st::AnyType type, std::string_view what) {
      psio::Schema schema;
      schema.ir_version = st::schema_ir_version;
      schema.insert("T", std::move(type));
      const st::AnyType root{st::Type{"T"}};
      ir::Value         value;
      bool              unknown = false;
      std::string       text;
      Bytes             out;
      return says(ir::validate_pssz(schema, root, std::span<const char>{}), what) &&
             says(ir::decode_auto(schema, root, std::span<const char>{}, value, unknown), what) &&
             says(ir::encode_auto(schema, root, ir::Value{ir::Sequence{}}, out), what) &&
             says(ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{"[]", 2}, out),
                  what) &&
             says(ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{}, text),
                  what) &&
             says(ir::to_key(schema, root, ir::Value{ir::Sequence{}}, out), what);
   };
   auto box   = [](st::AnyType t) { return st::Box<st::AnyType>{std::move(t)}; };
   auto u8    = [] { return st::AnyType{st::Int{8, false}}; };
   auto tuple = [&](std::size_t n) {
      st::Tuple t;
      for (std::size_t i = 0; i < n; ++i)
         t.members.push_back(box(u8()));
      return st::AnyType{std::move(t)};
   };
   auto bits = [&] {
      return st::AnyType{st::Custom{box(st::AnyType{st::BoundedList{box(st::Int{1, false}), 8}}),
                                    "bitlist"}};
   };
   // Well-formed neighbours.
   CHECK(!refused(st::Custom{box(st::List{box(tuple(2))}), "map"}, ""));
   CHECK(!refused(st::Custom{box(st::BoundedList{box(u8()), 3}), "set"}, ""));
   // "map" directly on a list of two-member tuples.
   CHECK(refused(st::Custom{box(st::List{box(tuple(3))}), "map"}, "two-member tuples"));
   CHECK(refused(st::Custom{box(st::List{box(tuple(1))}), "map"}, "two-member tuples"));
   CHECK(refused(st::Custom{box(st::List{box(u8())}), "map"}, "two-member tuples"));
   CHECK(refused(st::Custom{box(st::Array{box(tuple(2)), 2}), "map"}, "two-member tuples"));
   CHECK(refused(st::Custom{box(tuple(2)), "map"}, "two-member tuples"));
   CHECK(refused(st::Custom{box(st::Custom{box(st::List{box(tuple(2))}), "box"}), "map"},
                 "directly on its wire type"));
   CHECK(refused(st::Custom{box(st::Option{box(st::List{box(tuple(2))})}), "map"},
                 "two-member tuples"));
   // "set" directly on a list.
   CHECK(refused(st::Custom{box(st::Array{box(u8()), 2}), "set"}, "a set must sit directly"));
   CHECK(refused(st::Custom{box(u8()), "set"}, "a set must sit directly"));
   CHECK(refused(st::Custom{box(st::Custom{box(st::List{box(u8())}), "hex"}), "set"},
                 "a set must sit directly"));
   // The key type must have a key encoding.
   CHECK(refused(st::Custom{box(st::List{box(bits())}), "set"}, "no key encoding"));
   {
      st::Tuple entry;
      entry.members.push_back(box(st::Option{box(bits())}));
      entry.members.push_back(box(u8()));
      CHECK(refused(st::Custom{box(st::List{box(st::AnyType{std::move(entry)})}), "map"},
                    "no key encoding"));
   }
   CHECK(refused(st::Custom{box(st::List{box(st::Custom{box(u8()), "box"})}), "set"},
                 "no key encoding"));
   // (The value type need not.)
   {
      st::Tuple entry;
      entry.members.push_back(box(u8()));
      entry.members.push_back(box(bits()));
      CHECK(!refused(st::Custom{box(st::List{box(st::AnyType{std::move(entry)})}), "map"}, ""));
   }
}

TEST_CASE("assoc: schema-driven codecs agree with the typed ones", "[assoc][schema]")
{
   const Book        book   = sample();
   const Bytes       frame  = pssz(book);
   const auto        schema = schema_of<Book>("Book");
   const st::AnyType root{st::Type{"Book"}};
   ir::CustomHandlers handlers;
   handlers.add<Label>();
   const ir::JsonNames names(schema, handlers);

   ir::Value value;
   bool      unknown = true;
   REQUIRE(ir::decode_auto(schema, root, std::span<const char>{frame}, value, unknown).ok());
   CHECK(!unknown);
   Bytes again;
   REQUIRE(ir::encode_auto(schema, root, value, again).ok());
   CHECK(again == frame);
   for (std::size_t w : {std::size_t{2}, std::size_t{4}})
   {
      const Bytes explicit_frame = w == 2 ? psio::encode(psio::pssz_<2>{}, book)
                                          : psio::encode(psio::pssz_<4>{}, book);
      ir::Value   v;
      Bytes       back;
      if (w == 2)
      {
         REQUIRE(ir::decode_exact<2>(schema, root, std::span<const char>{explicit_frame}, v).ok());
         REQUIRE(ir::encode<2>(schema, root, v, back).ok());
      }
      else
      {
         REQUIRE(ir::decode_exact<4>(schema, root, std::span<const char>{explicit_frame}, v).ok());
         REQUIRE(ir::encode<4>(schema, root, v, back).ok());
      }
      CHECK(back == explicit_frame);
      CHECK(v == value);
   }

   // Text, with the key's handler (object) and without (pairs of the wire
   // type), and with a strict registry that lacks it (an error).
   std::string text;
   REQUIRE(ir::value_to_json(schema, root, psio::json{}, value, text, &handlers, &names).ok());
   CHECK(text == json(book));
   text.clear();
   REQUIRE(ir::value_to_json(schema, root, psio::ijson{}, value, text, &handlers).ok());
   CHECK(text == ijson(book));
   std::string plain;
   REQUIRE(ir::value_to_json(schema, root, psio::json{}, value, plain).ok());
   CHECK(plain.find(R"("labelled":[[{"id":9},[]],[{"id":10},[0,3,255]]])") != std::string::npos);
   CHECK(json(book).find(R"("labelled":{"L9":[],"L10":[0,3,255]})") != std::string::npos);
   Bytes from_plain;
   REQUIRE(ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{plain}, from_plain)
              .ok());
   CHECK(from_plain == frame);
   ir::CustomHandlers strict;
   strict.require_all = true;
   text.clear();
   CHECK(says(ir::value_to_json(schema, root, psio::json{}, value, text, &strict), "no handler"));
   CHECK(says(ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{plain}, again,
                               nullptr, psio::json_parser::automatic, &strict),
              "no handler"));

   // Every syntax layer reads the same value, in any order.
   const std::string typed = json(book);
   for (const auto parser : {psio::json_parser::text, psio::json_parser::simdjson,
                             psio::json_parser::simdjson_index})
   {
      ir::Value read;
      REQUIRE(ir::json_to_value(schema, root, psio::json{}, std::span<const char>{typed}, read,
                                &names, parser)
                 .ok());
      CHECK(read == value);
   }
   {
      using Counts = std::map<std::string, std::uint32_t>;
      const auto  counts = schema_of<Counts>("Counts");
      ir::Value   read;
      const std::string shuffled = R"({"pear":3,"":9,"apple pie":2,"apple":1})";
      REQUIRE(ir::json_to_value(counts, st::Type{"Counts"}, psio::ijson{},
                                std::span<const char>{shuffled}, read)
                 .ok());
      Bytes bytes;
      REQUIRE(ir::encode_auto(counts, st::Type{"Counts"}, read, bytes).ok());
      CHECK(bytes == pssz(book.counts));
      // The value a reader returns is already in canonical order.
      ir::Value decoded;
      REQUIRE(ir::decode_auto(counts, st::Type{"Counts"}, std::span<const char>{bytes}, decoded,
                              unknown)
                 .ok());
      CHECK(read == decoded);

      // The encoder sorts whatever order a caller built, and refuses
      // duplicates; so does to_key.
      auto& items = std::get<ir::Sequence>(decoded.value);
      std::reverse(items.begin(), items.end());
      Bytes sorted_bytes, sorted_key;
      REQUIRE(ir::encode_auto(counts, st::Type{"Counts"}, decoded, sorted_bytes).ok());
      CHECK(sorted_bytes == bytes);
      REQUIRE(ir::to_key(counts, st::Type{"Counts"}, decoded, sorted_key).ok());
      CHECK(sorted_key == key(book.counts));
      std::string sorted_text;
      REQUIRE(ir::value_to_json(counts, st::Type{"Counts"}, psio::json{}, decoded, sorted_text)
                 .ok());
      CHECK(sorted_text == json(book.counts));
      items.push_back(items.front());
      CHECK(says(ir::encode_auto(counts, st::Type{"Counts"}, decoded, sorted_bytes),
                 "duplicate keys"));
      CHECK(says(ir::to_key(counts, st::Type{"Counts"}, decoded, sorted_key), "duplicate keys"));
      CHECK(says(ir::value_to_json(counts, st::Type{"Counts"}, psio::json{}, decoded, sorted_text),
                 "duplicate keys"));
      items.back() = ir::Value{std::string{"not an entry"}};
      CHECK(!ir::encode_auto(counts, st::Type{"Counts"}, decoded, sorted_bytes).ok());
   }
}

namespace
{
   // The dynamic key encoder against the typed one.
   template <typename T>
   void check_dynamic_key(const T& value)
   {
      const auto        schema = schema_of<T>("T");
      const st::AnyType root{st::Type{"T"}};
      const Bytes       frame = pssz(value);
      ir::Value         dynamic;
      bool              unknown = false;
      REQUIRE(ir::decode_auto(schema, root, std::span<const char>{frame}, dynamic, unknown).ok());
      Bytes out{'k'};
      const auto status = ir::to_key(schema, root, dynamic, out);
      INFO(std::string(status.ok() ? "" : status.error().what));
      REQUIRE(status.ok());
      Bytes want{'k'};
      const Bytes typed = key(value);
      want.insert(want.end(), typed.begin(), typed.end());
      CHECK(hex(out) == hex(want));
   }
   struct KeyRecord
   {
      std::uint32_t               table = 0;
      std::string                 name;
      std::optional<std::int64_t> rank;
      Shade                       shade = Shade::dark;
      PSIO_REFLECT(KeyRecord, table, name, rank, shade)
   };
   struct KeyFixed
   {
      std::int16_t a = 0;
      bool         b = false;
      PSIO_REFLECT(KeyFixed, definitionWillNotChange(), a, b)
   };
}  // namespace

TEST_CASE("assoc: the schema-driven key encoder writes the typed key", "[assoc][key][schema]")
{
   check_dynamic_key(true);
   check_dynamic_key(std::uint8_t{200});
   check_dynamic_key(std::int8_t{-3});
   check_dynamic_key(std::int16_t{-300});
   check_dynamic_key(std::uint32_t{70000});
   check_dynamic_key(std::int64_t{std::numeric_limits<std::int64_t>::min()});
   check_dynamic_key(std::uint64_t{std::numeric_limits<std::uint64_t>::max()});
   check_dynamic_key(psio::uint128{1} << 100);
   check_dynamic_key(-(psio::int128{1} << 100));
   {
      psio::uint256 big{};
      big.limb[0] = 1, big.limb[3] = 0x8000000000000000ull;
      check_dynamic_key(big);
   }
   for (double d : {0.0, -0.0, 1.5, -1.5, std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()})
      check_dynamic_key(d);
   for (float f : {0.0f, -0.0f, 2.5f, -2.5f})
      check_dynamic_key(f);
   check_dynamic_key('x');
   check_dynamic_key(Shade::light);
   check_dynamic_key(std::chrono::milliseconds{-5});
   check_dynamic_key(std::chrono::sys_seconds{std::chrono::seconds{1700000000}});
   check_dynamic_key(std::string{"a\0b\0", 4});
   check_dynamic_key(psio::utf8_string<8>{"h\xC3\xA9"});
   check_dynamic_key(std::vector<std::uint8_t>{0, 1, 0, 255});
   check_dynamic_key(std::vector<char>{'a', 0, 'b'});
   check_dynamic_key(std::vector<std::int8_t>{-128, 0, 127});
   check_dynamic_key(std::array<std::uint8_t, 3>{0, 1, 2});
   check_dynamic_key(std::array<std::int8_t, 2>{-1, 1});
   check_dynamic_key(std::array<std::uint16_t, 2>{1, 513});
   check_dynamic_key(std::vector<std::uint16_t>{1, 513});
   check_dynamic_key(std::vector<std::string>{"", "a"});
   check_dynamic_key(std::optional<std::string>{});
   check_dynamic_key(std::optional<std::string>{"x"});
   check_dynamic_key(std::optional<std::optional<std::uint8_t>>{std::optional<std::uint8_t>{}});
   check_dynamic_key(std::variant<std::uint8_t, std::string>{std::string{"v"}});
   check_dynamic_key(std::tuple<>{});
   check_dynamic_key(std::tuple<std::uint8_t, std::string, std::optional<std::int32_t>>{1, "t", {}});
   check_dynamic_key(std::pair<std::uint8_t, std::string>{1, "p"});
   check_dynamic_key(KeyRecord{7, "seven", -1, Shade::light});
   check_dynamic_key(KeyRecord{7, "", std::nullopt, Shade::dark});
   check_dynamic_key(KeyFixed{-2, true});
   check_dynamic_key(psio::bitvector<12>{});
   check_dynamic_key(std::set<std::uint8_t>{3, 0, 1});
   check_dynamic_key(std::set<std::int8_t>{3, 0, -1});
   check_dynamic_key(std::set<char>{'b', 'a', 0});
   check_dynamic_key(std::unordered_set<std::string>{"b", "a"});
   check_dynamic_key(std::map<std::string, std::vector<KeyRecord>>{{"k", {KeyRecord{1, "n", 2}}}});
   check_dynamic_key(std::map<std::set<std::int32_t>, std::map<double, bool>>{
      {{2, -1}, {{1.0, true}, {-1.0, false}}}, {{}, {}}});
   check_dynamic_key(sample());

   // No key encoding: an error, not a guess.
   const auto  schema = schema_of<psio::bitlist<8>>("T");
   Bytes       out;
   ir::Value   dynamic;
   bool        unknown = false;
   const Bytes frame   = pssz(psio::bitlist<8>{});
   REQUIRE(ir::decode_auto(schema, st::Type{"T"}, std::span<const char>{frame}, dynamic, unknown)
              .ok());
   CHECK(says(ir::to_key(schema, st::Type{"T"}, dynamic, out), "no key encoding"));
   CHECK(out.empty());
}

TEST_CASE("assoc: hostile input to the schema-driven readers", "[assoc][schema][fuzz]")
{
   const Book        book   = sample();
   const Bytes       frame  = pssz(book);
   const auto        schema = schema_of<Book>("Book");
   const st::AnyType root{st::Type{"Book"}};
   std::size_t       accepted = 0, agreed = 0;
   auto              feed     = [&](const Bytes& bytes) {
      const std::span<const char> in{bytes};
      ir::Value                   value;
      bool                        unknown = false;
      const bool dynamic = ir::decode_auto(schema, root, in, value, unknown).ok();
      const bool typed   = psio::validate<Book>(psio::pssz{}, in).ok();
      // The two validators define one accepted set.
      CHECK(dynamic == typed);
      agreed += dynamic == typed;
      if (!dynamic)
         return;
      ++accepted;
      Bytes again;
      REQUIRE(ir::encode_auto(schema, root, value, again).ok());
      ir::Value back;
      REQUIRE(ir::decode_auto(schema, root, std::span<const char>{again}, back, unknown).ok());
      CHECK(back == value);
      Bytes dynamic_key;
      REQUIRE(ir::to_key(schema, root, value, dynamic_key).ok());
      CHECK(dynamic_key ==
            key(psio::decode<Book>(psio::pssz{}, std::span<const char>{again})));
   };
   for (std::size_t n = 0; n <= frame.size(); ++n)
      feed(Bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(n)));
   for (std::size_t i = 0; i < frame.size(); ++i)
      for (unsigned char flip : {0x01, 0x02, 0x80, 0xff})
      {
         Bytes bad = frame;
         bad[i]    = static_cast<char>(static_cast<unsigned char>(bad[i]) ^ flip);
         feed(bad);
      }
   CHECK(accepted > 1);

   // Random permutations of a set: accepted only in ascending order, by
   // every reader.
   std::mt19937               rng(88);
   std::vector<std::int16_t>  elements{-300, -1, 0, 7, 300};
   const auto set_schema = schema_of<std::set<std::int16_t>>("S");
   for (int round = 0; round < 200; ++round)
   {
      auto shuffled = elements;
      std::shuffle(shuffled.begin(), shuffled.end(), rng);
      if (round % 3 == 0)
         shuffled.push_back(shuffled[rng() % shuffled.size()]);
      const bool  ascending = shuffled == elements;
      const Bytes bytes     = pssz(shuffled);
      const std::span<const char> in{bytes};
      CHECK(psio::validate<std::set<std::int16_t>>(psio::pssz{}, in).ok() == ascending);
      CHECK(psio::validate<std::unordered_set<std::int16_t>>(psio::pssz{}, in).ok() == ascending);
      CHECK(psio::validate_strict<std::set<std::int16_t>>(psio::pssz{}, in).ok() == ascending);
      CHECK(ir::validate_pssz(set_schema, st::Type{"S"}, in).ok() == ascending);
      const Bytes key_bytes = key(shuffled);
      CHECK(psio::validate<std::set<std::int16_t>>(psio::key{}, std::span<const char>{key_bytes})
               .ok() == ascending);
      // Text: any order, but no repeats.
      const bool        unique = shuffled.size() == elements.size();
      const std::string text   = json(shuffled);
      CHECK(psio::validate_json<std::set<std::int16_t>>(psio::json{}, std::span<const char>{text})
               .ok() == unique);
      Bytes from_text_bytes;
      CHECK(ir::json_to_pssz(set_schema, st::Type{"S"}, psio::json{}, std::span<const char>{text},
                             from_text_bytes)
               .ok() == unique);
      if (unique)
         CHECK(from_text_bytes == pssz(elements));
   }
}

namespace
{
   struct Catalog
   {
      std::map<std::string, std::uint32_t>      stock;
      std::set<std::string>                     tags;
      std::map<std::uint32_t, std::uint32_t>    squares;
      std::pair<std::uint32_t, std::string>     head;
      PSIO_REFLECT(Catalog, stock, tags, squares, head)
   };
}  // namespace

TEST_CASE("assoc: graphql presents a map as the list of its entries", "[assoc][graphql]")
{
   const Catalog catalog{{{"pear", 3}, {"apple", 1}}, {"b", "a"}, {{3, 9}, {2, 4}}, {1, "h"}};
   std::string   out;
   psio::graphql::schema<Catalog>::run(
      catalog, "{ stock { _0 _1 } tags squares head { _0 _1 } }", out, {}, {});
   CHECK(out == R"({"data":{"stock":[{"_0":"apple","_1":1},{"_0":"pear","_1":3}],)"
                R"("tags":["a","b"],"squares":[[2,4],[3,9]],"head":{"_0":1,"_1":"h"}}})");
   out.clear();
   psio::graphql::schema<Catalog>::run(
      catalog, R"({ __type(name: "Catalog") { fields { name type { kind ofType { kind ofType { kind ofType { name } } } } } } })",
      out, {}, {});
   CHECK(out.find(R"("name":"Tuple_String_Int")") != std::string::npos);
}
