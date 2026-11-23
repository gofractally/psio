// Cross-language consistency round: one section per rule C++ was the looser
// or the inconsistent side of. Each section names the rule it pins.
#include <catch.hpp>

#include <psio/annotate.hpp>
#include <psio/bin.hpp>
#include <psio/ext_int.hpp>
#include <psio/frac.hpp>
#include <psio/ssz.hpp>
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
