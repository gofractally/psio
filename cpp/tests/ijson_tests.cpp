// psio::ijson (RFC 7493), tuples in both JSON formats, the integer-form
// annotations, typed validation and the checked entry points. The shared
// fixture (json_conformance_tests.cpp) carries the cross-language cases;
// this file covers C++-only types and behaviour.

#include <psio/ext_int.hpp>
#include <psio/json.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cmath>
#include <limits>
#include <random>

namespace ij
{
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      friend bool  operator==(const Point&, const Point&) = default;
   };
   PSIO_REFLECT(Point, x, y)

   struct Wide
   {
      std::uint64_t u = 0;
      std::int64_t  i = 0;
      std::uint32_t small = 0;
      friend bool   operator==(const Wide&, const Wide&) = default;
   };
   PSIO_REFLECT(Wide, u, i, small)

   enum class Color : std::uint8_t { Red = 0, Green = 1, Blue = 2 };
   enum class Big : std::uint64_t { Huge = 18446744073709551615ull };

   struct Wrapped
   {
      psio::byte_array<3>               fixed;
      psio::bounded<std::uint16_t, 2>   few;
      psio::utf8_string<4>              tag;
      std::vector<char>                 chars;
      std::array<std::byte, 2>          raw{};
      std::vector<std::int8_t>          small_ints;
      Color                             color = Color::Red;
      Big                               big   = Big::Huge;
   };
   PSIO_REFLECT(Wrapped, fixed, few, tag, chars, raw, small_ints, color, big)

   struct Tree
   {
      std::vector<Tree> children;
   };
   PSIO_REFLECT(Tree, children)

   struct Ints128
   {
      psio::uint128 u = 0;
      psio::int128  i = 0;
      psio::uint256 w;
   };
   PSIO_REFLECT(Ints128, u, i, w)

   // Forced forms and the per-member hex form of a wide integer.
   struct Ints128Forms
   {
      psio::uint128 as_string = 0;
      psio::uint128 as_number = 0;
      psio::uint128 as_hex    = 0;
   };
   PSIO_REFLECT(Ints128Forms, as_string, as_number, as_hex)

   struct HexMember
   {
      psio::uint128 plain = 0;
      std::uint64_t n     = 0;
   };
   PSIO_REFLECT(HexMember, plain, n)
}  // namespace ij

PSIO_FIELD_ATTRS(ij::Ints128Forms, as_string, psio::json_string | psio::ijson_string)
PSIO_FIELD_ATTRS(ij::Ints128Forms, as_number, psio::json_number | psio::ijson_number)
PSIO_FIELD_ATTRS(ij::Ints128Forms, as_hex, psio::as<psio::hex_tag>)

using namespace ij;

namespace
{
   template <typename T>
   std::string js(const T& v) { return psio::encode(psio::json{}, v); }
   template <typename T>
   std::string ijs(const T& v) { return psio::encode(psio::ijson{}, v); }
   template <typename Fmt, typename T>
   bool valid(std::string_view text)
   {
      return psio::validate<T>(Fmt{}, std::span<const char>{text}).ok();
   }
   template <typename Fmt, typename T>
   T read(std::string_view text)
   {
      T    out{};
      auto st = psio::decode_json_checked(Fmt{}, std::span<const char>{text}, out);
      REQUIRE(st.ok());
      return out;
   }
}  // namespace

TEST_CASE("ijson writes wide integers beyond 2^53 - 1 as decimal strings", "[ijson]")
{
   CHECK(ijs(std::uint64_t{18446744073709551615ull}) == "\"18446744073709551615\"");
   CHECK(ijs(std::numeric_limits<std::int64_t>::min()) == "\"-9223372036854775808\"");
   CHECK(ijs(std::uint64_t{0}) == "0");
   // The boundary: |v| <= 2^53 - 1 is a bare number, beyond it a string.
   CHECK(ijs(std::uint64_t{9007199254740991ull}) == "9007199254740991");
   CHECK(ijs(std::uint64_t{9007199254740992ull}) == "\"9007199254740992\"");
   CHECK(ijs(std::int64_t{9007199254740991ll}) == "9007199254740991");
   CHECK(ijs(std::int64_t{9007199254740992ll}) == "\"9007199254740992\"");
   CHECK(ijs(std::int64_t{-9007199254740991ll}) == "-9007199254740991");
   CHECK(ijs(std::int64_t{-9007199254740992ll}) == "\"-9007199254740992\"");
   CHECK(ijs(std::numeric_limits<std::int64_t>::max()) == "\"9223372036854775807\"");
   CHECK(ijs(std::uint32_t{4294967295u}) == "4294967295");
   CHECK(ijs(std::int8_t{-5}) == "-5");
   CHECK(ijs(true) == "true");
   CHECK(ijs(1.5) == "1.5");
   CHECK(ijs(Wide{1, -2, 3}) == R"({"u":1,"i":-2,"small":3})");
   // psio::json is unchanged: every integer is a bare number.
   CHECK(js(std::uint64_t{18446744073709551615ull}) == "18446744073709551615");
   CHECK(js(Wide{1, -2, 3}) == R"({"u":1,"i":-2,"small":3})");
}

TEST_CASE("ijson writes byte sequences as uppercase hex", "[ijson]")
{
   CHECK(ijs(std::vector<std::uint8_t>{0x01, 0xff}) == "\"01FF\"");
   CHECK(ijs(std::vector<std::uint8_t>{}) == "\"\"");
   CHECK(ijs(std::vector<char>{'\x0a', 'b'}) == "\"0A62\"");
   CHECK(ijs(std::vector<std::byte>{std::byte{0xab}}) == "\"AB\"");
   CHECK(ijs(std::array<unsigned char, 2>{{0xde, 0xad}}) == "\"DEAD\"");
   CHECK(ijs(psio::byte_array<2>{{{0xbe, 0xef}}}) == "\"BEEF\"");
   // int8_t is a small integer, not a byte.
   CHECK(ijs(std::vector<std::int8_t>{-1, 2}) == "[-1,2]");
   // psio::json: arrays of numbers.
   CHECK(js(std::vector<std::uint8_t>{0x01, 0xff}) == "[1,255]");
   CHECK(js(std::vector<std::byte>{std::byte{0xab}}) == "[171]");
   CHECK(js(psio::byte_array<2>{{{0xbe, 0xef}}}) == "[190,239]");

   CHECK(read<psio::ijson, std::vector<std::uint8_t>>("\"01ff\"") ==
         std::vector<std::uint8_t>{0x01, 0xff});
   CHECK(read<psio::ijson, std::vector<char>>("\"0A62\"") == std::vector<char>{'\x0a', 'b'});
   CHECK(read<psio::ijson, psio::byte_array<2>>("\"BEEF\"").storage() ==
         std::array<std::uint8_t, 2>{{0xbe, 0xef}});
   CHECK(read<psio::json, psio::byte_array<2>>("[190,239]").storage() ==
         std::array<std::uint8_t, 2>{{0xbe, 0xef}});
}

TEST_CASE("ijson names variant alternatives; json keeps [index, value]", "[ijson][variant]")
{
   using V = std::variant<std::uint32_t, std::string, Point, std::uint64_t, std::vector<std::int32_t>,
                          std::optional<bool>, double, float, bool, std::int8_t>;
   CHECK(ijs(V{std::in_place_index<0>, 42u}) == R"({"uint32":42})");
   CHECK(ijs(V{std::in_place_index<1>, "hi"}) == R"({"string":"hi"})");
   CHECK(ijs(V{std::in_place_index<2>, Point{1, 2}}) == R"({"Point":{"x":1,"y":2}})");
   CHECK(ijs(V{std::in_place_index<3>, std::uint64_t{7}}) == R"({"uint64":7})");
   CHECK(ijs(V{std::in_place_index<4>, std::vector<std::int32_t>{1}}) == R"({"int32[]":[1]})");
   CHECK(ijs(V{std::in_place_index<5>, std::optional<bool>{}}) == R"({"bool?":null})");
   CHECK(ijs(V{std::in_place_index<6>, 2.5}) == R"({"double":2.5})");
   CHECK(ijs(V{std::in_place_index<7>, 2.5f}) == R"({"float32":2.5})");
   CHECK(ijs(V{std::in_place_index<8>, true}) == R"({"bool":true})");
   CHECK(ijs(V{std::in_place_index<9>, std::int8_t{-1}}) == R"({"int8":-1})");
   CHECK(js(V{std::in_place_index<2>, Point{1, 2}}) == R"([2,{"x":1,"y":2}])");

   for (std::size_t i = 0; i < std::variant_size_v<V>; ++i)
   {
      V v;
      switch (i)
      {
         case 0: v.emplace<0>(42u); break;
         case 1: v.emplace<1>("hi"); break;
         case 2: v.emplace<2>(Point{1, 2}); break;
         case 3: v.emplace<3>(7u); break;
         case 4: v.emplace<4>(std::vector<std::int32_t>{1}); break;
         case 5: v.emplace<5>(true); break;
         case 6: v.emplace<6>(2.5); break;
         case 7: v.emplace<7>(2.5f); break;
         case 8: v.emplace<8>(true); break;
         case 9: v.emplace<9>(std::int8_t{-1}); break;
      }
      CHECK(read<psio::ijson, V>(ijs(v)) == v);
      CHECK(read<psio::json, V>(js(v)) == v);
   }
   CHECK(!valid<psio::ijson, V>(R"({"int32":1})"));
   CHECK(!valid<psio::ijson, V>(R"({"uint32":1,"string":"x"})"));
   CHECK(!valid<psio::ijson, V>("[0,1]"));
   CHECK(!valid<psio::json, V>(R"({"uint32":1})"));
   CHECK(!valid<psio::json, V>("[10,1]"));
}

TEST_CASE("tuples are arrays in json and ijson", "[json][ijson][tuple]")
{
   using T = std::tuple<std::uint32_t, std::string, std::uint64_t, std::tuple<>, std::tuple<bool>>;
   const T v{1, "two", 3, {}, {true}};
   CHECK(js(v) == R"([1,"two",3,[],[true]])");
   CHECK(ijs(v) == R"([1,"two",3,[],[true]])");
   CHECK(js(std::tuple<>{}) == "[]");
   CHECK(ijs(std::tuple<>{}) == "[]");
   CHECK(read<psio::json, T>(js(v)) == v);
   CHECK(read<psio::ijson, T>(ijs(v)) == v);
   CHECK(psio::decode<T>(psio::json{}, std::span<const char>{js(v)}) == v);
   CHECK(!valid<psio::json, T>(R"([1,"two",3,[]])"));
   CHECK(!valid<psio::json, T>(R"([1,"two",3,[],[true],null])"));
   CHECK(!valid<psio::json, std::tuple<>>("[null]"));
   CHECK(valid<psio::json, std::tuple<>>(" [ ] "));
}

TEST_CASE("wrappers, enums and small-integer vectors", "[json][ijson]")
{
   Wrapped w;
   w.fixed = psio::byte_array<3>{{{1, 2, 3}}};
   w.few   = psio::bounded<std::uint16_t, 2>{7, 8};
   w.tag   = psio::utf8_string<4>{"abcd"};
   w.chars = {'h', 'i'};
   w.raw   = {std::byte{0}, std::byte{255}};
   w.small_ints = {-1};
   w.color = Color::Blue;
   CHECK(js(w) ==
         R"({"fixed":[1,2,3],"few":[7,8],"tag":"abcd","chars":[104,105],"raw":[0,255],"small_ints":[-1],"color":2,"big":18446744073709551615})");
   CHECK(ijs(w) ==
         R"({"fixed":"010203","few":[7,8],"tag":"abcd","chars":"6869","raw":"00FF","small_ints":[-1],"color":2,"big":"18446744073709551615"})");
   for (const bool i : {false, true})
   {
      const std::string text = i ? ijs(w) : js(w);
      const Wrapped back = i ? read<psio::ijson, Wrapped>(text) : read<psio::json, Wrapped>(text);
      CHECK(back.fixed.storage() == w.fixed.storage());
      CHECK(back.few.storage() == w.few.storage());
      CHECK(back.tag == w.tag);
      CHECK(back.chars == w.chars);
      CHECK(back.raw == w.raw);
      CHECK(back.color == Color::Blue);
      CHECK(back.big == Big::Huge);
   }
   // Bounds are part of the type: reading reports them instead of aborting.
   CHECK(valid<psio::json, psio::bounded<std::uint16_t, 2>>("[1,2]"));
   CHECK(!valid<psio::json, psio::bounded<std::uint16_t, 2>>("[1,2,3]"));
   CHECK(valid<psio::ijson, psio::utf8_string<4>>("\"abcd\""));
   CHECK(!valid<psio::ijson, psio::utf8_string<4>>("\"abcde\""));
   psio::utf8_string<4> s;
   CHECK(!psio::decode_json_checked(psio::json{}, std::span<const char>{"\"abcde\"", 7}, s).ok());
   // An enum is its underlying integer; any value of that integer is read.
   CHECK(read<psio::json, Color>("7") == static_cast<Color>(7));
   CHECK(!valid<psio::json, Color>("256"));
   CHECK(!valid<psio::json, Color>("\"Red\""));
}

TEST_CASE("a value without a JSON text fails to encode", "[json][ijson][encode]")
{
   const std::string raw = "bad \xff";
   std::string       out = "keep";
   for (const bool i : {false, true})
   {
      auto st = i ? psio::encode_json_checked(psio::ijson{}, raw, out)
                  : psio::encode_json_checked(psio::json{}, raw, out);
      REQUIRE(!st.ok());
      CHECK(st.error().what == "json: string is not valid UTF-8");
      CHECK(out == "keep");
   }
   CHECK_THROWS_AS(psio::encode(psio::json{}, raw), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, raw), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, std::vector<std::string>{"ok", "\xC0\x80"}),
                   psio::codec_exception);
   // Valid UTF-8 is an ordinary string in both.
   CHECK(js(std::string("t\xC3\xA9xt")) == "\"t\xC3\xA9xt\"");
   CHECK(ijs(std::string("t\xC3\xA9xt")) == "\"t\xC3\xA9xt\"");

   // Non-finite floats have no JSON text: an error in both formats.
   CHECK(!psio::encode_json_checked(psio::ijson{}, std::nan(""), out).ok());
   CHECK(!psio::encode_json_checked(psio::ijson{}, -std::numeric_limits<float>::infinity(), out)
             .ok());
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, std::nan("")), psio::codec_exception);
   CHECK(!psio::encode_json_checked(psio::json{}, std::nan(""), out).ok());
   CHECK(!psio::encode_json_checked(psio::json{}, std::numeric_limits<double>::infinity(), out)
             .ok());
   CHECK(!psio::encode_json_checked(psio::json{}, -std::numeric_limits<float>::infinity(), out)
             .ok());
   CHECK(!psio::encode_json_checked(psio::json{}, std::vector<double>{1.0, std::nan("")}, out)
             .ok());
   CHECK_THROWS_AS(psio::encode(psio::json{}, std::numeric_limits<double>::infinity()),
                   psio::codec_exception);
   CHECK(out == "keep");
   // Noncharacters are refused by ijson only.
   CHECK(js(std::string("\xEF\xBF\xBF")) == "\"\xEF\xBF\xBF\"");
   CHECK(!psio::encode_json_checked(psio::ijson{}, std::string("\xEF\xBF\xBF"), out).ok());
   CHECK(out == "keep");
}

TEST_CASE("fail-fast decode raises codec_exception on bad text", "[json][ijson][decode]")
{
   auto bad = [](std::string_view text) { return std::span<const char>{text}; };
   CHECK_THROWS_AS(psio::decode<Point>(psio::ijson{}, bad(R"({"x":1,"y":"2"})")),
                   psio::codec_exception);
   CHECK_THROWS_AS(psio::decode<Point>(psio::json{}, bad(R"({"x":1,"y":)")), psio::codec_exception);
   CHECK_THROWS_AS(psio::decode<std::uint8_t>(psio::json{}, bad("256")), psio::codec_exception);
   CHECK(psio::make_boxed<Point>(psio::ijson{}, bad("[]")) == nullptr);
   // psio::json keeps its leniency for a missing member in the fail-fast
   // decode; validate and the checked decode report it.
   CHECK(psio::decode<Point>(psio::json{}, bad(R"({"x":5})")) == Point{5, 0});
   CHECK(!valid<psio::json, Point>(R"({"x":5})"));
   Point p;
   CHECK(!psio::decode_json_checked(psio::json{}, bad(R"({"x":5})"), p).ok());
   CHECK_THROWS_AS(psio::decode<Point>(psio::ijson{}, bad(R"({"x":5})")), psio::codec_exception);
}

TEST_CASE("json decode: unknown keys and unicode escapes (regressions)", "[json][decode]")
{
   // An unknown key used to leave the decoder spinning on its value.
   const std::string text = R"({"zzz":[1,{"k":"v"}],"x":3,"other":null,"y":4})";
   CHECK(psio::decode<Point>(psio::json{}, std::span<const char>{text}) == Point{3, 4});
   // \uXXXX used to decode as the letter 'u' followed by the digits.
   const std::string ctl = js(std::string("\x01"));
   CHECK(ctl == std::string("\"\\u") + "0001\"");
   CHECK(psio::decode<std::string>(psio::json{}, std::span<const char>{ctl}) == "\x01");
}

TEST_CASE("json validate is typed", "[json][validate]")
{
   CHECK(valid<psio::json, Point>(R"({"x":1,"y":2})"));
   CHECK(valid<psio::json, Point>(R"( { "y" : 2 , "x" : 1 , "extra" : [ ] } )"));
   CHECK(!valid<psio::json, Point>(R"({"x":"1","y":2})"));
   CHECK(!valid<psio::json, Point>(R"({"x":1})"));
   CHECK(!valid<psio::json, Point>(R"({"x":1,"y":2147483648})"));
   CHECK(!valid<psio::json, Point>(R"([1,2])"));
   CHECK(!valid<psio::json, Point>(R"({"x":1,"y":2} trailing)"));
   auto st = psio::validate<Point>(psio::json{}, std::span<const char>{R"({"x":1,"y":true})", 16});
   REQUIRE(!st.ok());
   CHECK(st.error().format_name == "json");
   CHECK(st.error().byte_offset == 11);
   auto st2 = psio::validate<Wide>(psio::ijson{}, std::span<const char>{R"({"u":"x"})", 9});
   REQUIRE(!st2.ok());
   CHECK(st2.error().format_name == "ijson");
}

TEST_CASE("depth bombs are rejected without deep recursion", "[json][ijson][depth]")
{
   const std::string arrays(1000000, '[');
   CHECK(!valid<psio::json, Tree>(arrays));
   CHECK(!valid<psio::ijson, std::vector<std::vector<std::uint8_t>>>(arrays));
   std::string nested;
   for (int i = 0; i < 300000; ++i) nested += "{\"children\":[";
   CHECK(!valid<psio::ijson, Tree>(nested));
   Tree t;
   CHECK(!psio::decode_json_checked(psio::json{}, std::span<const char>{nested}, t).ok());
   std::string unknown = "{\"x\":";
   for (int i = 0; i < 300000; ++i) unknown += "{\"k\":[";
   CHECK(!valid<psio::ijson, Point>(unknown));
   CHECK(!valid<psio::json, Point>(unknown));
}

TEST_CASE("128-bit and 256-bit integers are decimal", "[json][ijson][ext_int]")
{
   // json: a bare decimal number. ijson: a decimal string, always.
   Ints128 v;
   v.u = (psio::uint128{1} << 100) + 5;
   v.i = -42;
   v.w = psio::uint256{std::uint64_t{255}};
   CHECK(js(v) == R"({"u":1267650600228229401496703205381,"i":-42,"w":255})");
   CHECK(ijs(v) == R"({"u":"1267650600228229401496703205381","i":"-42","w":"255"})");
   for (const std::string text : {js(v), ijs(v)})
   {
      const auto back = read<psio::ijson, Ints128>(ijs(v));
      CHECK(back.u == v.u);
      CHECK(back.i == v.i);
      CHECK(back.w == v.w);
      (void)text;
   }
   const auto from_json = read<psio::json, Ints128>(js(v));
   CHECK(from_json.u == v.u);
   CHECK(from_json.i == v.i);
   CHECK(from_json.w == v.w);

   // The older "0x" text is still read, in both formats; a decimal string
   // and (within the safe range under ijson) a bare number are too.
   const std::string legacy = R"({"u":"0x10000000000000000000000005","i":"-0x2a","w":"0xff"})";
   for (bool i_json : {false, true})
   {
      const auto back = i_json ? read<psio::ijson, Ints128>(legacy)
                               : read<psio::json, Ints128>(legacy);
      CHECK(back.u == v.u);
      CHECK(back.i == v.i);
      CHECK(back.w == v.w);
   }
   CHECK(valid<psio::ijson, Ints128>(R"({"u":"5","i":"-0x2a","w":"0xff"})"));
   CHECK(valid<psio::ijson, Ints128>(R"({"u":5,"i":-7,"w":9007199254740991})"));
   CHECK(!valid<psio::ijson, Ints128>(R"({"u":9007199254740992,"i":0,"w":0})"));
   CHECK(valid<psio::json, Ints128>(R"({"u":9007199254740992,"i":0,"w":0})"));
   CHECK(!valid<psio::json, Ints128>(R"({"u":-1,"i":0,"w":0})"));
   CHECK(!valid<psio::json, Ints128>(R"({"u":340282366920938463463374607431768211456,"i":0,"w":0})"));
   CHECK(!valid<psio::ijson, Ints128>(R"({"u":"0x","i":0,"w":0})"));
   CHECK(!valid<psio::ijson, Ints128>(R"({"u":"05","i":0,"w":0})"));
   CHECK(!valid<psio::ijson, Ints128>(R"({"u":"5","i":"0x80000000000000000000000000000000","w":0})"));

   // A forced form, and the hex form kept per member with as<hex_tag>.
   Ints128Forms f;
   f.as_string = 7;
   f.as_number = (psio::uint128{1} << 70);
   f.as_hex    = 255;
   CHECK(js(f) == R"({"as_string":"7","as_number":1180591620717411303424,"as_hex":"0xff"})");
   CHECK(ijs(f) == R"({"as_string":"7","as_number":1180591620717411303424,"as_hex":"0xff"})");
   const auto fb = read<psio::ijson, Ints128Forms>(ijs(f));
   CHECK(fb.as_string == f.as_string);
   CHECK(fb.as_number == f.as_number);
   CHECK(fb.as_hex == f.as_hex);
   CHECK(!valid<psio::json, Ints128Forms>(R"({"as_string":7,"as_number":1,"as_hex":"0x1"})"));
   CHECK(!valid<psio::ijson, Ints128Forms>(R"({"as_string":"7","as_number":"1","as_hex":"0x1"})"));
}

namespace ijson_enum_test
{
   enum class Status : std::int16_t { pending = -1, active = 2, closed = 7 };
   PSIO_REFLECT_ENUM(Status, pending, active, closed)
   enum Plain : std::uint8_t { plain_a, plain_b };
   enum class Dense : std::uint64_t { zero, one, two };
   PSIO_REFLECT_ENUM(Dense, zero, one, two)
   struct Holder
   {
      enum class Mode { off, on };
      PSIO_REFLECT_ENUM(Mode, off, on)
      Mode                  mode = Mode::off;
      Status                status = Status::pending;
      std::vector<Status>   history;
      std::optional<Dense>  dense;
      Plain                 plain = plain_a;
   };
   PSIO_REFLECT(Holder, mode, status, history, dense, plain)
}

TEST_CASE("enums are written by name and read by name or number", "[json][ijson][enum]")
{
   using namespace ijson_enum_test;
   static_assert(psio::reflect_enum<Status>::is_reflected);
   static_assert(!psio::reflect_enum<Plain>::is_reflected);
   static_assert(!psio::reflect_enum<int>::is_reflected);
   static_assert(psio::reflect_enum<Status>::count == 3);
   static_assert(psio::reflect_enum<Status>::name == "Status");
   static_assert(psio::reflect_enum<Status>::name_of(Status::active) == "active");
   static_assert(!psio::reflect_enum<Status>::name_of(static_cast<Status>(3)));
   static_assert(psio::reflect_enum<Status>::value_of("closed") == Status::closed);
   static_assert(!psio::reflect_enum<Status>::value_of("Closed"));
   static_assert(psio::reflect_enum<Dense>::name_of(Dense::two) == "two");
   static_assert(!psio::reflect_enum<Dense>::name_of(static_cast<Dense>(3)));
   static_assert(psio::reflect_enum<Holder::Mode>::name_of(Holder::Mode::on) == "on");

   CHECK(js(Status::active) == R"("active")");
   CHECK(ijs(Status::pending) == R"("pending")");
   CHECK(js(static_cast<Status>(3)) == "3");
   CHECK(ijs(static_cast<Status>(-9)) == "-9");
   CHECK(js(plain_b) == "1");  // no names declared: its number, as before
   // An unnamed 64-bit value follows the integer rule of its format.
   CHECK(js(static_cast<Dense>(~std::uint64_t{0})) == "18446744073709551615");
   CHECK(ijs(static_cast<Dense>(~std::uint64_t{0})) == R"("18446744073709551615")");

   Holder h{Holder::Mode::on, Status::closed, {Status::active, static_cast<Status>(0)},
            Dense::one, plain_b};
   const std::string want =
      R"({"mode":"on","status":"closed","history":["active",0],"dense":"one","plain":1})";
   CHECK(js(h) == want);
   CHECK(ijs(h) == want);
   for (const char* text : {R"({"mode":"on","status":"closed","history":["active",0],"dense":"one","plain":1})",
                            R"({"mode":1,"status":7,"history":[2,"0"],"dense":"1","plain":1})",
                            R"({"mode":"on:1","status":"closed:7","history":["active:2","0"],"dense":"one:1","plain":1})"})
   {
      const auto back = read<psio::ijson, Holder>(text);
      CHECK(back.mode == h.mode);
      CHECK(back.status == h.status);
      CHECK(back.history == h.history);
      CHECK(back.dense == h.dense);
      CHECK(back.plain == h.plain);
      CHECK(js(read<psio::json, Holder>(text)) == want);
   }
   for (const char* bad : {R"("Active")", R"("active:3")", R"("active:")", R"(":2")", R"("")",
                           R"("02")", R"("2:active")", R"("40000")", "40000", "1.0", "null"})
   {
      CHECK(!valid<psio::json, Status>(bad));
      CHECK(!valid<psio::ijson, Status>(bad));
   }
   // An enum without names accepts numbers only.
   CHECK(!valid<psio::json, Plain>(R"("plain_a")"));
   CHECK(!valid<psio::json, Plain>(R"("1")"));
   CHECK(valid<psio::json, Plain>("1"));
}

TEST_CASE("validate and the checked decode agree on mutated text", "[json][ijson][fuzz]")
{
   Wrapped w;
   w.few   = psio::bounded<std::uint16_t, 2>{7};
   w.tag   = psio::utf8_string<4>{"ab"};
   w.chars = {'h'};
   using V = std::variant<std::uint32_t, std::string, Point, std::uint64_t>;
   using T = std::tuple<Wide, std::vector<V>, std::optional<std::vector<std::uint8_t>>, std::string,
                        double, std::tuple<>, std::vector<std::optional<std::int64_t>>>;
   const T value{{18446744073709551615ull, -9, 3},
                 {V{std::in_place_index<0>, 1u}, V{std::in_place_index<1>, "s\n\xC3\xA9"},
                  V{std::in_place_index<2>, Point{1, -2}}, V{std::in_place_index<3>, 99u}},
                 std::vector<std::uint8_t>{1, 2, 255},
                 "text \"q\" \x01",
                 -1.25e-7,
                 {},
                 {std::nullopt, 5}};
   std::mt19937 rng(12345);
   auto run = [&]<typename Fmt>(Fmt) {
      const std::string text = psio::encode(Fmt{}, value);
      REQUIRE(read<Fmt, T>(text) == value);
      const std::string alphabet = "0123456789\"\\[]{},:.-+eEaAfFnulrtx \n\x01\x7f\xC3\xff";
      std::size_t       accepted = 0;
      for (int round = 0; round < 60000; ++round)
      {
         std::string bad = text;
         const int   edits = 1 + static_cast<int>(rng() % 3);
         for (int e = 0; e < edits; ++e)
         {
            const std::size_t at = rng() % bad.size();
            switch (rng() % 4)
            {
               case 0: bad[at] = alphabet[rng() % alphabet.size()]; break;
               case 1: bad.erase(at, 1 + rng() % 3); break;
               case 2: bad.insert(at, 1, alphabet[rng() % alphabet.size()]); break;
               case 3: bad.resize(at); break;
            }
            if (bad.empty()) bad = "x";
         }
         const std::span<const char> in{bad};
         const bool ok = psio::validate<T>(Fmt{}, in).ok();
         T          out;
         const bool decoded = psio::decode_json_checked(Fmt{}, in, out).ok();
         REQUIRE(ok == decoded);
         if (ok)
         {
            ++accepted;
            // Whatever is accepted re-encodes and reads back as itself.
            std::string again;
            REQUIRE(psio::encode_json_checked(Fmt{}, out, again).ok());
            REQUIRE(read<Fmt, T>(again) == out);
         }
      }
      CHECK(accepted > 0);
   };
   run(psio::json{});
   run(psio::ijson{});
}
