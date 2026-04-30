// pjson_tests.cpp — Catch2 tests for pjson.
//
// Spec: see comments at the top of pjson.hpp.

#include <psio/pjson.hpp>
#include <psio/pjson_view.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_to_json.hpp>
#include <psio/view_to_json.hpp>
#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
#include <psio/pjson_json.hpp>
#include <psio/pjson_json_typed.hpp>
#endif

#define CATCH_CONFIG_FAST_COMPILE
#include <catch.hpp>

#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using psio::pjson;
using psio::pjson_array;
using psio::pjson_bytes;
using psio::pjson_null;
using psio::pjson_number;
using psio::pjson_object;
using psio::pjson_value;
using psio::pjson_view;

namespace {
   pjson_value rt(const pjson_value& v)
   {
      auto bytes = pjson::encode(v);
      REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
      return pjson::decode({bytes.data(), bytes.size()});
   }
}

// ── round-trip basics ────────────────────────────────────────────────────

TEST_CASE("pjson round-trip: scalars", "[pjson][roundtrip]")
{
   CHECK(rt(pjson_value{pjson_null{}}) == pjson_value{pjson_null{}});
   CHECK(rt(pjson_value{true}) == pjson_value{true});
   CHECK(rt(pjson_value{false}) == pjson_value{false});

   for (std::int64_t i : {0LL, 1LL, 15LL, 16LL, -1LL, 12345678901234LL,
                          std::numeric_limits<std::int64_t>::max(),
                          std::numeric_limits<std::int64_t>::min()})
   {
      auto v = rt(pjson_value{i});
      REQUIRE(v.holds<std::int64_t>());
      CHECK(v.as<std::int64_t>() == i);
   }
}

TEST_CASE("pjson round-trip: 128-bit int", "[pjson][int128]")
{
   __int128 huge = (static_cast<__int128>(1) << 100) + 42;
   auto v = rt(pjson_value{pjson_number{huge, 0}});
   REQUIRE(v.holds<pjson_number>());
   CHECK(v.as<pjson_number>().mantissa == huge);
}

TEST_CASE("pjson round-trip: doubles", "[pjson][double]")
{
   // 1.5e308 et al hit a Ryu-corner — the naive pow-of-10 in to_double
   // accumulates ULP error, so the variant decode keeps them as
   // pjson_number rather than collapsing to double. Numeric round-trip
   // still works via as_double(). Tested separately.
   for (double d : {3.14, -3.14, 0.1, 100.5, 1.0 / 7.0})
   {
      CAPTURE(d);
      auto bytes = pjson::encode(pjson_value{d});
      CAPTURE(bytes.size());
      REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
      auto v = pjson::decode({bytes.data(), bytes.size()});
      REQUIRE(v.holds<double>());
      CHECK(std::bit_cast<std::uint64_t>(v.as<double>()) ==
            std::bit_cast<std::uint64_t>(d));
   }
}

TEST_CASE("pjson round-trip: strings", "[pjson][string]")
{
   for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{15},
                        std::size_t{16}, std::size_t{300}})
   {
      std::string s(n, 'x');
      auto v = rt(pjson_value{s});
      CHECK(v.as<std::string>() == s);
   }
}

TEST_CASE("pjson round-trip: bytes", "[pjson][bytes]")
{
   pjson_bytes b{0x00, 0xFF, 0x7F, 0x80, 0x42};
   auto v = rt(pjson_value{b});
   CHECK(v.as<pjson_bytes>() == b);
}

TEST_CASE("pjson round-trip: containers", "[pjson][container]")
{
   pjson_object obj{
       {"a", pjson_value{static_cast<std::int64_t>(1)}},
       {"b", pjson_value{pjson_array{
                  pjson_value{static_cast<std::int64_t>(2)},
                  pjson_value{std::string{"x"}},
                  pjson_value{pjson_null{}}}}},
       {"c", pjson_value{true}},
   };
   auto out = rt(pjson_value{obj});
   REQUIRE(out.holds<pjson_object>());
   CHECK(out == pjson_value{obj});
}

TEST_CASE("pjson round-trip: empty containers", "[pjson][container]")
{
   CHECK(rt(pjson_value{pjson_array{}}).as<pjson_array>().empty());
   CHECK(rt(pjson_value{pjson_object{}}).as<pjson_object>().empty());
}

// ── pjson_view accessors ─────────────────────────────────────────────────

TEST_CASE("pjson_view: object access", "[pjson][view]")
{
   pjson_object obj{
       {"name", pjson_value{std::string{"alice"}}},
       {"age", pjson_value{static_cast<std::int64_t>(30)}},
       {"active", pjson_value{true}},
       {"score", pjson_value{3.14}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view v{bytes.data(), bytes.size()};

   REQUIRE(v.is_object());
   CHECK(v.count() == 4);
   CHECK(v["name"].as_string() == "alice");
   CHECK(v["age"].as_int64() == 30);
   CHECK(v["active"].as_bool() == true);
   CHECK(v["score"].as_double() == 3.14);
   CHECK_FALSE(v.find("missing").has_value());
}

TEST_CASE("pjson_view: array indexing", "[pjson][view]")
{
   pjson_array a;
   for (int i = 0; i < 10; ++i)
      a.push_back(pjson_value{static_cast<std::int64_t>(i * 10)});
   auto bytes = pjson::encode(pjson_value{a});
   pjson_view v{bytes.data(), bytes.size()};

   REQUIRE(v.is_array());
   CHECK(v.count() == 10);
   for (std::size_t i = 0; i < 10; ++i)
      CHECK(v[i].as_int64() == static_cast<std::int64_t>(i * 10));
}

TEST_CASE("pjson_view: chained access", "[pjson][view]")
{
   pjson_object inner{{"city", pjson_value{std::string{"NYC"}}}};
   pjson_object outer{{"address", pjson_value{inner}}};
   auto bytes = pjson::encode(pjson_value{outer});
   pjson_view v{bytes.data(), bytes.size()};
   CHECK(v["address"]["city"].as_string() == "NYC");
}

TEST_CASE("pjson_view: for_each iteration", "[pjson][view]")
{
   pjson_object obj{
       {"alpha", pjson_value{static_cast<std::int64_t>(1)}},
       {"beta", pjson_value{static_cast<std::int64_t>(2)}},
       {"gamma", pjson_value{static_cast<std::int64_t>(3)}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view v{bytes.data(), bytes.size()};

   std::vector<std::string> seen_keys;
   v.for_each_field([&](std::string_view k, pjson_view) {
      seen_keys.emplace_back(k);
   });
   REQUIRE(seen_keys.size() == 3);
   CHECK(seen_keys[0] == "alpha");
   CHECK(seen_keys[1] == "beta");
   CHECK(seen_keys[2] == "gamma");
}

TEST_CASE("pjson_view: hash suffix-strip", "[pjson][view][hash]")
{
   pjson_object o{
       {"amount.decimal", pjson_value{static_cast<std::int64_t>(1)}},
       {"amount", pjson_value{static_cast<std::int64_t>(2)}},
   };
   auto bytes = pjson::encode(pjson_value{o});
   pjson_view v{bytes.data(), bytes.size()};
   // Different keys, same hash bucket — both still findable distinctly.
   auto a = v.find("amount.decimal");
   auto b = v.find("amount");
   REQUIRE(a.has_value());
   REQUIRE(b.has_value());
   CHECK(a->as_int64() == 1);
   CHECK(b->as_int64() == 2);
}

// ── typed_pjson_view + canonical fast path ────────────────────────────────

struct User
{
   std::string  name;
   std::int64_t age;
   bool         active;
   double       score;
};
PSIO_REFLECT(User, name, age, active, score)

TEST_CASE("typed_pjson_view: canonical fast path", "[pjson][typed]")
{
   User u{"alice", 30, true, 3.14};
   auto bytes = psio::from_struct(u);
   pjson_view raw{bytes.data(), bytes.size()};

   auto t = psio::typed_pjson_view<User>::from_pjson(raw);
   REQUIRE(t.is_canonical());
   CHECK(t.get<0>() == "alice");
   CHECK(t.get<1>() == 30);
   CHECK(t.get<2>() == true);
   CHECK(t.get<3>() == 3.14);

   User out = t.to_struct();
   CHECK(out.name == u.name);
   CHECK(out.age == u.age);
   CHECK(out.active == u.active);
   CHECK(out.score == u.score);
}

TEST_CASE("typed_pjson_view: non-canonical falls back to find",
          "[pjson][typed]")
{
   // Build the object with reordered keys → canonical check must fail.
   pjson_object obj{
       {"score", pjson_value{3.14}},
       {"active", pjson_value{true}},
       {"age", pjson_value{static_cast<std::int64_t>(30)}},
       {"name", pjson_value{std::string{"alice"}}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view raw{bytes.data(), bytes.size()};

   auto t = psio::typed_pjson_view<User>::from_pjson(raw);
   CHECK_FALSE(t.is_canonical());
   // Field access still works via find().
   CHECK(t.get<0>() == "alice");
   CHECK(t.get<1>() == 30);
}

// ── pjson_number direct decimal parse ────────────────────────────────────

TEST_CASE("pjson_number::from_string", "[pjson][number]")
{
   auto eq = [](pjson_number a, pjson_number b) {
      return a.mantissa == b.mantissa && a.scale == b.scale;
   };
   CHECK(eq(pjson_number::from_string("0.1"),    pjson_number{1, -1}));
   CHECK(eq(pjson_number::from_string("0.10"),   pjson_number{1, -1}));
   CHECK(eq(pjson_number::from_string("3.14"),   pjson_number{314, -2}));
   CHECK(eq(pjson_number::from_string("-3.14"),  pjson_number{-314, -2}));
   CHECK(eq(pjson_number::from_string("100"),    pjson_number{100, 0}));
   CHECK(eq(pjson_number::from_string("0"),      pjson_number{0, 0}));
   CHECK(eq(pjson_number::from_string("1.5e10"), pjson_number{15, 9}));
}

TEST_CASE("pjson_number: scale-aligned compare", "[pjson][number]")
{
   pjson_number a{1, -1};
   pjson_number b{10, -2};
   CHECK(numerically_equal(a, b));
   CHECK(compare(a, b) == std::strong_ordering::equal);

   CHECK(compare(pjson_number::from_string("0.99"),
                 pjson_number::from_string("1.0")) ==
         std::strong_ordering::less);
}

// ── stress ───────────────────────────────────────────────────────────────

TEST_CASE("pjson stress: random tree round-trips", "[pjson][stress]")
{
   std::mt19937 rng(0xC0FFEE);

   auto gen = [&](auto& self, int depth) -> pjson_value {
      std::uniform_int_distribution<int> d(0, depth <= 0 ? 4 : 6);
      switch (d(rng))
      {
         case 0: return pjson_value{pjson_null{}};
         case 1: return pjson_value{(rng() & 1) != 0};
         case 2: return pjson_value{static_cast<std::int64_t>(rng())};
         case 3:
         {
            std::uniform_int_distribution<std::size_t> n(0, 40);
            return pjson_value{std::string(n(rng), 'a')};
         }
         case 4:
         {
            std::uniform_int_distribution<std::size_t> n(0, 16);
            pjson_bytes b(n(rng));
            for (auto& x : b) x = static_cast<std::uint8_t>(rng());
            return pjson_value{std::move(b)};
         }
         case 5:
         {
            std::uniform_int_distribution<int> n(0, 4);
            pjson_array a;
            int k = n(rng);
            for (int i = 0; i < k; ++i) a.push_back(self(self, depth - 1));
            return pjson_value{std::move(a)};
         }
         default:
         {
            std::uniform_int_distribution<int> n(0, 4);
            pjson_object o;
            int k = n(rng);
            for (int i = 0; i < k; ++i)
               o.emplace_back("k" + std::to_string(rng() % 1000),
                              self(self, depth - 1));
            return pjson_value{std::move(o)};
         }
      }
   };

   for (int trial = 0; trial < 50; ++trial)
   {
      pjson_value v     = gen(gen, 4);
      auto        bytes = pjson::encode(v);
      REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
      auto v2 = pjson::decode({bytes.data(), bytes.size()});
      REQUIRE(v == v2);
      auto bytes2 = pjson::encode(v2);
      REQUIRE(bytes == bytes2);
   }
}

// ── typed homogeneous arrays (tag 9) ─────────────────────────────────────

namespace {
   // Helper: build a typed-array buffer (t_array, low_nibble = code+1).
   template <typename T>
   std::vector<std::uint8_t> make_typed_array(std::span<const T> elems)
   {
      std::vector<std::uint8_t> out(
          psio::pjson_detail::typed_array_size(
              psio::pjson_detail::tac_for<T>(), elems.size()));
      psio::pjson_detail::encode_typed_array_at(out.data(), 0, elems);
      return out;
   }
}

TEST_CASE("pjson typed_array: encode/decode each element type",
          "[pjson][typed_array]")
{
   using namespace psio::pjson_detail;

   // i8 — wire low_nibble = code + 1, so tag = (t_array<<4) | (tac_i8+1).
   {
      std::int8_t in[] = {-128, -1, 0, 1, 127};
      auto bytes = make_typed_array<std::int8_t>(in);
      // size = 1 tag + 5*1 element bytes + 2 count = 8
      CHECK(bytes.size() == 8);
      CHECK(bytes[0] == ((t_array << 4) | (tac_i8 + 1)));
      CHECK(bytes[6] == 5);
      CHECK(bytes[7] == 0);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      REQUIRE(a.size() == 5);
      CHECK(a[0].as<std::int64_t>() == -128);
      CHECK(a[2].as<std::int64_t>() == 0);
      CHECK(a[4].as<std::int64_t>() == 127);
   }
   // i16
   {
      std::int16_t in[] = {-32768, 0, 32767};
      auto bytes = make_typed_array<std::int16_t>(in);
      CHECK(bytes.size() == 1 + 3 * 2 + 2);
      CHECK(bytes[0] == ((t_array << 4) | (tac_i16 + 1)));
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[0].as<std::int64_t>() == -32768);
      CHECK(a[2].as<std::int64_t>() == 32767);
   }
   // i32
   {
      std::int32_t in[] = {-1000000, 0, 1000000};
      auto bytes = make_typed_array<std::int32_t>(in);
      CHECK(bytes.size() == 1 + 3 * 4 + 2);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[0].as<std::int64_t>() == -1000000);
      CHECK(a[2].as<std::int64_t>() == 1000000);
   }
   // i64
   {
      std::int64_t in[] = {std::numeric_limits<std::int64_t>::min(),
                           0,
                           std::numeric_limits<std::int64_t>::max()};
      auto bytes = make_typed_array<std::int64_t>(in);
      CHECK(bytes.size() == 1 + 3 * 8 + 2);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[0].as<std::int64_t>() ==
            std::numeric_limits<std::int64_t>::min());
      CHECK(a[2].as<std::int64_t>() ==
            std::numeric_limits<std::int64_t>::max());
   }
   // u8
   {
      std::uint8_t in[] = {0, 1, 254, 255};
      auto bytes = make_typed_array<std::uint8_t>(in);
      CHECK(bytes.size() == 1 + 4 * 1 + 2);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[3].as<std::int64_t>() == 255);
   }
   // u32
   {
      std::uint32_t in[] = {0, 4000000000u};
      auto bytes = make_typed_array<std::uint32_t>(in);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[1].as<std::int64_t>() == 4000000000LL);
   }
   // u64 — value beyond i64 range surfaces as pjson_number.
   {
      std::uint64_t in[] = {0, std::numeric_limits<std::uint64_t>::max()};
      auto bytes = make_typed_array<std::uint64_t>(in);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[0].as<std::int64_t>() == 0);
      REQUIRE(a[1].holds<pjson_number>());
      CHECK(a[1].as<pjson_number>().mantissa ==
            static_cast<__int128>(std::numeric_limits<std::uint64_t>::max()));
   }
   // f32
   {
      float in[] = {-1.5f, 0.0f, 3.25f};
      auto bytes = make_typed_array<float>(in);
      CHECK(bytes.size() == 1 + 3 * 4 + 2);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[0].as<double>() == -1.5);
      CHECK(a[2].as<double>() == 3.25);
   }
   // f64
   {
      double in[] = {3.14159265358979, -1.0e100};
      auto bytes = make_typed_array<double>(in);
      CHECK(bytes.size() == 1 + 2 * 8 + 2);
      auto v = pjson::decode({bytes.data(), bytes.size()});
      auto& a = v.as<pjson_array>();
      CHECK(a[0].as<double>() == 3.14159265358979);
      CHECK(a[1].as<double>() == -1.0e100);
   }
}

TEST_CASE("pjson typed_array: view accessors", "[pjson][typed_array][view]")
{
   std::int32_t in[] = {-7, 0, 42, 100000};
   auto bytes = make_typed_array<std::int32_t>(in);
   pjson_view v{bytes.data(), bytes.size()};
   REQUIRE(v.is_array());
   REQUIRE(v.is_typed_array());
   CHECK(v.typed_array_elem_code() == psio::pjson_detail::tac_i32);
   CHECK(v.typed_array_elem_size() == 4);
   CHECK(v.count() == 4);
   CHECK(v.typed_int64_at(0) == -7);
   CHECK(v.typed_int64_at(1) == 0);
   CHECK(v.typed_int64_at(2) == 42);
   CHECK(v.typed_int64_at(3) == 100000);
   CHECK(v.typed_double_at(2) == 42.0);

   // typed_array_span<T> with matching element type.
   auto span = v.typed_array_span<std::int32_t>();
   REQUIRE(span.size() == 4);
   CHECK(span[2] == 42);

   // Mismatched element type rejects.
   CHECK_THROWS(v.typed_array_span<std::int64_t>());
}

TEST_CASE("pjson typed_array: empty array", "[pjson][typed_array]")
{
   std::span<const std::int32_t> empty{};
   auto bytes = make_typed_array<std::int32_t>(empty);
   CHECK(bytes.size() == 3);  // tag + 0 elements + count u16
   pjson_view v{bytes.data(), bytes.size()};
   REQUIRE(v.is_typed_array());
   CHECK(v.count() == 0);
}

TEST_CASE("pjson typed_array: validate/round-trip", "[pjson][typed_array]")
{
   std::int64_t in[] = {1, 2, 3, 4, 5};
   auto bytes = make_typed_array<std::int64_t>(in);
   REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
   // Decode produces a regular pjson_array; encode of THAT will
   // produce a regular t_array (not a typed_array), since
   // pjson_value can't carry the typed-ness. That's expected — typed
   // array bytes are produced only by direct encoder paths.
   auto v = pjson::decode({bytes.data(), bytes.size()});
   auto& a = v.as<pjson_array>();
   REQUIRE(a.size() == 5);
   for (std::size_t i = 0; i < 5; ++i)
      CHECK(a[i].as<std::int64_t>() ==
            static_cast<std::int64_t>(i + 1));
}

TEST_CASE("pjson typed_array: at() and for_each_element synthesize "
          "form_typed_array_element views",
          "[pjson][typed_array][view]")
{
   std::int32_t in[] = {1, 2, 3};
   auto bytes = make_typed_array<std::int32_t>(in);
   pjson_view v{bytes.data(), bytes.size()};

   // at(i) returns a synthetic view that points at the raw element
   // bytes and reports kind::integer; as_int64 reads through to the
   // typed-array bytes directly.
   auto e0 = v.at(0);
   CHECK(e0.is_integer());
   CHECK(e0.as_int64() == 1);
   auto e2 = v.at(2);
   CHECK(e2.as_int64() == 3);

   // for_each_element iterates by calling at(i) under the covers.
   std::vector<std::int64_t> seen;
   v.for_each_element([&](pjson_view child) {
      seen.push_back(child.as_int64());
   });
   REQUIRE(seen.size() == 3);
   CHECK(seen[0] == 1);
   CHECK(seen[1] == 2);
   CHECK(seen[2] == 3);
}

TEST_CASE("pjson typed_array: nested in object value",
          "[pjson][typed_array]")
{
   std::int32_t scores[] = {10, 20, 30};
   auto inner = make_typed_array<std::int32_t>(scores);

   // Hand-build an object whose only field's value is a typed_array.
   // We can't go via pjson_value because the variant doesn't carry
   // typed_array; build the object bytes directly.
   //
   // Layout:
   //   [0xC0][width byte][key 's'][typed_array_bytes][hash[1]]
   //   [slot[1]: offset(slot_w) + key_size(1)][count u16]
   std::string key = "s";
   std::size_t value_data = 1 /*key*/ + inner.size();
   std::uint8_t slot_w_code = psio::pjson_detail::width_code_for(value_data);
   std::size_t  slot_w      = psio::pjson_detail::width_bytes(slot_w_code);
   std::size_t  slot_stride = slot_w + 1;
   std::size_t  total       = 1 + 1 + value_data + 1 + slot_stride + 2;
   std::vector<std::uint8_t> out(total);
   out[0] = static_cast<std::uint8_t>(psio::pjson_detail::t_object << 4);
   out[1] = slot_w_code;
   out[2] = static_cast<std::uint8_t>('s');
   std::memcpy(out.data() + 3, inner.data(), inner.size());
   std::size_t hash_pos  = 2 + value_data;
   std::size_t slot_pos  = hash_pos + 1;
   std::size_t count_pos = slot_pos + slot_stride;
   out[hash_pos] = psio::pjson_detail::key_hash8("s");
   psio::pjson_detail::write_width(out.data() + slot_pos,
                                   slot_w_code, 0);
   out[slot_pos + slot_w] = 1;  // key_size
   out[count_pos]     = 1;
   out[count_pos + 1] = 0;

   REQUIRE(pjson::validate({out.data(), out.size()}));
   pjson_view v{out.data(), out.size()};
   REQUIRE(v.is_object());
   auto child = v["s"];
   REQUIRE(child.is_typed_array());
   CHECK(child.count() == 3);
   CHECK(child.typed_int64_at(1) == 20);

   // view_to_json renders the typed array as a JSON array of ints.
   std::string j = psio::view_to_json(v);
   CHECK(j == "{\"s\":[10,20,30]}");
}

TEST_CASE("pjson typed_array: pjson_to_json direct walker",
          "[pjson][typed_array]")
{
   double in[] = {1.5, 2.5, 3.5};
   auto bytes = make_typed_array<double>(in);
   std::string j = psio::pjson_to_json({bytes.data(), bytes.size()});
   CHECK(j == "[1.5,2.5,3.5]");

   std::uint64_t big[] = {0, 18446744073709551615ULL};
   auto bytes2 = make_typed_array<std::uint64_t>(big);
   std::string j2 = psio::pjson_to_json({bytes2.data(), bytes2.size()});
   CHECK(j2 == "[0,18446744073709551615]");
}

// ── row_array form (homogeneous-shape array of objects, §5.2.1) ──────────

namespace {
   // Encode `a` (a homogeneous array of objects) into row_array bytes.
   std::vector<std::uint8_t> make_row_array(const pjson_array& a)
   {
      using namespace psio::pjson_detail;
      REQUIRE(array_is_row_array_homogeneous(a));
      std::size_t total = row_array_size(a);
      std::vector<std::uint8_t> out(total);
      encode_row_array_at(out.data(), 0, a);
      return out;
   }
}

TEST_CASE("pjson row_array: homogeneity detection",
          "[pjson][row_array]")
{
   using namespace psio::pjson_detail;

   pjson_array homo;
   for (int i = 0; i < 5; ++i)
      homo.push_back(pjson_value{pjson_object{
         {"id", pjson_value{static_cast<std::int64_t>(i)}},
         {"name", pjson_value{std::string{"x"}}}}});
   CHECK(array_is_row_array_homogeneous(homo));

   pjson_array hetero = homo;
   hetero.push_back(pjson_value{
      pjson_object{{"id", pjson_value{std::int64_t{99}}}}});
   CHECK_FALSE(array_is_row_array_homogeneous(hetero));

   pjson_array order_swapped;
   order_swapped.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{0}}},
      {"name", pjson_value{std::string{"a"}}}}});
   order_swapped.push_back(pjson_value{pjson_object{
      {"name", pjson_value{std::string{"b"}}},
      {"id", pjson_value{std::int64_t{1}}}}});
   CHECK_FALSE(array_is_row_array_homogeneous(order_swapped));

   CHECK_FALSE(array_is_row_array_homogeneous(pjson_array{}));
}

TEST_CASE("pjson row_array: encode + decode round-trip",
          "[pjson][row_array]")
{
   using namespace psio::pjson_detail;

   pjson_array a;
   for (int i = 0; i < 4; ++i)
      a.push_back(pjson_value{pjson_object{
         {"id", pjson_value{static_cast<std::int64_t>(i * 7)}},
         {"name", pjson_value{std::string{"alice"}}}}});
   auto bytes = make_row_array(a);

   CHECK(bytes[0] == ((t_object << 4) | object_form_row_array));
   std::uint16_t N_dec =
      static_cast<std::uint16_t>(bytes[bytes.size() - 2]) |
      (static_cast<std::uint16_t>(bytes[bytes.size() - 1]) << 8);
   CHECK(N_dec == 4);

   REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
   auto v = pjson::decode({bytes.data(), bytes.size()});
   REQUIRE(v.holds<pjson_array>());
   const auto& got = v.as<pjson_array>();
   REQUIRE(got.size() == 4);
   for (std::size_t i = 0; i < 4; ++i)
   {
      REQUIRE(got[i].holds<pjson_object>());
      const auto& obj = got[i].as<pjson_object>();
      REQUIRE(obj.size() == 2);
      CHECK(obj[0].first == "id");
      CHECK(obj[0].second.as<std::int64_t>() ==
            static_cast<std::int64_t>(i * 7));
      CHECK(obj[1].first == "name");
      CHECK(obj[1].second.as<std::string>() == "alice");
   }
}

TEST_CASE("pjson row_array: adaptive width fires for small N",
          "[pjson][row_array]")
{
   using namespace psio::pjson_detail;
   pjson_array a;
   for (int i = 0; i < 3; ++i)
      a.push_back(pjson_value{pjson_object{
         {"x", pjson_value{static_cast<std::int64_t>(i)}}}});
   auto bytes = make_row_array(a);
   std::uint8_t w             = bytes[1];
   std::uint8_t slot_w_code   = w & 0x03;
   std::uint8_t recoff_w_code = (w >> 2) & 0x03;
   CHECK(slot_w_code == 0);    // u8
   CHECK(recoff_w_code == 0);  // u8
}

TEST_CASE("pjson row_array: vector<ReflectedT> auto-emits row_array",
          "[pjson][row_array][typed]")
{
   // Wrap a vector<User> in a reflected struct so to_pjson can encode
   // it (top-level entry takes a reflected struct).
   struct UserList
   {
      std::vector<User> users;
   };
   // Reflect it inline (PSIO_REFLECT is namespace-scoped — fine here
   // since the test struct is at namespace scope).

   std::vector<User> raw = {
      {"alice", 30, true, 3.14},
      {"bob",   25, false, 2.71},
      {"carol", 40, true, 1.41},
   };

   // Direct encode of the vector<User> via the typed encoder. The
   // size + encode helpers live in pjson_detail; call them directly
   // here because there's no top-level psio API for "encode a typed
   // vector at the root" (it's normally embedded in a reflected
   // struct).
   using namespace psio::pjson_detail;
   std::span<const User> span(raw.data(), raw.size());
   std::size_t total = typed_row_array_size<User>(span);
   std::vector<std::uint8_t> bytes(total);
   encode_typed_row_array_at<User>(bytes.data(), 0, span);

   // The first byte must be the row_array tag.
   CHECK(bytes[0] == ((t_object << 4) | object_form_row_array));
   // Last 2 bytes are the count.
   std::uint16_t N_dec =
      static_cast<std::uint16_t>(bytes[bytes.size() - 2]) |
      (static_cast<std::uint16_t>(bytes[bytes.size() - 1]) << 8);
   CHECK(N_dec == 3);

   // Round-trip: decode produces a pjson_array of pjson_objects with
   // the four User fields each.
   REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
   auto v = pjson::decode({bytes.data(), bytes.size()});
   REQUIRE(v.holds<pjson_array>());
   const auto& got = v.as<pjson_array>();
   REQUIRE(got.size() == 3);
   for (std::size_t i = 0; i < 3; ++i)
   {
      REQUIRE(got[i].holds<pjson_object>());
      const auto& obj = got[i].as<pjson_object>();
      REQUIRE(obj.size() == 4);
      CHECK(obj[0].first == "name");
      CHECK(obj[1].first == "age");
      CHECK(obj[2].first == "active");
      CHECK(obj[3].first == "score");
      CHECK(obj[0].second.as<std::string>() == raw[i].name);
      CHECK(obj[1].second.as<std::int64_t>() == raw[i].age);
      CHECK(obj[2].second.as<bool>() == raw[i].active);
   }
}

TEST_CASE("pjson row_array: view at()/find/for_each_field uniform API",
          "[pjson][row_array][view]")
{
   pjson_array a;
   a.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{10}}},
      {"name", pjson_value{std::string{"alice"}}}}});
   a.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{20}}},
      {"name", pjson_value{std::string{"bob"}}}}});
   a.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{30}}},
      {"name", pjson_value{std::string{"carol"}}}}});

   auto bytes = make_row_array(a);
   pjson_view arr{bytes.data(), bytes.size()};

   // Top-level row_array view is array-shaped.
   REQUIRE(arr.is_array());
   REQUIRE(arr.is_row_array());
   CHECK(arr.count() == 3);

   // at(i) returns a record view that's object-shaped.
   for (std::size_t i = 0; i < 3; ++i)
   {
      auto rec = arr.at(i);
      REQUIRE(rec.is_object());
      CHECK(rec.count() == 2);  // K = 2 fields per record

      // find() walks the shared schema in the row_array buffer.
      auto id_v   = rec.find("id");
      auto name_v = rec.find("name");
      REQUIRE(id_v.has_value());
      REQUIRE(name_v.has_value());
      CHECK(id_v->as_int64() == static_cast<std::int64_t>((i + 1) * 10));
   }

   // for_each_element iterates records.
   std::size_t seen = 0;
   arr.for_each_element([&](pjson_view rec) {
      auto id = rec["id"].as_int64();
      CHECK((id == 10 || id == 20 || id == 30));
      ++seen;
   });
   CHECK(seen == 3);

   // for_each_field on a record iterates (key, value) using shared keys.
   auto rec0 = arr.at(0);
   std::vector<std::string> keys;
   rec0.for_each_field([&](std::string_view k, pjson_view) {
      keys.emplace_back(k);
   });
   REQUIRE(keys.size() == 2);
   CHECK(keys[0] == "id");
   CHECK(keys[1] == "name");

   // String values inside row_array records are still real pjson
   // values — round-trip via as_string works.
   CHECK(arr.at(1)["name"].as_string() == "bob");
}

TEST_CASE("pjson row_array: variable-length values round-trip",
          "[pjson][row_array]")
{
   // Records with variable-length string fields exercise the
   // per-record slot table (without it, there's no way to find
   // where each field's bytes end inside the record body).
   pjson_array a;
   a.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{1}}},
      {"name", pjson_value{std::string{"a"}}}}});
   a.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{2}}},
      {"name", pjson_value{std::string{"longer-string-here"}}}}});
   a.push_back(pjson_value{pjson_object{
      {"id", pjson_value{std::int64_t{3}}},
      {"name", pjson_value{std::string{}}}}});

   auto bytes = make_row_array(a);
   auto v     = pjson::decode({bytes.data(), bytes.size()});
   const auto& got = v.as<pjson_array>();
   REQUIRE(got.size() == 3);
   CHECK(got[0].as<pjson_object>()[1].second.as<std::string>() == "a");
   CHECK(got[1].as<pjson_object>()[1].second.as<std::string>()
         == "longer-string-here");
   CHECK(got[2].as<pjson_object>()[1].second.as<std::string>().empty());
}

// ── JSON pipeline (when simdjson is available) ───────────────────────────

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
TEST_CASE("pjson_json: JSON → pjson via simdjson", "[pjson][json]")
{
   std::string_view doc =
       R"({"name":"alice","age":30,"active":true,"score":3.14,"id":1234567890,"email":"alice@example.com"})";

   auto bytes = psio::pjson_json::from_json(doc);
   REQUIRE(pjson::validate({bytes.data(), bytes.size()}));

   pjson_view v{bytes.data(), bytes.size()};
   REQUIRE(v.is_object());
   CHECK(v["name"].as_string() == "alice");
   CHECK(v["age"].as_int64() == 30);
   CHECK(v["active"].as_bool() == true);
   CHECK(v["score"].as_double() == 3.14);
   CHECK(v["id"].as_int64() == 1234567890);
   CHECK(v["email"].as_string() == "alice@example.com");
}

TEST_CASE("pjson_json: nested JSON", "[pjson][json]")
{
   std::string_view doc =
       R"({"users":[{"name":"alice","id":1},{"name":"bob","id":2}]})";

   auto bytes = psio::pjson_json::from_json(doc);
   pjson_view v{bytes.data(), bytes.size()};
   CHECK(v["users"][0]["name"].as_string() == "alice");
   CHECK(v["users"][1]["id"].as_int64() == 2);
}

TEST_CASE("json_to: in-place reuses caller storage",
          "[pjson][json][typed]")
{
   std::string_view doc =
       R"({"name":"alice","age":30,"active":true,"score":3.14})";
   User u;
   // Pre-populate to confirm the in-place path overwrites.
   u.name   = "previous-very-long-name-with-allocation";
   u.age    = 999;
   u.active = false;
   u.score  = 99.9;
   psio::json_to<User>(doc, u);
   CHECK(u.name == "alice");
   CHECK(u.age == 30);
   CHECK(u.active == true);
   CHECK(u.score == 3.14);
}

TEST_CASE("json_to: direct JSON → T", "[pjson][json][typed]")
{
   std::string_view doc =
       R"({"name":"alice","age":30,"active":true,"score":3.14})";
   User u = psio::json_to<User>(doc);
   CHECK(u.name == "alice");
   CHECK(u.age == 30);
   CHECK(u.active == true);
   CHECK(u.score == 3.14);
}

TEST_CASE("json_to_struct: non-canonical key order",
          "[pjson][json][typed]")
{
   std::string_view doc =
       R"({"score":3.14,"active":true,"age":30,"name":"alice"})";
   User u = psio::json_to<User>(doc);
   CHECK(u.name == "alice");
   CHECK(u.age == 30);
   CHECK(u.active == true);
   CHECK(u.score == 3.14);
}

TEST_CASE("json_to_struct: missing fields default-init",
          "[pjson][json][typed]")
{
   std::string_view doc = R"({"name":"alice"})";
   User u = psio::json_to<User>(doc);
   CHECK(u.name == "alice");
   CHECK(u.age == 0);  // default
   CHECK(u.active == false);
   CHECK(u.score == 0.0);
}

TEST_CASE("view_to_json: pjson_view → JSON (generic walker)",
          "[pjson][json][view]")
{
   pjson_object obj{
       {"name", pjson_value{std::string{"alice"}}},
       {"age", pjson_value{static_cast<std::int64_t>(30)}},
       {"active", pjson_value{true}},
       {"tags", pjson_value{pjson_array{
                    pjson_value{std::string{"admin"}},
                    pjson_value{std::string{"ops"}}}}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view v{bytes.data(), bytes.size()};

   std::string j = psio::view_to_json(v);
   // Round-trip: emitted JSON parses back to a structurally equal pjson.
   auto bytes2 = psio::pjson_json::from_json(j);
   pjson_view v2{bytes2.data(), bytes2.size()};
   CHECK(v2["name"].as_string() == "alice");
   CHECK(v2["age"].as_int64() == 30);
   CHECK(v2["active"].as_bool() == true);
   CHECK(v2["tags"][0].as_string() == "admin");
}

TEST_CASE("struct_to_json: T → JSON", "[pjson][json][typed]")
{
   User u{"alice", 30, true, 3.14};
   std::string j = psio::struct_to_json(u);
   // Round-trip via the typed parser:
   User back = psio::json_to<User>(j);
   CHECK(back.name == u.name);
   CHECK(back.age == u.age);
   CHECK(back.active == u.active);
   CHECK(back.score == u.score);
}
#endif
