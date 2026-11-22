// psio::json / psio::ijson with exceptions and RTTI disabled: every checked
// entry point reports through codec_status. Nothing here may throw or abort
// on bad input or on a value that has no JSON text.

#include <psio/json.hpp>
#include <psio/reflect.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

struct GuestInner
{
   std::uint64_t id = 0;
   std::string   label;
   friend bool   operator==(const GuestInner&, const GuestInner&) = default;
};
PSIO_REFLECT(GuestInner, id, label)

struct GuestCode
{
   std::uint8_t value = 0;
   friend bool  operator==(const GuestCode&, const GuestCode&) = default;
};
PSIO_REFLECT(GuestCode, value)
struct GuestCodeLess
{
   bool operator()(const GuestCode& a, const GuestCode& b) const { return a.value < b.value; }
};
struct GuestCodeText
{
   static std::size_t packsize(const GuestCode&) { return 1; }
   static void        encode(const GuestCode& c, std::string& s) { s.push_back(static_cast<char>('a' + c.value % 26)); }
   static GuestCode   decode(std::span<const char> in) { return GuestCode{static_cast<std::uint8_t>(in[0] - 'a')}; }
   static psio::codec_status validate(std::span<const char> in) noexcept
   {
      return in.size() == 1 && in[0] >= 'a' && in[0] <= 'z' ? psio::codec_ok()
                                                             : psio::codec_fail("GuestCode: one letter", 0, "GuestCode");
   }
   static psio::codec_status validate_strict(std::span<const char> in) noexcept { return validate(in); }
};
PSIO_ADAPTER(GuestCode, psio::text_category, GuestCodeText)

struct GuestRec
{
   std::uint64_t                                         big = 0;
   std::uint64_t                                         as_number = 0;
   std::uint32_t                                         small = 0;
   std::vector<std::uint8_t>                             bytes;
   std::optional<std::int64_t>                           maybe;
   std::variant<std::uint32_t, std::string, GuestInner>  choice;
   std::tuple<std::uint32_t, std::string>                pair;
   std::tuple<>                                          unit;
   GuestCode                                             code;
   double                                                ratio = 0;
   std::string                                           text;
   friend bool operator==(const GuestRec&, const GuestRec&) = default;
};
PSIO_REFLECT(GuestRec, big, as_number, small, bytes, maybe, choice, pair, unit, code, ratio, text)
PSIO_FIELD_ATTRS(GuestRec, as_number, psio::ijson_number | psio::json_string)

struct GuestTree
{
   std::vector<GuestTree> children;
};
PSIO_REFLECT(GuestTree, children)

template <typename Fmt>
int run(int base, const std::string& want)
{
   GuestRec value{18446744073709551615ull, 18446744073709551615ull, 7, {0x01, 0xff}, -5,
                  GuestInner{9, "nine"}, {3, "three"}, {}, {2}, 0.5, "t\xC3\xA9xt"};
   std::string text;
   if (!psio::encode_json_checked(Fmt{}, value, text).ok()) return base + 1;
   if (text != want) return base + 2;
   if (psio::encode(Fmt{}, value) != text) return base + 3;
   const std::span<const char> in{text};
   if (!psio::validate<GuestRec>(Fmt{}, in).ok()) return base + 4;
   if (!psio::validate_strict<GuestRec>(Fmt{}, in).ok()) return base + 5;
   GuestRec back;
   if (!psio::decode_json_checked(Fmt{}, in, back).ok() || !(back == value)) return base + 6;
   if (!(psio::decode<GuestRec>(Fmt{}, in) == value)) return base + 7;

   // Malformed and mistyped input: a status, never an abort.
   for (std::size_t cut = 0; cut < text.size(); ++cut)
   {
      const std::span<const char> part{text.data(), cut};
      if (psio::validate<GuestRec>(Fmt{}, part).ok()) return base + 8;
      if (psio::decode_json_checked(Fmt{}, part, back).ok()) return base + 9;
   }
   for (std::size_t at = 0; at < text.size(); ++at)
   {
      std::string bad = text;
      bad[at]         = static_cast<char>(bad[at] ^ 0x15);
      const bool v = psio::validate<GuestRec>(Fmt{}, std::span<const char>{bad}).ok();
      const bool d = psio::decode_json_checked(Fmt{}, std::span<const char>{bad}, back).ok();
      if (v != d) return base + 10;
   }
   const std::string bomb(200000, '[');
   if (psio::validate<GuestTree>(Fmt{}, std::span<const char>{bomb}).ok()) return base + 11;
   std::string nested;
   for (int i = 0; i < 100000; ++i) nested += "{\"children\":[";
   GuestTree tree;
   if (psio::decode_json_checked(Fmt{}, std::span<const char>{nested}, tree).ok()) return base + 12;
   std::string unknown = "{\"zzz\":";
   unknown += std::string(200000, '[');
   if (psio::validate<GuestInner>(Fmt{}, std::span<const char>{unknown}).ok()) return base + 13;

   // Values without a JSON text: a status from the checked writer.
   std::string out = "keep";
   if (psio::encode_json_checked(Fmt{}, std::string("\xff"), out).ok() || out != "keep")
      return base + 14;
   GuestInner raw{1, "bad \xC0\x80"};
   if (psio::encode_json_checked(Fmt{}, raw, out).ok() || out != "keep") return base + 15;
   return 0;
}

int main()
{
   if (int rc = run<psio::json>(
          100,
          "{\"big\":18446744073709551615,\"as_number\":\"18446744073709551615\",\"small\":7,"
          "\"bytes\":[1,255],\"maybe\":-5,\"choice\":[2,{\"id\":9,\"label\":\"nine\"}],"
          "\"pair\":[3,\"three\"],\"unit\":[],\"code\":\"c\",\"ratio\":0.5,\"text\":\"t\xC3\xA9xt\"}"))
      return rc;
   if (int rc = run<psio::ijson>(
          200,
          "{\"big\":\"18446744073709551615\",\"as_number\":18446744073709551615,\"small\":7,"
          "\"bytes\":\"01FF\",\"maybe\":-5,\"choice\":{\"GuestInner\":{\"id\":9,\"label\":\"nine\"}},"
          "\"pair\":[3,\"three\"],\"unit\":[],\"code\":\"c\",\"ratio\":0.5,\"text\":\"t\xC3\xA9xt\"}"))
      return rc;

   // I-JSON only: non-finite floats. A repeated key is accepted in both
   // formats and the last one wins.
   std::string out;
   if (psio::encode_json_checked(psio::ijson{}, std::numeric_limits<double>::infinity(), out).ok())
      return 1;
   if (psio::encode_json_checked(psio::ijson{}, std::nan(""), out).ok()) return 2;
   const std::string dup = R"({"id":"1","label":"a","id":"2"})";
   if (!psio::validate<GuestInner>(psio::ijson{}, std::span<const char>{dup}).ok()) return 3;
   GuestInner last;
   if (!psio::decode_json_checked(psio::ijson{}, std::span<const char>{dup}, last).ok() ||
       last.id != 2)
      return 3;
   const std::string dup_bad = R"({"id":true,"label":"a","id":"2"})";
   if (psio::validate<GuestInner>(psio::ijson{}, std::span<const char>{dup_bad}).ok()) return 3;
   const std::string range = R"({"id":"18446744073709551616","label":"a"})";
   GuestInner inner;
   if (psio::decode_json_checked(psio::ijson{}, std::span<const char>{range}, inner).ok()) return 4;
   const std::string hex = "\"0G\"";
   std::vector<std::uint8_t> bytes;
   if (psio::decode_json_checked(psio::ijson{}, std::span<const char>{hex}, bytes).ok()) return 5;
   const std::string code = "\"too long\"";
   GuestCode c;
   if (psio::decode_json_checked(psio::ijson{}, std::span<const char>{code}, c).ok()) return 6;

   // Maps and sets: an object for a text key, pairs otherwise, any order
   // in, canonical order out; a repeated key / element is a status (unlike
   // a repeated member name above).
   using Counts = std::map<std::string, std::uint64_t>;
   using ById   = std::unordered_map<std::uint32_t, GuestInner>;
   Counts            counts;
   const std::string counts_text = R"({"b":"18446744073709551615","a":1})";
   if (!psio::decode_json_checked(psio::ijson{}, std::span<const char>{counts_text}, counts).ok() ||
       counts != Counts{{"a", 1}, {"b", 18446744073709551615ull}})
      return 7;
   out.clear();
   if (!psio::encode_json_checked(psio::ijson{}, counts, out).ok() ||
       out != R"({"a":1,"b":"18446744073709551615"})")
      return 8;
   ById              by_id;
   const std::string by_id_text = R"([[2,{"id":2,"label":"two"}],[1,{"id":1,"label":"one"}]])";
   if (!psio::decode_json_checked(psio::ijson{}, std::span<const char>{by_id_text}, by_id).ok() ||
       by_id.size() != 2 || by_id[1].label != "one")
      return 9;
   out.clear();
   if (!psio::encode_json_checked(psio::json{}, by_id, out).ok() ||
       out != R"([[1,{"id":1,"label":"one"}],[2,{"id":2,"label":"two"}]])")
      return 10;
   const std::string dup_key = R"({"a":1,"a":1})";
   if (psio::validate<Counts>(psio::ijson{}, std::span<const char>{dup_key}).ok()) return 11;
   if (psio::decode_json_checked(psio::json{}, std::span<const char>{dup_key}, counts).ok())
      return 12;
   const std::string dup_element = R"(["x","y","x"])";
   std::set<std::string> tags;
   if (psio::decode_json_checked(psio::ijson{}, std::span<const char>{dup_element}, tags).ok())
      return 13;
   const std::string dup_pair = R"([[1,{"id":1,"label":"a"}],[1,{"id":1,"label":"a"}]])";
   if (psio::validate<ById>(psio::json{}, std::span<const char>{dup_pair}).ok()) return 14;
   std::map<GuestCode, int, GuestCodeLess> coded;
   const std::string coded_text = R"({"c":1,"too long":2})";
   if (psio::decode_json_checked(psio::ijson{}, std::span<const char>{coded_text}, coded).ok())
      return 15;
   const std::string pair_text = R"([7,"seven"])";
   std::pair<std::uint8_t, std::string> pair;
   if (!psio::decode_json_checked(psio::ijson{}, std::span<const char>{pair_text}, pair).ok() ||
       pair.first != 7 || pair.second != "seven")
      return 16;
   return 0;
}
