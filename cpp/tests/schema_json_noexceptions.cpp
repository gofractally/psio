// psio::Schema through psio::json / psio::ijson with exceptions and RTTI
// disabled: a schema is written and read back through the checked entry
// points, and malformed, mistyped and over-nested text is a status.

#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/schema_ir.hpp>

using namespace psio::schema_types;

namespace
{
   Schema sample()
   {
      using B = Box<AnyType>;
      Schema s;
      s.package = Package{"test:guest", "1.0.0", {Attribute{"final", std::nullopt}}};
      s.insert("point", Struct{{Member{"x", B{Int{32, true}}, {}}, Member{"y", B{Int{32, true}}, {}}}, {}});
      s.insert("grid", Array{B{List{B{Option{B{Type{"point"}}}}}}, 18446744073709551615ull});
      s.insert("pair", Tuple{{B{Float{11, 52}}, B{Custom{B{List{B{Int{8, false}}}}, "utf8"}}}});
      s.insert("kind", Enum{B{Int{8, false}}, {Enumerator{"a", 0}, Enumerator{"b", 9007199254740992ull}}});
      return s;
   }

   template <typename Fmt>
   int run(int base)
   {
      const Schema value = sample();
      std::string  text;
      if (!psio::encode_json_checked(Fmt{}, value, text).ok()) return base + 1;
      if (psio::encode(Fmt{}, value) != text) return base + 2;
      const std::span<const char> in{text};
      if (!psio::validate<Schema>(Fmt{}, in).ok()) return base + 3;
      if (!psio::validate_strict<Schema>(Fmt{}, in).ok()) return base + 4;
      Schema back;
      for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text,
                                psio::json_parser::simdjson, psio::json_parser::simdjson_index})
      {
         if (!psio::validate_json<Schema>(Fmt{}, in, parser).ok()) return base + 5;
         if (!psio::decode_json_checked(Fmt{}, in, back, parser).ok() || !(back == value)) return base + 6;
      }
      if (!(psio::decode<Schema>(Fmt{}, in) == value)) return base + 7;
      if (psio::encode(psio::pssz{}, back) != psio::encode(psio::pssz{}, value)) return base + 8;

      // Truncated and mutated text: a status, and one answer from validate
      // and decode.
      for (std::size_t cut = 0; cut < text.size(); ++cut)
      {
         const std::span<const char> part{text.data(), cut};
         if (psio::validate<Schema>(Fmt{}, part).ok()) return base + 9;
         if (psio::decode_json_checked(Fmt{}, part, back).ok()) return base + 10;
      }
      for (std::size_t at = 0; at < text.size(); ++at)
      {
         std::string bad = text;
         bad[at]         = static_cast<char>(bad[at] ^ 0x15);
         const bool v = psio::validate<Schema>(Fmt{}, std::span<const char>{bad}).ok();
         const bool d = psio::decode_json_checked(Fmt{}, std::span<const char>{bad}, back).ok();
         if (v != d) return base + 11;
      }

      // A type nested without end: stopped by the depth limit, not the stack.
      std::string bomb = R"({"types":[{"name":"T","type":)";
      for (int i = 0; i < 100000; ++i)
         bomb += Fmt::i_json ? R"({"value":{"List":{"type":)" : R"({"value":[3,{"type":)";
      if (psio::validate<Schema>(Fmt{}, std::span<const char>{bomb}).ok()) return base + 12;
      if (psio::decode_json_checked(Fmt{}, std::span<const char>{bomb}, back).ok()) return base + 13;

      // A moved-from Box holds nothing: the checked writer reports it and
      // leaves the output alone.
      Member      hollow{"m", Box<AnyType>{Int{8, false}}, {}};
      const auto  taken = std::move(hollow.type);
      std::string out   = "keep";
      if (psio::encode_json_checked(Fmt{}, hollow, out).ok() || out != "keep") return base + 14;
      if (!psio::encode_json_checked(Fmt{}, taken, out).ok()) return base + 15;
      return 0;
   }
}  // namespace

int main()
{
   if (int rc = run<psio::json>(100)) return rc;
   if (int rc = run<psio::ijson>(200)) return rc;
   return 0;
}
