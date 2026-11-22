// Transparent types, custom types and invariants with exceptions disabled
// (-fno-exceptions -fno-rtti, PSIO_EXCEPTIONS_ENABLED=0): every checked
// entry point reports a violated invariant through its status — the
// invariant's own — and never throws or aborts. The fail-fast entry points
// (psio::encode, the key decode) are not called on bad values here: without
// exceptions they abort by contract.

#include "custom_assoc_cases.hpp"
#include "custom_conformance_types.hpp"

#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace custom_conf;
namespace ir = psio::pssz_ir;
namespace pi = psio::detail::pssz_impl;

namespace
{
   int failures = 0;
   void expect(bool ok, const char* what, int line)
   {
      if (!ok)
      {
         std::printf("FAILED line %d: %s\n", line, what);
         ++failures;
      }
   }
#define EXPECT(cond) expect((cond), #cond, __LINE__)

   bool is(const psio::codec_status& status, std::string_view reason)
   {
      return !status.ok() && status.error().what == reason;
   }
   std::span<const char> sp(const std::vector<char>& b) { return {b.data(), b.size()}; }
   std::span<const char> sp(const std::string& s) { return {s.data(), s.size()}; }

   // Every checked pssz reader and writer on a value that violates `reason`.
   template <typename Fmt, typename T>
   void pssz_violation(const T& value, std::string_view reason)
   {
      std::vector<char> bytes;
      EXPECT(pi::encode_ignoring_invariants(Fmt{}, value, bytes).ok());
      EXPECT(is(psio::validate<T>(Fmt{}, sp(bytes)), reason));
      EXPECT(is(psio::validate_strict<T>(Fmt{}, sp(bytes)), reason));
      bool unknown = false;
      EXPECT(is(psio::validate_pssz_compatible<T>(Fmt{}, sp(bytes), unknown), reason));
      T out{};
      EXPECT(is(psio::decode_pssz_checked(Fmt{}, sp(bytes), out, unknown), reason));
      std::vector<char> sink{'x'};
      EXPECT(is(psio::encode_pssz_checked(Fmt{}, value, sink), reason));
      EXPECT(sink.size() == 1);
      // The trusted decode reads the bytes it is told are valid.
      const T trusted = psio::decode<T>(Fmt{}, sp(bytes));
      EXPECT(is(psio::check_invariant(trusted), reason) || !psio::has_invariant_v<T>);
   }
   template <typename T>
   void text_violation(const T& value, const std::string& json, const std::string& ijson,
                       std::string_view reason)
   {
      std::string out = "keep";
      EXPECT(is(psio::encode_json_checked(psio::json{}, value, out), reason));
      EXPECT(is(psio::encode_json_checked(psio::ijson{}, value, out), reason));
      EXPECT(out == "keep");
      T back{};
      for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text,
                                psio::json_parser::simdjson, psio::json_parser::simdjson_index})
      {
         EXPECT(is(psio::decode_json_checked(psio::json{}, sp(json), back, parser), reason));
         EXPECT(is(psio::decode_json_checked(psio::ijson{}, sp(ijson), back, parser), reason));
         EXPECT(is(psio::validate_json<T>(psio::json{}, sp(json), parser), reason));
         EXPECT(is(psio::validate_json<T>(psio::ijson{}, sp(ijson), parser), reason));
      }
      EXPECT(is(psio::validate<T>(psio::json{}, sp(json)), reason));
      EXPECT(is(psio::validate_strict<T>(psio::ijson{}, sp(ijson)), reason));
      EXPECT(psio::make_boxed<T>(psio::json{}, sp(json)) == nullptr);
      EXPECT(psio::make_boxed<T>(psio::ijson{}, sp(ijson)) == nullptr);
   }
   template <typename T, typename Plain>
   void key_violation(const Plain& plain, std::string_view reason)
   {
      const auto key = psio::encode(psio::key{}, plain);
      EXPECT(is(psio::validate<T>(psio::key{}, sp(key)), reason));
      EXPECT(is(psio::validate_strict<T>(psio::key{}, sp(key)), reason));
   }
}  // namespace

int main()
{
   // ── Transparent types: the member's bytes, text, key and view ─────────
   {
      const Id   id{258};
      const auto bytes = psio::encode(psio::pssz{}, id);
      EXPECT(bytes == psio::encode(psio::pssz{}, std::uint64_t{258}));
      EXPECT(psio::validate<Id>(psio::pssz{}, sp(bytes)).ok());
      EXPECT(psio::decode<Id>(psio::pssz{}, sp(bytes)) == id);
      EXPECT(psio::encode(psio::key{}, id) == psio::encode(psio::key{}, std::uint64_t{258}));
      std::string text;
      EXPECT(psio::encode_json_checked(psio::ijson{}, id, text).ok() && text == "258");
      Id back;
      EXPECT(psio::decode_json_checked(psio::json{}, sp(text), back).ok() && back == id);
      EXPECT((psio::view<Id, psio::pssz, psio::storage::const_borrow>{sp(bytes)}.get() == 258));
      const Blob blob{{1, 2, 3}};
      text.clear();
      EXPECT(psio::encode_json_checked(psio::json{}, blob, text).ok() && text == "\"AQID\"");
      std::vector<char> checked;
      EXPECT(psio::encode_pssz_checked(psio::pssz{}, blob, checked).ok());
      EXPECT(checked == psio::encode(psio::pssz{}, Bytes{1, 2, 3}));
   }

   // ── Invariants on every checked path ──────────────────────────────────
   const std::string_view name_reason = "Name: value is not a canonical name";
   const std::string_view der_reason  = "DerKey: not a DER sequence";
   const std::string_view level_reason = "Level: not a listed level";
   const Name             bad_name{std::uint64_t{1} << 60};
   pssz_violation<psio::pssz, Name>(bad_name, name_reason);
   pssz_violation<psio::pssz16, Name>(bad_name, name_reason);
   pssz_violation<psio::pssz, DerKey>(DerKey{}, der_reason);
   pssz_violation<psio::pssz, Range>(Range{2, 1}, "Range: lo is greater than hi");
   pssz_violation<psio::pssz, Level>(static_cast<Level>(9), level_reason);
   pssz_violation<psio::pssz, Ref>(Ref{std::string{}}, "Ref: a name is not empty");
   pssz_violation<psio::pssz, std::vector<Level>>({Level::low, static_cast<Level>(4)},
                                                  level_reason);
   pssz_violation<psio::pssz, std::optional<Name>>(bad_name, name_reason);
   pssz_violation<psio::pssz32, std::vector<Name>>({Name{1}, bad_name}, name_reason);
   Guarded bad = guarded_sample();
   bad.key     = DerKey{{4, 1}};
   pssz_violation<psio::pssz, Guarded>(bad, der_reason);
   pssz_violation<psio::pssz8, Guarded>(bad, der_reason);

   text_violation(bad_name, R"({"value":1152921504606846976})",
                  R"({"value":"1152921504606846976"})", name_reason);
   text_violation(DerKey{{0x31}}, "[49]", R"("31")", der_reason);
   text_violation(Ref{std::string{}}, R"("@")", R"("@")", "Ref: a name is not empty");
   text_violation(std::vector<Level>{static_cast<Level>(4)}, "[4]", "[4]", level_reason);
   {
      const Guarded good  = guarded_sample();
      std::string   json  = psio::encode(psio::json{}, good);
      std::string   ijson = psio::encode(psio::ijson{}, good);
      const auto    swap  = [](std::string& text, std::string_view from, std::string_view to) {
         const auto at = text.find(from);
         if (at != std::string::npos) text.replace(at, from.size(), to);
      };
      swap(json, R"("key":[48,3,1,2,3])", R"("key":[4,1])");
      swap(ijson, R"("key":"3003010203")", R"("key":"0401")");
      text_violation(bad, json, ijson, der_reason);
   }

   key_violation<Name, std::uint64_t>(std::uint64_t{1} << 60, name_reason);
   key_violation<DerKey, Bytes>(Bytes{}, der_reason);
   key_violation<Level, std::uint8_t>(std::uint8_t{7}, level_reason);
   key_violation<std::tuple<std::string, Name>, std::tuple<std::string, std::uint64_t>>(
      {"k", ~std::uint64_t{0}}, name_reason);
   {
      // Good values through the key codec.
      const auto key = psio::to_key(Name{5});
      EXPECT(psio::validate<Name>(psio::key{}, std::span<const char>{key.data(), key.size()}).ok());
      EXPECT(psio::decode<Name>(psio::key{}, std::span<const char>{key.data(), key.size()}) ==
             Name{5});
   }

   // ── Schema-driven, with and without the handlers ──────────────────────
   {
      const auto schema = psio::SchemaBuilder{}.insert<Guarded>("Guarded").build();
      const psio::schema_types::AnyType root = psio::schema_types::Type{"Guarded"};
      ir::CustomHandlers                handlers;
      for_each_custom([&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
      ir::CustomHandlers strict;
      strict.require_all = true;

      const Guarded     good       = guarded_sample();
      const auto        good_bytes = psio::encode(psio::pssz{}, good);
      std::vector<char> bytes;
      EXPECT(pi::encode_ignoring_invariants(psio::pssz{}, bad, bytes).ok());

      EXPECT(ir::validate_pssz(schema, root, sp(good_bytes), nullptr, &handlers).ok());
      EXPECT(ir::validate_pssz(schema, root, sp(bytes)).ok());
      EXPECT(is(ir::validate_pssz(schema, root, sp(bytes), nullptr, &handlers), der_reason));
      EXPECT(is(ir::validate_pssz(schema, root, sp(good_bytes), nullptr, &strict),
                "pssz IR: no handler registered for a custom type"));
      ir::Value value;
      bool      unknown = false;
      EXPECT(is(ir::decode_auto(schema, root, sp(bytes), value, unknown, &handlers), der_reason));
      EXPECT(ir::decode_auto(schema, root, sp(bytes), value, unknown).ok());
      std::vector<char> out;
      EXPECT(is(ir::encode_auto(schema, root, value, out, &handlers), der_reason));
      EXPECT(out.empty());
      EXPECT(is(ir::decode_compatible<4>(schema, root,
                                         sp([&] {
                                            std::vector<char> wide;
                                            (void)pi::encode_ignoring_invariants(psio::pssz32{},
                                                                                 bad, wide);
                                            return wide;
                                         }()),
                                         value, unknown, &handlers),
                der_reason));
      std::string text;
      EXPECT(is(ir::pssz_to_json(schema, root, psio::ijson{}, sp(bytes), text, nullptr, &handlers),
                der_reason));
      EXPECT(text.empty());
      EXPECT(ir::decode_auto(schema, root, sp(bytes), value, unknown).ok());
      EXPECT(is(ir::value_to_json(schema, root, psio::json{}, value, text, &handlers), der_reason));
      // Text in, with the handlers: typed text of the bad value.
      std::string typed = psio::encode(psio::ijson{}, good);
      const auto  at    = typed.find(R"("key":"3003010203")");
      EXPECT(at != std::string::npos);
      typed.replace(at, 18, R"("key":"0401")");
      for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text,
                                psio::json_parser::simdjson, psio::json_parser::simdjson_index})
      {
         EXPECT(is(ir::json_to_pssz(schema, root, psio::ijson{}, sp(typed), out, nullptr, parser,
                                    &handlers),
                   der_reason));
         EXPECT(out.empty());
      }
      // The good value round-trips through text with the handlers.
      text.clear();
      EXPECT(ir::pssz_to_json(schema, root, psio::ijson{}, sp(good_bytes), text, nullptr,
                              &handlers)
                .ok());
      EXPECT(text == psio::encode(psio::ijson{}, good));
      EXPECT(ir::json_to_pssz(schema, root, psio::ijson{}, sp(text), out, nullptr,
                              psio::json_parser::automatic, &handlers)
                .ok());
      EXPECT(out == good_bytes);
   }

   // With maps, sets, pairs and nested values (custom_assoc_cases.hpp).
   failures += custom_assoc_cases::run();

   if (failures == 0) std::printf("custom types without exceptions: ok\n");
   return failures == 0 ? 0 : 1;
}
