// Cross-language consistency round: one section per rule C++ was the looser
// or the inconsistent side of. Each section names the rule it pins.
#include <catch.hpp>

#include <psio/annotate.hpp>
#include <psio/bin.hpp>
#include <psio/ext_int.hpp>
#include <psio/frac.hpp>
#include <psio/ssz.hpp>
#include <psio/to_hex.hpp>
#include <psio/compress_name.hpp>
#include <psio/get_type_name.hpp>
#include <psio/name.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstdint>
#include <map>
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

// ── The key writer enforces bounds and UTF-8, like the pssz writer ───────
namespace cons_key
{
   struct Bounded
   {
      std::uint32_t            id = 0;
      std::string              name;
      std::vector<std::string> tags;
   };
   PSIO_REFLECT(Bounded, id, attr(name, utf8 | max<4>), attr(tags, max<2> | element(max<3>)))
   struct Wrapped
   {
      psio::utf8_string<4>                    name;
      psio::bounded<std::uint16_t, 2>         codes;
      std::optional<psio::utf8_string<4>>     alias;
      std::vector<std::tuple<std::uint8_t, Bounded>> rows;
   };
   PSIO_REFLECT(Wrapped, name, codes, alias, rows)
   struct Plain
   {
      std::uint32_t            id = 0;
      std::string              name;
      std::vector<std::string> tags;
   };
   PSIO_REFLECT(Plain, id, name, tags)
}  // namespace cons_key

TEST_CASE("key: the writer refuses a value outside a bound or not UTF-8", "[consistency][key]")
{
   using namespace cons_key;
   namespace ki = psio::detail::key_impl;
   // Keys without bounds are not checked at all.
   STATIC_REQUIRE(!ki::has_writer_checks<Plain, false>());
   STATIC_REQUIRE(!ki::has_writer_checks<std::tuple<std::uint32_t, std::string>, false>());
   STATIC_REQUIRE(!ki::has_writer_checks<std::uint64_t, true>());
   STATIC_REQUIRE(ki::has_writer_checks<Bounded, false>());
   STATIC_REQUIRE(ki::has_writer_checks<Wrapped, false>());
   STATIC_REQUIRE(ki::has_writer_checks<std::vector<std::optional<Bounded>>, false>());

   auto refused = [](const auto& value, std::string_view message) {
      INFO(message);
      // The status form, the throwing forms, the helper, the sized write.
      std::vector<char> out{'x'};
      const auto        status = psio::encode_key_checked(value, out);
      REQUIRE(!status.ok());
      CHECK(std::string_view{status.error().what} == message);
      CHECK(out == std::vector<char>{'x'});
      CHECK_THROWS_AS(psio::encode(psio::key{}, value), psio::codec_exception);
      CHECK_THROWS_AS(psio::encode(psio::key{}, value, out), psio::codec_exception);
      CHECK_THROWS_AS(psio::to_key(value), psio::codec_exception);
      std::vector<char>     buffer(256);
      psio::fast_buf_stream sink{buffer.data(), buffer.size()};
      CHECK_THROWS_AS(psio::encode(psio::key{}, value, sink), psio::codec_exception);
      // The pssz writer refuses the same value.
      std::vector<char> frame;
      CHECK(!psio::encode_pssz_checked(psio::pssz{}, value, frame).ok());
   };
   auto accepted = [](const auto& value) {
      std::vector<char> out;
      REQUIRE(psio::encode_key_checked(value, out).ok());
      const auto key = psio::to_key(value);
      CHECK(out == std::vector<char>(key.begin(), key.end()));
      CHECK(psio::validate_key<std::remove_cvref_t<decltype(value)>>(bytes_of(key)).ok());
   };

   accepted(Bounded{1, "abcd", {"abc", "x"}});
   refused(Bounded{1, "abcde", {}}, "key: member length_bound violated");
   refused(Bounded{1, "\xff", {}}, "utf8_spec: invalid leading byte");
   refused(Bounded{1, "", {"a", "b", "c"}}, "key: member length_bound violated");
   refused(Bounded{1, "", {"abcd"}}, "key: member length_bound violated");

   Wrapped good;
   good.name  = psio::utf8_string<4>{"abcd"};
   good.codes = psio::bounded<std::uint16_t, 2>{std::vector<std::uint16_t>{1, 2}};
   good.rows.emplace_back(std::uint8_t{1}, Bounded{1, "ab", {"x"}});
   accepted(good);
   {
      Wrapped bad = good;
      bad.name = psio::utf8_string<4>{"\xc0\x80"};
      refused(bad, "key: utf8_string is not valid UTF-8");
      bad = good;
      bad.alias = psio::utf8_string<4>{"\xff"};
      refused(bad, "key: utf8_string is not valid UTF-8");
      bad = good;
      bad.codes.storage() = {1, 2, 3};
      refused(bad, "key: bounded list exceeds its bound");
      bad = good;
      std::get<1>(bad.rows[0]).name = "abcde";   // inside a list, a tuple and a record
      refused(bad, "key: member length_bound violated");
   }

   // What the writer refuses, the reader rejects: the key bytes an unchecked
   // writer would have produced for the same value.
   const auto over = psio::to_key(Plain{1, "abcde", {}});
   CHECK(!psio::validate_key<Bounded>(bytes_of(over)).ok());
   const auto raw = psio::to_key(Plain{1, "\xff", {}});
   CHECK(!psio::validate_key<Bounded>(bytes_of(raw)).ok());

   // A prefix element is checked as the element it is.
   using Key = std::tuple<psio::utf8_string<4>, std::uint32_t>;
   CHECK_NOTHROW(psio::key_prefix<Key>(psio::utf8_string<4>{"abcd"}));
   CHECK_THROWS_AS(psio::key_prefix<Key>(psio::utf8_string<4>{"\xff"}), psio::codec_exception);

   // The sizing pass checks nothing (the write that follows does).
   CHECK(psio::key_size(Bounded{1, "abcde", {}}) == over.size());
}

// ── A text-only as<Tag> does not reach the binary formats ────────────────
namespace cons_as
{
   // as<hex_tag> on a wide integer selects its "0x" text in JSON / I-JSON.
   struct Wide
   {
      std::uint32_t id = 0;
      psio::uint128 amount{};
      psio::uint256 hash{};
      std::string   note;
   };
   PSIO_REFLECT(Wide, id, attr(amount, as<hex_tag>), attr(hash, as<hex_tag>), note)
   // The same record without the presentation.
   struct Plain
   {
      std::uint32_t id = 0;
      psio::uint128 amount{};
      psio::uint256 hash{};
      std::string   note;
   };
   PSIO_REFLECT(Plain, id, amount, hash, note)
   struct WideFixed
   {
      psio::uint128 amount{};
   };
   PSIO_REFLECT(WideFixed, definitionWillNotChange(), attr(amount, as<hex_tag>))
}  // namespace cons_as

TEST_CASE("as<hex_tag> on a wide integer is a text presentation only", "[consistency][adapter]")
{
   using namespace cons_as;
   Wide w;
   w.id     = 7;
   w.amount = (static_cast<psio::uint128>(1) << 100) + 5;
   w.note   = "n";
   const Plain p{w.id, w.amount, w.hash, w.note};

   // JSON: the member is written in its "0x" form and read back.
   const auto text = psio::to_json(w);
   CHECK(text.find("\"amount\":\"0x") != std::string::npos);
   CHECK(psio::to_json(p).find("0x") == std::string::npos);
   CHECK(psio::from_json<Wide>(sp(text)).amount == w.amount);
   CHECK(psio::from_ijson<Wide>(sp(psio::to_ijson(w))).amount == w.amount);

   // Every binary format: exactly the bytes of the record without it.
   STATIC_REQUIRE(psio::has_as_override_v<psio::effective_annotations_for<
                     Wide, psio::uint128, &Wide::amount>::value_t>);
   STATIC_REQUIRE(!psio::has_binary_as_override_v<
                  psio::uint128,
                  psio::effective_annotations_for<Wide, psio::uint128, &Wide::amount>::value_t>);
   STATIC_REQUIRE(psio::max_encoded_size<Wide>() == psio::max_encoded_size<Plain>());
   STATIC_REQUIRE(psio::auto_pssz_width_v<Wide> == psio::auto_pssz_width_v<Plain>);
   STATIC_REQUIRE(psio::max_encoded_size<WideFixed>() == std::optional<std::size_t>{16});
   const auto frame = psio::to_pssz(w);
   CHECK(std::vector<char>(frame.begin(), frame.end()) ==
         [&] { auto b = psio::to_pssz(p); return std::vector<char>(b.begin(), b.end()); }());
   CHECK(psio::validate_pssz<Wide>(bytes_of(frame)).ok());
   CHECK(psio::validate_strict<Wide>(psio::pssz{}, bytes_of(frame)).ok());
   CHECK(psio::from_pssz<Wide>(bytes_of(frame)).amount == w.amount);
   CHECK(psio::to_pssz(WideFixed{w.amount}).size() == 16);
   CHECK(psio::encode(psio::key{}, w) == psio::encode(psio::key{}, p));
   CHECK(psio::from_key<Wide>(bytes_of(psio::to_key(w))).amount == w.amount);
   CHECK(psio::encode(psio::bin{}, w) == psio::encode(psio::bin{}, p));
   CHECK(psio::decode<Wide>(psio::bin{}, bytes_of(psio::encode(psio::bin{}, w))).amount == w.amount);
   CHECK(psio::encode(psio::ssz{}, w) == psio::encode(psio::ssz{}, p));
   CHECK(psio::encode(psio::experimental::frac32{}, w) ==
         psio::encode(psio::experimental::frac32{}, p));

   // The Schema IR: the members are the plain integers (the builder gives
   // the two records the same member types).
   auto object_of = []<typename T>() {
      psio::SchemaBuilder builder;
      builder.insert<T>("R");
      const auto schema = std::move(builder).build();
      for (const auto& entry : schema.types)
         if (auto object = std::get_if<psio::schema_types::Object>(&entry.type->value))
            return *object;
      return psio::schema_types::Object{};
   };
   CHECK(object_of.operator()<Wide>().members.size() == 4);
   CHECK(object_of.operator()<Wide>() == object_of.operator()<Plain>());
}

// ── pssz_evolves_from compares the annotations that are part of the type ──
namespace cons_evo
{
#define CONS_EVO(NAME, OWNER, TAGS, ...)                                    \
   struct NAME                                                              \
   {                                                                        \
      std::uint64_t            id = 0;                                      \
      std::string              owner;                                       \
      std::vector<std::string> tags;                                        \
      PSIO_REFLECT(NAME, id, attr(owner, OWNER), attr(tags, TAGS) __VA_OPT__(, ) __VA_ARGS__) \
   };
   CONS_EVO(V1, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(Same, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(Wider, utf8 | max<64>, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(Tighter, utf8 | max<16>, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(Raw, max<32>, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(ElemWider, utf8 | max<32>, max<8> | element(max<20>), maxDynamicData(1024))
   CONS_EVO(ElemTighter, utf8 | max<32>, max<8> | element(max<8>), maxDynamicData(1024))
   CONS_EVO(ElemUtf8, utf8 | max<32>, max<8> | element(utf8 | max<16>), maxDynamicData(1024))
   CONS_EVO(CountWider, utf8 | max<32>, max<12> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(WithMin, utf8 | max<32> | min<1>, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(Unbounded, utf8, max<8> | element(max<16>), maxDynamicData(1024))
   CONS_EVO(Sorted, utf8 | max<32>, max<8> | sorted | element(max<16>), maxDynamicData(1024))
   CONS_EVO(CapWider, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(4096))
   CONS_EVO(CapTighter, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(512))
   CONS_EVO(CapOtherWidth, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(100000))
   // Without a cap every type has the width of its own bound: here the
   // record is 2 bytes wide and its list of tags 1. A bound that moves the
   // list into the next width class is not compatible although the record's
   // own width stays.
   CONS_EVO(Open, utf8 | max<300>, max<8> | element(max<16>))
   CONS_EVO(OpenWideList, utf8 | max<300>, max<8> | element(max<40>))
   CONS_EVO(OpenWider, utf8 | max<400>, max<8> | element(max<16>))
   CONS_EVO(OpenCapped, utf8 | max<300>, max<8> | element(max<16>), maxDynamicData(600))
   CONS_EVO(OpenCappedLow, utf8 | max<300>, max<8> | element(max<16>), maxDynamicData(400))
#undef CONS_EVO

   struct Fields3
   {
      std::uint32_t                a = 0;
      std::optional<std::uint32_t> b;
      PSIO_REFLECT(Fields3, a, b, maxFields(3), maxDynamicData(64))
   };
   struct Fields3Grown
   {
      std::uint32_t                a = 0;
      std::optional<std::uint32_t> b, c;
      PSIO_REFLECT(Fields3Grown, a, b, c, maxFields(3), maxDynamicData(64))
   };
   struct Fields3Raised   // within its own cap, beyond the original's
   {
      std::uint32_t                a = 0;
      std::optional<std::uint32_t> b, c, d;
      PSIO_REFLECT(Fields3Raised, a, b, c, d, maxFields(8), maxDynamicData(64))
   };
   struct Fields3Dropped
   {
      std::uint32_t                a = 0;
      std::optional<std::uint32_t> b, c;
      PSIO_REFLECT(Fields3Dropped, a, b, c, maxDynamicData(64))
   };
   struct Fields3Lowered   // lowered to the current count: the promise still holds
   {
      std::uint32_t                a = 0;
      std::optional<std::uint32_t> b;
      PSIO_REFLECT(Fields3Lowered, a, b, maxFields(2), maxDynamicData(64))
   };
}  // namespace cons_evo

TEST_CASE("pssz_evolves_from: member annotations and caps", "[consistency][evolves]")
{
   using namespace cons_evo;
   using psio::pssz_evolves_from;
   STATIC_REQUIRE(pssz_evolves_from<V1, Same>);
   // A bound may only widen (the new reader must read every old record),
   // and only while every width stays the same.
   STATIC_REQUIRE(pssz_evolves_from<V1, Wider>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, Tighter>);
   STATIC_REQUIRE(!pssz_evolves_from<Wider, V1>);
   STATIC_REQUIRE(pssz_evolves_from<V1, ElemWider>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, ElemTighter>);
   STATIC_REQUIRE(pssz_evolves_from<V1, CountWider>);
   STATIC_REQUIRE(!pssz_evolves_from<CountWider, V1>);
   STATIC_REQUIRE(pssz_evolves_from<V1, Unbounded>);      // under the cap the widths are the cap's
   STATIC_REQUIRE(!pssz_evolves_from<Unbounded, V1>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, WithMin>);       // a new lower bound refuses old values
   STATIC_REQUIRE(pssz_evolves_from<WithMin, V1>);
   // utf8 / raw and sorted are the member's type.
   STATIC_REQUIRE(!pssz_evolves_from<V1, Raw>);
   STATIC_REQUIRE(!pssz_evolves_from<Raw, V1>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, ElemUtf8>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, Sorted>);
   STATIC_REQUIRE(!pssz_evolves_from<Sorted, V1>);
   // The cap may grow within its width class and may not shrink.
   STATIC_REQUIRE(pssz_evolves_from<V1, CapWider>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, CapTighter>);
   STATIC_REQUIRE(!pssz_evolves_from<V1, CapOtherWidth>);
   // Adding a cap: only one the old type's inferred bound fits.
   STATIC_REQUIRE(*psio::max_encoded_size<Open>() <= 600);
   STATIC_REQUIRE(*psio::max_encoded_size<Open>() > 400);
   STATIC_REQUIRE(pssz_evolves_from<Open, OpenCapped>);
   STATIC_REQUIRE(!pssz_evolves_from<Open, OpenCappedLow>);
   STATIC_REQUIRE(pssz_evolves_from<OpenCapped, Open>);   // dropping it: nothing old is refused
   // Without a cap, a widened bound that moves a nested list's offset table
   // into another width class changes the bytes.
   STATIC_REQUIRE(psio::auto_pssz_width_v<Open> == psio::auto_pssz_width_v<OpenWideList>);
   STATIC_REQUIRE(!pssz_evolves_from<Open, OpenWideList>);
   STATIC_REQUIRE(pssz_evolves_from<Open, OpenWider>);

   // What the check promises, on bytes: every record of V1 is a record of
   // Wider, and a V1 record at its bound is not a record of Tighter.
   V1 full;
   full.owner = std::string(32, 'a');
   full.tags  = {std::string(16, 'b')};
   const auto frame = psio::to_pssz(full);
   CHECK(psio::validate_pssz<Wider>(bytes_of(frame)).ok());
   CHECK(psio::validate_pssz<CapWider>(bytes_of(frame)).ok());
   CHECK(!psio::validate_pssz<Tighter>(bytes_of(frame)).ok());
   CHECK(!psio::validate_pssz<ElemTighter>(bytes_of(frame)).ok());

   // maxFields: a revision has no more fields than the original's cap and
   // does not raise or drop it.
   STATIC_REQUIRE(pssz_evolves_from<Fields3, Fields3Grown>);
   STATIC_REQUIRE(!pssz_evolves_from<Fields3, Fields3Raised>);
   STATIC_REQUIRE(!pssz_evolves_from<Fields3, Fields3Dropped>);
   STATIC_REQUIRE(pssz_evolves_from<Fields3, Fields3Lowered>);
   STATIC_REQUIRE(!pssz_evolves_from<Fields3Lowered, Fields3Grown>);
}

// ── sorted / unique / unique_keys are enforced by every codec ────────────
namespace cons_order
{
   struct Ranked
   {
      std::vector<std::uint32_t>                         scores;    // sorted
      std::vector<std::string>                           names;     // strictly ascending
      std::vector<std::int32_t>                          falling;   // descending
      std::vector<std::pair<std::string, std::uint32_t>> entries;   // unique by .first
      std::vector<std::vector<std::uint8_t>>             rows;      // every row sorted
      std::array<std::int16_t, 3>                        triple{};  // sorted
      friend bool operator==(const Ranked&, const Ranked&) = default;
   };
   PSIO_REFLECT(Ranked,
                attr(scores, sorted | max<8>),
                attr(names, unique),
                attr(falling, sorted_spec{.ascending = false}),
                attr(entries, unique_keys),
                attr(rows, element(sorted)),
                attr(triple, sorted))
   // The same record with no constraint: what an unchecked writer produces.
   struct Loose
   {
      std::vector<std::uint32_t>                         scores;
      std::vector<std::string>                           names;
      std::vector<std::int32_t>                          falling;
      std::vector<std::pair<std::string, std::uint32_t>> entries;
      std::vector<std::vector<std::uint8_t>>             rows;
      std::array<std::int16_t, 3>                        triple{};
   };
   PSIO_REFLECT(Loose, attr(scores, max<8>), names, falling, entries, rows, triple)

   inline Ranked good()
   {
      return Ranked{{1, 1, 2, 300}, {"a", "ab", "b"}, {5, 5, -1}, {{"x", 1}, {"a", 1}},
                    {{1, 2, 2}, {}, {9}}, {-5, 0, 0}};
   }
   inline Loose loose(const Ranked& r)
   {
      return Loose{r.scores, r.names, r.falling, r.entries, r.rows, r.triple};
   }
}  // namespace cons_order

TEST_CASE("sorted / unique / unique_keys: writers and checking readers", "[consistency][order]")
{
   using namespace cons_order;
   const Ranked ok = good();
   const auto   frame = psio::to_pssz(ok);
   const auto   text  = psio::to_json(ok);
   const auto   key   = psio::to_key(ok);
   CHECK(psio::validate_pssz<Ranked>(bytes_of(frame)).ok());
   CHECK(psio::validate_strict<Ranked>(psio::pssz{}, bytes_of(frame)).ok());
   CHECK(psio::from_pssz<Ranked>(bytes_of(frame)) == ok);
   CHECK(psio::from_json<Ranked>(sp(text)) == ok);
   CHECK(psio::validate_key<Ranked>(bytes_of(key)).ok());
   CHECK(psio::from_key<Ranked>(bytes_of(key)) == ok);
   // The constraints change no byte and no width.
   CHECK(std::vector<char>(frame.begin(), frame.end()) ==
         [&] { auto b = psio::to_pssz(loose(ok)); return std::vector<char>(b.begin(), b.end()); }());
   STATIC_REQUIRE(psio::max_encoded_size<Ranked>() == psio::max_encoded_size<Loose>());

   auto refused = [](const char* what, auto&& change, std::string_view message) {
      INFO(what);
      Ranked bad = good();
      change(bad);
      // Writers.
      std::vector<char> out;
      const auto        status = psio::encode_pssz_checked(psio::pssz{}, bad, out);
      REQUIRE(!status.ok());
      CHECK(std::string_view{status.error().what} == message);
      CHECK_THROWS_AS(psio::to_pssz(bad), psio::codec_exception);
      std::string json;
      CHECK(!psio::encode_json_checked(psio::json{}, bad, json).ok());
      CHECK(!psio::encode_json_checked(psio::ijson{}, bad, json).ok());
      CHECK(!psio::encode_key_checked(bad, out).ok());
      CHECK_THROWS_AS(psio::to_key(bad), psio::codec_exception);
      // Readers, on what an unchecked writer produces.
      const Loose raw      = loose(bad);
      const auto  frame    = psio::to_pssz(raw);
      const auto  raw_text = psio::to_json(raw);
      const auto  raw_key  = psio::to_key(raw);
      const auto  st       = psio::validate_pssz<Ranked>(bytes_of(frame));
      REQUIRE(!st.ok());
      CHECK(std::string_view{st.error().what} == message);
      CHECK(!psio::validate_strict<Ranked>(psio::pssz{}, bytes_of(frame)).ok());
      CHECK_THROWS_AS(psio::from_pssz<Ranked>(bytes_of(frame)), psio::codec_exception);
      CHECK_THROWS_AS(psio::decode<Ranked>(psio::pssz{}, bytes_of(frame)), psio::codec_exception);
      Ranked decoded;
      bool   unknown = false;
      CHECK(!psio::decode_pssz_checked(psio::pssz{}, bytes_of(frame), decoded, unknown).ok());
      CHECK(!psio::validate_json<Ranked>(sp(raw_text)).ok());
      CHECK(!psio::validate<Ranked>(psio::ijson{}, sp(psio::to_ijson(raw))).ok());
      CHECK(!psio::decode_json_checked(psio::json{}, sp(raw_text), decoded).ok());
      CHECK(!psio::validate_key<Ranked>(bytes_of(raw_key)).ok());
      CHECK_THROWS_AS(psio::from_key<Ranked>(bytes_of(raw_key)), psio::codec_exception);
   };
   const std::string_view order  = "sorted: elements are not in order";
   const std::string_view strict = "sorted: elements are not in strictly ascending order";
   refused("scores descend", [](Ranked& r) { r.scores = {2, 1}; }, order);
   refused("names repeat", [](Ranked& r) { r.names = {"a", "a"}; }, strict);
   refused("names descend", [](Ranked& r) { r.names = {"b", "a"}; }, strict);
   refused("falling ascends", [](Ranked& r) { r.falling = {1, 2}; }, order);
   refused("key order, not signed order", [](Ranked& r) { r.falling = {-1, 1}; }, order);
   refused("entries repeat a key", [](Ranked& r) { r.entries = {{"a", 1}, {"b", 2}, {"a", 3}}; },
           "unique_keys: two elements have the same key");
   refused("a row descends", [](Ranked& r) { r.rows = {{1}, {3, 2}}; }, order);
   refused("the array descends", [](Ranked& r) { r.triple = {1, 0, 2}; }, order);

   // A map / set is ordered and unique by type; the specs it carries
   // inherently are not checked a second time.
   using Inherent = std::tuple<psio::sorted_spec, psio::unique_keys_spec>;
   STATIC_REQUIRE(!psio::detail::ann::has_value_checks_v<std::map<std::string, int>, Inherent>);
   STATIC_REQUIRE(psio::detail::ann::has_value_checks_v<std::vector<int>, Inherent>);
}

// ── maxFields(N) caps the number of fields ───────────────────────────────
namespace cons_fields
{
   struct Capped
   {
      std::uint32_t                a = 0;
      std::optional<std::string>   b;
      PSIO_REFLECT(Capped, a, b, maxFields(4))
   };
   struct Service
   {
      void put(std::uint32_t id, std::string text);
      void plain(std::uint32_t id);
      PSIO_REFLECT(Service,
                   method(put, id, attr(text, max<8>), maxFields(4), maxDynamicData(64)),
                   method(plain, id))
   };
   using PutArgs = psio::reflect<Service>::method_arg_tuple<0>;
   struct ServiceApi {};
}  // namespace cons_fields

TEST_CASE("maxFields: the cap on the number of fields is enforced", "[consistency][fields]")
{
   using namespace cons_fields;
   STATIC_REQUIRE(psio::max_fields_v<Capped> == std::optional<std::size_t>{4});
   STATIC_REQUIRE(psio::effective_max_fields_v<Capped> == std::optional<std::size_t>{4});
   STATIC_REQUIRE(psio::effective_max_fields_v<cons_cap::Capped> == std::optional<std::size_t>{2});
   // A method's argument list takes the same cap.
   STATIC_REQUIRE(psio::reflect<Service>::method_max_fields<0> == std::optional<std::size_t>{4});
   STATIC_REQUIRE(!psio::reflect<Service>::method_max_fields<1>.has_value());
   STATIC_REQUIRE(psio::max_fields_v<PutArgs> == std::optional<std::size_t>{4});
   STATIC_REQUIRE(psio::max_dynamic_data_v<PutArgs> == std::optional<std::size_t>{64});

   // In the Schema IR it is the Object's attribute, beside maxDynamicData.
   using namespace psio::schema_types;
   psio::SchemaBuilder builder;
   builder.insert<Capped>("Capped");
   builder.insert<PutArgs>("PutArgs");
   Schema              schema = std::move(builder).build();
   auto attribute = [&](std::string_view type, std::string_view name) -> std::optional<std::string> {
      const AnyType* node = schema.get(type);
      while (node)
      {
         if (auto ref = std::get_if<Type>(&node->value))
         {
            node = schema.get(ref->type);
            continue;
         }
         if (auto object = std::get_if<Object>(&node->value))
            for (const auto& a : object->attributes)
               if (a.name == name)
                  return a.value;
         break;
      }
      return std::nullopt;
   };
   CHECK(attribute("Capped", "maxFields") == std::optional<std::string>{"4"});
   CHECK(attribute("PutArgs", "maxFields") == std::optional<std::string>{"4"});
   CHECK(attribute("PutArgs", "maxDynamicData") == std::optional<std::string>{"64"});
   CHECK(psio::pssz_ir::validate_schema(schema).ok());

   // A schema-driven reader refuses a record with more members than its
   // maxFields, more than 255 members, or a maxFields above 255.
   auto record = [](std::size_t members, std::optional<std::string> max_fields) {
      Object o;
      for (std::size_t i = 0; i < members; ++i)
         o.members.push_back(
            Member{"m" + std::to_string(i), Box<AnyType>{AnyType{Int{8, false}}}, {}});
      if (max_fields)
         o.attributes.push_back(Attribute{"maxFields", *max_fields});
      return AnyType{std::move(o)};
   };
   auto reads = [](const AnyType& root) {
      // Any schema-driven entry point starts by checking the schema.
      const Schema empty{};
      std::size_t  width = 0;
      return psio::pssz_ir::auto_width(empty, root, width).ok();
   };
   CHECK(reads(record(3, std::nullopt)));
   CHECK(reads(record(3, "3")));
   CHECK(reads(record(3, "255")));
   CHECK(!reads(record(4, "3")));
   CHECK(!reads(record(3, "256")));
   CHECK(!reads(record(3, "many")));
   CHECK(reads(record(255, std::nullopt)));
   CHECK(!reads(record(256, std::nullopt)));

   // validate_schema: a function's parameter list is a tuple.
   auto with_params = [&](std::size_t params, std::vector<Attribute> attributes) {
      Schema    out = schema;
      Interface api;
      api.name = "api";
      Func f;
      f.name = "call";
      for (std::size_t i = 0; i < params; ++i)
         f.params.push_back(
            Member{"p" + std::to_string(i), Box<AnyType>{AnyType{Int{32, false}}}, {}});
      f.attributes = std::move(attributes);
      api.funcs.push_back(std::move(f));
      out.interfaces.push_back(std::move(api));
      return out;
   };
   CHECK(psio::pssz_ir::validate_schema(with_params(255, {})).ok());
   CHECK(!psio::pssz_ir::validate_schema(with_params(256, {})).ok());
   CHECK(psio::pssz_ir::validate_schema(with_params(2, {Attribute{"maxFields", "2"}})).ok());
   CHECK(!psio::pssz_ir::validate_schema(with_params(3, {Attribute{"maxFields", "2"}})).ok());
   CHECK(!psio::pssz_ir::validate_schema(with_params(1, {Attribute{"maxFields", "256"}})).ok());
   CHECK(!psio::pssz_ir::validate_schema(with_params(1, {Attribute{"maxDynamicData", "x"}})).ok());
}

// ── A transparent type may bound its member: it is then that bounded type ─
namespace cons_tr
{
   namespace ann
   {
      // psibase's Memo: a first-class type that is UTF-8 and at most 80 bytes.
      struct Memo
      {
         std::string text;
         friend bool operator==(const Memo&, const Memo&)  = default;
         friend auto operator<=>(const Memo&, const Memo&) = default;
         PSIO_REFLECT(Memo, transparent(), attr(text, utf8 | max<80>))
      };
      struct Tags   // bounds below the member
      {
         std::vector<std::string> list;
         friend bool operator==(const Tags&, const Tags&) = default;
         PSIO_REFLECT(Tags, transparent(), attr(list, max<4> | element(utf8 | max<8>)))
      };
   }  // namespace ann
   namespace wrap
   {
      // The same types spelled with wrapper types.
      struct Memo
      {
         psio::utf8_string<80> text;
         friend bool operator==(const Memo&, const Memo&)  = default;
         friend auto operator<=>(const Memo&, const Memo&) = default;
         PSIO_REFLECT(Memo, transparent(), text)
      };
      struct Tags
      {
         psio::bounded<psio::utf8_string<8>, 4> list;
         PSIO_REFLECT(Tags, transparent(), list)
      };
   }  // namespace wrap
   template <typename M, typename T>
   struct Holder
   {
      std::uint32_t                  id = 0;
      M                              memo;
      std::optional<M>               maybe;
      std::vector<M>                 many;
      std::map<std::uint8_t, M>      by_id;
      T                              tags;
      PSIO_REFLECT(Holder, id, memo, maybe, many, by_id, tags)
   };
   using AnnHolder  = Holder<ann::Memo, ann::Tags>;
   using WrapHolder = Holder<wrap::Memo, wrap::Tags>;
   struct Unbounded
   {
      std::string text;
      PSIO_REFLECT(Unbounded, transparent(), text)
   };
   namespace wider
   {
      struct Memo
      {
         std::string text;
         PSIO_REFLECT(Memo, transparent(), attr(text, utf8 | max<120>))
      };
   }
   namespace raw
   {
      struct Memo
      {
         std::string text;
         PSIO_REFLECT(Memo, transparent(), attr(text, max<80>))
      };
   }

   struct Small
   {
      ann::Memo a;
      PSIO_REFLECT(Small, a)
   };

   template <typename T>
   std::vector<char> ir_of(std::string name)
   {
      psio::SchemaBuilder builder;
      builder.insert<T>(std::move(name));
      const auto bytes = psio::to_pssz(std::move(builder).build());
      return std::vector<char>(bytes.begin(), bytes.end());
   }
   // The node of `name` with every reference to an anonymous (@N) type
   // replaced by that type's node: what the schema says, whichever of its
   // parts happen to be interned.
   inline void inline_anonymous(const psio::Schema& schema, psio::schema_types::AnyType& node,
                                int depth = 0)
   {
      using namespace psio::schema_types;
      if (depth > 32)
         return;
      if (auto ref = std::get_if<Type>(&node.value); ref && ref->type.starts_with("@"))
      {
         if (const AnyType* target = schema.get(ref->type))
         {
            AnyType copy = *target;
            node         = std::move(copy);
            inline_anonymous(schema, node, depth + 1);
         }
         return;
      }
      std::visit(
         [&](auto& t) {
            if constexpr (requires { t.type.get(); })
            {
               if (t.type.get())
                  inline_anonymous(schema, *t.type, depth + 1);
            }
            else if constexpr (requires { t.members; })
               for (auto& member : t.members)
               {
                  if constexpr (requires { member.type; })
                  {
                     if (member.type.get())
                        inline_anonymous(schema, *member.type, depth + 1);
                  }
                  else if (member.get())
                     inline_anonymous(schema, *member, depth + 1);
               }
         },
         node.value);
   }
   template <typename T>
   std::string node_of(const std::string& name, std::initializer_list<const char*> also = {})
   {
      psio::SchemaBuilder builder;
      builder.insert<T>(name);
      const auto schema = std::move(builder).build();
      std::string out;
      auto add = [&](const std::string& n) {
         psio::schema_types::AnyType node = *schema.get(n);
         inline_anonymous(schema, node);
         out += n + "=" + psio::to_json(node) + ";";
      };
      add(name);
      for (const char* n : also)
         add(n);
      return out;
   }
   template <typename B>
   std::vector<char> vec(const B& b) { return std::vector<char>(b.begin(), b.end()); }
}  // namespace cons_tr

TEST_CASE("transparent: a bounded member makes the wrapper that bounded type",
          "[consistency][transparent]")
{
   using namespace cons_tr;
   // Width inference sees the bound, exactly as it sees utf8_string<80>.
   STATIC_REQUIRE(psio::is_transparent_v<ann::Memo>);
   STATIC_REQUIRE(psio::max_encoded_size<ann::Memo>() == std::optional<std::size_t>{80});
   STATIC_REQUIRE(psio::max_encoded_size<ann::Memo>() == psio::max_encoded_size<wrap::Memo>());
   STATIC_REQUIRE(psio::max_encoded_size<ann::Tags>() == psio::max_encoded_size<wrap::Tags>());
   STATIC_REQUIRE(psio::max_encoded_size<ann::Tags>().has_value());
   STATIC_REQUIRE(!psio::max_encoded_size<Unbounded>().has_value());
   STATIC_REQUIRE(psio::max_encoded_size<AnnHolder>() == psio::max_encoded_size<WrapHolder>());
   STATIC_REQUIRE(psio::auto_pssz_width_v<AnnHolder> == psio::auto_pssz_width_v<WrapHolder>);
   STATIC_REQUIRE(psio::max_encoded_size<std::vector<ann::Memo>>() ==
                  psio::max_encoded_size<std::vector<wrap::Memo>>());
   STATIC_REQUIRE(psio::auto_pssz_width_v<Small> == 1);   // 80 bytes, a header and a slot

   // The same Schema IR as the wrapper-type spelling: a named alias of the
   // bounded UTF-8 string / the bounded list of bounded UTF-8 strings.
   // The Memo schema is byte for byte the wrapper spelling's:
   //    Memo -> @1,  @1 = Custom{BoundedList{u8, 80}, "utf8"}
   CHECK(ir_of<ann::Memo>("Memo") == ir_of<wrap::Memo>("Memo"));
   CHECK(node_of<ann::Memo>("Memo") ==
         R"(Memo={"value":[11,{"type":{"value":[4,{"type":{"value":[8,{"bits":8,"isSigned":false}]},"maxCount":80}]},"id":"utf8"}]};)");
   // Deeper bounds are the same nodes; which of them is interned as an
   // anonymous type differs between the spellings, as it does for an
   // annotated record member.
   CHECK(node_of<ann::Tags>("Tags") == node_of<wrap::Tags>("Tags"));
   CHECK(node_of<AnnHolder>("Holder", {"Memo", "Tags"}) ==
         node_of<WrapHolder>("Holder", {"Memo", "Tags"}));
   CHECK(node_of<ann::Memo>("Memo") != node_of<Unbounded>("Memo"));
   CHECK(node_of<ann::Memo>("Memo") != node_of<raw::Memo>("Memo"));

   // The same bytes, key and text, bare and wherever it is held.
   AnnHolder a;
   a.id    = 7;
   a.memo  = ann::Memo{"h\xc3\xa9llo"};
   a.maybe = ann::Memo{""};
   a.many  = {ann::Memo{"a"}, ann::Memo{std::string(80, 'x')}};
   a.by_id = {{1, ann::Memo{"one"}}};
   a.tags  = ann::Tags{{"t1", "t2"}};
   WrapHolder w;
   w.id    = 7;
   w.memo  = wrap::Memo{psio::utf8_string<80>{"h\xc3\xa9llo"}};
   w.maybe = wrap::Memo{psio::utf8_string<80>{""}};
   w.many  = {wrap::Memo{psio::utf8_string<80>{"a"}},
              wrap::Memo{psio::utf8_string<80>{std::string(80, 'x')}}};
   w.by_id = {{1, wrap::Memo{psio::utf8_string<80>{"one"}}}};
   w.tags.list.push_back(psio::utf8_string<8>{"t1"});
   w.tags.list.push_back(psio::utf8_string<8>{"t2"});
   const auto frame = vec(psio::to_pssz(a));
   CHECK(frame == vec(psio::to_pssz(w)));
   CHECK(vec(psio::to_pssz(a.memo)) == vec(psio::to_pssz(w.memo)));
   CHECK(vec(psio::to_pssz(a.memo)) == vec(psio::to_pssz(psio::utf8_string<80>{"h\xc3\xa9llo"})));
   CHECK(vec(psio::to_key(a)) == vec(psio::to_key(w)));
   CHECK(psio::to_json(a) == psio::to_json(w));
   CHECK(psio::to_ijson(a.memo) == "\"h\xc3\xa9llo\"");
   CHECK(psio::validate_pssz<AnnHolder>(frame).ok());
   CHECK(psio::validate_strict<AnnHolder>(psio::pssz{}, frame).ok());
   CHECK(psio::from_pssz<AnnHolder>(frame).many[1].text.size() == 80);
   CHECK(psio::from_json<AnnHolder>(sp(psio::to_json(a))).memo == a.memo);
   CHECK(psio::from_key<AnnHolder>(bytes_of(psio::to_key(a))).memo == a.memo);
   // A schema-driven reader of the schema reads the typed bytes.
   {
      psio::SchemaBuilder builder;
      const auto          root   = builder.insert<AnnHolder>();
      const auto          schema = std::move(builder).build();
      bool                unknown = false;
      CHECK(psio::pssz_ir::validate_auto(schema, root, frame, &unknown).ok());
   }

   // Every writer refuses, and every reader rejects, a value outside the
   // bound or not UTF-8 — bare, and at every place it is held.
   auto refused = [&](const char* what, auto&& change) {
      INFO(what);
      AnnHolder bad = a;
      change(bad);
      std::vector<char> out;
      CHECK(!psio::encode_pssz_checked(psio::pssz{}, bad, out).ok());
      CHECK_THROWS_AS(psio::to_pssz(bad), psio::codec_exception);
      std::string text;
      CHECK(!psio::encode_json_checked(psio::json{}, bad, text).ok());
      CHECK(!psio::encode_key_checked(bad, out).ok());
      CHECK_THROWS_AS(psio::to_key(bad), psio::codec_exception);
      // What an unchecked writer produces: the same record over plain strings.
      Holder<Unbounded, std::vector<std::string>> raw;
      raw.id   = bad.id;
      raw.memo = Unbounded{bad.memo.text};
      if (bad.maybe)
         raw.maybe = Unbounded{bad.maybe->text};
      for (const auto& m : bad.many)
         raw.many.push_back(Unbounded{m.text});
      for (const auto& [k, m] : bad.by_id)
         raw.by_id.emplace(k, Unbounded{m.text});
      raw.tags = bad.tags.list;
      // (Its widths are those of unbounded strings; compare through the
      // schema-free forms instead: text and key.)
      std::string raw_text;   // (bytes that are not UTF-8 have no text at all)
      if (psio::encode_json_checked(psio::json{}, raw, raw_text).ok())
      {
         CHECK(!psio::validate_json<AnnHolder>(sp(raw_text)).ok());
         AnnHolder decoded;
         CHECK(!psio::decode_json_checked(psio::json{}, sp(raw_text), decoded).ok());
         CHECK_THROWS_AS(psio::from_json<AnnHolder>(sp(raw_text)), psio::codec_exception);
      }
      const auto raw_key = psio::to_key(raw);
      CHECK(!psio::validate_key<AnnHolder>(bytes_of(raw_key)).ok());
      const auto bytes = psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, bad);
      CHECK(!psio::validate_pssz<AnnHolder>(bytes).ok());
      CHECK(!psio::validate_strict<AnnHolder>(psio::pssz{}, bytes).ok());
      CHECK_THROWS_AS(psio::decode<AnnHolder>(psio::pssz{}, bytes), psio::codec_exception);
      // The wrapper-type spelling rejects the same bytes.
      CHECK(!psio::validate_pssz<WrapHolder>(bytes).ok());
   };
   refused("memo over 80 bytes", [](AnnHolder& h) { h.memo.text.assign(81, 'x'); });
   refused("memo not UTF-8", [](AnnHolder& h) { h.memo.text = "\xff"; });
   refused("optional memo over the bound", [](AnnHolder& h) { h.maybe->text.assign(81, 'x'); });
   refused("list element not UTF-8", [](AnnHolder& h) { h.many[0].text = "\xc0\x80"; });
   refused("map value over the bound", [](AnnHolder& h) { h.by_id[1].text.assign(81, 'x'); });
   refused("too many tags", [](AnnHolder& h) { h.tags.list.assign(5, "t"); });
   refused("tag over its bound", [](AnnHolder& h) { h.tags.list = {"123456789"}; });
   // Bare.
   {
      std::vector<char> out;
      CHECK(!psio::encode_pssz_checked(psio::pssz{}, ann::Memo{std::string(81, 'x')}, out).ok());
      CHECK(!psio::validate_pssz<ann::Memo>(std::vector<char>(81, 'x')).ok());
      CHECK(psio::validate_pssz<ann::Memo>(std::vector<char>(80, 'x')).ok());
      CHECK(!psio::validate_pssz<ann::Memo>(std::vector<char>{char(0xff)}).ok());
      CHECK(!psio::validate_json<ann::Memo>(sp("\"" + std::string(81, 'x') + "\"")).ok());
      CHECK(psio::validate_json<ann::Memo>(sp("\"" + std::string(80, 'x') + "\"")).ok());
   }

   // Evolution: the wrapper keeps its identity, and its bound may only widen.
   STATIC_REQUIRE(psio::pssz_evolves_from<ann::Memo, ann::Memo>);
   STATIC_REQUIRE(psio::pssz_evolves_from<ann::Memo, wider::Memo>);
   STATIC_REQUIRE(!psio::pssz_evolves_from<wider::Memo, ann::Memo>);
   STATIC_REQUIRE(!psio::pssz_evolves_from<ann::Memo, raw::Memo>);
}

// ── max_encoded_size is the inferred bound, for an argument tuple too ─────
TEST_CASE("max_encoded_size of a capped argument tuple is its inferred bound", "[consistency][caps]")
{
   // docs/pssz-spec.md 3.2: the cap is not part of max_encoded_size, for a
   // record and for a method's argument tuple alike. The cap rules the width
   // (3.1) and is what the type contributes to one that holds it.
   using cons_fields::PutArgs;   // (u32, string max 8), maxDynamicData(64)
   STATIC_REQUIRE(psio::max_dynamic_data_v<PutArgs> == std::optional<std::size_t>{64});
   STATIC_REQUIRE(psio::max_encoded_size<PutArgs>() == std::optional<std::size_t>{1 + 4 + 1 + 8});
   STATIC_REQUIRE(psio::effective_max_dynamic_v<PutArgs> == std::optional<std::size_t>{64});
   STATIC_REQUIRE(psio::auto_pssz_width_v<PutArgs> == 1);
   // The same for a record with the same members and cap.
   STATIC_REQUIRE(psio::max_dynamic_data_v<cons_cap::Capped> == std::optional<std::size_t>{16});
   STATIC_REQUIRE(psio::max_encoded_size<cons_cap::Capped>() == std::optional<std::size_t>{16});
   // Held by another type, a capped tuple reserves its cap.
   STATIC_REQUIRE(psio::max_encoded_size<std::tuple<PutArgs>>() ==
                  std::optional<std::size_t>{1 + 1 + 64});
}

// ── The JSON form of a method's arguments ────────────────────────────────
namespace cons_args
{
   struct Token
   {
      void transfer(std::string to, std::uint64_t amount, std::optional<std::string> memo);
      void add(std::uint32_t a, std::uint32_t b);   // no parameter names
      void ping();
      void half(std::uint32_t a, std::uint32_t b);  // one name of two: not named
      PSIO_REFLECT(Token,
                   method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>),
                          maxDynamicData(256)),
                   method(add),
                   method(ping),
                   method(half, a))
   };
   using Transfer = psio::reflect<Token>::method_arg_tuple<0>;
   using Add      = psio::reflect<Token>::method_arg_tuple<1>;
   using Ping     = psio::reflect<Token>::method_arg_tuple<2>;
   using Half     = psio::reflect<Token>::method_arg_tuple<3>;
}  // namespace cons_args

TEST_CASE("method arguments in JSON: named object or positional array", "[consistency][args]")
{
   using namespace cons_args;
   STATIC_REQUIRE(psio::annotated_tuple_named_v<Transfer>);
   STATIC_REQUIRE(!psio::annotated_tuple_named_v<Add>);
   STATIC_REQUIRE(!psio::annotated_tuple_named_v<Half>);
   STATIC_REQUIRE(psio::annotated_tuple_named_v<Ping>);

   const Transfer args{std::tuple{std::string{"bob"}, std::uint64_t{5},
                                  std::optional<std::string>{"hi"}}};
   const auto     frame = psio::to_pssz(args);
   // The writer: an object keyed by parameter name, in declaration order ...
   CHECK(psio::to_json(args) == R"({"to":"bob","amount":5,"memo":"hi"})");
   CHECK(psio::to_ijson(args) == R"({"to":"bob","amount":5,"memo":"hi"})");
   CHECK(psio::to_json(Transfer{std::tuple{std::string{"bob"}, ~std::uint64_t{0},
                                          std::optional<std::string>{}}}) ==
         R"({"to":"bob","amount":18446744073709551615,"memo":null})");
   CHECK(psio::to_ijson(Transfer{std::tuple{std::string{"bob"}, ~std::uint64_t{0},
                                           std::optional<std::string>{}}}) ==
         R"({"to":"bob","amount":"18446744073709551615","memo":null})");
   // ... and the array when the method has no parameter names.
   CHECK(psio::to_json(Add{std::tuple{std::uint32_t{1}, std::uint32_t{2}}}) == "[1,2]");
   CHECK(psio::to_json(Half{std::tuple{std::uint32_t{1}, std::uint32_t{2}}}) == "[1,2]");
   CHECK(psio::to_json(Ping{}) == "{}");
   // Binary stays the positional tuple (2-byte offsets: the cap is 256).
   CHECK(psio::to_hex(std::span<const char>{frame.data(), frame.size()}) ==
         "0C00" "0C00" "0500000000000000" "0F00" "626F62" "01" "6869");

   // The readers: either form for a named list.
   auto reads = [&](const char* text, const Transfer& expect) {
      INFO(text);
      for (int ij = 0; ij < 2; ++ij)
      {
         Transfer got;
         const auto status = ij ? psio::decode_json_checked(psio::ijson{}, text, got)
                                : psio::decode_json_checked(psio::json{}, text, got);
         REQUIRE(status.ok());
         CHECK(got.tuple() == expect.tuple());
         CHECK((ij ? psio::validate_json<Transfer>(psio::ijson{}, text)
                   : psio::validate_json<Transfer>(psio::json{}, text))
                  .ok());
      }
   };
   const Transfer none{std::tuple{std::string{"bob"}, std::uint64_t{5},
                                  std::optional<std::string>{}}};
   reads(R"({"to":"bob","amount":5,"memo":"hi"})", args);
   reads(R"({"memo":"hi","amount":5,"to":"bob"})", args);              // any order
   reads(R"({"to":"x","to":"bob","amount":5,"memo":"hi"})", args);    // the last of a repeated key
   reads(R"({"to":"bob","extra":[1,{}],"amount":5,"memo":"hi"})", args); // unknown keys skipped
   reads(R"({"to":"bob","amount":5})", none);                          // a missing optional is None
   reads(R"({"to":"bob","amount":5,"memo":null})", none);
   reads(R"(["bob",5,"hi"])", args);                                   // the positional array
   reads(R"(["bob",5,null])", none);
   auto rejects = [&](const char* text) {
      INFO(text);
      Transfer got;
      CHECK(!psio::decode_json_checked(psio::json{}, text, got).ok());
      CHECK(!psio::decode_json_checked(psio::ijson{}, text, got).ok());
      CHECK(!psio::validate_json<Transfer>(psio::json{}, text).ok());
   };
   rejects(R"({"amount":5,"memo":"hi"})");                 // a missing required argument
   rejects(R"(["bob",5])");                                // an array has every element
   rejects(R"(["bob",5,"hi",1])");
   rejects(R"({"to":"12345678901234567","amount":5})");    // a parameter outside its bound
   rejects(R"(["12345678901234567",5,null])");
   rejects(R"({"to":5,"amount":5})");
   rejects(R"("bob")");
   // A list without names is the array only.
   Add sum;
   CHECK(psio::decode_json_checked(psio::json{}, "[1,2]", sum).ok());
   CHECK(std::get<1>(sum) == 2);
   CHECK(!psio::decode_json_checked(psio::json{}, R"({"0":1,"1":2})", sum).ok());
   Ping ping;
   CHECK(psio::decode_json_checked(psio::json{}, "{}", ping).ok());
   CHECK(psio::decode_json_checked(psio::json{}, "[]", ping).ok());

   // The schema-driven codec agrees, from the schema alone.
   psio::SchemaBuilder builder;
   const auto          transfer_type = builder.insert<Transfer>();
   const auto          add_type      = builder.insert<Add>();
   const auto          schema        = std::move(builder).build();
   REQUIRE(psio::pssz_ir::validate_schema(schema).ok());
   auto dynamic = [&](const auto& root, const char* text, std::span<const char> expect) {
      INFO(text);
      std::vector<char> out;
      const bool        ok =
         psio::pssz_ir::json_to_pssz(schema, root, psio::json{}, text, out).ok() &&
         psio::pssz_ir::json_to_pssz(schema, root, psio::ijson{}, text, out).ok();
      if (expect.data() == nullptr)
         return !ok;
      return ok && out.size() == 2 * expect.size() &&
             std::equal(expect.begin(), expect.end(), out.begin());
   };
   CHECK(dynamic(transfer_type, R"({"to":"bob","amount":5,"memo":"hi"})", frame));
   CHECK(dynamic(transfer_type, R"({"memo":"hi","amount":5,"to":"bob"})", frame));
   CHECK(dynamic(transfer_type, R"(["bob",5,"hi"])", frame));
   CHECK(dynamic(transfer_type, R"(["bob",5])", {}));
   CHECK(dynamic(transfer_type, R"({"amount":5})", {}));
   CHECK(dynamic(transfer_type, R"(["12345678901234567",5,null])", {}));
   const auto add_frame = psio::to_pssz(Add{std::tuple{std::uint32_t{1}, std::uint32_t{2}}});
   CHECK(dynamic(add_type, "[1,2]", add_frame));
   CHECK(dynamic(add_type, R"({"0":1,"1":2})", {}));
   std::string text;
   bool        unknown = false;
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, transfer_type, psio::json{}, frame, text, &unknown)
              .ok());
   CHECK(text == R"({"to":"bob","amount":5,"memo":"hi"})");
   text.clear();
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, add_type, psio::json{}, add_frame, text, &unknown)
              .ok());
   CHECK(text == "[1,2]");
}

// ── A result is encoded as the type returns(...) makes of it ─────────────
namespace cons_result
{
   struct Api
   {
      std::vector<std::string> names(std::uint32_t limit) const;
      std::string              plain() const;
      PSIO_REFLECT(Api,
                   method(names, limit, returns(max<4> | element(utf8 | max<16>))),
                   method(plain))
   };
}  // namespace cons_result

TEST_CASE("a method result has the bytes of its returns(...) type", "[consistency][result]")
{
   using cons_result::Api;
   const std::vector<std::string> two{"alice", "bob"};
   std::vector<char>              bytes;
   REQUIRE(psio::encode_method_result<Api, 0>(two, bytes).ok());
   // BoundedList(BoundedString(16), 4): a 1-byte offset table ...
   CHECK(psio::to_hex(std::span<const char>{bytes}) == "0207616C696365626F62");
   CHECK(bytes == [&] {
      psio::bounded<psio::utf8_string<16>, 4> same;
      same.push_back(psio::utf8_string<16>{"alice"});
      same.push_back(psio::utf8_string<16>{"bob"});
      auto b = psio::to_pssz(same);
      return std::vector<char>(b.begin(), b.end());
   }());
   // ... not the 4-byte one of the plain C++ type.
   CHECK(psio::to_pssz(two).size() == 8 + 8);
   CHECK(psio::validate_method_result<Api, 0>(bytes).ok());
   std::vector<std::string> back;
   REQUIRE(psio::decode_method_result<Api, 0>(bytes, back).ok());
   CHECK(back == two);
   // Outside returns(...): no encoding, and its frame is rejected.
   std::vector<char> none;
   CHECK(!psio::encode_method_result<Api, 0>(std::vector<std::string>(5, "a"), none).ok());
   CHECK(!psio::encode_method_result<Api, 0>(std::vector<std::string>{"\xff"}, none).ok());
   CHECK(none.empty());
   const auto over = psio::detail::pssz_impl::encode_method_result_unchecked<Api, 0>(
      std::vector<std::string>(5, "a"));
   CHECK(!psio::validate_method_result<Api, 0>(over).ok());
   CHECK(!psio::decode_method_result<Api, 0>(over, back).ok());
   // Without returns(...) it is the plain codec of the result type.
   std::vector<char> plain;
   REQUIRE(psio::encode_method_result<Api, 1>(std::string{"hi"}, plain).ok());
   CHECK(plain == std::vector<char>{'h', 'i'});
}
