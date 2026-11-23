#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz.hpp>

#include <algorithm>
#include <string>
#include <string_view>

// Maps and sets through a hand-written schema: the order check, the text
// forms and the key encoder, all status-returning.
static int maps_and_sets()
{
   namespace ir = psio::schema_types;
   namespace runtime = psio::pssz_ir;
   auto box = [](ir::AnyType t) { return ir::Box<ir::AnyType>{std::move(t)}; };
   auto string = [&] {
      return ir::AnyType{ir::Custom{box(ir::List{box(ir::Int{8, false})}), "string"}};
   };
   ir::Schema schema;
   schema.ir_version = ir::schema_ir_version;
   // { counts: map<string, u32>, ids: set<i16> }
   ir::Tuple entry;
   entry.members.push_back(box(string()));
   entry.members.push_back(box(ir::Int{32, false}));
   schema.insert("Maps",
                 ir::Object{{ir::Member{"counts",
                                        box(ir::Custom{box(ir::List{box(ir::AnyType{entry})}),
                                                       "map"}),
                                        {}},
                             ir::Member{"ids",
                                        box(ir::Custom{box(ir::List{box(ir::Int{16, true})}),
                                                       "set"}),
                                        {}}},
                            {}});
   const ir::AnyType root{ir::Type{"Maps"}};
   const std::string text = R"({"counts":{"b":2,"a":1},"ids":[300,-1,7]})";
   std::vector<char> bytes;
   if (!runtime::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{text}, bytes).ok())
      return 20;
   std::string canonical;
   if (!runtime::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, canonical)
           .ok() ||
       canonical != R"({"counts":{"a":1,"b":2},"ids":[-1,7,300]})")
      return 21;
   runtime::Value value;
   bool           unknown = false;
   if (!runtime::decode_auto(schema, root, std::span<const char>{bytes}, value, unknown).ok())
      return 22;
   std::vector<char> key;
   if (!runtime::to_key(schema, root, value, key).ok() || key.empty()) return 23;
   // Swap the two i16 elements at the end of the frame: no longer ascending.
   std::vector<char> unsorted = bytes;
   std::swap_ranges(unsorted.end() - 6, unsorted.end() - 4, unsorted.end() - 4);
   if (runtime::validate_pssz(schema, root, std::span<const char>{unsorted}).ok()) return 24;
   canonical.clear();
   if (runtime::pssz_to_json(schema, root, psio::json{}, std::span<const char>{unsorted}, canonical)
          .ok())
      return 25;
   for (std::string_view bad : {std::string_view{R"({"counts":{"a":1,"a":2},"ids":[]})"},
                                std::string_view{R"({"counts":{},"ids":[1,1]})"},
                                std::string_view{R"({"counts":[["a",1]],"ids":[]})"}})
   {
      std::vector<char> out;
      if (runtime::json_to_pssz(schema, root, psio::json{}, std::span<const char>{bad}, out).ok())
         return 26;
   }
   // A malformed schema is refused before any bytes are read.
   ir::Schema malformed;
   malformed.ir_version = ir::schema_ir_version;
   malformed.insert("Bad", ir::Custom{box(ir::List{box(ir::Int{8, false})}), "map"});
   if (runtime::validate_pssz(malformed, ir::AnyType{ir::Type{"Bad"}}, std::span<const char>{}).ok())
      return 27;
   return 0;
}

// The schema-driven text <-> PSSZ path with exceptions and RTTI disabled:
// every failure is a status.
static int schema_driven()
{
   namespace ir = psio::schema_types;
   namespace runtime = psio::pssz_ir;
   auto bounded_string = [](std::uint64_t n) {
      return ir::AnyType{ir::Custom{
          ir::Box<ir::AnyType>{ir::AnyType{ir::BoundedList{ir::AnyType{ir::Int{8, false}}, n}}},
          "string"}};
   };
   ir::Schema schema;
   // { id: u64, name: string<=8, tags: list<u16> (at least 1, at most 2),
   //   kind: variant<u32 "uint32", string "string">, extra: optional<u64> }
   // The bounds make it a 1-byte-offset type.
   schema.insert("Action",
                 ir::Object{{ir::Member{"id", ir::AnyType{ir::Int{64, false}}, {}},
                             ir::Member{"name", bounded_string(8), {}},
                             ir::Member{"tags",
                                        ir::AnyType{ir::List{ir::AnyType{ir::Int{16, false}}}},
                                        {ir::Attribute{"lengthBound", "min=1,max=2"}}},
                             ir::Member{"kind",
                                        ir::AnyType{ir::Variant{
                                            {ir::Member{"uint32", ir::AnyType{ir::Int{32, false}}, {}},
                                             ir::Member{"string", bounded_string(4), {}}},
                                            {}}},
                                        {}},
                             ir::Member{"extra",
                                        ir::AnyType{ir::Option{ir::AnyType{ir::Int{64, false}}}},
                                        {ir::Attribute{"ijsonInt", "string"}}}},
                            {}});
   auto span = [](std::string_view text) { return std::span<const char>{text}; };
   std::size_t width = 0;
   if (!runtime::auto_width(schema, "Action", width).ok() || width != 1)
      return 20;  // the variant member is bounded by its widest alternative: 1 + 4
   std::vector<char> bytes;
   const std::string_view text =
       R"({"id":"18446744073709551615","name":"abc","tags":[1,2],"kind":{"string":"hi"},"extra":null})";
   if (!runtime::json_to_pssz(schema, "Action", psio::ijson{}, span(text), bytes).ok())
      return 21;
   if (!runtime::validate_pssz(schema, "Action", bytes).ok())
      return 22;
   std::string back;
   if (!runtime::pssz_to_json(schema, "Action", psio::ijson{}, bytes, back).ok() || back != text)
      return 23;
   back.clear();
   if (!runtime::pssz_to_json(schema, "Action", psio::json{}, bytes, back).ok() ||
       back != R"({"id":18446744073709551615,"name":"abc","tags":[1,2],"kind":[1,"hi"],"extra":null})")
      return 24;
   // The same value from the json form gives the same bytes.
   std::vector<char> again;
   if (!runtime::json_to_pssz(schema, "Action", psio::json{}, span(back), again).ok() ||
       again != bytes)
      return 25;
   // Rejections: bounds, forms, names, arity, range, depth, truncation.
   const std::string_view bad[] = {
       R"({"id":1,"name":"123456789","tags":[1],"kind":{"uint32":1}})",   // name over bound
       R"({"id":1,"name":"a","tags":[],"kind":{"uint32":1}})",            // tags under min
       R"({"id":1,"name":"a","tags":[1,2,3],"kind":{"uint32":1}})",       // tags over max
       R"({"id":1,"name":"a","tags":[65536],"kind":{"uint32":1}})",       // element range
       R"({"id":1,"name":"a","tags":[1],"kind":{"int32":1}})",            // unknown alternative
       R"({"id":1,"name":"a","tags":[1],"kind":{"uint32":1,"string":""}})",
       R"({"id":1,"name":"a","tags":[1],"kind":[0,1]})",                  // json form in ijson
       R"({"id":1,"name":"a","tags":[1],"kind":{"uint32":1},"extra":5})", // forced string
       R"({"id":9007199254740992,"name":"a","tags":[1],"kind":{"uint32":1}})",
       R"({"id":1,"name":"a","tags":[1]})",                               // missing member
       R"({"id":1,"name":"a","tags":[1],"kind":{"uint32":1})",            // truncated
       R"({"id":1,"name":"a","tags":[1],"kind":{"uint32":1}} x)",
       "",
   };
   for (auto input : bad)
   {
      std::vector<char> out{'x'};
      if (runtime::json_to_pssz(schema, "Action", psio::ijson{}, span(input), out).ok() ||
          out != std::vector<char>{'x'})
         return 26;
   }
   std::string bomb = R"({"zzz":)";
   bomb.append(100000, '[');
   if (runtime::json_to_pssz(schema, "Action", psio::ijson{}, span(bomb), again).ok())
      return 27;
   // Bytes: every truncation and single-byte mutation is a status.
   for (std::size_t i = 0; i < bytes.size(); ++i)
   {
      const std::vector<char> head(bytes.begin(), bytes.begin() + i);
      // (A prefix can itself be a valid frame: the last string is shorter.)
      if (i < 4 && runtime::validate_pssz(schema, "Action", head).ok())
         return 28;
      (void)runtime::validate_pssz(schema, "Action", head);
      auto flipped = bytes;
      flipped[i]   = static_cast<char>(flipped[i] ^ 0xff);
      std::string out = "x";
      if (!runtime::pssz_to_json(schema, "Action", psio::ijson{}, flipped, out).ok() &&
          out != "x")
         return 29;
   }
   if (runtime::validate_pssz(schema, "Missing", bytes).ok())
      return 30;
   return 0;
}

int main()
{
   namespace ir = psio::schema_types;
   namespace runtime = psio::pssz_ir;
   ir::Schema schema;
   ir::AnyType root = ir::Object{{ir::Member{"value", ir::AnyType{ir::Int{32, false}}, {}}}, {}};
   runtime::Value value = runtime::Record{{"value", runtime::Unsigned{7}}}, decoded;
   std::vector<char> bytes;
   if (!runtime::encode<4>(schema, root, value, bytes).ok())
      return 1;
   if (!runtime::decode_exact<4>(schema, root, bytes, decoded).ok() || decoded != value)
      return 2;
   const auto previous = bytes;
   if (runtime::encode<4>(schema, ir::Int{8, false}, runtime::Unsigned{256}, bytes).ok())
      return 3;
   if (bytes != previous)
      return 4;
   psio::bounded<unsigned,2> bounded;
   bounded.push_back(1);bounded.push_back(2);
   psio::utf8_string<4> string{std::string("text")};
   psio::bitlist<8> bits;bits.push_back(true);bits.push_back(false);
   auto packed=psio::encode(psio::pssz32{},bits);
   psio::bitlist<8> unpacked;bool unknown=false;
   if(!psio::decode_pssz_checked(psio::pssz32{},packed,unpacked,unknown).ok())return 5;
   if(unknown||unpacked.size()!=2||!unpacked.test(0)||unpacked.test(1))return 6;
   if(bounded.size()!=2||string.size()!=4)return 7;
   if (int rc = maps_and_sets()) return rc;
   return schema_driven();
}
