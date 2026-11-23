// Emitter for conformance/variant-label-vectors.json: the label of every
// alternative of the variants in variant_label_conformance_types.hpp, the
// Schema IR that carries the labels, and one value per alternative with its
// bytes and text (conformance/variant-label-vectors.md).
//
//   psio_variant_label_conformance_emit <out.json>          write the fixture
//   psio_variant_label_conformance_emit --check <ref.json>  regenerate and compare
//
// Every row is computed by the typed C++ codecs and checked here against the
// schema-driven codecs over the fixture's own schema; generation aborts when
// they disagree. Output is deterministic.
#include "variant_label_conformance_types.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace
{
   using namespace label_conf;
   namespace ir = psio::pssz_ir;
   using Bytes  = std::vector<char>;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "variant label conformance emit: %s\n", message.c_str());
      std::exit(2);
   }
   std::string to_hex(std::span<const char> bytes)
   {
      std::string out;
      for (unsigned char c : bytes)
      {
         out += "0123456789abcdef"[c >> 4];
         out += "0123456789abcdef"[c & 15];
      }
      return out;
   }
   std::span<const char> sp(const std::string& s) { return {s.data(), s.size()}; }

   struct VariantRow
   {
      std::string              name;
      std::vector<std::string> labels;
   };
   PSIO_REFLECT(VariantRow, name, labels)
   struct Row
   {
      std::string   name;
      std::string   type;
      std::uint32_t index = 0;
      std::string   label;
      std::string   pssz;
      std::string   json;
      std::string   ijson;
   };
   PSIO_REFLECT(Row, name, type, index, label, pssz, json, ijson)

   std::vector<VariantRow> variants;
   std::vector<Row>        rows;
   std::set<std::string>   names;

   const psio::Schema& schema() { return fixture_schema(); }

   template <typename V>
   void describe(const std::string& name, const std::vector<std::string>& expected)
   {
      VariantRow v{name, {}};
      for (auto label : psio::variant_label_table<V>::_names_array_)
         v.labels.emplace_back(label);
      if (v.labels != expected)
         die(name + ": the labels are not the expected ones");
      // The schema names each alternative by its label and says nothing else.
      const auto* node = schema().get(name);
      const auto* wire = node ? node->resolve(schema()) : nullptr;
      const auto* var  = wire ? std::get_if<psio::schema_types::Variant>(&wire->value) : nullptr;
      if (!var || var->members.size() != v.labels.size())
         die(name + ": the schema has no such variant");
      for (std::size_t i = 0; i < v.labels.size(); ++i)
         if (var->members[i].name != v.labels[i] || !var->members[i].attributes.empty())
            die(name + ": the schema does not name alternative " + std::to_string(i) +
                " by its label");
      variants.push_back(std::move(v));
   }

   template <typename V>
   void value(const std::string& type, const std::string& name, const V& v)
   {
      const std::string full = type + "/" + name;
      if (!names.insert(full).second)
         die("duplicate row " + full);
      Row r{full, type, static_cast<std::uint32_t>(v.index()),
            std::string{psio::variant_label_table<V>::_names_array_[v.index()]}, {}, {}, {}};
      const Bytes       bytes = psio::encode(psio::pssz{}, v);
      const std::string json  = psio::encode(psio::json{}, v);
      const std::string ijson = psio::encode(psio::ijson{}, v);
      // The tagged text form is keyed by the label.
      if (ijson.rfind("{" + psio::encode(psio::json{}, r.label) + ":", 0) != 0)
         die(full + ": the I-JSON text is not keyed by the label: " + ijson);
      const psio::schema_types::AnyType root{psio::schema_types::Type{type}};
      for (int ij = 0; ij < 2; ++ij)
      {
         const std::string& text = ij ? ijson : json;
         V                  back;
         if (!(ij ? psio::decode_json_checked(psio::ijson{}, sp(text), back)
                  : psio::decode_json_checked(psio::json{}, sp(text), back))
                 .ok() ||
             !(back == v))
            die(full + ": the text does not read back");
         Bytes       dyn;
         std::string dyn_text;
         bool        unknown = false;
         if (!(ij ? ir::json_to_pssz(schema(), root, psio::ijson{}, sp(text), dyn)
                  : ir::json_to_pssz(schema(), root, psio::json{}, sp(text), dyn))
                 .ok() ||
             dyn != bytes)
            die(full + ": the schema-driven reader disagrees on " + text);
         if (!(ij ? ir::pssz_to_json(schema(), root, psio::ijson{}, bytes, dyn_text, &unknown)
                  : ir::pssz_to_json(schema(), root, psio::json{}, bytes, dyn_text, &unknown))
                 .ok() ||
             dyn_text != text)
            die(full + ": the schema-driven writer writes " + dyn_text + ", not " + text);
      }
      r.pssz  = to_hex(bytes);
      r.json  = json;
      r.ijson = ijson;
      rows.push_back(std::move(r));
   }

   void emit()
   {
      if (!ir::validate_schema(schema()).ok())
         die("the fixture schema is not valid");
      describe<Records>("Records", {"Circle", "Square"});
      describe<Scalars>("Scalars", {"bool", "uint8", "int16", "uint32", "int64", "uint64",
                                    "float32", "double", "string"});
      describe<Generic>("Generic", {"uint32[]", "uint32?", "tuple_uint32_string", "uint16[2]",
                                    "Circle[]", "uint8[]"});
      describe<Repeated>("Repeated", {"uint32", "1", "2", "Circle"});
      describe<Stated>("Stated", {"circle", "square", "side"});

      value("Records", "circle", Records{Circle{3}});
      value("Records", "square", Records{Square{4}});

      value("Scalars", "bool", Scalars{true});
      value("Scalars", "uint8", Scalars{u8{200}});
      value("Scalars", "int16", Scalars{i16{-2}});
      value("Scalars", "uint32", Scalars{u32{7}});
      value("Scalars", "int64", Scalars{i64{-9007199254740993}});
      value("Scalars", "uint64", Scalars{~u64{0}});
      value("Scalars", "float32", Scalars{1.5f});
      value("Scalars", "double", Scalars{-0.25});
      value("Scalars", "string", Scalars{std::string{"hi"}});

      value("Generic", "list", Generic{std::vector<u32>{1, 2}});
      value("Generic", "optional_some", Generic{std::optional<u32>{5}});
      value("Generic", "optional_none", Generic{std::optional<u32>{}});
      value("Generic", "tuple", Generic{std::tuple<u32, std::string>{9, "x"}});
      value("Generic", "array", Generic{std::array<u16, 2>{1, 2}});
      value("Generic", "records", Generic{std::vector<Circle>{Circle{1}, Circle{2}}});
      value("Generic", "bytes", Generic{std::vector<u8>{0xde, 0xad}});

      value("Repeated", "number", Repeated{std::in_place_index<0>, u32{1}});
      value("Repeated", "first_string", Repeated{std::in_place_index<1>, "a"});
      value("Repeated", "second_string", Repeated{std::in_place_index<2>, "b"});
      value("Repeated", "circle", Repeated{std::in_place_index<3>, Circle{6}});

      value("Stated", "circle", Stated{Circle{3}});
      value("Stated", "square", Stated{Square{4}});
      value("Stated", "side", Stated{u32{5}});
   }

   std::string render()
   {
      const auto        ir_bytes      = psio::encode(psio::pssz{}, schema());
      const std::string declaration[] = {PSIO_VARIANT_LABEL_CONF_DECLARATION};
      std::string       out           = "{\"version\":1,\n\"declaration\":[\n";
      for (std::size_t i = 0; i < std::size(declaration); ++i)
         out += psio::encode(psio::json{}, declaration[i]) +
                (i + 1 < std::size(declaration) ? ",\n" : "\n");
      out += "],\n\"schema_ir\":\"" + to_hex(ir_bytes) + "\",\n";
      out += "\"schema_json\":" + psio::encode(psio::json{}, psio::encode(psio::json{}, schema())) +
             ",\n";
      out += "\"variants\":[\n";
      for (std::size_t i = 0; i < variants.size(); ++i)
         out += psio::encode(psio::json{}, variants[i]) + (i + 1 < variants.size() ? ",\n" : "\n");
      out += "],\n\"vectors\":[\n";
      for (std::size_t i = 0; i < rows.size(); ++i)
         out += psio::encode(psio::json{}, rows[i]) + (i + 1 < rows.size() ? ",\n" : "\n");
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
   emit();
   const std::string text = render();
   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "variant label conformance emit: %s is not what the implementation "
                      "generates; regenerate it only for an intended format change\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("wrote %zu variants, %zu vectors to %s\n", variants.size(), rows.size(), argv[1]);
   return out ? 0 : 1;
}
