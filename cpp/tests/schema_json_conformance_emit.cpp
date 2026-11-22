// Emitter for conformance/schema-json-vectors.json: the Schema IR
// (psio::Schema) as PSSZ bytes, psio::json text and psio::ijson text.
//
//   psio_schema_json_conformance_emit <out.json>          write the fixture
//   psio_schema_json_conformance_emit --check <ref.json>  regenerate and compare
//
// The schemas are the ones the other shared fixtures record (built by the
// same code, from the same type headers) plus one written by hand that holds
// every IR alternative. Every row is computed by the C++ implementation and
// checked here against every typed entry point and the schema-driven codec
// over the IR of the IR; generation aborts when they disagree, so the
// fixture cannot silently record a bug. Output is deterministic.

#include "json_conformance_types.hpp"
#include "pssz_conformance_types.hpp"
#include "schema_json_conformance_types.hpp"
#include "time_key_conformance_types.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace
{
   using namespace schema_json_conf;
   using namespace psio::schema_types;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "schema json conformance emit: %s\n", message.c_str());
      std::exit(2);
   }

   Fixture               fx;
   std::set<std::string> names;

   void unique(const std::string& name)
   {
      if (!names.insert(name).second)
         die("duplicate row " + name);
   }

   // The I-JSON name of AnyType alternative A: its reflected name, the one
   // the writer uses. Reject and accept texts are spelled with it.
   template <typename A>
   std::string alt()
   {
      return std::string(psio::get_type_name(static_cast<const A*>(nullptr)));
   }
   // The json index of AnyType alternative A.
   template <typename A>
   std::string index_of()
   {
      return std::to_string(AnyType{A{}}.value.index());
   }

   template <typename Fmt>
   std::string text_of(const Schema& schema, const std::string& name)
   {
      const auto bytes = psio::encode(psio::pssz{}, schema);
      const auto text  = write_schema_text<Fmt>(std::span<const char>{bytes});
      if (!text)
         die(name + ": the " + Fmt::name + " writers disagree");
      bool       mixed = false;
      const auto back  = read_schema_text<Fmt>(*text, &mixed);
      if (mixed || !back || *back != bytes)
         die(name + ": the " + Fmt::name + " text does not read back to the same bytes");
      return *text;
   }

   void schema(const std::string& name, const Schema& value)
   {
      unique(name);
      const auto bytes = psio::encode(psio::pssz{}, value);
      if (!(psio::decode<Schema>(psio::pssz{}, std::span<const char>{bytes}) == value))
         die(name + ": the PSSZ bytes do not read back");
      fx.schemas.push_back(SchemaRow{name, to_hex(bytes), text_of<psio::json>(value, name),
                                     text_of<psio::ijson>(value, name)});
   }

   template <typename Fmt>
   void accept(const std::string& name, const std::string& text, const Schema& value)
   {
      const std::string full = std::string(Fmt::name) + "/" + name;
      unique("accept/" + full);
      bool       mixed = false;
      const auto bytes = read_schema_text<Fmt>(text, &mixed);
      if (mixed || !bytes)
         die("accept/" + full + ": not accepted by every reader: " + text);
      if (*bytes != psio::encode(psio::pssz{}, value))
         die("accept/" + full + ": reads as another schema: " + text);
      fx.accepts.push_back(AcceptRow{full, Fmt::name, text, to_hex(*bytes)});
   }

   template <typename Fmt>
   void reject(const std::string& name, const std::string& text)
   {
      const std::string full = std::string(Fmt::name) + "/" + name;
      unique("reject/" + full);
      bool mixed = false;
      if (read_schema_text<Fmt>(text, &mixed) || mixed)
         die("reject/" + full + ": accepted by a reader: " + text);
      fx.rejects.push_back(RejectRow{full, Fmt::name, text});
   }

   // ── A schema holding every IR alternative ──────────────────────────────
   //
   // It is data for the IR's own codecs, not a schema to encode values
   // with: it has no ir_version, and several of its types have no PSSZ form.
   Schema every_alternative()
   {
      using B = Box<AnyType>;
      constexpr std::uint64_t safe = (std::uint64_t{1} << 53) - 1;  // largest bare I-JSON integer
      const Attribute         flag{"final", std::nullopt};

      Schema s;
      s.package = Package{"test:every-alternative", "1.2.3",
                          {Attribute{"since", "\"1.0\""}, Attribute{"unstable", "feature = \"x\\y\""}, flag}};
      s.uses    = {Use{"wasi:io", "streams", "0.2.0",
                       {UseItem{"input-stream", std::nullopt}, UseItem{"output-stream", "out"}}},
                   Use{"test:other", "types", "", {}}};
      s.interfaces = {
         Interface{"api",
                   {"point", "shape"},
                   {Func{"area", {Member{"shape", B{Type{"shape"}}, {flag}}}, B{Float{11, 52}},
                         {Attribute{"pure", std::nullopt}}},
                    Func{"reset", {}, std::nullopt, {}}},
                   {Attribute{"since", "2"}}},
         Interface{"empty", {}, {}, {}}};
      s.worlds = {World{"service", {UseRef{"wasi:io", "streams"}}, {"api"}, {Attribute{"deprecated", "3"}}},
                  World{"bare", {}, {}, {}}};

      s.insert("point", Struct{{Member{"x", B{Int{32, true}}, {}}, Member{"y", B{Int{32, true}}, {}}},
                               {Attribute{"definitionWillNotChange", std::nullopt}}});
      s.insert("shape",
               Object{{Member{"name", B{Type{"text"}}, {Attribute{"lengthBound", "min=1,max=8"}}},
                       Member{"id", B{Int{64, false}}, {Attribute{"ijsonInt", "number"}, Attribute{"jsonInt", "string"}}},
                       Member{"corners", B{List{B{Type{"point"}}}}, {}}},
                      {Attribute{"maxDynamicData", "65535"}}});
      // 64-bit integers on both sides of what a bare I-JSON number holds.
      s.insert("array-small", Array{B{Int{8, false}}, 4});
      s.insert("array-safe", Array{B{Int{16, false}}, safe});
      s.insert("array-beyond-safe", Array{B{Int{1, false}}, safe + 1});
      s.insert("array-max", Array{B{Type{"point"}}, ~std::uint64_t{0}});
      s.insert("list", List{B{List{B{Option{B{Type{"shape"}}}}}}});
      s.insert("bounded-small", BoundedList{B{Float{8, 23}}, 0});
      s.insert("bounded-beyond-safe", BoundedList{B{Type{"point"}}, safe + 2});
      s.insert("option", Option{B{Option{B{Int{128, false}}}}});
      s.insert("variant",
               Variant{{Member{"none", B{Tuple{}}, {}},
                        Member{"some", B{Type{"point"}}, {Attribute{"ijsonName", "Point"}}},
                        Member{"text", B{Type{"text"}}, {}}},
                       {flag}});
      s.insert("tuple-empty", Tuple{});
      s.insert("tuple", Tuple{{B{Int{256, false}}, B{Type{"text"}}, B{Tuple{{B{Int{8, true}}}}}}});
      s.insert("int", Int{64, true});
      s.insert("float", Float{11, 52});
      s.insert("nested", psio::schema_types::FracPack{B{Type{"shape"}}});
      s.insert("text", Custom{B{List{B{Int{8, false}}}}, "utf8"});
      s.insert("boxed", Custom{B{Type{"shape"}}, "box"});
      s.insert("resource",
               Resource{"connection",
                        {Func{"send", {Member{"data", B{Type{"text"}}, {}}, Member{"flags", B{Int{32, false}}, {}}},
                              B{Option{B{Int{64, false}}}}, {}},
                         Func{"close", {}, std::nullopt, {flag}}},
                        {Attribute{"since", "\"0.2\""}}});
      s.insert("alias", Type{"point"});
      s.insert("enum", Enum{B{Int{64, true}},
                            {Enumerator{"zero", 0}, Enumerator{"safe", safe}, Enumerator{"beyond", safe + 1},
                             Enumerator{"minus-one", ~std::uint64_t{0}}}});
      // Names are strings: escapes and non-ASCII text.
      s.insert("na\xC3\xAFve \"quoted\" \\ \x01\n\t\xF0\x9F\x99\x82", Int{8, false});
      s.types.back().attributes = {Attribute{"doc", "line one\nline two"}, Attribute{"", ""}};
      s.ir_version = std::nullopt;

      // Every alternative is present.
      std::set<std::size_t> seen;
      for (const auto& m : s.types)
         seen.insert(m.type->value.index());
      if (seen.size() != std::variant_size_v<decltype(AnyType::value)>)
         die("every-alternative does not hold every alternative");
      return s;
   }

   // ── Texts no writer produces ───────────────────────────────────────────
   //
   // One schema with one type, spelled around the text of that type.
   const std::string head =
      R"({"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],)"
      R"("types":[{"name":"T","type":)";
   const std::string tail = R"(,"attributes":[]}],"ir_version":1})";

   Schema one(AnyType type)
   {
      Schema s;
      s.insert("T", std::move(type));
      return s;
   }

   void emit_texts()
   {
      constexpr std::uint64_t safe = (std::uint64_t{1} << 53) - 1;
      const std::string       safe_text = std::to_string(safe), beyond = std::to_string(safe + 1);
      const Schema            u32  = one(Int{32, false});
      const Schema            arr  = one(Array{Box<AnyType>{Int{8, false}}, 7});
      const Schema            big  = one(Array{Box<AnyType>{Int{8, false}}, safe + 1});
      const std::string       elem_i = R"({"value":{")" + alt<Int>() + R"(":{"bits":8,"isSigned":false}}})";
      const std::string       elem_j = R"({"value":[)" + index_of<Int>() + R"(,{"bits":8,"isSigned":false}]})";
      auto ijson_int = [&](const std::string& body) { return R"({"value":{")" + alt<Int>() + "\":" + body + "}}"; };
      auto json_int  = [&](const std::string& body) { return R"({"value":[)" + index_of<Int>() + "," + body + "]}"; };
      auto ijson_arr = [&](const std::string& len) {
         return R"({"value":{")" + alt<Array>() + R"(":{"type":)" + elem_i + R"(,"len":)" + len + "}}}";
      };
      auto json_arr = [&](const std::string& len) {
         return R"({"value":[)" + index_of<Array>() + R"(,{"type":)" + elem_j + R"(,"len":)" + len + "}]}";
      };
      const std::string int_body = R"({"bits":32,"isSigned":false})";

      // The writers' own text of these schemas, as the controls the other
      // rows differ from in one place.
      if (psio::encode(psio::ijson{}, u32) != head + ijson_int(int_body) + tail ||
          psio::encode(psio::json{}, u32) != head + json_int(int_body) + tail ||
          psio::encode(psio::ijson{}, arr) != head + ijson_arr("7") + tail ||
          psio::encode(psio::json{}, arr) != head + json_arr("7") + tail ||
          psio::encode(psio::ijson{}, big) != head + ijson_arr("\"" + beyond + "\"") + tail ||
          psio::encode(psio::json{}, big) != head + json_arr(beyond) + tail)
         die("the control texts are not what the writers produce");

      auto both_accept = [&](const std::string& name, const std::string& before,
                             const std::string& ijson_type, const std::string& json_type,
                             const std::string& after, const Schema& value) {
         accept<psio::ijson>(name, before + ijson_type + after, value);
         accept<psio::json>(name, before + json_type + after, value);
      };
      auto both_reject = [&](const std::string& name, const std::string& before,
                             const std::string& ijson_type, const std::string& json_type,
                             const std::string& after) {
         reject<psio::ijson>(name, before + ijson_type + after);
         reject<psio::json>(name, before + json_type + after);
      };

      // ── accepted ─────────────────────────────────────────────────────────
      both_accept("whitespace", " {\"package\" : {\"name\":\"\",\"version\":\"\",\"attributes\":[ ]},\n"
                                "\"interfaces\":[],\t\"worlds\":[],\"uses\":[],\r\n\"types\":[{\"name\":\"T\",\"type\":",
                  ijson_int(int_body), json_int(int_body), ",\"attributes\":[]} ],\"ir_version\":1 } ", u32);
      both_accept("members_reordered",
                  R"({"ir_version":1,"types":[{"attributes":[],"type":)", ijson_int(R"({"isSigned":false,"bits":32})"),
                  json_int(R"({"isSigned":false,"bits":32})"),
                  R"(,"name":"T"}],"uses":[],"worlds":[],"interfaces":[],"package":{"attributes":[],"version":"","name":""}})",
                  u32);
      both_accept("unknown_keys_skipped", R"({"$schema":{"a":[1,{"b":null}]},)" + head.substr(1),
                  ijson_int(R"({"bits":32,"doc":"x","isSigned":false})"),
                  json_int(R"({"bits":32,"doc":"x","isSigned":false})"), R"(,"resolved":null)" + tail, u32);
      both_accept("repeated_key_last_wins", head, ijson_int(R"({"bits":8,"isSigned":true,"bits":32,"isSigned":false})"),
                  json_int(R"({"bits":8,"isSigned":true,"bits":32,"isSigned":false})"), tail, u32);
      {
         // ir_version is an optional member: absent and null are the same.
         Schema unversioned    = u32;
         unversioned.ir_version = std::nullopt;
         both_accept("ir_version_missing", head, ijson_int(int_body), json_int(int_body), R"(,"attributes":[]}]})",
                     unversioned);
         both_accept("ir_version_null", head, ijson_int(int_body), json_int(int_body),
                     R"(,"attributes":[]}],"ir_version":null})", unversioned);
         Schema later    = u32;
         later.ir_version = 4294967295u;
         both_accept("ir_version_u32_max", head, ijson_int(int_body), json_int(int_body),
                     R"(,"attributes":[]}],"ir_version":4294967295})", later);
      }
      // I-JSON reads a 64-bit integer from its string form at any value.
      accept<psio::ijson>("u64_small_as_string", head + ijson_arr("\"7\"") + tail, arr);
      accept<psio::ijson>("u64_safe_max_bare", head + ijson_arr(safe_text) + tail,
                          one(Array{Box<AnyType>{Int{8, false}}, safe}));
      accept<psio::ijson>("u64_max_as_string", head + ijson_arr("\"18446744073709551615\"") + tail,
                          one(Array{Box<AnyType>{Int{8, false}}, ~std::uint64_t{0}}));
      accept<psio::json>("u64_max_bare", head + json_arr("18446744073709551615") + tail,
                         one(Array{Box<AnyType>{Int{8, false}}, ~std::uint64_t{0}}));

      // ── rejected ─────────────────────────────────────────────────────────
      reject<psio::ijson>("unknown_alternative_name", head + R"({"value":{"Integer":)" + int_body + "}}" + tail);
      reject<psio::ijson>("alternative_name_wrong_case", head + R"({"value":{"int":)" + int_body + "}}" + tail);
      reject<psio::ijson>("variant_object_two_members",
                          head + R"({"value":{")" + alt<Int>() + "\":" + int_body + ",\"" + alt<Float>() +
                             R"(":{"exp":8,"mantissa":23}}})" + tail);
      reject<psio::ijson>("variant_object_repeated_member",
                          head + R"({"value":{")" + alt<Int>() + "\":" + int_body + ",\"" + alt<Int>() + "\":" +
                             int_body + "}}" + tail);
      reject<psio::ijson>("variant_object_empty", head + R"({"value":{}})" + tail);
      reject<psio::ijson>("variant_in_json_form", head + json_int(int_body) + tail);
      reject<psio::json>("variant_in_ijson_form", head + ijson_int(int_body) + tail);
      reject<psio::json>("variant_index_out_of_range",
                         head + R"({"value":[)" +
                            std::to_string(std::variant_size_v<decltype(AnyType::value)>) + "," + int_body + "]}" +
                            tail);
      reject<psio::json>("variant_index_of_another_alternative",
                         head + R"({"value":[)" + index_of<Type>() + "," + int_body + "]}" + tail);
      reject<psio::json>("variant_index_negative", head + R"({"value":[-1,)" + int_body + "]}" + tail);
      reject<psio::json>("variant_index_as_string",
                         head + R"({"value":[")" + index_of<Int>() + "\"," + int_body + "]}" + tail);
      reject<psio::json>("variant_array_one_element", head + R"({"value":[)" + index_of<Int>() + "]}" + tail);
      reject<psio::json>("variant_array_three_elements",
                         head + R"({"value":[)" + index_of<Int>() + "," + int_body + ",null]}" + tail);
      both_reject("missing_required_member_bits", head, ijson_int(R"({"isSigned":false})"),
                  json_int(R"({"isSigned":false})"), tail);
      both_reject("missing_required_member_types",
                  R"({"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],"ir_version":1})",
                  "", "", "");
      both_reject("missing_required_member_attributes", head, ijson_int(int_body), json_int(int_body),
                  R"(}],"ir_version":1})");
      both_reject("missing_any_type_value", head, "{}", "{}", tail);
      both_reject("any_type_without_its_record", head, R"({")" + alt<Int>() + "\":" + int_body + "}",
                  "[" + index_of<Int>() + "," + int_body + "]", tail);
      reject<psio::ijson>("u64_bare_beyond_safe", head + ijson_arr(beyond) + tail);
      reject<psio::ijson>("u64_bare_max", head + ijson_arr("18446744073709551615") + tail);
      reject<psio::json>("u64_as_string", head + json_arr("\"7\"") + tail);
      both_reject("u64_out_of_range", head, ijson_arr("\"18446744073709551616\""),
                  json_arr("18446744073709551616"), tail);
      both_reject("u64_negative", head, ijson_arr("-1"), json_arr("-1"), tail);
      both_reject("u64_fraction", head, ijson_arr("7.0"), json_arr("7.0"), tail);
      both_reject("u32_out_of_range", head, ijson_int(R"({"bits":4294967296,"isSigned":false})"),
                  json_int(R"({"bits":4294967296,"isSigned":false})"), tail);
      both_reject("bool_as_number", head, ijson_int(R"({"bits":32,"isSigned":0})"),
                  json_int(R"({"bits":32,"isSigned":0})"), tail);
      both_reject("null_for_a_type", head, "null", "null", tail);
      both_reject("ir_version_as_string", head, ijson_int(int_body), json_int(int_body),
                  R"(,"attributes":[]}],"ir_version":"1"})");
      both_reject("trailing_text", head, ijson_int(int_body), json_int(int_body), tail + "{}");
      both_reject("truncated", head, ijson_int(int_body), json_int(int_body), tail.substr(0, tail.size() - 1));
      both_reject("root_is_an_array", "[" + head, ijson_int(int_body), json_int(int_body), tail + "]");

      // ── nesting ──────────────────────────────────────────────────────────
      //
      // The readers' limit of 64 nested arrays / objects bounds how deep a
      // type nests inline: each List level is three (the AnyType record, the
      // variant, the alternative's record) under the five of the schema, the
      // types list, the member and the innermost Int.
      auto lists = [](std::size_t levels) {
         AnyType type = Int{8, false};
         for (std::size_t i = 0; i < levels; ++i)
            type = List{Box<AnyType>{std::move(type)}};
         return type;
      };
      auto nested_text = [&]<typename Fmt>(Fmt, std::size_t levels) {
         std::string open, close;
         for (std::size_t i = 0; i < levels; ++i)
         {
            open += Fmt::i_json ? R"({"value":{")" + alt<List>() + R"(":{"type":)"
                                : R"({"value":[)" + index_of<List>() + R"(,{"type":)";
            close += Fmt::i_json ? "}}}" : "}]}";
         }
         return head + open + (Fmt::i_json ? elem_i : elem_j) + close + tail;
      };
      constexpr std::size_t deepest = (psio::kMaxValidationDepth - 6) / 3;  // 19
      if (psio::encode(psio::ijson{}, one(lists(deepest))) != nested_text(psio::ijson{}, deepest) ||
          psio::encode(psio::json{}, one(lists(deepest))) != nested_text(psio::json{}, deepest))
         die("the nested control text is not what the writers produce");
      schema("lists-nested-to-the-limit", one(lists(deepest)));
      reject<psio::ijson>("nested_beyond_the_depth_limit", nested_text(psio::ijson{}, deepest + 1));
      reject<psio::json>("nested_beyond_the_depth_limit", nested_text(psio::json{}, deepest + 1));
   }

   std::string quoted(const std::string& s) { return psio::encode(psio::json{}, s); }

   std::string render()
   {
      std::string out = "{\"version\":1,\"schemas\":[\n";
      for (std::size_t i = 0; i < fx.schemas.size(); ++i)
      {
         const auto& r = fx.schemas[i];
         out += "{\"name\":" + quoted(r.name) + ",\"ir_hex\":" + quoted(r.ir_hex) + ",\"json\":" +
                quoted(r.json) + ",\"ijson\":" + quoted(r.ijson) + (i + 1 < fx.schemas.size() ? "},\n" : "}\n");
      }
      out += "],\"accepts\":[\n";
      for (std::size_t i = 0; i < fx.accepts.size(); ++i)
      {
         const auto& r = fx.accepts[i];
         out += "{\"name\":" + quoted(r.name) + ",\"format\":" + quoted(r.format) + ",\"text\":" +
                quoted(r.text) + ",\"ir_hex\":" + quoted(r.ir_hex) + (i + 1 < fx.accepts.size() ? "},\n" : "}\n");
      }
      out += "],\"rejects\":[\n";
      for (std::size_t i = 0; i < fx.rejects.size(); ++i)
      {
         const auto& r = fx.rejects[i];
         out += "{\"name\":" + quoted(r.name) + ",\"format\":" + quoted(r.format) + ",\"text\":" +
                quoted(r.text) + (i + 1 < fx.rejects.size() ? "},\n" : "}\n");
      }
      out += "]}\n";
      return out;
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   if (!check && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json>\n", argv[0]);
      return 2;
   }
   // The schemas the other shared fixtures record as `schema_ir`.
   schema("pssz-vectors", pssz_conf::fixture_schema());
   schema("json-vectors", json_conf::fixture_schema());
   schema("time-key-vectors", tk_conf::fixture_schema());
   // The IR of the IR: psio::Schema described by a schema.
   schema("schema-of-schema", meta_schema());
   schema("empty", Schema{});
   schema("every-alternative", every_alternative());
   emit_texts();

   const std::string text = render();
   Fixture           parsed;
   if (!psio::decode_json_checked(psio::json{}, std::span<const char>{text}, parsed).ok() ||
       parsed.schemas.size() != fx.schemas.size() || parsed.accepts.size() != fx.accepts.size() ||
       parsed.rejects.size() != fx.rejects.size())
      die("the rendered fixture does not read back");

   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "schema json conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended format change\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("wrote %zu schemas, %zu accepts, %zu rejects to %s\n", fx.schemas.size(),
               fx.accepts.size(), fx.rejects.size(), argv[1]);
   return out ? 0 : 1;
}
