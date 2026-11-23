// Cross-language consistency round: one section per rule C++ was the looser
// or the inconsistent side of. Each section names the rule it pins.
#include <catch.hpp>

#include <psio/annotate.hpp>
#include <psio/compress_name.hpp>
#include <psio/get_type_name.hpp>
#include <psio/name.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wrappers.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
   std::span<const char> sp(std::string_view text) { return {text.data(), text.size()}; }
   template <typename B>
   std::span<const char> bytes_of(const B& b) { return {b.data(), b.size()}; }
}  // namespace

// ── `min<A> | max<B>` is one length bound ────────────────────────────────
namespace cons_bounds
{
   struct Ranged
   {
      std::string              name;
      std::vector<std::string> tags;
   };
   PSIO_REFLECT(Ranged,
                attr(name, min<2> | max<8>),
                attr(tags, max<4> | json_number | min<1> | element(max<3> | utf8)))
}  // namespace cons_bounds

TEST_CASE("min<A> | max<B> merge into one length_bound", "[consistency][annotate]")
{
   using namespace cons_bounds;
   constexpr auto both = psio::min<2> | psio::max<8>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(both)> == 1);
   STATIC_REQUIRE(std::get<psio::length_bound>(both) ==
                  psio::length_bound{.max = 8, .min = 2});
   // Order and what stands between them do not matter; the other specs stay.
   constexpr auto mixed = psio::max<8> | psio::utf8 | psio::min<2>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(mixed)> == 2);
   STATIC_REQUIRE(std::get<psio::length_bound>(mixed) == std::get<psio::length_bound>(both));
   // The same limit twice with one value is one statement.
   STATIC_REQUIRE(psio::detail::length_bounds_consistent(psio::max<8> | psio::max<8>));
   STATIC_REQUIRE(!psio::detail::length_bounds_consistent(psio::max<8> | psio::max<16>));
   STATIC_REQUIRE(!psio::detail::length_bounds_consistent(psio::min<9> | psio::max<8>));
   STATIC_REQUIRE(!psio::detail::length_bounds_consistent(psio::exact<9> | psio::max<8>));
   STATIC_REQUIRE(!psio::detail::length_bounds_consistent(
      psio::to_spec_tuple(psio::element(psio::max<1> | psio::max<2>))));

   // Both limits are enforced by every codec, and the upper one bounds the width.
   STATIC_REQUIRE(psio::max_encoded_size<Ranged>().has_value());
   const Ranged good{"abc", {"x"}};
   const auto   frame = psio::to_pssz(good);
   CHECK(psio::validate_pssz<Ranged>(bytes_of(frame)).ok());
   CHECK(psio::from_json<Ranged>(psio::to_json(good).c_str()).name == "abc");
   std::vector<char> out;
   CHECK(!psio::encode_pssz_checked(psio::pssz{}, Ranged{"a", {"x"}}, out).ok());           // below min
   CHECK(!psio::encode_pssz_checked(psio::pssz{}, Ranged{"abcdefghi", {"x"}}, out).ok());   // above max
   CHECK(!psio::encode_pssz_checked(psio::pssz{}, Ranged{"abc", {}}, out).ok());            // below min
   CHECK(!psio::validate_json<Ranged>(R"({"name":"a","tags":["x"]})").ok());
   CHECK(!psio::validate_json<Ranged>(R"({"name":"abc","tags":[]})").ok());
   CHECK(psio::validate_json<Ranged>(R"({"name":"ab","tags":["x"]})").ok());
}

// ── A string literal is its characters at every text entry point ─────────
TEST_CASE("text entry points take a C string without its terminator", "[consistency][json]")
{
   // The span of a char array includes the NUL: "200" would be four bytes,
   // which is not a JSON text. Every entry point that takes text has the
   // overload the to_/from_ helpers have.
   std::uint8_t v = 0;
   CHECK(psio::decode_json_checked(psio::json{}, "200", v).ok());
   CHECK(v == 200);
   CHECK(psio::decode_json_checked(psio::ijson{}, "7", v, psio::json_parser::text).ok());
   CHECK(v == 7);
   CHECK(!psio::decode_json_checked(psio::json{}, "256", v).ok());
   CHECK(psio::validate_json<std::uint8_t>(psio::json{}, "200").ok());
   CHECK(!psio::validate_json<std::uint8_t>(psio::json{}, "2 0").ok());
   CHECK(psio::decode<std::uint8_t>(psio::json{}, "200") == 200);
   CHECK(psio::validate<std::uint8_t>(psio::json{}, "200").ok());
   CHECK(psio::validate_strict<std::uint8_t>(psio::ijson{}, "200").ok());
   CHECK(*psio::make_boxed<std::uint8_t>(psio::json{}, "200") == 200);
   // The span of the same literal is still what it says: four bytes.
   const char literal[] = "200";
   CHECK(!psio::validate_json<std::uint8_t>(psio::json{}, std::span<const char>{literal}).ok());

   // The schema-driven codec.
   psio::SchemaBuilder builder;
   const auto          root   = builder.insert<std::uint8_t>();
   const auto          schema = std::move(builder).build();
   std::vector<char>   bytes;
   CHECK(psio::pssz_ir::json_to_pssz(schema, root, psio::json{}, "200", bytes).ok());
   CHECK(bytes == std::vector<char>{static_cast<char>(200)});
   psio::pssz_ir::Value value;
   CHECK(psio::pssz_ir::json_to_value(schema, root, psio::ijson{}, "200", value).ok());
}

// ── maxDynamicData caps the encoded PSSZ size, not the JSON text ─────────
namespace cons_cap
{
   struct Capped
   {
      std::uint64_t a = 0;
      std::uint64_t b = 0;
   };
   PSIO_REFLECT(Capped, definitionWillNotChange(), maxDynamicData(16), a, b)
}  // namespace cons_cap

TEST_CASE("a type's cap is not applied to the length of its text", "[consistency][json]")
{
   using cons_cap::Capped;
   const Capped value{~std::uint64_t{0}, 2};
   REQUIRE(psio::to_pssz(value).size() == 16);   // within the cap
   const std::string text = psio::to_json(value);
   REQUIRE(text.size() > 16);                    // 35 characters of text
   // Every reader of the type's own text accepts it.
   CHECK(psio::validate<Capped>(psio::json{}, sp(text)).ok());
   CHECK(psio::validate<Capped>(psio::ijson{}, sp(psio::to_ijson(value))).ok());
   CHECK(psio::validate_strict<Capped>(psio::json{}, sp(text)).ok());
   CHECK(psio::validate_json<Capped>(sp(text)).ok());
   CHECK(psio::from_json<Capped>(sp(text)).b == 2);
   // Padding does not matter either: the text is not what is capped.
   const std::string padded = "   " + text + std::string(64, ' ');
   CHECK(psio::validate<Capped>(psio::json{}, sp(padded)).ok());
}

// ── A name's number decodes back to the name ─────────────────────────────
TEST_CASE("name_to_number: a number is given only to a name it decodes to", "[consistency][name]")
{
   using psio::name_to_number;
   using psio::number_to_name;
   static_assert(name_to_number("alice") == 6697231);
   static_assert(name_to_number("a-b") == 15765525);
   CHECK(number_to_name(6697231) == "alice");
   // A doubled hyphen has no frequency in the model: the code it used to get
   // (690109440766) decodes to other text, so it is not a name.
   static_assert(name_to_number("x--b") == 0);
   static_assert(name_to_number("a--b") == 0);
   CHECK(number_to_name(690109440766ull) == "x-wqulitcebeu");
   // A code whose last bits run past 64 was cut, and two texts shared it.
   static_assert(name_to_number("grryhn4d3x0") == 0);
   static_assert(name_to_number("sqhwqzfkinp") == 0);
   // Unchanged: not a name at all.
   static_assert(name_to_number("") == 0 && name_to_number("1abc") == 0 &&
                 name_to_number("-abc") == 0 && name_to_number("Abc") == 0 &&
                 name_to_number("abcdefghijklmnopqrs") == 0);
   static_assert(psio::name_id{"x--b"}.value == 0);
   // Every accepted text of a pseudo-random corpus reads back.
   std::uint64_t     state = 0x9e3779b97f4a7c15ull;
   const std::string alphabet = "abcdefghijklmnopqrstuvwxyz0123456789-";
   std::size_t       accepted = 0;
   for (int i = 0; i < 200000; ++i)
   {
      std::string text;
      state ^= state << 13, state ^= state >> 7, state ^= state << 17;
      const std::size_t len = 1 + state % 18;
      for (std::size_t j = 0; j < len; ++j)
      {
         state ^= state << 13, state ^= state >> 7, state ^= state << 17;
         text += alphabet[state % alphabet.size()];
      }
      if (const auto number = name_to_number(text))
      {
         ++accepted;
         if (number_to_name(number) != text)
            FAIL("name_to_number(" << text << ") = " << number << " decodes to "
                                   << number_to_name(number));
      }
   }
   CHECK(accepted > 1000);
}

TEST_CASE("method_to_number: compressed only when the number decodes to the name",
          "[consistency][name]")
{
   using namespace psio::detail;
   static_assert(method_to_number("spiderman") == 311625498215ull);
   static_assert(method_to_number("transfer") == method_to_number("Transfer"));
   CHECK(number_to_method(method_to_number("Transfer")) == "transfer");
   // The code of "getSymbolType" overflows 64 bits; the cut-off code decoded
   // to "getsymboltyp". It is a hash now, like any name that does not fit.
   static_assert(method_to_number("getSymbolType") == 0xd55df3070b8758ebull);
   static_assert(is_hash_name(method_to_number("getSymbolType")));
   CHECK(number_to_method(0x7ea96086faa7f7c0ull) == "getsymboltyp");   // the old, cut-off code
   static_assert(method_to_number("getsymboltyp") != 0x7ea96086faa7f7c0ull);
   CHECK(number_to_method(0xd55df3070b8758ebull) == "#hmlnslheseoypnnp");
   static_assert(method_to_number("#hmlnslheseoypnnp") == 0xd55df3070b8758ebull);
   // The hash is of the bytes as written: case matters there.
   static_assert(method_to_number("getSymbolType") != method_to_number("getsymboltype"));
   static_assert(is_hash_name(method_to_number("getsymboltype")));
   // Malformed hash text is not a name: 0 (it was 2^64 - 1).
   static_assert(method_to_number("#") == 0 && method_to_number("#abc") == 0 &&
                 method_to_number("#hmlnslheseoypnnpe") == 0 &&
                 method_to_number("#hmlnslheseoypnnz") == 0);
   // ... nor is well-formed hash text of a number that is not a hash number.
   static_assert(method_to_number("#eeeeeeeeeeeeeeee") == 0);
   // Outside the alphabet, empty: 0, as before.
   static_assert(method_to_number("") == 0 && method_to_number("a-b") == 0);
   // psio::hash_name / is_compressed_name are the same function.
   static_assert(psio::hash_name("getSymbolType") == 0xd55df3070b8758ebull);
   static_assert(!psio::is_compressed_name(psio::hash_name("getSymbolType")));

   std::uint64_t     state = 0x2545f4914f6cdd1dull;
   const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCXYZ_019";
   for (int i = 0; i < 200000; ++i)
   {
      std::string text;
      state ^= state << 13, state ^= state >> 7, state ^= state << 17;
      const std::size_t len = 1 + state % 16;
      for (std::size_t j = 0; j < len; ++j)
      {
         state ^= state << 13, state ^= state >> 7, state ^= state << 17;
         text += alphabet[state % alphabet.size()];
      }
      const auto number = method_to_number(text);
      REQUIRE(number != 0);
      if (method_to_number(number_to_method(number)) != number)
         FAIL("method_to_number(" << text << ") = " << number << " is not the number of "
                                  << number_to_method(number));
      if (!is_hash_name(number))
      {
         std::string canonical;
         for (unsigned char c : text)
            canonical += func_model::symbol_to_char[func_model::char_to_symbol[c]];
         if (number_to_method(number) != canonical)
            FAIL("method_to_number(" << text << ") decodes to " << number_to_method(number));
      }
   }
}

// ── PSIO's own Custom ids cannot be claimed, and sit on their wire shape ──
TEST_CASE("reserved custom ids", "[consistency][schema]")
{
   for (std::string_view id : {"bool", "char", "string", "utf8", "hex", "bitvector", "bitlist", "map",
                               "set", "box", "unknown", "time-point", "time-point-seconds",
                               "duration", "duration-1/60", "monotonic-millis"})
      CHECK(psio::reserved_custom_id(id));
   for (std::string_view id : {"", "Bool", "AccountNumber", "time", "durations", "time-points",
                               "monotonically", "hexadecimal", "Memo"})
      CHECK(!psio::reserved_custom_id(id));

   // A handler cannot be registered under one.
   psio::pssz_ir::CustomHandlers handlers;
   CHECK(!handlers.try_add("time-point-seconds", {}).ok());
   CHECK(!handlers.try_add("hex", {}).ok());
   CHECK(handlers.try_add("AccountNumber", {}).ok());
   CHECK_THROWS_AS(handlers.add("bool", psio::pssz_ir::CustomHandlers::Handler{}),
                   psio::codec_exception);
   CHECK(handlers.find("bool") == nullptr);
   CHECK(handlers.find("AccountNumber") != nullptr);

   // A schema in which a reserved id sits on another wire shape is malformed
   // for every schema-driven reader.
   using namespace psio::schema_types;
   auto refuses = [](AnyType root, const char* text) {
      const Schema      schema{};
      std::vector<char> out;
      bool              unknown = false;
      psio::pssz_ir::Value value;
      const std::vector<char> zero(8, 0);
      INFO(text);
      CHECK(!psio::pssz_ir::json_to_pssz(schema, root, psio::json{}, text, out).ok());
      CHECK(!psio::pssz_ir::validate_auto(schema, root, zero, &unknown).ok());
      CHECK(!psio::pssz_ir::validate_auto(schema, root, std::span<const char>{}, &unknown).ok());
   };
   auto u8list = [] { return AnyType{List{Box<AnyType>{AnyType{Int{8, false}}}}}; };
   refuses(AnyType{Custom{Box<AnyType>{u8list()}, "bool"}}, "true");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{32, false}}}, "bool"}}, "true");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{32, false}}}, "char"}}, "\"a\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "string"}}, "\"a\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "utf8"}}, "\"a\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "hex"}}, "\"00\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "bitlist"}}, "\"0\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "bitvector"}}, "\"0\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "map"}}, "{}");
   refuses(AnyType{Custom{Box<AnyType>{u8list()}, "map"}}, "{}");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, false}}}, "set"}}, "[]");
   refuses(AnyType{Custom{Box<AnyType>{u8list()}, "time-point-seconds"}}, "\"1970-01-01T00:00:00Z\"");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Float{11, 52}}}, "duration-millis"}}, "5");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, true}}}, "time-point"}}, "5");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{64, true}}}, "duration-bogus"}}, "5");
   refuses(AnyType{Custom{Box<AnyType>{AnyType{Int{32, false}}}, "unknown"}}, "5");
}

// ── An undeclared enumerator is a value in every C++ reader ──────────────
namespace cons_enum
{
   enum class Level : std::uint8_t { low = 0, high = 2 };
   PSIO_REFLECT_ENUM(Level, low, high)
   struct Holder
   {
      Level              level{};
      std::vector<Level> more;
   };
   PSIO_REFLECT(Holder, level, more)
}  // namespace cons_enum

TEST_CASE("an undeclared enum value is carried by every typed C++ codec", "[consistency][enum]")
{
   // docs/pssz-spec.md 4.5: the wire is the underlying integer, any value;
   // a C++ enum holds any value of its underlying type, so no C++ reader
   // refuses one (adding an enumerator stays compatible both ways).
   using namespace cons_enum;
   const Holder value{static_cast<Level>(7), {Level::high, static_cast<Level>(200)}};
   const auto   frame = psio::to_pssz(value);
   CHECK(psio::validate_pssz<Holder>(bytes_of(frame)).ok());
   CHECK(psio::validate_strict<Holder>(psio::pssz{}, bytes_of(frame)).ok());
   CHECK(psio::from_pssz<Holder>(bytes_of(frame)).level == static_cast<Level>(7));
   const auto key = psio::to_key(value);
   CHECK(psio::validate_key<Holder>(bytes_of(key)).ok());
   CHECK(psio::from_key<Holder>(bytes_of(key)).more[1] == static_cast<Level>(200));
   // Text: the name for a declared value, the number otherwise; both read.
   CHECK(psio::to_json(value) == R"({"level":7,"more":["high",200]})");
   CHECK(psio::from_json<Holder>(R"({"level":7,"more":["high",200]})").more[1] ==
         static_cast<Level>(200));
   CHECK(psio::from_ijson<Holder>(R"({"level":"7","more":[2]})").level == static_cast<Level>(7));
   // An unknown NAME has no value; a number outside the integer is not one.
   CHECK(!psio::validate_json<Holder>(R"({"level":"medium","more":[]})").ok());
   CHECK(!psio::validate_json<Holder>(R"({"level":256,"more":[]})").ok());
}
