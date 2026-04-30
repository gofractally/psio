// Phase 9 — JSON format.
//
// Exercises the v3 architecture against a text format. Scope matches the
// other format tests: primitives, strings, vectors, arrays, optionals,
// reflected records.

#include <psio/json.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct JsonPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(JsonPoint, x, y)

struct JsonPerson
{
   std::string  name;
   std::int32_t age;
};
PSIO_REFLECT(JsonPerson, name, age)

struct JsonNested
{
   JsonPerson  owner;
   std::string title;
};
PSIO_REFLECT(JsonNested, owner, title)

TEST_CASE("json encodes bool", "[json][primitive]")
{
   REQUIRE(psio::encode(psio::json{}, true) == "true");
   REQUIRE(psio::encode(psio::json{}, false) == "false");
}

TEST_CASE("json round-trips bool", "[json][primitive]")
{
   auto s = psio::encode(psio::json{}, true);
   auto v = psio::decode<bool>(psio::json{}, std::span<const char>{s});
   REQUIRE(v == true);
}

TEST_CASE("json round-trips integers", "[json][primitive]")
{
   auto s = psio::encode(psio::json{}, std::int32_t{-42});
   REQUIRE(s == "-42");
   auto v =
      psio::decode<std::int32_t>(psio::json{}, std::span<const char>{s});
   REQUIRE(v == -42);
}

TEST_CASE("json round-trips large unsigned integer", "[json][primitive]")
{
   std::uint64_t big = 0xDEADBEEFCAFEBABE;
   auto          s   = psio::encode(psio::json{}, big);
   auto          v   = psio::decode<std::uint64_t>(
      psio::json{}, std::span<const char>{s});
   REQUIRE(v == big);
}

TEST_CASE("json escapes string quotes and newlines", "[json][string]")
{
   std::string in = "a\"b\nc";
   auto        s  = psio::encode(psio::json{}, in);
   REQUIRE(s == R"("a\"b\nc")");

   auto back =
      psio::decode<std::string>(psio::json{}, std::span<const char>{s});
   REQUIRE(back == in);
}

TEST_CASE("json round-trips std::vector<int>", "[json][vector]")
{
   std::vector<std::int32_t> v{1, 2, 3};
   auto                      s = psio::encode(psio::json{}, v);
   REQUIRE(s == "[1,2,3]");

   auto back = psio::decode<std::vector<std::int32_t>>(
      psio::json{}, std::span<const char>{s});
   REQUIRE(back == v);
}

TEST_CASE("json round-trips empty std::vector", "[json][vector]")
{
   std::vector<std::int32_t> v;
   auto                      s = psio::encode(psio::json{}, v);
   REQUIRE(s == "[]");
   auto back = psio::decode<std::vector<std::int32_t>>(
      psio::json{}, std::span<const char>{s});
   REQUIRE(back.empty());
}

TEST_CASE("json round-trips std::array", "[json][array]")
{
   std::array<std::uint32_t, 3> arr{{10, 20, 30}};
   auto                         s = psio::encode(psio::json{}, arr);
   REQUIRE(s == "[10,20,30]");
   auto back = psio::decode<std::array<std::uint32_t, 3>>(
      psio::json{}, std::span<const char>{s});
   REQUIRE(back == arr);
}

TEST_CASE("json round-trips std::optional<T>", "[json][optional]")
{
   std::optional<std::int32_t> some = 7;
   auto                        s    = psio::encode(psio::json{}, some);
   REQUIRE(s == "7");
   auto back = psio::decode<std::optional<std::int32_t>>(
      psio::json{}, std::span<const char>{s});
   REQUIRE(back.has_value());
   REQUIRE(*back == 7);

   std::optional<std::int32_t> none;
   auto                        s2 = psio::encode(psio::json{}, none);
   REQUIRE(s2 == "null");
   auto back2 = psio::decode<std::optional<std::int32_t>>(
      psio::json{}, std::span<const char>{s2});
   REQUIRE(!back2.has_value());
}

TEST_CASE("json round-trips reflected records", "[json][record]")
{
   JsonPoint p{3, -5};
   auto      s = psio::encode(psio::json{}, p);
   REQUIRE(s == R"({"x":3,"y":-5})");
   auto back =
      psio::decode<JsonPoint>(psio::json{}, std::span<const char>{s});
   REQUIRE(back.x == 3);
   REQUIRE(back.y == -5);
}

TEST_CASE("json round-trips records with string fields", "[json][record]")
{
   JsonPerson pp{"Alice", 30};
   auto       s = psio::encode(psio::json{}, pp);
   REQUIRE(s == R"({"name":"Alice","age":30})");
   auto back =
      psio::decode<JsonPerson>(psio::json{}, std::span<const char>{s});
   REQUIRE(back.name == "Alice");
   REQUIRE(back.age == 30);
}

TEST_CASE("json decoder tolerates whitespace and key reordering",
          "[json][record]")
{
   std::string text = R"(  { "age" : 30 , "name" : "Bob" } )";
   auto        back =
      psio::decode<JsonPerson>(psio::json{}, std::span<const char>{text});
   REQUIRE(back.name == "Bob");
   REQUIRE(back.age == 30);
}

TEST_CASE("json round-trips nested records", "[json][record][nested]")
{
   JsonNested n{{"Carol", 25}, "Dr"};
   auto       s = psio::encode(psio::json{}, n);
   REQUIRE(s == R"({"owner":{"name":"Carol","age":25},"title":"Dr"})");
   auto back =
      psio::decode<JsonNested>(psio::json{}, std::span<const char>{s});
   REQUIRE(back.owner.name == "Carol");
   REQUIRE(back.owner.age == 25);
   REQUIRE(back.title == "Dr");
}

TEST_CASE("json validate accepts well-formed input", "[json][validate]")
{
   std::string s  = R"({"x":1,"y":2})";
   auto        st = psio::validate<JsonPoint>(psio::json{},
                                        std::span<const char>{s});
   REQUIRE(st.ok());
}

TEST_CASE("json validate rejects empty input", "[json][validate]")
{
   char tiny = '\0';
   auto st = psio::validate<JsonPoint>(psio::json{},
                                         std::span<const char>{&tiny, 0});
   REQUIRE(!st.ok());
}

TEST_CASE("json scoped sugar matches generic CPO", "[json][format_tag_base]")
{
   JsonPoint p{1, 2};
   auto      a = psio::encode(psio::json{}, p);
   auto      b = psio::json::encode(p);
   REQUIRE(a == b);

   auto v = psio::json::decode<JsonPoint>(std::span<const char>{a});
   REQUIRE(v.x == 1);
   REQUIRE(v.y == 2);
}

TEST_CASE("json encodes std::variant as [index, value]", "[json][variant]")
{
   using V = std::variant<std::int32_t, std::string>;
   REQUIRE(psio::encode(psio::json{}, V{std::int32_t{42}}) ==
           "[0,42]");
   REQUIRE(psio::encode(psio::json{}, V{std::string("hi")}) ==
           "[1,\"hi\"]");
}

TEST_CASE("json round-trips std::variant", "[json][variant][round-trip]")
{
   using V = std::variant<std::int32_t, std::string>;
   V     in = std::string("payload");
   auto  bv = psio::encode(psio::json{}, in);
   auto  out =
      psio::decode<V>(psio::json{}, std::span<const char>{bv});
   REQUIRE(out.index() == 1);
   REQUIRE(std::get<1>(out) == "payload");
}
