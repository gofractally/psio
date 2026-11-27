// pjson: what a view, a decode and a JSON conversion do on any bytes, and
// what JSON text keeps (gofractally/psio#17; docs/annotations.md 7.2 "On
// bytes that are not a frame"; docs/pjson-spec.md §3, §7, §9).
//
//   * a pjson_view never reads outside its bytes and does bounded work; a
//     member it cannot place is a failure (std::runtime_error), not a read;
//   * what pjson::validate accepts, pjson::decode decodes, pjson_to_json
//     converts and a view walks to the end — except a NaN or an infinity,
//     which JSON does not have (§7.3) and pjson_to_json refuses;
//   * what the validator refuses, decode and pjson_to_json refuse;
//   * a JSON number keeps its value or is an error: never zero, never a
//     rounded double, never `inf` / `null`.
//
// Also built with AddressSanitizer + UndefinedBehaviorSanitizer.

#include <psio/pjson.hpp>
#include <psio/pjson_json.hpp>
#include <psio/pjson_to_json.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace
{
   using Bytes = std::vector<std::uint8_t>;

   struct PcPoint
   {
      std::uint32_t x = 0;
      std::uint32_t y = 0;
      friend bool   operator==(const PcPoint&, const PcPoint&) = default;
      PSIO_REFLECT(PcPoint, x, y)
   };
   struct PcNarrow
   {
      std::uint8_t  x = 0;
      std::uint32_t y = 0;
      PSIO_REFLECT(PcNarrow, x, y)
   };
   struct PcRecord
   {
      std::uint64_t              id  = 0;
      std::int64_t               neg = 0;
      double                     d   = 0;
      bool                       b   = false;
      std::string                s;
      std::optional<std::uint32_t> o;
      std::vector<std::uint16_t> nums;
      std::vector<double>        reals;
      std::vector<PcPoint>       rows;
      std::vector<std::string>   strs;
      std::vector<std::uint8_t>  bytes;
      PcPoint                    nested;
      friend bool                operator==(const PcRecord&, const PcRecord&) = default;
      PSIO_REFLECT(PcRecord, id, neg, d, b, s, o, nums, reals, rows, strs, bytes, nested)
   };
   PcRecord record()
   {
      return PcRecord{18446744073709551615ull, -9223372036854775807ll - 1, 0.1, true, "h\xc3\xa9llo", 7,
                      {1, 2, 3}, {0.5, 1e300}, {{1, 2}, {3, 4}, {5, 6}}, {"a", "bb", ""}, {9, 8}, {5, 6}};
   }

   // Everything a view reaches. Returns false when the walk met a float
   // that is NaN or infinite (which JSON cannot carry).
   struct walker
   {
      const std::uint8_t* begin  = nullptr;
      const std::uint8_t* end    = nullptr;
      std::size_t         budget = 0;
      bool                inside = true;
      bool                wide   = false;  // met an integer of more than 127 bits
      std::uint64_t       hash   = 1469598103934665603ull;

      void mix(std::uint64_t v) { hash = (hash ^ v) * 1099511628211ull; }
      void span(const void* p, std::size_t n)
      {
         const auto* b = static_cast<const std::uint8_t*>(p);
         if (n && (b < begin || b + n > end))
            inside = false;
         else
            for (std::size_t i = 0; i < n; ++i)
               mix(b[i]);
      }
      void value(psio::pjson_view v, int depth)
      {
         using K = psio::pjson_view::kind;
         if (depth > 80 || budget == 0)
            return;
         --budget;
         switch (v.type())
         {
            case K::boolean:
               mix(v.as_bool());
               break;
            case K::integer:
               // (2^127 and above: a valid integer the signed accessors and
               // the dynamic value cannot hold; as_uint128 reads it.)
               try
               {
                  mix(static_cast<std::uint64_t>(v.as_int128()));
                  (void)v.as_number();
               }
               catch (const std::out_of_range&)
               {
                  wide = true;
               }
               break;
            case K::decimal:
               mix(static_cast<std::uint64_t>(v.as_number().mantissa));
               break;
            case K::floating:
            {
               const double d = v.as_double();
               std::uint64_t bits;
               std::memcpy(&bits, &d, 8);
               mix(bits);
               if (std::isfinite(d))
                  (void)v.as_number();
               break;
            }
            case K::string:
            {
               const auto s = v.as_string();
               span(s.data(), s.size());
               break;
            }
            case K::bytes:
            {
               const auto s = v.as_bytes();
               span(s.data(), s.size());
               break;
            }
            case K::array:
            {
               const std::size_t n = v.count();
               mix(n);
               // A list never announces more than its bytes can hold.
               REQUIRE(n <= static_cast<std::size_t>(end - begin));
               v.for_each_element([&](psio::pjson_view c) { value(c, depth + 1); });
               if (n)
                  value(v.at(n - 1), depth + 1);
               break;
            }
            case K::object:
            {
               const std::size_t n = v.count();
               mix(n);
               REQUIRE(n <= static_cast<std::size_t>(end - begin));
               v.for_each_field([&](std::string_view k, psio::pjson_view c) {
                  span(k.data(), k.size());
                  value(c, depth + 1);
               });
               (void)v.find("id");
               (void)v.find("no such key");
               break;
            }
            default:
               break;
         }
      }
   };

   struct tally
   {
      std::size_t tried = 0, accepted = 0, view_failed = 0;
   };

   // The bytes in a heap block of exactly their size.
   void probe(const Bytes& bytes, tally& t)
   {
      std::unique_ptr<std::uint8_t[]> exact(new std::uint8_t[bytes.size() ? bytes.size() : 1]);
      if (!bytes.empty())
         std::memcpy(exact.get(), bytes.data(), bytes.size());
      const std::span<const std::uint8_t> b{exact.get(), bytes.size()};
      ++t.tried;
      const bool valid = psio::pjson::validate(b);

      // The view, over the bytes whatever they are.
      walker w;
      w.begin  = b.data();
      w.end    = b.data() + b.size();
      w.budget = 20000;
      bool view_ok = true;
      try
      {
         w.value(psio::pjson_view{b}, 0);
      }
      catch (const std::exception&)
      {
         view_ok = false;
      }
      REQUIRE(w.inside);

      // Decode and JSON: exactly what the validator says.
      // (The dynamic value's number is a signed 128-bit mantissa: a frame
      // holding a wider integer is valid and has no pjson_value.)
      const auto decoded = psio::pjson::try_decode(b);
      if (!(valid && view_ok && w.wide))
         REQUIRE(decoded.has_value() == valid);
      bool        json_ok = true;
      std::string text;
      try
      {
         text = psio::pjson_to_json(b);
      }
      catch (const std::exception& e)
      {
         json_ok = false;
         text    = e.what();
      }
      if (!valid)
      {
         REQUIRE_FALSE(json_ok);
         t.view_failed += !view_ok;
         return;
      }
      ++t.accepted;
      INFO("accepted frame; view_ok=" << view_ok << " json: " << text);
      // A validated frame is walked to the end.
      REQUIRE(view_ok);
      // ... and converts to JSON, unless it holds a NaN or an infinity.
      if (!json_ok)
         REQUIRE(text == "pjson_to_json: NaN and infinity have no JSON form");
   }

   std::size_t rounds()
   {
      if (const char* text = std::getenv("PSIO_PJSON_CONTRACT_ROUNDS"))
         return static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
      return 3000;
   }

   tally mutations(const Bytes& good, std::uint64_t seed)
   {
      tally t;
      probe(good, t);
      REQUIRE(t.accepted == 1);
      for (std::size_t n = 0; n < good.size(); ++n)
         probe(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)), t);
      for (std::size_t i = 0; i < good.size(); ++i)
         for (int v : {0, 0xff, good[i] + 1, good[i] - 1, 0x7f})
         {
            Bytes bad = good;
            bad[i]    = static_cast<std::uint8_t>(v);
            probe(bad, t);
         }
      std::mt19937_64 rng{seed};
      const auto      below = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
      for (std::size_t r = 0; r < rounds(); ++r)
      {
         Bytes bad = good;
         for (std::size_t e = 1 + below(3); e > 0 && !bad.empty(); --e)
            switch (below(5))
            {
               case 0:
                  bad[below(bad.size())] = static_cast<std::uint8_t>(rng());
                  break;
               case 1:
                  bad[below(bad.size())] = static_cast<std::uint8_t>(below(bad.size() + 2));
                  break;
               case 2:
                  bad.erase(bad.begin() + static_cast<std::ptrdiff_t>(below(bad.size())));
                  break;
               case 3:
                  bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(below(bad.size() + 1)),
                             static_cast<std::uint8_t>(rng()));
                  break;
               default:
                  bad.resize(below(bad.size() + 1));
                  break;
            }
         probe(bad, t);
      }
      return t;
   }

   Bytes hex(std::string_view text)
   {
      Bytes out;
      for (std::size_t i = 0; i + 1 < text.size(); i += 2)
         out.push_back(static_cast<std::uint8_t>(std::stoi(std::string{text.substr(i, 2)}, nullptr, 16)));
      return out;
   }
}  // namespace

TEST_CASE("pjson view: as_number of a typed array element does not recurse", "[pjson][contract]")
{
   // The frame on which as_number() and as_int128() called each other
   // without end: {"values": <typed u32 array [1, 256, 65536]>}.
   const Bytes frame = hex("c00076616c756573b70100000000010000000001000300af00060100");
   REQUIRE(psio::pjson::validate(frame));
   const psio::pjson_view v{std::span<const std::uint8_t>{frame}};
   const auto             values = v["values"];
   REQUIRE(values.is_typed_array());
   CHECK(values.at(2).as_number() == psio::pjson_number{65536, 0});
   CHECK(values.at(1).as_int128() == 256);
   CHECK(psio::pjson_to_json(frame) == "{\"values\":[1,256,65536]}");
   // A u64 element above 2^63 is its value.
   const Bytes big = hex("b8ffffffffffffffff0100");
   REQUIRE(psio::pjson::validate(big));
   CHECK(psio::pjson_view{std::span<const std::uint8_t>{big}}.at(0).as_int128() ==
         static_cast<__int128>(18446744073709551615ull));
}

TEST_CASE("pjson: an integer of 128 bits of magnitude decodes to itself", "[pjson][contract]")
{
   // To 127 bits the dynamic value holds it.
   for (const char* frame : {"4fffffffffffffffffffffffffffffff7f", "5f00000000000000000000000000000080",
                             "47ffffffffffffffff"})
   {
      const Bytes b = hex(frame);
      REQUIRE(psio::pjson::validate(b));
      INFO(frame);
      CHECK(psio::pjson::encode(psio::pjson::decode(b)) == b);
   }
   // Above that it is still a value: the view and the JSON text have it
   // exactly, and the dynamic value refuses it instead of wrapping it.
   const Bytes max = hex("4fffffffffffffffffffffffffffffffff");
   REQUIRE(psio::pjson::validate(max));
   const psio::pjson_view v{std::span<const std::uint8_t>{max}};
   CHECK(v.as_uint128() == ~static_cast<unsigned __int128>(0));
   CHECK_THROWS_AS(v.as_int128(), std::out_of_range);
   CHECK(psio::pjson_to_json(max) == "340282366920938463463374607431768211455");
   CHECK_FALSE(psio::pjson::try_decode(max).has_value());
   CHECK_FALSE(psio::pjson::try_decode(hex("5f01000000000000000000000000000080")).has_value());
}

TEST_CASE("pjson: null with a nonzero low nibble is not a value", "[pjson][contract]")
{
   for (const char* frame : {"01", "0f", "b00001000100"})
   {
      const Bytes b = hex(frame);
      CHECK_FALSE(psio::pjson::validate(b));
      CHECK_FALSE(psio::pjson::try_decode(b).has_value());
      CHECK_THROWS(psio::pjson_to_json(b));
   }
   CHECK(psio::pjson::validate(hex("00")));
}

TEST_CASE("pjson typed: every unsigned width reads back; a value the member cannot hold is an error",
          "[pjson][contract]")
{
   const PcRecord r     = record();
   const Bytes    bytes = psio::from_struct(r);
   REQUIRE(psio::pjson::validate(bytes));
   const auto back = psio::view<PcRecord, psio::pjson_format>::from_pjson(
                        psio::pjson_view{std::span<const std::uint8_t>{bytes}})
                        .to_struct();
   CHECK(back == r);
   // The same record as JSON: rows and typed arrays included.
   const std::string json = psio::pjson_to_json(bytes);
   CHECK(json.find("\"id\":18446744073709551615") != std::string::npos);
   CHECK(json.find("\"rows\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4},{\"x\":5,\"y\":6}]") != std::string::npos);
   CHECK(json.find("\"nums\":[1,2,3]") != std::string::npos);
   CHECK(json.find("\"bytes\":\"CQg=\"") != std::string::npos);
   CHECK(json.find("\"s\":\"h\xc3\xa9llo\"") != std::string::npos);
   // {"x": 300, "y": 2} read as a point whose x is a uint8_t.
   const Bytes wide = psio::from_struct(PcPoint{300, 2});
   CHECK_THROWS(psio::view<PcNarrow, psio::pjson_format>::from_pjson(
                   psio::pjson_view{std::span<const std::uint8_t>{wide}})
                   .to_struct());
}

TEST_CASE("pjson: a native minus zero keeps its sign", "[pjson][contract]")
{
   const Bytes b = psio::pjson::encode(psio::pjson_value{-0.0});
   CHECK(b == hex("610080"));
   const auto back = psio::pjson::decode(b);
   REQUIRE(back.holds<double>());
   CHECK(std::signbit(std::get<double>(back.v)));
   CHECK(psio::pjson_to_json(b) == "-0");
   CHECK(psio::pjson::encode(psio::pjson_value{0.0}) == hex("20"));
}

TEST_CASE("pjson: NaN and infinity have no JSON form", "[pjson][contract]")
{
   for (const char* frame : {"63000000000000f07f", "63000000000000f8ff", "ba000000000000f07f0100"})
   {
      const Bytes b = hex(frame);
      REQUIRE(psio::pjson::validate(b));
      CHECK_THROWS_WITH(psio::pjson_to_json(b), "pjson_to_json: NaN and infinity have no JSON form");
   }
}

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
TEST_CASE("pjson from JSON: a number keeps its value or is an error", "[pjson][contract][json]")
{
   const auto round = [](const std::string& json) { return psio::pjson_to_json(psio::pjson_json::from_json(json)); };
   // Integers to 128 bits of magnitude.
   CHECK(round("18446744073709551615") == "18446744073709551615");
   CHECK(round("340282366920938463463374607431768211455") == "340282366920938463463374607431768211455");
   CHECK(round("-340282366920938463463374607431768211455") == "-340282366920938463463374607431768211455");
   // More digits than 128 bits hold: an error, not zero.
   CHECK_THROWS(psio::pjson_json::from_json("340282366920938463463374607431768211456"));
   CHECK_THROWS(psio::pjson_json::from_json("[123456789012345678901234567890123456789012345]"));
   // ... unless they are trailing zeros: 10^40 is 1e40.
   CHECK(round("1" + std::string(40, '0')) == "1" + std::string(40, '0'));
   CHECK(round("2" + std::string(45, '0')) == "2" + std::string(45, '0'));
   CHECK(round("2e45") == "2e45");
   CHECK(round("34" + std::string(38, '0')) == "34" + std::string(38, '0'));
   // What binary64 cannot hold is a decimal, exactly.
   CHECK(round("3.14159265358979323846264338327950288") == "3.14159265358979323846264338327950288");
   CHECK(round("1e400") == "1e400");
   CHECK(round("-1.5e-400") == "-1.5e-400");
   CHECK(round("0.1") == "0.1");
   CHECK(round("0.30000000000000004") == "0.30000000000000004");
   CHECK(round("12345.678901234567") == "12345.678901234567");
   // Minus zero.
   CHECK(round("-0") == "-0");
   CHECK(round("-0.0") == "-0");
   CHECK(psio::pjson_json::from_json("[-0e5]") == hex("b000610080000100"));
   // §7.1: trailing zeros and exponent notation are not kept; the value is.
   CHECK(round("100.00") == "100");
   CHECK(round("1.0") == "1");
   CHECK(round("1.5e10") == "15000000000");
   // A document is any value (§6).
   CHECK(round("5") == "5");
   CHECK(round("\"x\"") == "\"x\"");
   CHECK(round("null") == "null");
   CHECK(round("true") == "true");
   CHECK(round(" 2.5 ") == "2.5");
   // Order, duplicates and text are kept.
   CHECK(round("{\"z\":1,\"a\":2,\"a\":3}") == "{\"z\":1,\"a\":2,\"a\":3}");
   CHECK(round("\"\\u4e2d caf\\u00e9 \xc3\xa9\"") == "\"\\u4e2d caf\\u00e9 \xc3\xa9\"");
}
#endif

TEST_CASE("pjson: a view stays inside any bytes; validate, decode and JSON agree", "[pjson][contract][fuzz]")
{
   std::vector<Bytes> seeds;
   seeds.push_back(psio::from_struct(record()));
   seeds.push_back(psio::from_struct(PcPoint{1, 2}));
   for (const char* frame :
        {"c00076616c756573b70100000000010000000001000300af00060100",                       // typed array in an object
         "c10002000000010100000111e57879212200012324000100040200",                         // row array
         "b0002122230001020300", "b601000200030003000", "c000612161221f1f000102010200",    // arrays, duplicate keys
         "4fffffffffffffffffffffffffffffffff", "5fffffffffffffffffffffffffffffffff",       // 128-bit magnitudes
         "610080", "63000000000000f03f", "620000803f", "7002600c", "701d610c",             // floats, decimals
         "9068c3a9", "915c7530306539", "a00908", "00", "11", "25", "35"})
   {
      Bytes b = hex(frame);
      if (psio::pjson::validate(b))
         seeds.push_back(std::move(b));
   }
   // A long key (the escape of §5.4) and a deep nesting.
   {
      psio::pjson_object o;
      o.emplace_back(std::string(300, 'k'), psio::pjson_value{std::int64_t{1}});
      o.emplace_back("short", psio::pjson_value{std::string("v")});
      seeds.push_back(psio::pjson::encode(psio::pjson_value{o}));
      psio::pjson_value deep{std::int64_t{7}};
      for (int i = 0; i < 20; ++i)
         deep = psio::pjson_value{psio::pjson_array{deep}};
      seeds.push_back(psio::pjson::encode(deep));
   }
   REQUIRE(seeds.size() > 15);
   tally total;
   std::uint64_t seed = 17;
   for (const Bytes& s : seeds)
   {
      const tally t = mutations(s, seed++);
      total.tried += t.tried;
      total.accepted += t.accepted;
      total.view_failed += t.view_failed;
   }
   // Most mutations are not a value, and a view of those mostly fails.
   CHECK(total.accepted < total.tried);
   CHECK(total.accepted > seeds.size());
   CHECK(total.view_failed > total.tried / 20);
}
