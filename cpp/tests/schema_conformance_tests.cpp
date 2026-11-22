// Schema-driven conformance: a consumer that holds ONLY a Schema IR (no C++
// type) must agree with the typed codecs. This test reads the two shared
// fixtures and uses nothing but the IR recorded in them:
//
//   conformance/json-vectors.json   IR + text  -> the typed psio::pssz bytes
//                                   IR + bytes -> the typed json / ijson text
//                                   IR + rejected text -> rejected
//   conformance/pssz-vectors.json   IR + bytes -> the typed validate outcome
//                                   (canonical / unknown / reject), the
//                                   re-encoding, and a JSON round trip
//
// The Rust counterpart is rust/psio/tests/schema_conformance.rs.

#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// The C++ fixture types: only for their text adapters, which are registered
// as the handlers of the fixture's custom ids. Everything else in this file
// works from the recorded Schema IR alone.
#include "json_conformance_types.hpp"

namespace schema_conf
{
   struct TypeRow
   {
      std::string         id;
      bool                ir = false;
      std::optional<bool> pssz;
      std::optional<std::uint32_t> auto_width;
      std::optional<bool>          custom;
   };
   PSIO_REFLECT(TypeRow, id, ir, pssz, auto_width, custom)

   struct JsonRow
   {
      std::string                name;
      std::string                type;
      std::string                kind;
      std::optional<std::string> format;
      std::optional<std::string> json;
      std::optional<std::string> ijson;
      std::optional<std::string> text;
      std::optional<std::string> text_hex;
      std::optional<std::string> canonical;
      std::optional<std::string> pssz;
      std::optional<std::string> plain_json;
      std::optional<std::string> plain_ijson;
      std::optional<std::string> reason;
   };
   PSIO_REFLECT(JsonRow, name, type, kind, format, json, ijson, text, text_hex, canonical, pssz,
                plain_json, plain_ijson, reason)

   struct SchemaRejectRow
   {
      std::string name;
      std::string schema_ir;
      std::string root;
   };
   PSIO_REFLECT(SchemaRejectRow, name, schema_ir, root)
   struct SchemaAcceptRow
   {
      std::string name;
      std::string schema_ir;
      std::string root;
      std::string pssz;
      std::string json;
      std::string ijson;
   };
   PSIO_REFLECT(SchemaAcceptRow, name, schema_ir, root, pssz, json, ijson)

   struct JsonFixture
   {
      std::uint32_t                version = 0;
      std::string                  schema_ir;
      std::vector<TypeRow>         types;
      std::vector<JsonRow>         vectors;
      std::vector<SchemaRejectRow> schema_rejects;
      std::vector<SchemaAcceptRow> schema_accepts;
   };
   PSIO_REFLECT(JsonFixture, version, schema_ir, types, vectors, schema_rejects, schema_accepts)

   // psio::Schema as it was before `ir_version` was appended.
   struct OldSchema
   {
      psio::schema_types::Package                package;
      std::vector<psio::schema_types::Interface> interfaces;
      std::vector<psio::schema_types::World>     worlds;
      std::vector<psio::schema_types::Use>       uses;
      std::vector<psio::schema_types::Member>    types;
      PSIO_REFLECT(OldSchema, package, interfaces, worlds, uses, types)
   };

   struct PsszRow
   {
      std::string name;
      std::string type;
      std::string format;
      std::string hex;
      std::string expect;
      std::string reason;
      std::string reencoded;
   };
   PSIO_REFLECT(PsszRow, name, type, format, hex, expect, reason, reencoded)

   struct PsszFixture
   {
      std::uint32_t        version = 0;
      std::string          schema_ir;
      std::vector<TypeRow> types;
      std::vector<PsszRow> vectors;
   };
   PSIO_REFLECT(PsszFixture, version, schema_ir, types, vectors)
}  // namespace schema_conf

namespace
{
   using namespace schema_conf;
   namespace ir = psio::pssz_ir;
   using Bytes  = std::vector<char>;

   // The custom text handlers the helpers below pass to the schema path
   // (null: none registered).
   const ir::CustomHandlers* active_handlers = nullptr;
   struct use_handlers
   {
      const ir::CustomHandlers* previous;
      explicit use_handlers(const ir::CustomHandlers* h) : previous(active_handlers)
      {
         active_handlers = h;
      }
      ~use_handlers() { active_handlers = previous; }
   };

   template <typename F>
   F load(const char* path)
   {
      std::ifstream     in(path, std::ios::binary);
      std::stringstream buffer;
      buffer << in.rdbuf();
      const std::string text = buffer.str();
      REQUIRE(!text.empty());
      F out;
      REQUIRE(psio::decode_json_checked(psio::json{}, std::span<const char>{text}, out).ok());
      return out;
   }

   Bytes from_hex(std::string_view hex)
   {
      auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
      Bytes out;
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
         out.push_back(static_cast<char>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
      return out;
   }

   // The IR travels as its own psio::pssz encoding.
   psio::Schema schema_of(const std::string& hex)
   {
      const Bytes bytes = from_hex(hex);
      REQUIRE(psio::validate<psio::Schema>(psio::pssz{}, std::span<const char>{bytes}).ok());
      return psio::decode<psio::Schema>(psio::pssz{}, std::span<const char>{bytes});
   }

   std::string input_text(const JsonRow& row)
   {
      if (row.text) return *row.text;
      const Bytes bytes = from_hex(row.text_hex.value());
      return std::string(bytes.begin(), bytes.end());
   }

   // JSON has one null: Some(None) of a nested optional reads back as None
   // (in the typed text codec too), so such a value does not round-trip
   // through text.
   bool has_some_none(const ir::Value& value)
   {
      if (auto o = std::get_if<ir::Optional>(&value.value))
         return o->some && (ir::detail::none(*o->some) || has_some_none(*o->some));
      if (auto s = std::get_if<ir::Sequence>(&value.value))
      {
         for (const auto& item : *s)
            if (has_some_none(item)) return true;
      }
      else if (auto r = std::get_if<ir::Record>(&value.value))
      {
         for (const auto& [name, item] : *r)
            if (has_some_none(item)) return true;
      }
      else if (auto v = std::get_if<ir::Variant>(&value.value))
         return v->payload && has_some_none(*v->payload);
      return false;
   }

   template <typename Fmt>
   std::string to_bytes(const psio::Schema& schema, const std::string& type,
                        const std::string& text, Bytes& out)
   {
      out.clear();
      auto st = ir::json_to_pssz(schema, psio::schema_types::Type{type}, Fmt{},
                                 std::span<const char>{text}, out, nullptr,
                                 psio::json_parser::automatic, active_handlers);
      // The same decode with the schema's names compiled once (hash lookup
      // instead of a member scan; the custom handlers bound to their nodes
      // once instead of looked up by id) gives the same bytes or the same
      // error.
      const ir::CustomHandlers none;
      const ir::JsonNames names(schema, active_handlers ? *active_handlers : none);
      Bytes               indexed;
      auto st2 = ir::json_to_pssz(schema, psio::schema_types::Type{type}, Fmt{},
                                  std::span<const char>{text}, indexed, &names);
      REQUIRE(st.ok() == st2.ok());
      if (st.ok())
         REQUIRE(indexed == out);
      else
         REQUIRE(std::string_view{st.error().what} == std::string_view{st2.error().what});
      // Every syntax layer gives the same bytes or rejects.
      for (const auto parser : {psio::json_parser::text, psio::json_parser::simdjson,
                                psio::json_parser::simdjson_index})
      {
         Bytes other;
         auto  st3 = ir::json_to_pssz(schema, psio::schema_types::Type{type}, Fmt{},
                                      std::span<const char>{text}, other, &names, parser);
         REQUIRE(st.ok() == st3.ok());
         if (st.ok())
            REQUIRE(other == out);
      }
      return st.ok() ? "" : std::string(st.error().what);
   }
   template <typename Fmt>
   std::string to_text(const psio::Schema& schema, const std::string& type, const Bytes& bytes,
                       std::string& out)
   {
      out.clear();
      auto st = ir::pssz_to_json(schema, psio::schema_types::Type{type}, Fmt{},
                                 std::span<const char>{bytes}, out, nullptr, active_handlers);
      if (st.ok() && active_handlers)
      {
         // Bound once through JsonNames: the same text.
         const ir::JsonNames names(schema, *active_handlers);
         std::string         bound;
         REQUIRE(ir::pssz_to_json(schema, psio::schema_types::Type{type}, Fmt{},
                                  std::span<const char>{bytes}, bound, nullptr, nullptr, &names)
                    .ok());
         REQUIRE(bound == out);
      }
      return st.ok() ? "" : std::string(st.error().what);
   }
}  // namespace

TEST_CASE("schema IR + JSON text agrees with the typed json, ijson and pssz codecs",
          "[schema][conformance][json]")
{
   const auto         fx     = load<JsonFixture>(PSIO_JSON_VECTORS);
   const psio::Schema schema = schema_of(fx.schema_ir);
   // The fixture's custom text forms: each type's own text adapter,
   // registered under the id its schema node carries.
   ir::CustomHandlers handlers;
   handlers.add<json_conf::Version>().add<json_conf::Account>();
   // ... and the custom types of any shape, with their invariants.
   custom_conf::for_each_custom(
      [&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
   REQUIRE(handlers.find("Version"));
   REQUIRE(handlers.find("AccountName"));
   for (const char* id : {"Blob", "Endpoint", "Ratio", "Ref", "Name", "DerKey", "Range", "Level"})
      REQUIRE(handlers.find(id));
   REQUIRE(handlers.find("Endpoint")->to_text);
   REQUIRE_FALSE(handlers.find("Endpoint")->check);   // a text form, no invariant
   REQUIRE(handlers.find("Name")->check);
   REQUIRE_FALSE(handlers.find("Name")->to_text);     // an invariant, no text form
   REQUIRE(handlers.find("Ref")->check);
   REQUIRE(handlers.find("Ref")->from_text);          // both
   const use_handlers  scope(&handlers);
   const ir::JsonNames names(schema, handlers);
   std::map<std::string, TypeRow> types;
   for (const auto& t : fx.types) types[t.id] = t;
   REQUIRE(types.size() >= 40);
   // Every type with a pssz form: IR-derived offset width == typed width.
   std::size_t widths = 0;
   for (const auto& t : fx.types)
   {
      if (!t.ir || !t.pssz.value_or(false)) continue;
      INFO(t.id);
      REQUIRE(t.auto_width.has_value());
      std::size_t width = 0;
      REQUIRE(ir::auto_width(schema, psio::schema_types::Type{t.id}, width).ok());
      REQUIRE(width == *t.auto_width);
      ++widths;
   }
   REQUIRE(widths >= 40);

   std::size_t values = 0, accepted = 0, rejected = 0, with_bytes = 0, plain = 0, violated = 0;
   std::set<std::string> covered;
   for (const auto& row : fx.vectors)
   {
      REQUIRE(types.count(row.type));
      const TypeRow& type = types[row.type];
      if (!type.ir) continue;
      INFO(row.name);
      covered.insert(row.type);
      REQUIRE(row.pssz.has_value() == (type.pssz.value_or(false) && row.kind != "reject"));
      Bytes       bytes, again;
      std::string text;
      if (row.kind == "value")
      {
         ++values;
         REQUIRE(to_bytes<psio::json>(schema, row.type, *row.json, bytes) == "");
         REQUIRE(to_bytes<psio::ijson>(schema, row.type, *row.ijson, again) == "");
         REQUIRE(bytes == again);
         if (row.pssz)
         {
            ++with_bytes;
            REQUIRE(bytes == from_hex(*row.pssz));
         }
         REQUIRE(ir::validate_pssz(schema, psio::schema_types::Type{row.type},
                                   std::span<const char>{bytes})
                    .ok());
         REQUIRE(to_text<psio::json>(schema, row.type, bytes, text) == "");
         REQUIRE(text == *row.json);
         REQUIRE(to_text<psio::ijson>(schema, row.type, bytes, text) == "");
         REQUIRE(text == *row.ijson);
         // A type holding a custom text form: with NO handler the schema
         // path writes the wire type's own text (the row's plain_*), reads
         // it back to the same bytes, and the bytes never depended on it.
         REQUIRE(row.plain_json.has_value() == type.custom.value_or(false));
         REQUIRE(row.plain_ijson.has_value() == type.custom.value_or(false));
         if (row.plain_json)
         {
            ++plain;
            const use_handlers without(nullptr);
            Bytes              plain_bytes;
            REQUIRE(to_text<psio::json>(schema, row.type, bytes, text) == "");
            REQUIRE(text == *row.plain_json);
            REQUIRE(to_bytes<psio::json>(schema, row.type, text, plain_bytes) == "");
            REQUIRE(plain_bytes == bytes);
            REQUIRE(to_text<psio::ijson>(schema, row.type, bytes, text) == "");
            REQUIRE(text == *row.plain_ijson);
            REQUIRE(to_bytes<psio::ijson>(schema, row.type, text, plain_bytes) == "");
            REQUIRE(plain_bytes == bytes);
            // A caller that requires the exact presentation gets an error
            // instead of the fallback.
            ir::CustomHandlers strict;
            strict.require_all = true;
            const use_handlers required(&strict);
            REQUIRE(to_text<psio::json>(schema, row.type, bytes, text) ==
                    "pssz IR: no handler registered for a custom type");
            REQUIRE(to_bytes<psio::json>(schema, row.type, *row.json, plain_bytes) ==
                    "pssz IR: no handler registered for a custom type");
         }
      }
      else if (row.kind == "accept")
      {
         ++accepted;
         const bool ij = row.format == "ijson";
         REQUIRE((ij ? to_bytes<psio::ijson>(schema, row.type, input_text(row), bytes)
                     : to_bytes<psio::json>(schema, row.type, input_text(row), bytes)) == "");
         if (row.pssz) REQUIRE(bytes == from_hex(*row.pssz));
         REQUIRE((ij ? to_text<psio::ijson>(schema, row.type, bytes, text)
                     : to_text<psio::json>(schema, row.type, bytes, text)) == "");
         REQUIRE(text == *row.canonical);
      }
      else if (row.kind == "invariant")
      {
         // A value that violates an invariant. The schema cannot say so:
         // with no handler the value is its wire type — valid bytes, the
         // wire type's text. With the handlers every entry point reports
         // the invariant's own message; a strict registry without the
         // handler refuses the type.
         ++violated;
         const bool                        ij   = row.format == "ijson";
         const psio::schema_types::AnyType root = psio::schema_types::Type{row.type};
         bytes                                  = from_hex(*row.pssz);
         const std::span<const char> in{bytes};
         REQUIRE((ij ? to_bytes<psio::ijson>(schema, row.type, input_text(row), again)
                     : to_bytes<psio::json>(schema, row.type, input_text(row), again)) ==
                 *row.reason);
         REQUIRE(to_text<psio::json>(schema, row.type, bytes, text) == *row.reason);
         REQUIRE(to_text<psio::ijson>(schema, row.type, bytes, text) == *row.reason);
         auto status = ir::validate_pssz(schema, root, in, nullptr, &handlers);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what == *row.reason);
         ir::Value value;
         bool      unknown = false;
         status            = ir::decode_auto(schema, root, in, value, unknown, &handlers);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what == *row.reason);
         {
            const use_handlers without(nullptr);
            REQUIRE(ir::validate_pssz(schema, root, in).ok());
            REQUIRE(ir::decode_auto(schema, root, in, value, unknown).ok());
            REQUIRE(to_text<psio::json>(schema, row.type, bytes, text) == "");
            REQUIRE(text == *row.plain_json);
            REQUIRE(to_bytes<psio::json>(schema, row.type, text, again) == "");
            REQUIRE(again == bytes);
            REQUIRE(to_text<psio::ijson>(schema, row.type, bytes, text) == "");
            REQUIRE(text == *row.plain_ijson);
            REQUIRE(to_bytes<psio::ijson>(schema, row.type, text, again) == "");
            REQUIRE(again == bytes);
         }
         // Writers refuse the value too: binary and text.
         again.clear();
         status = ir::encode_auto(schema, root, value, again, &handlers);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what == *row.reason);
         REQUIRE(again.empty());
         text   = "keep";
         status = ir::value_to_json(schema, root, psio::ijson{}, value, text, &handlers);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what == *row.reason);
         REQUIRE(text == "keep");
         ir::CustomHandlers strict;
         strict.require_all = true;
         status             = ir::validate_pssz(schema, root, in, nullptr, &strict);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what ==
                 std::string_view{"pssz IR: no handler registered for a custom type"});
      }
      else
      {
         REQUIRE(row.kind == "reject");
         ++rejected;
         const bool ij = row.format == "ijson";
         bytes         = {'x'};
         auto st = ij ? ir::json_to_pssz(schema, psio::schema_types::Type{row.type}, psio::ijson{},
                                         std::span<const char>{input_text(row)}, bytes, nullptr,
                                         psio::json_parser::automatic, &handlers)
                      : ir::json_to_pssz(schema, psio::schema_types::Type{row.type}, psio::json{},
                                         std::span<const char>{input_text(row)}, bytes, nullptr,
                                         psio::json_parser::automatic, &handlers);
         REQUIRE_FALSE(st.ok());
         REQUIRE(bytes == Bytes{'x'});
         auto st2 = ij ? ir::json_to_pssz(schema, psio::schema_types::Type{row.type},
                                          psio::ijson{}, std::span<const char>{input_text(row)},
                                          bytes, &names)
                       : ir::json_to_pssz(schema, psio::schema_types::Type{row.type},
                                          psio::json{}, std::span<const char>{input_text(row)},
                                          bytes, &names);
         REQUIRE_FALSE(st2.ok());
         REQUIRE(std::string_view{st.error().what} == std::string_view{st2.error().what});
         REQUIRE(bytes == Bytes{'x'});
         for (const auto parser : {psio::json_parser::text, psio::json_parser::simdjson,
                                   psio::json_parser::simdjson_index})
         {
            auto st3 = ij ? ir::json_to_pssz(schema, psio::schema_types::Type{row.type},
                                             psio::ijson{},
                                             std::span<const char>{input_text(row)}, bytes,
                                             &names, parser)
                          : ir::json_to_pssz(schema, psio::schema_types::Type{row.type},
                                             psio::json{},
                                             std::span<const char>{input_text(row)}, bytes,
                                             &names, parser);
            REQUIRE_FALSE(st3.ok());
            REQUIRE(bytes == Bytes{'x'});
         }
      }
   }
   REQUIRE(values >= 600);
   REQUIRE(accepted >= 60);
   REQUIRE(rejected >= 300);
   REQUIRE(with_bytes >= 600);
   REQUIRE(plain >= 15);
   REQUIRE(violated >= 40);
   // Every expressible type is exercised.
   for (const auto& [id, t] : types)
      if (t.ir) REQUIRE(covered.count(id));
}

TEST_CASE("schema IR + PSSZ bytes agrees with the typed pssz validator",
          "[schema][conformance][pssz]")
{
   const auto         fx     = load<PsszFixture>(PSIO_PSSZ_VECTORS);
   const psio::Schema schema = schema_of(fx.schema_ir);
   std::map<std::string, bool> expressible;
   for (const auto& t : fx.types) expressible[t.id] = t.ir;
   REQUIRE(expressible.size() >= 200);

   // Every type: the offset width derived from the IR alone is the width
   // the typed codec uses (recorded by the emitter as `auto_width`).
   std::size_t widths = 0;
   for (const auto& t : fx.types)
   {
      INFO(t.id);
      REQUIRE(t.ir);  // nothing in this fixture is beyond the IR
      REQUIRE(t.auto_width.has_value());
      std::size_t width = 0;
      REQUIRE(ir::auto_width(schema, psio::schema_types::Type{t.id}, width).ok());
      REQUIRE(width == *t.auto_width);
      ++widths;
   }
   REQUIRE(widths == fx.types.size());

   // The handlers of the fixture's custom types (custom_conformance_types.hpp).
   ir::CustomHandlers handlers;
   custom_conf::for_each_custom(
      [&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
   ir::CustomHandlers strict;
   strict.require_all = true;

   std::size_t canonical = 0, unknown = 0, rejected = 0, raw_non_utf8 = 0, json_trips = 0,
               no_json = 0, explicit_width = 0, explicit_capped = 0, explicit_top_cap = 0,
               violated = 0, violated_explicit = 0;
   for (const auto& row : fx.vectors)
   {
      if (!expressible.at(row.type)) continue;
      INFO(row.name);
      const psio::schema_types::AnyType root = psio::schema_types::Type{row.type};
      const Bytes                       bytes = from_hex(row.hex);
      const std::string                 cls   = row.name.substr(0, row.name.find('/'));

      if (cls == "invariant")
      {
         // A structurally valid frame of a value that violates an
         // invariant: `reject` is the typed outcome. The schema alone
         // accepts it; with the handlers it is rejected with the
         // invariant's message, under every width profile.
         REQUIRE(row.expect == "reject");
         auto run = [&]<std::size_t W>(std::integral_constant<std::size_t, W>,
                                       const ir::CustomHandlers* registry) {
            ir::Value value;
            bool      has_unknown = false;
            return ir::decode_compatible<W>(schema, root, std::span<const char>{bytes}, value,
                                            has_unknown, registry);
         };
         auto with = [&](const ir::CustomHandlers* registry) {
            return row.format == "auto"
                      ? run(std::integral_constant<std::size_t, 0>{}, registry)
                      : row.format == "pssz8"
                           ? run(std::integral_constant<std::size_t, 1>{}, registry)
                           : row.format == "pssz16"
                                ? run(std::integral_constant<std::size_t, 2>{}, registry)
                                : run(std::integral_constant<std::size_t, 4>{}, registry);
         };
         REQUIRE(with(nullptr).ok());
         auto status = with(&handlers);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what == row.reason);
         status = with(&strict);
         REQUIRE_FALSE(status.ok());
         REQUIRE(status.error().what ==
                 std::string_view{"pssz IR: no handler registered for a custom type"});
         ++(row.format == "auto" ? violated : violated_explicit);
         continue;
      }

      if (row.format != "auto")
      {
         // The explicit profiles (one inherited width). A maxDynamicData cap
         // is enforced on the top-level type only, as psio::pssz_<W> does;
         // `cap-overflow-explicit-nested` records exactly that outcome.
         ir::Value value;
         bool      has_unknown = false;
         auto      run = [&]<std::size_t W>(std::integral_constant<std::size_t, W>) {
            return ir::decode_compatible<W>(schema, root, std::span<const char>{bytes}, value,
                                            has_unknown);
         };
         const auto st = row.format == "pssz8"
                            ? run(std::integral_constant<std::size_t, 1>{})
                            : row.format == "pssz16" ? run(std::integral_constant<std::size_t, 2>{})
                                                     : run(std::integral_constant<std::size_t, 4>{});
         REQUIRE((st.ok() ||
                  st.error().what != "pssz IR: unsupported schema constraint or annotation"));
         ++explicit_width;
         explicit_capped += cls.starts_with("cap-overflow");
         // The top-level type over its cap under an explicit width.
         explicit_top_cap += cls == "width" && row.name.find("/cap/") != std::string::npos &&
                             row.expect == "reject";
         if (row.expect == "reject")
            REQUIRE_FALSE(st.ok());
         else
         {
            REQUIRE(st.ok());
            REQUIRE(has_unknown == (row.expect == "unknown"));
         }
         continue;
      }

      bool       has_unknown = false;
      const auto st = ir::validate_pssz(schema, root, std::span<const char>{bytes}, &has_unknown);
      // The handlers change nothing for a value that satisfies its
      // invariants, or for a type that has none.
      REQUIRE(ir::validate_pssz(schema, root, std::span<const char>{bytes}, nullptr, &handlers)
                 .ok() == st.ok());
      if (cls == "string-raw-non-utf8")
      {
         // The IR tells a raw string ("string") from a UTF-8 one ("utf8"):
         // bytes that are not UTF-8 in a raw string are valid PSSZ through
         // the schema, exactly as for the typed codec, and round-trip. They
         // have no JSON text in either format: writing it is an error.
         REQUIRE(row.expect == "canonical");
         REQUIRE(st.ok());
         REQUIRE_FALSE(has_unknown);
         ir::Value raw;
         REQUIRE(ir::decode_auto(schema, root, std::span<const char>{bytes}, raw, has_unknown)
                    .ok());
         Bytes raw_again;
         REQUIRE(ir::encode_auto(schema, root, raw, raw_again).ok());
         REQUIRE(raw_again == bytes);
         std::string raw_text = "keep";
         REQUIRE(to_text<psio::ijson>(schema, row.type, bytes, raw_text) ==
                 "json: string is not valid UTF-8");
         REQUIRE(to_text<psio::json>(schema, row.type, bytes, raw_text) ==
                 "json: string is not valid UTF-8");
         ++raw_non_utf8;
         continue;
      }
      if (row.expect == "reject")
      {
         REQUIRE_FALSE(st.ok());
         ++rejected;
         continue;
      }
      REQUIRE(st.ok());
      REQUIRE(has_unknown == (row.expect == "unknown"));
      ir::Value value;
      REQUIRE(ir::decode_auto(schema, root, std::span<const char>{bytes}, value, has_unknown).ok());
      Bytes again;
      REQUIRE(ir::encode_auto(schema, root, value, again).ok());
      if (row.expect == "unknown")
      {
         REQUIRE(again == from_hex(row.reencoded));
         ++unknown;
         continue;
      }
      REQUIRE(again == bytes);
      ++canonical;

      // Bytes -> text -> bytes, in both text formats.
      std::string text;
      const auto  ijson = to_text<psio::ijson>(schema, row.type, bytes, text);
      if (ijson.find("no JSON") != std::string::npos)
      {
         ++no_json;  // wide integers, bit types, non-finite floats
         continue;
      }
      const bool lossy = has_some_none(value);
      // A string holding a Unicode noncharacter is JSON but not I-JSON.
      if (ijson != "ijson: string holds a Unicode noncharacter")
      {
         REQUIRE(ijson == "");
         REQUIRE(to_bytes<psio::ijson>(schema, row.type, text, again) == "");
         REQUIRE((again == bytes) == !lossy);
      }
      REQUIRE(to_text<psio::json>(schema, row.type, bytes, text) == "");
      REQUIRE(to_bytes<psio::json>(schema, row.type, text, again) == "");
      REQUIRE((again == bytes) == !lossy);
      json_trips += !lossy;
   }
   // Coverage floors (the fixture has 1301 automatic-profile vectors).
   CAPTURE(canonical, unknown, rejected, raw_non_utf8, json_trips, no_json, explicit_width);
   REQUIRE(canonical >= 850);
   REQUIRE(unknown >= 240);
   REQUIRE(rejected >= 180);
   REQUIRE(raw_non_utf8 >= 11);
   REQUIRE(json_trips >= 800);
   REQUIRE(no_json >= 1);
   REQUIRE(explicit_width >= 1000);
   REQUIRE(explicit_capped == 15);
   REQUIRE(explicit_top_cap >= 12);
   REQUIRE(violated >= 25);
   REQUIRE(violated_explicit >= 75);
}

TEST_CASE("the schema-driven entry points are checked", "[schema][conformance]")
{
   const auto         fx     = load<JsonFixture>(PSIO_JSON_VECTORS);
   const psio::Schema schema = schema_of(fx.schema_ir);
   Bytes              bytes;
   std::string        text;

   // A named type is addressed by its name.
   REQUIRE(ir::json_to_pssz(schema, "Inner", psio::ijson{},
                            std::span<const char>{std::string_view{
                               R"({"id":"18446744073709551615","label":"x","flag":true})"}},
                            bytes)
              .ok());
   REQUIRE(ir::pssz_to_json(schema, "Inner", psio::ijson{}, std::span<const char>{bytes}, text)
              .ok());
   REQUIRE(text == R"({"id":"18446744073709551615","label":"x","flag":true})");
   text.clear();
   REQUIRE(ir::pssz_to_json(schema, "Inner", psio::json{}, std::span<const char>{bytes}, text)
              .ok());
   REQUIRE(text == R"({"id":18446744073709551615,"label":"x","flag":true})");

   // Unknown type names, truncated and mutated bytes: a status, never a crash.
   REQUIRE_FALSE(ir::validate_pssz(schema, "NoSuchType", std::span<const char>{bytes}).ok());
   REQUIRE_FALSE(ir::json_to_pssz(schema, "NoSuchType", psio::json{},
                                  std::span<const char>{std::string_view{"1"}}, bytes)
                    .ok());
   for (const auto& row : fx.vectors)
   {
      if (!row.pssz || row.kind != "value") continue;
      const Bytes good = from_hex(*row.pssz);
      const psio::schema_types::AnyType root = psio::schema_types::Type{row.type};
      for (std::size_t cut = 0; cut < good.size() && cut < 48; ++cut)
      {
         const Bytes head(good.begin(), good.begin() + cut);
         (void)ir::validate_pssz(schema, root, std::span<const char>{head});
         Bytes flipped = good;
         flipped[cut]  = static_cast<char>(flipped[cut] ^ 0xff);
         std::string out;
         (void)ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{flipped}, out);
      }
      // Truncated text is rejected and leaves the output alone.
      const std::string& full = *row.ijson;
      for (std::size_t cut = 0; cut < full.size() && cut < 48; ++cut)
      {
         Bytes out{'x'};
         const std::string head = full.substr(0, cut);
         if (ir::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{head}, out).ok())
            continue;  // a prefix can be a value ("12" of "123")
         REQUIRE(out == Bytes{'x'});
      }
   }

   // Depth bombs in text.
   const std::string arrays(5000, '[');
   REQUIRE_FALSE(
      ir::json_to_pssz(schema, "Tree", psio::json{}, std::span<const char>{arrays}, bytes).ok());
   std::string objects;
   for (int i = 0; i < 5000; ++i) objects += "{\"children\":[";
   REQUIRE_FALSE(
      ir::json_to_pssz(schema, "Tree", psio::ijson{}, std::span<const char>{objects}, bytes).ok());
   std::string unknown = "{\"zzz\":";
   for (int i = 0; i < 5000; ++i) unknown += "[";
   REQUIRE_FALSE(ir::json_to_pssz(schema, "AllOptional", psio::ijson{},
                                  std::span<const char>{unknown}, bytes)
                    .ok());
}

// Malformed schemas: all three languages refuse the same ones, with an
// error, at every schema-driven entry point; their well-formed neighbours
// work. (Rust schema_conformance.rs, JS pssz-conformance.test.ts.)
TEST_CASE("schema fixture: malformed schemas are refused, their neighbours accepted",
          "[schema][conformance]")
{
   namespace ir = psio::pssz_ir;
   const auto fx = load<JsonFixture>(PSIO_JSON_VECTORS);
   REQUIRE(fx.schema_rejects.size() >= 25);
   REQUIRE(fx.schema_accepts.size() >= 8);
   std::set<std::string> names;
   for (const auto& row : fx.schema_rejects)
   {
      INFO(row.name);
      CHECK(names.insert(row.name).second);
      // The IR itself is a valid Schema value: it is its meaning that is refused.
      const psio::Schema                schema = schema_of(row.schema_ir);
      const psio::schema_types::AnyType root{psio::schema_types::Type{row.root}};
      std::size_t                       width = 0;
      const auto                        why   = ir::auto_width(schema, root, width);
      REQUIRE_FALSE(why.ok());
      CHECK(!why.error().what.empty());
      for (std::size_t n : {0u, 1u, 4u, 8u})
      {
         const std::vector<char> bytes(n, '\0');
         ir::Value               value;
         std::string             text;
         CHECK_FALSE(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());
         CHECK_FALSE(ir::decode_exact<4>(schema, root, std::span<const char>{bytes}, value).ok());
         CHECK_FALSE(
            ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, text).ok());
      }
      for (const std::string text : {"0", "\"\"", "null", "[]", "{}", "false"})
      {
         std::vector<char> out;
         ir::Value         value;
         CHECK_FALSE(
            ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{text}, out).ok());
         CHECK_FALSE(
            ir::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{text}, out).ok());
         CHECK_FALSE(
            ir::json_to_value(schema, root, psio::ijson{}, std::span<const char>{text}, value).ok());
      }
      std::vector<char> sink;
      CHECK_FALSE(ir::encode_auto(schema, root, ir::Value{true}, sink).ok());
   }
   for (const auto& row : fx.schema_accepts)
   {
      INFO(row.name);
      CHECK(names.insert(row.name).second);
      const psio::Schema                schema = schema_of(row.schema_ir);
      const psio::schema_types::AnyType root{psio::schema_types::Type{row.root}};
      const auto                        bytes = from_hex(row.pssz);
      CHECK(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());
      std::string json, ijson;
      REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, json).ok());
      REQUIRE(
         ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{bytes}, ijson).ok());
      CHECK(json == row.json);
      CHECK(ijson == row.ijson);
      std::vector<char> a, b;
      REQUIRE(ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{row.json}, a).ok());
      REQUIRE(
         ir::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{row.ijson}, b).ok());
      CHECK(a == bytes);
      CHECK(b == bytes);
   }
}

// The IR root says which version of the IR it is written in. The field was
// appended: a schema without it still decodes, and the codecs refuse it
// with an error that says why.
TEST_CASE("schema IR: the ir_version marker is required and backward compatible",
          "[schema][conformance]")
{
   namespace ir = psio::pssz_ir;
   const auto         fx      = load<JsonFixture>(PSIO_JSON_VECTORS);
   const psio::Schema current = schema_of(fx.schema_ir);
   REQUIRE(current.ir_version == std::optional<std::uint32_t>{psio::schema_ir_version});
   CHECK(psio::Schema{}.ir_version == std::optional<std::uint32_t>{psio::schema_ir_version});
   {
      psio::SchemaBuilder builder;
      builder.insert<std::uint32_t>("U");
      CHECK(std::move(builder).build().ir_version ==
            std::optional<std::uint32_t>{psio::schema_ir_version});
   }

   // The record as it was before the field existed.
   using schema_conf::OldSchema;
   STATIC_REQUIRE(psio::pssz_evolves_from<OldSchema, psio::Schema>);
   const OldSchema old{current.package, current.interfaces, current.worlds, current.uses,
                       current.types};
   const auto      old_bytes = psio::encode(psio::pssz{}, old);
   const auto      new_bytes = from_hex(fx.schema_ir);
   CHECK(old_bytes != new_bytes);
   // An unversioned schema is still a Schema: it decodes, with no version,
   // and re-encodes to the bytes it had.
   REQUIRE(psio::validate<psio::Schema>(psio::pssz{}, std::span<const char>{old_bytes}).ok());
   const auto decoded = psio::decode<psio::Schema>(psio::pssz{}, std::span<const char>{old_bytes});
   CHECK_FALSE(decoded.ir_version.has_value());
   CHECK(decoded.types == current.types);
   CHECK(psio::encode(psio::pssz{}, decoded) == old_bytes);
   // And a reader that predates the field reads a versioned schema.
   {
      OldSchema reader;
      bool      unknown = false;
      REQUIRE(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{new_bytes}, reader,
                                        unknown)
                 .ok());
      CHECK(unknown);
      CHECK(reader.types == current.types);
   }

   std::string                        id;
   for (const auto& t : fx.types)
      if (t.ir && t.pssz.value_or(false))
      {
         id = t.id;
         break;
      }
   REQUIRE(!id.empty());
   const psio::schema_types::AnyType root{psio::schema_types::Type{id}};
   std::size_t                       width = 0;
   CHECK(ir::auto_width(current, root, width).ok());

   auto newer = current;
   newer.ir_version = psio::schema_ir_version + 1;
   auto zero = current;
   zero.ir_version = 0;
   const std::pair<const psio::Schema*, std::string_view> cases[] = {
      {&decoded, "no ir_version marker"},
      {&newer, "newer than this library"},
      {&zero, "newer than this library"}};
   for (const auto& [schema, what] : cases)
   {
      INFO(what);
      ir::Value         value;
      std::vector<char> out;
      std::string       text;
      const std::string zero_text = "0";
      const psio::codec_status failures[] = {
         ir::auto_width(*schema, root, width),
         ir::validate_pssz(*schema, root, std::span<const char>{}),
         ir::encode_auto(*schema, root, ir::Value{true}, out),
         ir::decode_exact<1>(*schema, root, std::span<const char>{}, value),
         ir::json_to_pssz(*schema, root, psio::json{}, std::span<const char>{zero_text}, out),
         ir::json_to_value(*schema, root, psio::ijson{}, std::span<const char>{zero_text}, value),
         ir::value_to_json(*schema, root, psio::json{}, ir::Value{true}, text),
         ir::pssz_to_json(*schema, root, psio::ijson{}, std::span<const char>{}, text)};
      for (const auto& failure : failures)
      {
         REQUIRE_FALSE(failure.ok());
         CHECK(failure.error().what.find(what) != std::string_view::npos);
      }
   }
}
