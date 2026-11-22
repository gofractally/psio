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

#include <catch.hpp>

#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace schema_conf
{
   struct TypeRow
   {
      std::string         id;
      bool                ir = false;
      std::optional<bool> pssz;
   };
   PSIO_REFLECT(TypeRow, id, ir, pssz)

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
   };
   PSIO_REFLECT(JsonRow, name, type, kind, format, json, ijson, text, text_hex, canonical, pssz)

   struct JsonFixture
   {
      std::uint32_t        version = 0;
      std::string          schema_ir;
      std::vector<TypeRow> types;
      std::vector<JsonRow> vectors;
   };
   PSIO_REFLECT(JsonFixture, version, schema_ir, types, vectors)

   struct PsszRow
   {
      std::string name;
      std::string type;
      std::string format;
      std::string hex;
      std::string expect;
      std::string reencoded;
   };
   PSIO_REFLECT(PsszRow, name, type, format, hex, expect, reencoded)

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
                                 std::span<const char>{text}, out);
      // The same decode with the schema's names compiled once (hash lookup
      // instead of a member scan) gives the same bytes or the same error.
      const ir::JsonNames names(schema);
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
                                 std::span<const char>{bytes}, out);
      return st.ok() ? "" : std::string(st.error().what);
   }
}  // namespace

TEST_CASE("schema IR + JSON text agrees with the typed json, ijson and pssz codecs",
          "[schema][conformance][json]")
{
   const auto         fx     = load<JsonFixture>(PSIO_JSON_VECTORS);
   const psio::Schema schema = schema_of(fx.schema_ir);
   const ir::JsonNames names(schema);
   std::map<std::string, TypeRow> types;
   for (const auto& t : fx.types) types[t.id] = t;
   REQUIRE(types.size() >= 40);

   std::size_t values = 0, accepted = 0, rejected = 0, with_bytes = 0;
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
      else
      {
         REQUIRE(row.kind == "reject");
         ++rejected;
         const bool ij = row.format == "ijson";
         bytes         = {'x'};
         auto st = ij ? ir::json_to_pssz(schema, psio::schema_types::Type{row.type}, psio::ijson{},
                                         std::span<const char>{input_text(row)}, bytes)
                      : ir::json_to_pssz(schema, psio::schema_types::Type{row.type}, psio::json{},
                                         std::span<const char>{input_text(row)}, bytes);
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

   std::size_t canonical = 0, unknown = 0, rejected = 0, raw_non_utf8 = 0, json_trips = 0,
               no_json = 0, explicit_width = 0;
   for (const auto& row : fx.vectors)
   {
      if (!expressible.at(row.type)) continue;
      INFO(row.name);
      const psio::schema_types::AnyType root = psio::schema_types::Type{row.type};
      const Bytes                       bytes = from_hex(row.hex);
      const std::string                 cls   = row.name.substr(0, row.name.find('/'));

      if (row.format != "auto")
      {
         // The explicit profiles (one inherited width). The IR codec does
         // not implement caps there and says so; those types are skipped.
         if (cls == "cap-overflow-explicit-nested") continue;
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
         if (!st.ok() &&
             st.error().what == "pssz IR: unsupported schema constraint or annotation")
            continue;
         ++explicit_width;
         if (cls == "string-raw-non-utf8")
            REQUIRE_FALSE(st.ok());
         else if (row.expect == "reject")
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
      if (cls == "string-raw-non-utf8")
      {
         // The IR has one "string" for raw and UTF-8 strings and the codec
         // treats it as UTF-8: these frames, valid for the typed raw
         // string, are rejected through the schema. Pinned so that a change
         // is noticed; see docs/json.md.
         REQUIRE(row.expect == "canonical");
         REQUIRE_FALSE(st.ok());
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
