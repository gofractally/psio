// Phase 14c — transcode free function.
//
// Exercises the compile-time T path across every pair of currently-
// implemented formats (ssz, pssz16, pssz32, json, frac16, frac32, bin).

#include <psio/bin.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/transcode.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct XcodePoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(XcodePoint, x, y)

struct XcodePerson
{
   std::string                name;
   std::int32_t               age;
   std::vector<std::uint32_t> scores;
};
PSIO_REFLECT(XcodePerson, name, age, scores)

TEST_CASE("transcode<T> ssz → json produces equivalent JSON",
          "[transcode][ssz][json]")
{
   XcodePoint p{7, 9};
   auto       src = psio::encode(psio::ssz{}, p);

   auto json_out = psio::transcode<XcodePoint>(
      psio::ssz{}, std::span<const char>{src}, psio::json{});

   REQUIRE(json_out == R"({"x":7,"y":9})");
}

TEST_CASE("transcode<T> json → ssz round-trips through T",
          "[transcode][json][ssz]")
{
   std::string src_json = R"({"x":11,"y":-5})";
   auto        ssz_out  = psio::transcode<XcodePoint>(
      psio::json{}, std::span<const char>{src_json}, psio::ssz{});

   auto back = psio::decode<XcodePoint>(psio::ssz{},
                                          std::span<const char>{ssz_out});
   REQUIRE(back.x == 11);
   REQUIRE(back.y == -5);
}

TEST_CASE("transcode<T> frac → pssz preserves the value",
          "[transcode][frac][pssz]")
{
   XcodePerson pp{"carol", 40, {9, 8, 7}};
   auto        src = psio::encode(psio::frac32{}, pp);
   auto        dst = psio::transcode<XcodePerson>(
      psio::frac32{}, std::span<const char>{src}, psio::pssz16{});

   auto back =
      psio::decode<XcodePerson>(psio::pssz16{}, std::span<const char>{dst});
   REQUIRE(back.name == "carol");
   REQUIRE(back.age == 40);
   REQUIRE(back.scores == std::vector<std::uint32_t>{9, 8, 7});
}

TEST_CASE("transcode<T> through every format pair preserves the record",
          "[transcode][cross]")
{
   XcodePerson pp{"dave", 32, {1, 2, 3}};

   // Original SSZ encoding.
   auto ssz_src = psio::encode(psio::ssz{}, pp);

   // SSZ → bin → frac32 → pssz32 → json and back to a value.
   auto bin_bytes = psio::transcode<XcodePerson>(
      psio::ssz{}, std::span<const char>{ssz_src}, psio::bin{});
   auto frac_bytes = psio::transcode<XcodePerson>(
      psio::bin{}, std::span<const char>{bin_bytes}, psio::frac32{});
   auto pssz_bytes = psio::transcode<XcodePerson>(
      psio::frac32{}, std::span<const char>{frac_bytes}, psio::pssz32{});
   auto json_str = psio::transcode<XcodePerson>(
      psio::pssz32{}, std::span<const char>{pssz_bytes}, psio::json{});

   auto back = psio::decode<XcodePerson>(psio::json{},
                                           std::span<const char>{json_str});
   REQUIRE(back.name == "dave");
   REQUIRE(back.age == 32);
   REQUIRE(back.scores == std::vector<std::uint32_t>{1, 2, 3});
}

TEST_CASE("transcode_via_json round-trips a dynamic value through itself",
          "[transcode][dynamic]")
{
   std::string src = R"({"x":42,"y":99})";
   auto        sc  = psio::schema_of<XcodePoint>();
   auto        out = psio::transcode_via_json(sc, std::span<const char>{src});
   REQUIRE(out == R"({"x":42,"y":99})");
}

TEST_CASE("transcode_ssz_to_json converts SSZ → JSON via runtime schema",
          "[transcode][dynamic][ssz][json]")
{
   XcodePoint p{101, -7};
   auto       ssz_bytes = psio::encode(psio::ssz{}, p);

   auto sc       = psio::schema_of<XcodePoint>();
   auto json_str = psio::transcode_ssz_to_json(
      sc, std::span<const char>{ssz_bytes});

   REQUIRE(json_str == R"({"x":101,"y":-7})");
}

TEST_CASE("transcode_json_to_ssz converts JSON → SSZ via runtime schema",
          "[transcode][dynamic][ssz][json]")
{
   std::string src   = R"({"x":55,"y":-33})";
   auto        sc    = psio::schema_of<XcodePoint>();
   auto        bytes = psio::transcode_json_to_ssz(
      sc, std::span<const char>{src});

   auto back = psio::decode<XcodePoint>(psio::ssz{},
                                          std::span<const char>{bytes});
   REQUIRE(back.x == 55);
   REQUIRE(back.y == -33);
}

TEST_CASE("runtime-schema SSZ↔JSON round-trips a record with variable fields",
          "[transcode][dynamic][ssz][json][variable]")
{
   XcodePerson pp{"eve", 29, {7, 11, 13}};

   auto ssz_bytes = psio::encode(psio::ssz{}, pp);
   auto sc        = psio::schema_of<XcodePerson>();
   auto as_json   = psio::transcode_ssz_to_json(
      sc, std::span<const char>{ssz_bytes});
   auto ssz_again = psio::transcode_json_to_ssz(
      sc, std::span<const char>{as_json});

   auto back =
      psio::decode<XcodePerson>(psio::ssz{},
                                  std::span<const char>{ssz_again});
   REQUIRE(back.name == "eve");
   REQUIRE(back.age == 29);
   REQUIRE(back.scores == std::vector<std::uint32_t>{7, 11, 13});
}

TEST_CASE("generic psio::transcode(ssz → json) dispatches via CPOs",
          "[transcode][cpo][dynamic]")
{
   XcodePoint p{12, 34};
   auto       ssz_bytes = psio::encode(psio::ssz{}, p);
   auto       sc        = psio::schema_of<XcodePoint>();

   auto js = psio::transcode(psio::ssz{}, sc,
                              std::span<const char>{ssz_bytes},
                              psio::json{});
   REQUIRE(js == R"({"x":12,"y":34})");
}

TEST_CASE("generic psio::transcode(json → ssz) dispatches via CPOs",
          "[transcode][cpo][dynamic]")
{
   std::string src = R"({"x":44,"y":-11})";
   auto        sc  = psio::schema_of<XcodePoint>();

   auto bytes = psio::transcode(psio::json{}, sc,
                                 std::span<const char>{src}, psio::ssz{});
   auto back  = psio::decode<XcodePoint>(psio::ssz{},
                                           std::span<const char>{bytes});
   REQUIRE(back.x == 44);
   REQUIRE(back.y == -11);
}

TEST_CASE("generic transcode chains ssz → json → ssz via CPOs",
          "[transcode][cpo][dynamic][variable]")
{
   XcodePerson pp{"frank", 41, {100, 200}};
   auto        src = psio::encode(psio::ssz{}, pp);
   auto        sc  = psio::schema_of<XcodePerson>();

   auto js = psio::transcode(psio::ssz{}, sc,
                              std::span<const char>{src}, psio::json{});
   auto back_bytes = psio::transcode(psio::json{}, sc,
                                       std::span<const char>{js},
                                       psio::ssz{});
   auto back =
      psio::decode<XcodePerson>(psio::ssz{},
                                  std::span<const char>{back_bytes});

   REQUIRE(back.name == "frank");
   REQUIRE(back.age == 41);
   REQUIRE(back.scores == std::vector<std::uint32_t>{100, 200});
}

TEST_CASE("generic transcode works for bin", "[transcode][cpo][dynamic][bin]")
{
   XcodePerson pp{"grace", 52, {10, 20, 30}};
   auto        bin_bytes = psio::encode(psio::bin{}, pp);
   auto        sc        = psio::schema_of<XcodePerson>();

   auto as_json  = psio::transcode(psio::bin{}, sc,
                                    std::span<const char>{bin_bytes},
                                    psio::json{});
   auto as_ssz   = psio::transcode(psio::json{}, sc,
                                    std::span<const char>{as_json},
                                    psio::ssz{});
   auto as_bin2  = psio::transcode(psio::ssz{}, sc,
                                    std::span<const char>{as_ssz},
                                    psio::bin{});

   auto back = psio::decode<XcodePerson>(psio::bin{},
                                           std::span<const char>{as_bin2});
   REQUIRE(back.name == "grace");
   REQUIRE(back.age == 52);
   REQUIRE(back.scores == std::vector<std::uint32_t>{10, 20, 30});
}

TEST_CASE("generic transcode works for frac32", "[transcode][cpo][dynamic][frac]")
{
   XcodePerson pp{"heidi", 28, {1, 2}};
   auto        sc = psio::schema_of<XcodePerson>();

   // frac32 → json → frac32 round-trip.
   auto frac_bytes = psio::encode(psio::frac32{}, pp);
   auto json_str   = psio::transcode(psio::frac32{}, sc,
                                      std::span<const char>{frac_bytes},
                                      psio::json{});
   auto frac_back  = psio::transcode(psio::json{}, sc,
                                      std::span<const char>{json_str},
                                      psio::frac32{});

   auto back = psio::decode<XcodePerson>(
      psio::frac32{}, std::span<const char>{frac_back});
   REQUIRE(back.name == "heidi");
   REQUIRE(back.age == 28);
   REQUIRE(back.scores == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("generic transcode chains every format pair via dynamic",
          "[transcode][cpo][dynamic][cross]")
{
   XcodePerson pp{"ivan", 60, {99}};
   auto        sc = psio::schema_of<XcodePerson>();

   // Start with pssz32 bytes, route through json, ssz, bin, frac32,
   // and back to pssz32 — every format's dynamic codec in the mix.
   auto a = psio::encode(psio::pssz32{}, pp);
   auto b = psio::transcode(psio::pssz32{}, sc, std::span<const char>{a},
                             psio::json{});
   auto c = psio::transcode(psio::json{}, sc, std::span<const char>{b},
                             psio::ssz{});
   auto d = psio::transcode(psio::ssz{}, sc, std::span<const char>{c},
                             psio::bin{});
   auto e = psio::transcode(psio::bin{}, sc, std::span<const char>{d},
                             psio::frac32{});
   auto f = psio::transcode(psio::frac32{}, sc, std::span<const char>{e},
                             psio::pssz32{});

   auto back = psio::decode<XcodePerson>(psio::pssz32{},
                                           std::span<const char>{f});
   REQUIRE(back.name == "ivan");
   REQUIRE(back.age == 60);
   REQUIRE(back.scores == std::vector<std::uint32_t>{99});
}
