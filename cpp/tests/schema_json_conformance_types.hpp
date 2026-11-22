#pragma once
//
// Rows of conformance/schema-json-vectors.json: the Schema IR
// (psio::Schema) in its three serialized forms. Shared by the emitter
// (schema_json_conformance_emit.cpp) and the consuming test
// (schema_json_conformance_tests.cpp).

#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/schema_ir.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace schema_json_conf
{
   //   schemas   one Schema: `ir_hex` is its PSSZ encoding, `json` and
   //             `ijson` its two texts. bytes -> Schema -> text gives exactly
   //             the texts; either text -> Schema -> bytes gives `ir_hex`.
   //   accepts   `text` is not what a writer produces (members reordered,
   //             an unknown key, a repeated key, a missing optional member, a
   //             small 64-bit integer as a string) but reads, under `format`,
   //             as the Schema whose PSSZ encoding is `ir_hex`.
   //   rejects   `text` is not a Schema under `format`.
   struct SchemaRow
   {
      std::string name, ir_hex, json, ijson;
   };
   PSIO_REFLECT(SchemaRow, name, ir_hex, json, ijson)

   struct AcceptRow
   {
      std::string name, format, text, ir_hex;
   };
   PSIO_REFLECT(AcceptRow, name, format, text, ir_hex)

   struct RejectRow
   {
      std::string name, format, text;
   };
   PSIO_REFLECT(RejectRow, name, format, text)

   struct Fixture
   {
      std::uint32_t          version = 0;
      std::vector<SchemaRow> schemas;
      std::vector<AcceptRow> accepts;
      std::vector<RejectRow> rejects;
   };
   PSIO_REFLECT(Fixture, version, schemas, accepts, rejects)

   inline std::string to_hex(std::span<const char> bytes)
   {
      std::string out;
      for (unsigned char c : bytes)
      {
         out += "0123456789abcdef"[c >> 4];
         out += "0123456789abcdef"[c & 15];
      }
      return out;
   }
   inline std::optional<std::vector<char>> from_hex(std::string_view text)
   {
      auto nib = [](char c) {
         return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      };
      if (text.size() % 2)
         return std::nullopt;
      std::vector<char> out;
      for (std::size_t i = 0; i < text.size(); i += 2)
      {
         const int hi = nib(text[i]), lo = nib(text[i + 1]);
         if (hi < 0 || lo < 0)
            return std::nullopt;
         out.push_back(static_cast<char>(hi << 4 | lo));
      }
      return out;
   }

   // The Schema IR of psio::Schema itself: what the schema-driven codec
   // (pssz_ir_json.hpp) needs to read and write a schema as a value.
   inline const psio::Schema& meta_schema()
   {
      static const psio::Schema schema = [] {
         psio::SchemaBuilder builder;
         builder.insert<psio::Schema>("Schema");
         return std::move(builder).build();
      }();
      return schema;
   }

   inline constexpr psio::json_parser all_parsers[] = {
      psio::json_parser::automatic, psio::json_parser::text, psio::json_parser::simdjson,
      psio::json_parser::simdjson_index};

   // Every way C++ turns `text` into the PSSZ bytes of a Schema: the typed
   // readers on each syntax layer (validate, validate_strict, the checked
   // and the fail-fast decode) and the schema-driven reader over the IR of
   // the IR. Returns the bytes when all of them accept and agree, nullopt
   // when all of them reject; `*mixed` is set when they disagree.
   template <typename Fmt>
   std::optional<std::vector<char>> read_schema_text(const std::string& text, bool* mixed)
   {
      const std::span<const char>      in{text};
      std::optional<std::vector<char>> bytes;
      std::size_t                      accepted = 0, tried = 0;
      auto                             note     = [&](std::optional<std::vector<char>> got) {
         ++tried;
         if (!got)
            return;
         if (accepted++ != 0 && *got != *bytes)
            *mixed = true;
         bytes = std::move(got);
      };
      auto typed = [](const psio::Schema& s) { return psio::encode(psio::pssz{}, s); };
      for (const auto parser : all_parsers)
      {
         psio::Schema out;
         const bool   decoded = psio::decode_json_checked(Fmt{}, in, out, parser).ok();
         if (decoded != psio::validate_json<psio::Schema>(Fmt{}, in, parser).ok())
            *mixed = true;
         note(decoded ? std::optional{typed(out)} : std::nullopt);

         std::vector<char> driven;
         const bool        ok = psio::pssz_ir::json_to_pssz(meta_schema(), "Schema", Fmt{}, in, driven,
                                                            nullptr, parser)
                            .ok();
         note(ok ? std::optional{std::move(driven)} : std::nullopt);
      }
      const bool valid = psio::validate<psio::Schema>(Fmt{}, in).ok();
      if (valid != psio::validate_strict<psio::Schema>(Fmt{}, in).ok())
         *mixed = true;
      // The fail-fast decode is run only on text the checked readers took.
      note(valid ? std::optional{typed(psio::decode<psio::Schema>(Fmt{}, in))} : std::nullopt);
      if (accepted != 0 && accepted != tried)
         *mixed = true;
      return bytes;
   }

   // Every way C++ turns the PSSZ bytes of a Schema into text: the typed
   // writers and the schema-driven one. nullopt when they disagree or fail.
   template <typename Fmt>
   std::optional<std::string> write_schema_text(std::span<const char> bytes)
   {
      if (!psio::validate<psio::Schema>(psio::pssz{}, bytes).ok())
         return std::nullopt;
      const psio::Schema schema = psio::decode<psio::Schema>(psio::pssz{}, bytes);
      const std::string  text   = psio::encode(Fmt{}, schema);
      std::string        checked, driven;
      if (!psio::encode_json_checked(Fmt{}, schema, checked).ok() || checked != text)
         return std::nullopt;
      if (!psio::pssz_ir::pssz_to_json(meta_schema(), "Schema", Fmt{}, bytes, driven).ok() ||
          driven != text)
         return std::nullopt;
      return text;
   }
}  // namespace schema_json_conf
