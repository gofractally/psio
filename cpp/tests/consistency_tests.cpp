// Cross-language consistency round: one section per rule C++ was the looser
// or the inconsistent side of. Each section names the rule it pins.
#include <catch.hpp>

#include <psio/annotate.hpp>
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
