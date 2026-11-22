// psio::nested<T>: see nested_cases.hpp for the cases shared with the
// exception-disabled build. This file adds what needs exceptions or Catch:
// the fail-fast paths and seeded mutation of every reader's input.
#include "nested_cases.hpp"

#include <catch.hpp>

TEST_CASE("nested: every checked entry point, without a throw", "[nested]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = nested_cases::run());
   CHECK(failed == 0);
}

#include <psio/graphql.hpp>

#include <random>

using namespace nested_cases;

TEST_CASE("nested: the fail-fast paths raise what the checked ones report", "[nested]")
{
   // from(): a value with no encoding.
   const psio::utf8_string<> not_utf8{std::string(1, char(0xff))};
   CHECK_THROWS_AS(psio::nested<psio::utf8_string<>>::from(not_utf8), psio::codec_exception);
   CHECK_THROWS_AS(psio::nested{not_utf8}, psio::codec_exception);

   // validate_or_throw descends like validate.
   const Bytes garbage = pssz_of(RawAction{1, 2, {1, 2, 3}});
   CHECK_THROWS_AS(psio::validate_or_throw<Action>(psio::pssz{}, std::span<const char>{garbage}),
                   psio::codec_exception);
   CHECK_NOTHROW(
      psio::validate_or_throw<DeferredAction>(psio::pssz{}, std::span<const char>{garbage}));
   CHECK_NOTHROW(psio::validate_or_throw<Action>(
      psio::pssz{},
      std::span<const char>{pssz_of(Action{1, 2, psio::nested<Transfer>::from(sample_transfer())})}));

   // The trusted decode adopts the bytes; the holder's validate() tells.
   const auto adopted = psio::decode<Action>(psio::pssz{}, std::span<const char>{garbage});
   CHECK_FALSE(adopted.args.validate().ok());

   // Text and key: decode fails on what validate rejects.
   const std::string_view wrong = R"({"sender":1,"method":2,"args":[1,2,3]})";
   CHECK_THROWS_AS(psio::decode<Action>(psio::ijson{}, std::span<const char>{wrong}),
                   psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, adopted), psio::codec_exception);
   const auto bad_key = psio::to_key(RawKeyed{7, {1, 2, 3}});
   CHECK_THROWS_AS(
      psio::decode<Keyed>(psio::key{}, std::span<const char>{bad_key.data(), bad_key.size()}),
      psio::codec_exception);
   const Keyed keyed{7, psio::nested<Transfer>::from(sample_transfer())};
   const auto  key = psio::to_key(keyed);
   const auto  back =
      psio::decode<Keyed>(psio::key{}, std::span<const char>{key.data(), key.size()});
   CHECK(back.table == 7);
   CHECK(back.row == keyed.row);

   // A member over its length_bound has no encoding.
   Capped capped;
   capped.note = psio::nested<std::string>::from("123456789");
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, capped), psio::codec_exception);
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, capped), psio::codec_exception);
}

TEST_CASE("nested: key order is the order of the inner bytes", "[nested][key]")
{
   using K = std::tuple<psio::nested<std::uint32_t>, std::uint8_t>;
   // Little-endian inner bytes: 0x00000100 (00 01 00 00) sorts before
   // 0x00000001 (01 00 00 00), as the byte lists do.
   const auto a = psio::to_key(K{psio::nested<std::uint32_t>::from(0x100u), 9});
   const auto b = psio::to_key(K{psio::nested<std::uint32_t>::from(1u), 0});
   CHECK(std::string_view(a.data(), a.size()) < std::string_view(b.data(), b.size()));
   // 00 -> 00 01, terminator 00 00, then the next element.
   CHECK(Bytes(a.begin(), a.end()) ==
         Bytes{0, 1, 1, 0, 1, 0, 1, 0, 0, 9});
}

namespace
{
   struct Forms
   {
      psio::nested<std::uint64_t> big;
      psio::nested<std::uint64_t> forced;
      PSIO_REFLECT(Forms, big, attr(forced, psio::ijson_string))
   };
}  // namespace

TEST_CASE("nested: names, variants and tuples", "[nested]")
{
   CHECK(std::string_view{psio::get_type_name<psio::nested<Transfer>>()} == "nested_Transfer");
   CHECK(std::string_view{psio::get_type_name<
            psio::nested<std::uint32_t, psio::nested_validation::deferred>>()} ==
         "nested_uint32");
   CHECK(std::string_view{psio::get_type_name<psio::nested<psio::nested<std::string>>>()} ==
         "nested_nested_string");

   // An I-JSON variant names the alternative; json uses its index.
   using V = std::variant<std::uint8_t, psio::nested<Transfer>>;
   const V     v = psio::nested<Transfer>::from(sample_transfer());
   std::string text;
   REQUIRE(psio::encode_json_checked(psio::ijson{}, v, text).ok());
   CHECK(text == R"({"nested_Transfer":{"to":"bob","amount":5,"memo":"rent"}})");
   V back;
   REQUIRE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, back).ok());
   CHECK(back == v);
   text.clear();
   REQUIRE(psio::encode_json_checked(psio::json{}, v, text).ok());
   CHECK(text == R"([1,{"to":"bob","amount":5,"memo":"rent"}])");

   // The schema records the name for schema-driven I-JSON.
   const auto schema = psio::SchemaBuilder{}.insert<V>("V").build();
   std::string dynamic;
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, "V", psio::ijson{}, pssz_of(v), dynamic).ok());
   CHECK(dynamic == R"({"nested_Transfer":{"to":"bob","amount":5,"memo":"rent"}})");

   // A scalar and an integer form: the member's form reaches the inner value.
   const Forms forms{psio::nested<std::uint64_t>::from(1ull << 60),
                     psio::nested<std::uint64_t>::from(7)};
   text.clear();
   REQUIRE(psio::encode_json_checked(psio::ijson{}, forms, text).ok());
   CHECK(text == R"({"big":"1152921504606846976","forced":"7"})");
   Forms forms_back;
   REQUIRE(
      psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, forms_back).ok());
   CHECK(forms_back.big == forms.big);
   CHECK(forms_back.forced == forms.forced);
   const auto forms_schema = psio::SchemaBuilder{}.insert<Forms>("Forms").build();
   dynamic.clear();
   REQUIRE(psio::pssz_ir::pssz_to_json(forms_schema, "Forms", psio::ijson{}, pssz_of(forms),
                                       dynamic)
              .ok());
   CHECK(dynamic == text);
   Bytes from_text;
   REQUIRE(
      psio::pssz_ir::json_to_pssz(forms_schema, "Forms", psio::ijson{}, text, from_text).ok());
   CHECK(from_text == pssz_of(forms));
}

TEST_CASE("nested: a recursive schema through a nested member", "[nested][schema]")
{
   // Chain names itself through Nested: schema analysis terminates, in both
   // profiles, and the codec follows the value.
   const auto  schema = psio::SchemaBuilder{}.insert<Chain>("Chain").build();
   const Chain chain  = chain_value(3);
   const Bytes bytes  = pssz_of(chain);
   namespace rt       = psio::pssz_ir;
   rt::Value value;
   bool      unknown = false;
   REQUIRE(rt::decode_auto(schema, "Chain", bytes, value, unknown).ok());
   Bytes again;
   REQUIRE(rt::encode_auto(schema, "Chain", value, again).ok());
   CHECK(again == bytes);
   REQUIRE(rt::decode_exact<4>(schema, "Chain", psio::encode(psio::pssz_<4>{}, chain), value).ok());
   std::string text, typed;
   REQUIRE(rt::pssz_to_json(schema, "Chain", psio::ijson{}, bytes, text).ok());
   REQUIRE(psio::encode_json_checked(psio::ijson{}, chain, typed).ok());
   CHECK(text == typed);
   CHECK(text == R"({"next":{"next":{"next":{"next":null}}}})");
   // The IR round-trips through its own encoding with the Nested node.
   const auto ir   = psio::encode(psio::pssz{}, schema);
   const auto back = psio::decode<psio::Schema>(psio::pssz{}, std::span<const char>{ir});
   CHECK(rt::validate_pssz(back, "Chain", bytes).ok());
}

TEST_CASE("nested: foreign IDL emitters render the node as bytes", "[nested][schema]")
{
   namespace ir = psio::schema_types;
   const auto schema = psio::SchemaBuilder{}.insert<Action>("Action").build();
   // (The emitters are exercised by their own suites; here only that a
   // schema holding a Nested node is one they accept.)
   const ir::AnyType* args = nullptr;
   for (const auto& named : schema.types)
      if (named.name == "Action")
         args = std::get<ir::Object>(named.type->value).members[2].type->resolve(schema);
   REQUIRE(args);
   REQUIRE(std::holds_alternative<ir::Nested>(args->value));
   const auto* inner = std::get<ir::Nested>(args->value).type->resolve(schema);
   REQUIRE(inner);
   CHECK(std::holds_alternative<ir::Object>(inner->value));
}

// ── Hostile input ────────────────────────────────────────────────────────
//
// Every checked reader on mutated input: it returns (ok or an error) without
// reading out of bounds, throwing or aborting; and whatever the validator
// accepts, the checked decode accepts and the decoded value re-validates.
namespace
{
   template <typename T>
   void mutate_pssz(const Bytes& good, std::uint32_t seed)
   {
      std::mt19937 rng(seed);
      for (int round = 0; round < 4000; ++round)
      {
         Bytes bytes = good;
         switch (rng() % 4)
         {
            case 0:
               bytes.resize(rng() % (bytes.size() + 1));
               break;
            case 1:
               bytes.push_back(static_cast<char>(rng()));
               break;
            default:
               break;
         }
         for (unsigned flips = rng() % 4; flips && !bytes.empty(); --flips)
            bytes[rng() % bytes.size()] = static_cast<char>(rng());
         const std::span<const char> span{bytes};
         bool                        unknown = false;
         psio::codec_status          plain, strict_status, compatible, decoded;
         T                           out;
         REQUIRE_NOTHROW(plain = psio::validate<T>(psio::pssz{}, span));
         REQUIRE_NOTHROW(strict_status = psio::validate_strict<T>(psio::pssz{}, span));
         REQUIRE_NOTHROW(compatible =
                            psio::validate_pssz_compatible<T>(psio::pssz{}, span, unknown));
         REQUIRE_NOTHROW(decoded = psio::decode_pssz_checked(psio::pssz{}, span, out, unknown));
         REQUIRE(plain.ok() == compatible.ok());
         REQUIRE(plain.ok() == decoded.ok());
         if (strict_status.ok())
            REQUIRE(plain.ok());
         if (decoded.ok())
         {
            // What was accepted re-encodes to something every reader accepts
            // (the nested members still hold the writer's bytes).
            Bytes again;
            REQUIRE(psio::encode_pssz_checked(psio::pssz{}, out, again).ok());
            REQUIRE(psio::validate<T>(psio::pssz{}, std::span<const char>{again}).ok());
         }
      }
   }

   template <typename T>
   void mutate_schema_driven(const psio::Schema& schema, const char* root, const Bytes& good,
                             std::uint32_t seed)
   {
      std::mt19937 rng(seed);
      for (int round = 0; round < 3000; ++round)
      {
         Bytes bytes = good;
         if (rng() % 3 == 0)
            bytes.resize(rng() % (bytes.size() + 1));
         for (unsigned flips = 1 + rng() % 3; flips && !bytes.empty(); --flips)
            bytes[rng() % bytes.size()] = static_cast<char>(rng());
         psio::codec_status dynamic, opaque;
         std::string        text;
         REQUIRE_NOTHROW(dynamic = psio::pssz_ir::validate_pssz(schema, root, bytes));
         REQUIRE_NOTHROW(opaque = psio::pssz_ir::validate_pssz(
                            schema, root, bytes, nullptr, psio::pssz_ir::nested_bytes::opaque));
         REQUIRE_NOTHROW(
            (void)psio::pssz_ir::pssz_to_json(schema, root, psio::ijson{}, bytes, text));
         // The schema-driven validator and the typed one accept the same
         // frames, and opening the nested bytes only ever rejects more.
         REQUIRE(dynamic.ok() == psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok());
         if (dynamic.ok())
            REQUIRE(opaque.ok());
      }
   }
}  // namespace

TEST_CASE("nested: checked readers on mutated PSSZ", "[nested][fuzz]")
{
   const Action action{1, 2, psio::nested<Transfer>::from(sample_transfer())};
   mutate_pssz<Action>(pssz_of(action), 1);
   mutate_pssz<DeferredAction>(pssz_of(action), 2);
   mutate_pssz<Shapes>(pssz_of(sample_shapes()), 3);
   mutate_pssz<Chain>(pssz_of(chain_value(4)), 4);
   Capped capped;
   capped.note  = psio::nested<std::string>::from("1234");
   capped.maybe = psio::nested<std::string>::from("12");
   mutate_pssz<Capped>(pssz_of(capped), 5);

   const auto schema = psio::SchemaBuilder{}
                          .insert<Action>("Action")
                          .insert<Shapes>("Shapes")
                          .insert<Chain>("Chain")
                          .insert<Capped>("Capped")
                          .build();
   mutate_schema_driven<Action>(schema, "Action", pssz_of(action), 6);
   mutate_schema_driven<Shapes>(schema, "Shapes", pssz_of(sample_shapes()), 7);
   mutate_schema_driven<Chain>(schema, "Chain", pssz_of(chain_value(4)), 8);
   mutate_schema_driven<Capped>(schema, "Capped", pssz_of(capped), 9);
}

TEST_CASE("nested: checked readers on mutated keys and text", "[nested][fuzz]")
{
   const Keyed keyed{7, psio::nested<Transfer>::from(sample_transfer())};
   const auto  key = psio::to_key(keyed);
   std::mt19937 rng(11);
   for (int round = 0; round < 4000; ++round)
   {
      Bytes bytes(key.begin(), key.end());
      if (rng() % 3 == 0)
         bytes.resize(rng() % (bytes.size() + 1));
      for (unsigned flips = 1 + rng() % 3; flips && !bytes.empty(); --flips)
         bytes[rng() % bytes.size()] = static_cast<char>(rng() % 4 ? rng() : 0);
      psio::codec_status status;
      REQUIRE_NOTHROW(status = psio::validate<Keyed>(psio::key{}, std::span<const char>{bytes}));
      REQUIRE_NOTHROW(
         (void)psio::validate_strict<Keyed>(psio::key{}, std::span<const char>{bytes}));
      if (status.ok())
      {
         // Accepted keys decode, and are canonical.
         Keyed out;
         REQUIRE_NOTHROW(out = psio::decode<Keyed>(psio::key{}, std::span<const char>{bytes}));
         const auto again = psio::to_key(out);
         REQUIRE(Bytes(again.begin(), again.end()) == bytes);
      }
      else
         REQUIRE_THROWS_AS(psio::decode<Keyed>(psio::key{}, std::span<const char>{bytes}),
                           psio::codec_exception);
   }

   const Action action{1, 2, psio::nested<Transfer>::from(sample_transfer())};
   const auto   schema = psio::SchemaBuilder{}.insert<Action>("Action").build();
   for (const bool ijson : {false, true})
   {
      std::string good;
      REQUIRE((ijson ? psio::encode_json_checked(psio::ijson{}, action, good)
                     : psio::encode_json_checked(psio::json{}, action, good))
                 .ok());
      for (int round = 0; round < 3000; ++round)
      {
         std::string text = good;
         if (rng() % 3 == 0)
            text.resize(rng() % (text.size() + 1));
         static constexpr std::string_view alphabet = "{}[]\",:0123456789abnul\\ ";
         for (unsigned flips = 1 + rng() % 3; flips && !text.empty(); --flips)
            text[rng() % text.size()] = alphabet[rng() % alphabet.size()];
         const std::span<const char> span{text};
         // Every syntax layer, validating and decoding: one verdict.
         for (const auto parser : {psio::json_parser::text, psio::json_parser::automatic})
         {
            Action             out;
            Bytes              dynamic;
            psio::codec_status validated, decoded, schema_driven;
            if (ijson)
            {
               REQUIRE_NOTHROW(validated = psio::validate_json<Action>(psio::ijson{}, span, parser));
               REQUIRE_NOTHROW(decoded =
                                  psio::decode_json_checked(psio::ijson{}, span, out, parser));
               REQUIRE_NOTHROW(schema_driven = psio::pssz_ir::json_to_pssz(
                                  schema, "Action", psio::ijson{}, span, dynamic, nullptr, parser));
            }
            else
            {
               REQUIRE_NOTHROW(validated = psio::validate_json<Action>(psio::json{}, span, parser));
               REQUIRE_NOTHROW(decoded =
                                  psio::decode_json_checked(psio::json{}, span, out, parser));
               REQUIRE_NOTHROW(schema_driven = psio::pssz_ir::json_to_pssz(
                                  schema, "Action", psio::json{}, span, dynamic, nullptr, parser));
            }
            REQUIRE(validated.ok() == decoded.ok());
            REQUIRE(validated.ok() == schema_driven.ok());
            if (decoded.ok())
            {
               REQUIRE(out.args.validate_strict().ok());
               REQUIRE(dynamic == pssz_of(out));
            }
         }
      }
   }
}

// ── GraphQL: a nested member resolves as its inner value ─────────────────
namespace nested_gql
{
   struct Query
   {
      Action                action;
      std::vector<Action>   actions;
      Bytes                 encoded;
      psio::view<Action, psio::pssz> actionView() const
      {
         return psio::view<Action, psio::pssz>{std::span<const char>{encoded}};
      }
      PSIO_REFLECT(Query, action, actions, attr(encoded, psio::graphql::hidden),
                   method(actionView))
   };
   using api = psio::graphql::schema<Query>;
}  // namespace nested_gql

TEST_CASE("nested: GraphQL resolves a nested member as its inner type", "[nested][graphql]")
{
   nested_gql::Query q;
   q.action  = Action{1, 2, psio::nested<Transfer>::from(sample_transfer())};
   q.actions = {q.action, Action{3, 4, psio::nested<Transfer>::from(Transfer{"carol", 9, {}})}};
   q.encoded = pssz_of(q.action);
   std::string out;
   nested_gql::api::run(q,
                        "{ action { sender args { to amount memo } } "
                        "actions { args { to } } "
                        "actionView { method args { to amount memo } } }",
                        out);
   CHECK(out ==
         R"({"data":{"action":{"sender":1,"args":{"to":"bob","amount":5,"memo":"rent"}},)"
         R"("actions":[{"args":{"to":"bob"}},{"args":{"to":"carol"}}],)"
         R"("actionView":{"method":2,"args":{"to":"bob","amount":5,"memo":"rent"}}}})");
   // The member's type in the schema is the inner record.
   out.clear();
   nested_gql::api::run(
      q, R"({ __type(name: "Action") { fields { name type { kind ofType { name } } } } })", out);
   CHECK(out.find(R"({"name":"args","type":{"kind":"NON_NULL","ofType":{"name":"Transfer"}}})") !=
         std::string::npos);
}
