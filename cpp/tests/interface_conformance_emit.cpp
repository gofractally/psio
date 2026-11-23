// Emitter for conformance/interface-vectors.json: what a method declaration
// becomes — the Interface / Func Schema IR, the argument list's own type and
// width — and the bytes and text of argument lists and results
// (conformance/interface-vectors.md).
//
//   psio_interface_conformance_emit <out.json>          write the fixture
//   psio_interface_conformance_emit --check <ref.json>  regenerate and compare
//
// Every row is computed by the typed C++ codecs and checked here against the
// schema-driven codecs over the fixture's own schema; generation aborts when
// they disagree. Output is deterministic.
#include "interface_conformance_types.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace
{
   using namespace iface_conf;
   namespace ir = psio::pssz_ir;
   using Bytes  = std::vector<char>;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "interface conformance emit: %s\n", message.c_str());
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

   struct MethodRow
   {
      std::string                  name;
      std::vector<std::string>     params;   // the parameter names the declaration lists
      bool                         named = false;
      std::string                  args;     // the schema name of the argument list
      bool                         result = false;
      std::optional<std::uint64_t> max_dynamic_data;
      std::optional<std::uint64_t> max_fields;
      std::uint32_t                width = 0;
      std::optional<std::uint64_t> max_encoded_size;
   };
   PSIO_REFLECT(MethodRow, name, params, named, args, result, max_dynamic_data, max_fields, width,
                max_encoded_size)
   struct Row
   {
      std::string                name;
      std::string                kind;
      std::string                method;
      std::optional<std::string> pssz;
      std::optional<std::string> json;
      std::optional<std::string> ijson;
      std::optional<std::string> text;
      std::optional<std::string> reason;
   };
   PSIO_REFLECT(Row, name, kind, method, pssz, json, ijson, text, reason)

   std::vector<MethodRow> methods;
   std::vector<Row>       rows;
   std::set<std::string>  names;

   Row& add(const std::string& kind, const std::string& method, const std::string& name)
   {
      const std::string full = kind + "/" + method + "/" + name;
      if (!names.insert(full).second)
         die("duplicate row " + full);
      rows.push_back(Row{full, kind, method, {}, {}, {}, {}, {}});
      return rows.back();
   }

   const psio::Schema& schema() { return fixture_schema(); }
   const psio::schema_types::Func& func(std::size_t i) { return schema().interfaces.at(0).funcs.at(i); }
   psio::schema_types::AnyType args_root(std::size_t i)
   {
      return psio::schema_types::AnyType{
         psio::schema_types::Type{"token." + std::string{R::method_names[i]}}};
   }

   // ── Argument lists ─────────────────────────────────────────────────────
   template <std::size_t I>
   void describe()
   {
      MethodRow m;
      m.name = std::string{R::method_names[I]};
      for (auto n : R::method_param_names<I>)
         m.params.emplace_back(n);
      m.named  = psio::annotated_tuple_named_v<Args<I>>;
      m.args   = "token." + m.name;
      m.result = !std::is_void_v<R::method_result<I>>;
      if (R::method_max_dynamic_data<I>)
         m.max_dynamic_data = *R::method_max_dynamic_data<I>;
      if (R::method_max_fields<I>)
         m.max_fields = *R::method_max_fields<I>;
      m.width = static_cast<std::uint32_t>(psio::auto_pssz_width_v<Args<I>>);
      if (constexpr auto bound = psio::max_encoded_size<Args<I>>(); bound)
         m.max_encoded_size = *bound;
      // The schema says the same.
      std::size_t width = 0;
      if (!ir::auto_width(schema(), args_root(I), width).ok() || width != m.width)
         die(m.name + ": the schema-driven width of the argument list differs");
      const auto& f = func(I);
      if (f.name != m.name || f.params.size() != std::tuple_size_v<typename Args<I>::tuple_type> ||
          f.result.has_value() != m.result)
         die(m.name + ": the Func does not describe the method");
      methods.push_back(std::move(m));
   }

   template <std::size_t I>
   void args_value(const std::string& name, const Args<I>& value)
   {
      Row&        r     = add("args", std::string{R::method_names[I]}, name);
      const Bytes bytes = [&] {
         Bytes out;
         if (!psio::encode_pssz_checked(psio::pssz{}, value, out).ok())
            die(r.name + ": the arguments do not encode");
         return out;
      }();
      std::string json, ijson;
      if (!psio::encode_json_checked(psio::json{}, value, json).ok() ||
          !psio::encode_json_checked(psio::ijson{}, value, ijson).ok())
         die(r.name + ": the arguments have no text");
      // Typed: the text reads back; schema-driven: the same bytes and text.
      for (int ij = 0; ij < 2; ++ij)
      {
         const std::string& text = ij ? ijson : json;
         Args<I>            back;
         if (!(ij ? psio::decode_json_checked(psio::ijson{}, sp(text), back)
                  : psio::decode_json_checked(psio::json{}, sp(text), back))
                 .ok() ||
             psio::encode(psio::pssz{}, back) != bytes)
            die(r.name + ": the text does not read back");
         Bytes       dyn;
         std::string dyn_text;
         bool        unknown = false;
         if (!(ij ? ir::json_to_pssz(schema(), args_root(I), psio::ijson{}, sp(text), dyn)
                  : ir::json_to_pssz(schema(), args_root(I), psio::json{}, sp(text), dyn))
                 .ok() ||
             dyn != bytes)
            die(r.name + ": the schema-driven reader disagrees on " + text);
         if (!(ij ? ir::pssz_to_json(schema(), args_root(I), psio::ijson{}, bytes, dyn_text,
                                     &unknown)
                  : ir::pssz_to_json(schema(), args_root(I), psio::json{}, bytes, dyn_text,
                                     &unknown))
                 .ok() ||
             dyn_text != text)
            die(r.name + ": the schema-driven writer writes " + dyn_text + ", not " + text);
      }
      r.pssz  = to_hex(bytes);
      r.json  = json;
      r.ijson = ijson;
   }

   template <std::size_t I>
   void args_accept(const std::string& name, const std::string& text, const Args<I>& value)
   {
      Row&        r     = add("args-accept", std::string{R::method_names[I]}, name);
      const Bytes bytes = psio::encode(psio::pssz{}, value);
      for (int ij = 0; ij < 2; ++ij)
      {
         Args<I> back;
         if (!(ij ? psio::decode_json_checked(psio::ijson{}, sp(text), back)
                  : psio::decode_json_checked(psio::json{}, sp(text), back))
                 .ok() ||
             psio::encode(psio::pssz{}, back) != bytes)
            die(r.name + ": expected to read as the given arguments");
         Bytes dyn;
         if (!(ij ? ir::json_to_pssz(schema(), args_root(I), psio::ijson{}, sp(text), dyn)
                  : ir::json_to_pssz(schema(), args_root(I), psio::json{}, sp(text), dyn))
                 .ok() ||
             dyn != bytes)
            die(r.name + ": the schema-driven reader does not read the same arguments");
      }
      r.text = text;
      r.pssz = to_hex(bytes);
      r.json = psio::encode(psio::json{}, value);
   }

   template <std::size_t I>
   void args_reject(const std::string& name, const std::string& text)
   {
      Row& r = add("args-reject", std::string{R::method_names[I]}, name);
      Args<I> back;
      Bytes   dyn;
      if (psio::decode_json_checked(psio::json{}, sp(text), back).ok() ||
          psio::decode_json_checked(psio::ijson{}, sp(text), back).ok() ||
          psio::validate_json<Args<I>>(psio::json{}, sp(text)).ok())
         die(r.name + ": the typed reader accepts " + text);
      if (ir::json_to_pssz(schema(), args_root(I), psio::json{}, sp(text), dyn).ok() ||
          ir::json_to_pssz(schema(), args_root(I), psio::ijson{}, sp(text), dyn).ok())
         die(r.name + ": the schema-driven reader accepts " + text);
      r.text = text;
   }

   // A frame every reader of the argument list rejects (a parameter outside
   // its bound, the whole over its cap), with the typed validator's message.
   template <std::size_t I>
   void args_frame_reject(const std::string& name, const Bytes& bytes, const std::string& reason)
   {
      Row&       r      = add("args-pssz-reject", std::string{R::method_names[I]}, name);
      const auto status = psio::validate<Args<I>>(psio::pssz{}, bytes);
      if (status.ok() || std::string{status.error().what} != reason)
         die(r.name + ": expected the typed validator to say '" + reason + "', it says '" +
             (status.ok() ? std::string{"ok"} : std::string{status.error().what}) + "'");
      bool unknown = false;
      if (ir::validate_auto(schema(), args_root(I), bytes, &unknown).ok())
         die(r.name + ": the schema-driven validator accepts the frame");
      r.pssz   = to_hex(bytes);
      r.reason = reason;
   }

   // ── Results ────────────────────────────────────────────────────────────
   //
   // returns(...) makes the result a bounded type: its bytes are that type's
   // (psio::encode_method_result), and the Func's result type in the schema
   // is the same type.
   template <std::size_t I, typename V>
   void result_value(const std::string& name, const V& value)
   {
      Row&  r = add("result", std::string{R::method_names[I]}, name);
      Bytes bytes;
      if (!psio::encode_method_result<token, I>(value, bytes).ok())
         die(r.name + ": the result does not encode");
      V back{};
      if (!psio::validate_method_result<token, I>(bytes).ok() ||
          !psio::decode_method_result<token, I>(bytes, back).ok() || !(back == value))
         die(r.name + ": the result does not read back");
      const std::string json  = psio::encode(psio::json{}, value);
      const std::string ijson = psio::encode(psio::ijson{}, value);
      const auto&       root  = **func(I).result;
      bool              unknown = false;
      Bytes             dyn;
      std::string       dyn_text;
      if (!ir::validate_auto(schema(), root, bytes, &unknown).ok() ||
          !ir::json_to_pssz(schema(), root, psio::json{}, sp(json), dyn).ok() || dyn != bytes ||
          !ir::pssz_to_json(schema(), root, psio::ijson{}, bytes, dyn_text, &unknown).ok() ||
          dyn_text != ijson)
         die(r.name + ": the schema-driven codec disagrees on the result");
      r.pssz  = to_hex(bytes);
      r.json  = json;
      r.ijson = ijson;
   }
   template <std::size_t I, typename V>
   void result_reject(const std::string& name, const V& value, const std::string& reason)
   {
      Row&  r = add("result-pssz-reject", std::string{R::method_names[I]}, name);
      Bytes refused;
      if (psio::encode_method_result<token, I>(value, refused).ok() || !refused.empty())
         die(r.name + ": the result is inside returns(...)");
      // The frame a callee that does not check would return.
      const Bytes bytes =
         psio::detail::pssz_impl::encode_method_result_unchecked<token, I>(value);
      const auto status = psio::validate_method_result<token, I>(bytes);
      if (status.ok() || std::string{status.error().what} != reason)
         die(r.name + ": expected the typed validator to say '" + reason + "', it says '" +
             (status.ok() ? std::string{"ok"} : std::string{status.error().what}) + "'");
      V back{};
      if (psio::decode_method_result<token, I>(bytes, back).ok())
         die(r.name + ": the typed reader accepts the result");
      bool unknown = false;
      if (ir::validate_auto(schema(), **func(I).result, bytes, &unknown).ok())
         die(r.name + ": the schema-driven validator accepts the result");
      r.pssz   = to_hex(bytes);
      r.reason = reason;
   }

   void emit()
   {
      if (!ir::validate_schema(schema()).ok())
         die("the fixture schema is not valid");
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (describe<Is>(), ...);
      }(std::make_index_sequence<method_count>{});

      using Text = std::string;
      using Opt  = std::optional<std::string>;
      auto transfer = [](Text to, u64 amount, Opt memo) {
         return Args<0>{std::tuple{std::move(to), amount, std::move(memo)}};
      };
      const Text s16(16, 'a'), s17(17, 'a'), s80(80, 'm');

      // transfer: named, bounded parameters, a cap (2-byte offsets), an
      // optional trailing parameter.
      args_value<0>("plain", transfer("bob", 5, "hi"));
      args_value<0>("no_memo", transfer("bob", 5, std::nullopt));
      args_value<0>("empty", transfer("", 0, ""));
      args_value<0>("at_the_bounds", transfer(s16, ~u64{0}, s80));
      args_value<0>("multibyte", transfer("h\xc3\xa9", (u64{1} << 53) - 1, "\xe2\x82\xac"));
      // Read: the object by the record rule, or the positional array.
      args_accept<0>("any_order", R"({"memo":"hi","amount":5,"to":"bob"})", transfer("bob", 5, "hi"));
      args_accept<0>("repeated_key_last_wins", R"({"to":"x","to":"bob","amount":5,"memo":"hi"})",
                     transfer("bob", 5, "hi"));
      args_accept<0>("unknown_key_skipped", R"({"to":"bob","fee":[1,{"a":null}],"amount":5,"memo":"hi"})",
                     transfer("bob", 5, "hi"));
      args_accept<0>("missing_optional_is_none", R"({"to":"bob","amount":5})",
                     transfer("bob", 5, std::nullopt));
      args_accept<0>("array", R"(["bob",5,"hi"])", transfer("bob", 5, "hi"));
      args_accept<0>("array_null_memo", R"(["bob",5,null])", transfer("bob", 5, std::nullopt));
      args_accept<0>("array_with_spaces", " [ \"bob\" , 5 , \"hi\" ] ", transfer("bob", 5, "hi"));
      args_reject<0>("missing_required", R"({"amount":5,"memo":"hi"})");
      args_reject<0>("array_too_short", R"(["bob",5])");
      args_reject<0>("array_too_long", R"(["bob",5,"hi",1])");
      args_reject<0>("to_over_bound", R"({"to":")" + s17 + R"(","amount":5})");
      args_reject<0>("array_to_over_bound", R"([")" + s17 + R"(",5,null])");
      args_reject<0>("memo_over_bound", R"({"to":"bob","amount":5,"memo":")" + s80 + R"(m"})");
      args_reject<0>("wrong_type", R"({"to":5,"amount":5})");
      args_reject<0>("a_string", R"("bob")");
      args_reject<0>("null", "null");
      {
         const auto frame = [](const Args<0>& value) {
            return psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, value);
         };
         args_frame_reject<0>("to_over_bound", frame(transfer(s17, 1, std::nullopt)),
                              "pssz: member length_bound violated");
         args_frame_reject<0>("to_not_utf8", frame(transfer("\xff", 1, std::nullopt)),
                              "utf8_spec: invalid leading byte");
         args_frame_reject<0>("memo_over_bound", frame(transfer("a", 1, s80 + "m")),
                              "pssz: member length_bound violated");
      }

      // names(limit): one named parameter; the result is bounded by returns().
      args_value<1>("limit", Args<1>{std::tuple{u32{10}}});
      args_accept<1>("array", "[10]", Args<1>{std::tuple{u32{10}}});
      args_reject<1>("out_of_range", R"({"limit":4294967296})");
      result_value<1>("two", std::vector<Text>{"alice", "bob"});
      result_value<1>("none", std::vector<Text>{});
      result_value<1>("at_the_bounds", std::vector<Text>(4, s16));
      result_reject<1>("five_names", std::vector<Text>(5, "a"),
                       "pssz: member length_bound violated");
      result_reject<1>("name_over_bound", std::vector<Text>{s17},
                       "pssz: member length_bound violated");
      result_reject<1>("name_not_utf8", std::vector<Text>{"\xff"},
                       "utf8_spec: invalid leading byte");

      // note(text): a raw bounded parameter under a 1-byte cap.
      args_value<2>("short", Args<2>{std::tuple{Text{"hey"}}});
      args_value<2>("at_the_bound", Args<2>{std::tuple{Text(62, 'n')}});
      args_reject<2>("over_bound", R"({"text":")" + Text(63, 'n') + R"("})");
      {
         // One byte past the bound is also past the cap (1 + 1 + 63 > 64):
         // the frame an unchecked writer would produce, by hand.
         Bytes frame{1, 1};
         frame.insert(frame.end(), 63, 'n');
         args_frame_reject<2>("over_bound_and_cap", frame,
                              "pssz: encoded size exceeds maxDynamicData cap");
      }

      // add(a, b): no parameter names — the positional array, and only it.
      args_value<3>("sum", Args<3>{std::tuple{u32{1}, u32{2}}});
      args_value<3>("max", Args<3>{std::tuple{~u32{0}, u32{0}}});
      args_reject<3>("object_by_position", R"({"0":1,"1":2})");
      args_reject<3>("object_by_name", R"({"a":1,"b":2})");
      args_reject<3>("array_too_short", "[1]");
      result_value<3>("small", u64{3});
      result_value<3>("beyond_2_53", ~u64{0});

      // ping(): no parameters.
      args_value<4>("nothing", Args<4>{});
      args_accept<4>("empty_array", "[]", Args<4>{});
      args_accept<4>("unknown_key", R"({"x":1})", Args<4>{});
      args_reject<4>("array_with_an_element", "[1]");

      // batch(names, tip): bounds below a parameter; a bool result.
      args_value<5>("two", Args<5>{std::tuple{std::vector<Text>{"a", "bc"}, u8{7}}});
      args_value<5>("none", Args<5>{std::tuple{std::vector<Text>{}, u8{0}}});
      args_accept<5>("array", R"([["a","bc"],7])",
                     Args<5>{std::tuple{std::vector<Text>{"a", "bc"}, u8{7}}});
      args_reject<5>("five_names", R"({"names":["a","b","c","d","e"],"tip":1})");
      args_reject<5>("name_over_bound", R"({"names":["123456789"],"tip":1})");
      result_value<5>("true", true);

      // transfer's result: a record with a bounded member.
      result_value<0>("receipt", Receipt{7, "ok"});
   }

   std::string render()
   {
      const auto        ir_bytes = psio::encode(psio::pssz{}, schema());
      const std::string declaration[] = {PSIO_INTERFACE_CONF_DECLARATION};
      std::string       out = "{\"version\":1,\n\"interface\":\"token\",\n\"declaration\":[\n";
      for (std::size_t i = 0; i < std::size(declaration); ++i)
         out += psio::encode(psio::json{}, declaration[i]) +
                (i + 1 < std::size(declaration) ? ",\n" : "\n");
      out += "],\n\"schema_ir\":\"" + to_hex(ir_bytes) + "\",\n";
      out += "\"schema_json\":" + psio::encode(psio::json{}, psio::encode(psio::json{}, schema())) +
             ",\n";
      out += "\"methods\":[\n";
      for (std::size_t i = 0; i < methods.size(); ++i)
         out += psio::encode(psio::json{}, methods[i]) + (i + 1 < methods.size() ? ",\n" : "\n");
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
   // The schema's own text reads back to the same schema.
   {
      psio::Schema back;
      const auto   json = psio::encode(psio::json{}, schema());
      if (!psio::decode_json_checked(psio::json{}, std::span<const char>{json}, back).ok() ||
          !(back == schema()))
         die("the schema text does not read back");
   }
   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "interface conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended format change\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("wrote %zu methods, %zu vectors to %s\n", methods.size(), rows.size(), argv[1]);
   return out ? 0 : 1;
}
