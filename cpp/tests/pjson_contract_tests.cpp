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

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
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
      bool                bytes  = false;  // met a bytes value (a string once it is JSON text)
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
               bytes = true;
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
      std::size_t tried = 0, accepted = 0, view_failed = 0, canonical = 0;
   };

   // The bytes in a heap block of exactly their size.
   void probe(const Bytes& bytes, tally& t)
   {
      std::unique_ptr<std::uint8_t[]> exact(new std::uint8_t[bytes.size() ? bytes.size() : 1]);
      if (!bytes.empty())
         std::memcpy(exact.get(), bytes.data(), bytes.size());
      const std::span<const std::uint8_t> b{exact.get(), bytes.size()};
      ++t.tried;
      // `valid`: a well-formed frame (the lazy tag). `canonical`: the one
      // encoding of its value (psio::pjson).
      const bool valid     = psio::pjson_lazy::validate(b);
      const bool canonical = psio::pjson::validate(b);
      REQUIRE((!canonical || valid));

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
      const auto decoded = psio::pjson_lazy::try_decode(b);
      if (!(valid && view_ok && w.wide))
      {
         REQUIRE(decoded.has_value() == valid);
         REQUIRE(psio::pjson::try_decode(b).has_value() == canonical);
      }
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

      // The canonical form of a well-formed frame: it exists unless the
      // frame holds text that is not text; it is canonical; it is its own
      // canonical form; and it is the frame itself exactly when the frame
      // is canonical.
      Bytes made;
      bool  has_form = true;
      try
      {
         made = psio::pjson::canonicalize(b);
      }
      catch (const std::exception&)
      {
         has_form = false;
      }
      if (!has_form)
      {
         REQUIRE_FALSE(canonical);
         return;
      }
      t.canonical += canonical;
      REQUIRE(psio::pjson::validate(made));
      REQUIRE(psio::pjson::canonicalize(made) == made);
      REQUIRE((made == bytes) == canonical);
#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
      // ... and it is the same VALUE: the JSON text of the frame, read
      // back by the canonical transcoder, is the JSON text of its
      // canonical form (and, with no bytes in it, its very bytes).
      if (json_ok)
      {
         const Bytes again = psio::pjson_json::from_json(text);
         REQUIRE(psio::pjson_to_json(again) == psio::pjson_to_json(made));
         if (!w.bytes)
            REQUIRE(again == made);
      }
#endif
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
   // (The binary64 forms are well-formed; the canonical ones are binary16.)
   for (const char* frame : {"63000000000000f07f", "63000000000000f8ff", "ba000000000000f07f0100", "61007c", "6100fc",
                             "61007e"})
   {
      const Bytes b = hex(frame);
      REQUIRE(psio::pjson_lazy::validate(b));
      CHECK(psio::pjson::validate(b) == (b.size() == 3));
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
   CHECK(round("1" + std::string(40, '0')) == "1e40");
   CHECK(round("2" + std::string(45, '0')) == "2e45");
   CHECK(round("2e45") == "2e45");
   CHECK(round("34" + std::string(38, '0')) == "3.4e39");
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
   // (An array of numbers is a typed array: -0 in binary32.)
   CHECK(psio::pjson_json::from_json("[-0e5]") == hex("b9000000800100"));
   CHECK(psio::pjson_json::from_json_lazy("[-0e5]") == hex("b000610080000100"));
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
   // (The canonical form holds the text; the lazy transcode holds it as
   // it was spelled.)
   CHECK(round("\"\\u4e2d caf\\u00e9 \xc3\xa9\"") == "\"\xe4\xb8\xad caf\xc3\xa9 \xc3\xa9\"");
   CHECK(psio::pjson_to_json(psio::pjson_json::from_json_lazy("\"\\u4e2d caf\\u00e9 \xc3\xa9\"")) ==
         "\"\\u4e2d caf\\u00e9 \xc3\xa9\"");
}

// The three guarantees of gofractally/psio#17 (comment 7208563552). The
// same documents are in rust/psio/tests/pjson_contract.rs.
TEST_CASE("pjson from JSON and back: values, order and duplicate names of full JSON are kept",
          "[pjson][contract][json]")
{
   const auto round = [](const std::string& json) { return psio::pjson_to_json(psio::pjson_json::from_json(json)); };
   const std::string in  = R"j({"z":null,"a":[true,false,-0,0,1e400,0.1,18446744073709551616,-1.5e-400,"é中","1e5"],"a":{"k":1,"k":"1"},"":[],"o":{}})j";
   const std::string out = R"j({"z":null,"a":[true,false,-0,0,1e400,0.1,18446744073709551616,-1.5e-400,"é中","1e5"],"a":{"k":1,"k":"1"},"":[],"o":{}})j";
   CHECK(round(in) == out);
   CHECK(round(out) == out);
}

TEST_CASE("pjson from I-JSON and back: nothing is promoted; a string stays a string", "[pjson][contract][json]")
{
   const auto round = [](const std::string& json) { return psio::pjson_to_json(psio::pjson_json::from_json(json)); };
   // An I-JSON document: no repeated names, numbers binary64 holds exactly,
   // large integers already strings. It comes back as the same text.
   const std::string doc = R"j({"id":"9007199254740993","n":9007199254740991,"m":-9007199254740991,"x":0.1,"s":"1e400","t":"true","u":"null","e":"","list":["0x10","-0",1.5,false,null],"nested":{"k":"340282366920938463463374607431768211455"}})j";
   CHECK(round(doc) == doc);
   const auto bytes = psio::pjson_json::from_json(doc);
   const psio::pjson_view v{bytes.data(), bytes.size()};
   CHECK(v["id"].is_string());
   CHECK(v["s"].is_string());
   CHECK(v["n"].is_number());
}
#endif

// ── One value, one encoding (gofractally/psio#17, stage S2) ────────────────
namespace
{
   template <typename T>
   struct Box
   {
      T v{};
      PSIO_REFLECT(Box, v)
   };
   // The frame of the member `v` of a one-member record: what the typed
   // encoder writes for a T.
   template <typename T>
   Bytes typed(const T& value)
   {
      const Bytes whole = psio::from_struct(Box<T>{value});
      REQUIRE(psio::pjson::validate(whole));
      const auto member = psio::pjson_view{std::span<const std::uint8_t>{whole}}["v"].raw();
      return Bytes(member.begin(), member.end());
   }
   template <typename T>
   T typed_back(const Bytes& member)
   {
      // {"v": member}
      Bytes whole{0xc0, 0x00, 0x76};
      whole.insert(whole.end(), member.begin(), member.end());
      whole.insert(whole.end(), {psio::pjson_detail::key_hash8("v"), 0x00, 0x01, 0x01, 0x00});
      REQUIRE(psio::pjson_lazy::validate(whole));
      return psio::view<Box<T>, psio::pjson_format>::from_pjson(psio::pjson_view{std::span<const std::uint8_t>{whole}})
         .to_struct()
         .v;
   }
   struct PcWide
   {
      std::uint64_t                             id = 0;
      std::int64_t                              neg = 0;
      double                                    d = 0;
      float                                     f = 0;
      bool                                      b = false;
      std::string                               s;
      std::optional<std::uint32_t>              o;
      std::vector<std::uint16_t>                u16s;
      std::vector<std::int32_t>                 i32s;
      std::vector<double>                       ds;
      std::vector<float>                        fs;
      std::vector<PcPoint>                      rows;
      std::vector<std::string>                  strs;
      std::vector<std::vector<std::uint32_t>>   nested;
      PcPoint                                   point;
      friend bool operator==(const PcWide&, const PcWide&) = default;
      PSIO_REFLECT(PcWide, id, neg, d, f, b, s, o, u16s, i32s, ds, fs, rows, strs, nested, point)
   };
}  // namespace

TEST_CASE("pjson canonical: 5 is 5, whatever declared it", "[pjson][canonical]")
{
   const Bytes five = hex("25");
   CHECK(typed<std::uint8_t>(5) == five);
   CHECK(typed<std::uint16_t>(5) == five);
   CHECK(typed<std::uint64_t>(5) == five);
   CHECK(typed<std::int32_t>(5) == five);
   CHECK(typed<float>(5.0f) == five);
   CHECK(typed<double>(5.0) == five);
   CHECK(psio::pjson::encode(psio::pjson_value{std::int64_t{5}}) == five);
   CHECK(psio::pjson::encode(psio::pjson_value{5.0}) == five);
   CHECK(typed<std::int64_t>(-5) == hex("35"));
   CHECK(typed<double>(-5.0) == hex("35"));
   // ... and read back as whatever the reader declares.
   CHECK(typed_back<std::uint16_t>(five) == 5);
   CHECK(typed_back<double>(five) == 5.0);
   CHECK(typed_back<float>(five) == 5.0f);
   CHECK(typed_back<std::uint8_t>(typed<double>(200.0)) == 200);
   CHECK_THROWS(typed_back<std::uint8_t>(typed<double>(300.0)));
   CHECK_THROWS(typed_back<std::uint32_t>(typed<double>(0.5)));
   CHECK_THROWS(typed_back<std::uint32_t>(typed<std::int32_t>(-1)));

   // Lists: the form is the values', not the element type's.
   const Bytes one_two_three = hex("b50102030300");
   CHECK(typed<std::vector<std::uint16_t>>({1, 2, 3}) == one_two_three);
   CHECK(typed<std::vector<std::uint64_t>>({1, 2, 3}) == one_two_three);
   CHECK(typed<std::vector<std::int32_t>>({1, 2, 3}) == one_two_three);
   CHECK(typed<std::vector<double>>({1, 2, 3}) == one_two_three);
   CHECK(typed<std::vector<float>>({1, 2, 3}) == one_two_three);
   CHECK(typed_back<std::vector<std::uint64_t>>(one_two_three) == std::vector<std::uint64_t>{1, 2, 3});
   CHECK(typed_back<std::vector<double>>(one_two_three) == std::vector<double>{1, 2, 3});
   CHECK(typed<std::vector<std::int64_t>>({-1, 2}) == hex("b1ff020200"));
   CHECK(typed<std::vector<std::int64_t>>({0, 256}) == hex("b6000000010200"));
   // The empty list has one form.
   const Bytes empty = hex("b0000000");
   CHECK(typed<std::vector<std::uint16_t>>({}) == empty);
   CHECK(typed<std::vector<double>>({}) == empty);
   CHECK(typed<std::vector<PcPoint>>({}) == empty);
   CHECK(typed<std::vector<std::string>>({}) == empty);
   CHECK(typed_back<std::vector<PcPoint>>(empty).empty());
   CHECK(typed_back<std::vector<std::uint16_t>>(empty).empty());
}

TEST_CASE("pjson canonical: a float is the shortest decimal that reads back to it at its width", "[pjson][canonical]")
{
   // DEFAULT CHOSEN, AWAITING OWNER (psio/pjson_number_model.hpp
   // `float_model`): this case is what changes if the reading does.
   REQUIRE(psio::pjson_detail::float_model == psio::pjson_detail::float_reading::declared_width);
   // 0.1 is 0.1 from an f32, an f64 and JSON text.
   const Bytes tenth = hex("700201");
   CHECK(typed<float>(0.1f) == tenth);
   CHECK(typed<double>(0.1) == tenth);
   CHECK(psio::pjson_to_json(tenth) == "0.1");
   CHECK(typed_back<float>(tenth) == 0.1f);
   CHECK(typed_back<double>(tenth) == 0.1);
   // A float with more digits is carried by the narrowest float that
   // carries it, whatever declared it.
   const Bytes third32 = hex("62abaaaa3e");
   CHECK(typed<float>(1.0f / 3) == third32);
   CHECK(typed<double>(0.33333334) == third32);
   CHECK(psio::pjson_to_json(third32) == "0.33333334");
   CHECK(typed_back<float>(third32) == 1.0f / 3);
   CHECK(typed_back<double>(third32) == 0.33333334);          // through its decimal: not a widening cast
   CHECK(typed_back<double>(third32) != static_cast<double>(1.0f / 3));
   CHECK(typed<double>(1.0 / 3) == hex("63555555555555d53f"));
   CHECK(typed_back<double>(hex("63555555555555d53f")) == 1.0 / 3);
   // The double that happens to be exact in binary32 is not the float:
   // docs/pjson-spec.md §15.2 as it stood turned it into 0.1.
   const double widened = static_cast<double>(0.1f);
   CHECK(typed<double>(widened) == hex("63000000a09999b93f"));
   CHECK(typed_back<double>(typed<double>(widened)) == widened);
   CHECK(psio::pjson_to_json(typed<double>(widened)) == "0.10000000149011612");
   // Lists.
   const Bytes tenths = hex("b9cdcccc3dcdcc4c3e0200");
   CHECK(typed<std::vector<float>>({0.1f, 0.2f}) == tenths);
   CHECK(typed<std::vector<double>>({0.1, 0.2}) == tenths);
   CHECK(typed_back<std::vector<double>>(tenths) == std::vector<double>{0.1, 0.2});
   CHECK(typed_back<std::vector<float>>(tenths) == std::vector<float>{0.1f, 0.2f});
   CHECK(typed<std::vector<double>>({0.1, 1.0 / 3}) == hex("ba9a9999999999b93f555555555555d53f0200"));
   // -0, NaN and the infinities are binary16; there is one NaN.
   CHECK(typed<double>(-0.0) == hex("610080"));
   CHECK(typed<float>(-0.0f) == hex("610080"));
   CHECK(typed<double>(std::numeric_limits<double>::quiet_NaN()) == hex("61007e"));
   CHECK(typed<float>(-std::numeric_limits<float>::infinity()) == hex("6100fc"));
   CHECK(std::signbit(typed_back<double>(hex("610080"))));
   CHECK(std::isnan(typed_back<float>(hex("61007e"))));
   // Same-type round trips are exact, across the range.
   std::mt19937_64 rng{7};
   for (int i = 0; i < 20000; ++i)
   {
      const auto   bits = rng();
      const double d    = std::bit_cast<double>(bits);
      const float  f    = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
      if (!std::isnan(d))
         REQUIRE(std::bit_cast<std::uint64_t>(typed_back<double>(typed<double>(d))) == bits);
      if (!std::isnan(f))
         REQUIRE(std::bit_cast<std::uint32_t>(typed_back<float>(typed<float>(f))) == static_cast<std::uint32_t>(bits));
      // ... and a float survives being read as a double and written again.
      if (!std::isnan(f))
         REQUIRE(typed<double>(typed_back<double>(typed<float>(f))) == typed<float>(f));
   }
}

TEST_CASE("pjson canonical: every typed encoding is canonical and reads back", "[pjson][canonical]")
{
   std::mt19937_64 rng{11};
   const auto      below = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
   const auto      number = [&]() -> double {
      switch (below(6))
      {
         case 0: return static_cast<double>(below(300));
         case 1: return -static_cast<double>(below(70000));
         case 2: return static_cast<double>(below(1000)) / 100;
         case 3: return static_cast<double>(static_cast<float>(below(1000)) / 7);
         case 4: return std::bit_cast<double>(rng() & 0x7fefffffffffffffull);
         default: return 1.0 / static_cast<double>(1 + below(9));
      }
   };
   std::size_t typed_arrays = 0, rows = 0;
   for (int round = 0; round < 3000; ++round)
   {
      PcWide r;
      r.id  = rng() >> below(64);
      r.neg = -static_cast<std::int64_t>(rng() >> (1 + below(63)));
      r.d   = number();
      const auto finite = [&] {
         const float f = static_cast<float>(number());
         return std::isfinite(f) ? f : 1.5f;   // (an infinity has no JSON text to compare)
      };
      r.f   = finite();
      r.b   = below(2);
      r.s   = std::string(below(5), 'x') + (below(3) ? "" : "h\xc3\xa9");
      if (below(2)) r.o = static_cast<std::uint32_t>(rng());
      for (std::size_t n = below(5); n; --n) r.u16s.push_back(static_cast<std::uint16_t>(rng() >> (48 + below(16))));
      for (std::size_t n = below(5); n; --n) r.i32s.push_back(static_cast<std::int32_t>(rng()) >> below(32));
      for (std::size_t n = below(5); n; --n) r.ds.push_back(number());
      for (std::size_t n = below(5); n; --n) r.fs.push_back(finite());
      for (std::size_t n = below(4); n; --n) r.rows.push_back(PcPoint{static_cast<std::uint32_t>(below(1000)), static_cast<std::uint32_t>(rng())});
      for (std::size_t n = below(4); n; --n) r.strs.push_back(std::string(below(4), 'a'));
      for (std::size_t n = below(3); n; --n) r.nested.push_back(std::vector<std::uint32_t>(below(3), static_cast<std::uint32_t>(below(70000))));
      r.point = PcPoint{static_cast<std::uint32_t>(below(20)), 7};

      const Bytes bytes = psio::from_struct(r);
      INFO(psio::pjson_to_json(bytes));
      INFO(psio::pjson_to_json(psio::pjson::canonicalize(bytes)));
      REQUIRE(psio::pjson::validate(bytes));
      REQUIRE(psio::pjson::canonicalize(bytes) == bytes);
      const psio::pjson_view view{std::span<const std::uint8_t>{bytes}};
      typed_arrays += view["u16s"].is_typed_array() + view["ds"].is_typed_array();
      rows         += view["rows"].is_row_array();
      const auto back = psio::view<PcWide, psio::pjson_format>::from_pjson(view).to_struct();
      REQUIRE(back == r);
#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
      // The typed encoding is the canonical transcode of its own JSON
      // text: a typed value and the anonymous document that spells it are
      // one value.
      REQUIRE(psio::pjson_json::from_json(psio::pjson_to_json(bytes)) == bytes);
#endif
   }
   CHECK(typed_arrays > 1000);
   CHECK(rows > 1000);
   // A string that is not UTF-8 has no pjson form.
   CHECK_THROWS(psio::from_struct(Box<std::string>{"\xff"}));
}

TEST_CASE("pjson canonical: a well-formed frame that is not the canonical one", "[pjson][canonical]")
{
   for (const auto& [frame, canonical] : std::vector<std::pair<const char*, const char*>>{
           {"410500", "25"},                                  // an integer in more bytes than it needs
           {"700a00", "25"},                                  // a decimal that is an integer
           {"700204", "4064"},                                // 1e2
           {"63000000000000f03f", "21"},                      // a float that is an integer
           {"639a9999999999b93f", "700201"},                  // a float a shorter decimal carries
           {"62cdcccc3d", "700201"},
           {"73ac40f9030f", "62abaaaa3e"},                    // a decimal a shorter float carries
           {"6b000000000000f83f", "701e01"},                  // the notation hint
           {"63000000000000f87f", "61007e"},                  // NaN in binary64
           {"630100000000f8ff7f", "61007e"},                  // a NaN with a payload
           {"915c7530306539", "90c3a9"},                      // a string in escape form
           {"916869", "906869"},
           {"b0002122230001020300", "b50102030300"},          // a generic array of numbers
           {"b60100020003000300", "b50102030300"},            // a typed array wider than its values
           {"b50000", "b0000000"},                            // an empty typed array
           {"b000c00078211100010100000100", "c100010000000111782100000100"},   // one object: a row array
        })
   {
      INFO(frame);
      const Bytes b = hex(frame);
      CHECK(psio::pjson_lazy::validate(b));
      CHECK_FALSE(psio::pjson::validate(b));
      CHECK_FALSE(psio::pjson::try_decode(b).has_value());
      CHECK(psio::pjson::canonicalize(b) == hex(canonical));
      CHECK(psio::pjson::validate(hex(canonical)));
   }
   // Text that is not text has no canonical form.
   Bytes bad_name = hex("c000ff21");   // {"\xff": 1}
   bad_name.insert(bad_name.end(), {psio::pjson_detail::key_hash8("\xff"), 0x00, 0x01, 0x01, 0x00});
   for (const Bytes& b : {hex("90ff"), hex("915c7564383030"), hex("9122"), bad_name})
   {
      CHECK(psio::pjson_lazy::validate(b));
      CHECK_FALSE(psio::pjson::validate(b));
      CHECK_THROWS(psio::pjson::canonicalize(b));
   }
}

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
TEST_CASE("pjson canonical: JSON that is not a type, canonical or lazy by the caller's choice", "[pjson][canonical][json]")
{
   for (const auto& [json, canonical] : std::vector<std::pair<const char*, const char*>>{
           {"[1,2,3]", "b50102030300"},
           {"[0.1,0.2]", "b9cdcccc3dcdcc4c3e0200"},
           {"[1,0.5]", "b90000803f0000003f0200"},
           {"[]", "b0000000"},
           {"[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]", "c10002000000010100000111e57879212200012324000100040200"},
           {"\"a\\u00e9\\n\"", "9061c3a90a"},
           {"{\"k\\n\":1e2}", "c0006b0a4064f800020100"},
           {"0.33333334", "62abaaaa3e"},
           {"5.0", "25"},
           {"[1e400,1]", "b0007002600c2100040200"},
        })
   {
      INFO(json);
      const Bytes made = psio::pjson_json::from_json(json);
      const Bytes lazy = psio::pjson_json::from_json_lazy(json);
      CHECK(made == hex(canonical));
      CHECK(psio::pjson::validate(made));
      CHECK(psio::pjson_lazy::validate(lazy));
      CHECK(psio::pjson::canonicalize(lazy) == made);
      // The two are the same document.
      CHECK(psio::pjson_json::from_json(psio::pjson_to_json(lazy)) == made);
   }
   // The lazy transcode copies what it parses.
   CHECK(psio::pjson_json::from_json_lazy("[1,2,3]") == hex("b0002122230001020300"));
   CHECK_FALSE(psio::pjson::validate(psio::pjson_json::from_json_lazy("[1,2,3]")));
   // A member name is text in both: the escape is not stored.
   CHECK(psio::pjson_json::from_json_lazy("{\"k\\n\":1}") == psio::pjson_json::from_json("{\"k\\n\":1}"));
   CHECK(psio::pjson_to_json(psio::pjson_json::from_json("{\"k\\n\\\"\":1}")) == "{\"k\\n\\\"\":1}");
   // A lone surrogate is not text: neither transcode has a form for it
   // (the JSON reader refuses it).
   CHECK_THROWS(psio::pjson_json::from_json_lazy("\"\\ud800\""));
   CHECK_THROWS(psio::pjson_json::from_json("\"\\ud800\""));
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
