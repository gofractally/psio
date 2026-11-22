// Bounds by annotation at any depth (psio/annotate.hpp position specs:
// element / keys / values / at<I>), and the same bounds stated by wrapper
// types (psio::bounded<T, N>, psio::utf8_string<N>).
//
//   - the inferred bound and the offset width of every annotation form
//     (static_assert);
//   - the annotation spelling and the wrapper spelling give the same bytes,
//     the same width and the same schema IR;
//   - encode, validate, the checked decode and the fail-fast decode refuse a
//     value at bound + 1 at every depth and accept one at the bound;
//   - psio::json / psio::ijson and psio::key enforce the same tree;
//   - the schema-driven codec reads and refuses the same frames.

#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace at_test
{
   using Text16 = psio::utf8_string<16>;

   // ── A list of bounded strings ──────────────────────────────────────────
   struct TagsA  // annotation
   {
      friend bool operator==(const TagsA&, const TagsA&) = default;
      std::vector<std::string> tags;
      PSIO_REFLECT(TagsA, attr(tags, max<8> | element(utf8 | max<16>)))
   };
   struct TagsW  // wrapper types
   {
      friend bool operator==(const TagsW&, const TagsW&) = default;
      psio::bounded<Text16, 8> tags;
      PSIO_REFLECT(TagsW, tags)
   };
   struct TagsRaw  // raw (not UTF-8) bounded strings: annotation only
   {
      friend bool operator==(const TagsRaw&, const TagsRaw&) = default;
      std::vector<std::string> tags;
      PSIO_REFLECT(TagsRaw, attr(tags, max<8> | element(max<16>)))
   };
   struct TagsCountOnly  // the element count alone: strings unbounded
   {
      friend bool operator==(const TagsCountOnly&, const TagsCountOnly&) = default;
      std::vector<std::string> tags;
      PSIO_REFLECT(TagsCountOnly, attr(tags, max<8>))
   };

   // ── Two levels ─────────────────────────────────────────────────────────
   struct GridA
   {
      friend bool operator==(const GridA&, const GridA&) = default;
      std::vector<std::vector<std::uint16_t>> rows;
      PSIO_REFLECT(GridA, attr(rows, max<3> | element(max<4>)))
   };
   struct GridW
   {
      friend bool operator==(const GridW&, const GridW&) = default;
      psio::bounded<psio::bounded<std::uint16_t, 4>, 3> rows;
      PSIO_REFLECT(GridW, rows)
   };
   struct DeepA  // three levels, the innermost a string
   {
      friend bool operator==(const DeepA&, const DeepA&) = default;
      std::vector<std::vector<std::vector<std::string>>> v;
      PSIO_REFLECT(DeepA, attr(v, max<2> | element(max<2> | element(max<2> | element(max<3>)))))
   };

   // ── Optional payload, array elements ───────────────────────────────────
   struct OptA
   {
      friend bool operator==(const OptA&, const OptA&) = default;
      std::optional<std::vector<std::string>> names;
      PSIO_REFLECT(OptA, attr(names, max<2> | element(utf8 | max<16>)))
   };
   struct OptW
   {
      friend bool operator==(const OptW&, const OptW&) = default;
      std::optional<psio::bounded<Text16, 2>> names;
      PSIO_REFLECT(OptW, names)
   };
   struct ArrA  // an array of variable elements has its own offset width
   {
      friend bool operator==(const ArrA&, const ArrA&) = default;
      std::array<std::string, 3> names;
      PSIO_REFLECT(ArrA, attr(names, element(utf8 | max<16>)))
   };
   struct ArrW
   {
      friend bool operator==(const ArrW&, const ArrW&) = default;
      std::array<Text16, 3> names;
      PSIO_REFLECT(ArrW, names)
   };

   // ── Maps and sets ──────────────────────────────────────────────────────
   struct MapA
   {
      friend bool operator==(const MapA&, const MapA&) = default;
      std::map<std::string, std::vector<std::uint32_t>> m;
      PSIO_REFLECT(MapA, attr(m, max<4> | keys(utf8 | max<16>) | values(max<2>)))
   };
   struct MapW
   {
      friend bool operator==(const MapW&, const MapW&) = default;
      // The entry count is a member annotation in both spellings: there is
      // no bounded map type.
      std::map<Text16, psio::bounded<std::uint32_t, 2>> m;
      PSIO_REFLECT(MapW, attr(m, max<4>))
   };
   struct SetA
   {
      friend bool operator==(const SetA&, const SetA&) = default;
      std::set<std::string> s;
      PSIO_REFLECT(SetA, attr(s, max<4> | element(utf8 | max<16>)))
   };
   struct SetW
   {
      friend bool operator==(const SetW&, const SetW&) = default;
      std::set<Text16> s;
      PSIO_REFLECT(SetW, attr(s, max<4>))
   };

   // ── Tuple / pair elements, variant alternatives ────────────────────────
   struct TupA
   {
      friend bool operator==(const TupA&, const TupA&) = default;
      std::tuple<std::string, std::uint32_t, std::vector<std::uint8_t>> t;
      std::pair<std::string, std::string>                              p;
      PSIO_REFLECT(TupA, attr(t, at<0>(utf8 | max<16>) | at<2>(max<5>)),
                   attr(p, at<0>(utf8 | max<16>) | at<1>(utf8 | max<16>)))
   };
   struct TupW
   {
      friend bool operator==(const TupW&, const TupW&) = default;
      std::tuple<Text16, std::uint32_t, psio::bounded<std::uint8_t, 5>> t;
      std::pair<Text16, Text16>                                         p;
      PSIO_REFLECT(TupW, t, p)
   };
   struct VarA
   {
      friend bool operator==(const VarA&, const VarA&) = default;
      std::variant<std::uint32_t, std::string, std::vector<std::string>> v;
      PSIO_REFLECT(VarA, attr(v, at<1>(utf8 | max<16>) | at<2>(max<2> | element(utf8 | max<16>))))
   };
   struct VarW
   {
      friend bool operator==(const VarW&, const VarW&) = default;
      std::variant<std::uint32_t, Text16, psio::bounded<Text16, 2>> v;
      PSIO_REFLECT(VarW, v)
   };
}  // namespace at_test
using namespace at_test;

// ── Bounds and widths ─────────────────────────────────────────────────────
//
// In the bound every header and offset slot counts 4 (docs/pssz-spec.md 3.2).

// header 4 + slot 4 + 8 x (16 + slot 4)
static_assert(psio::max_encoded_size<TagsA>() == 4 + 4 + 8 * (16 + 4));
static_assert(psio::max_encoded_size<TagsW>() == psio::max_encoded_size<TagsA>());
static_assert(psio::max_encoded_size<TagsRaw>() == psio::max_encoded_size<TagsA>());
static_assert(psio::auto_pssz_width_v<TagsA> == 1);
static_assert(psio::auto_pssz_width_v<TagsW> == 1);
static_assert(psio::auto_pssz_width_v<TagsRaw> == 1);
// The count alone bounds nothing: the strings are unbounded.
static_assert(!psio::max_encoded_size<TagsCountOnly>().has_value());
static_assert(psio::auto_pssz_width_v<TagsCountOnly> == 4);

// header 4 + slot 4 + 3 x (4 x 2 + slot 4)
static_assert(psio::max_encoded_size<GridA>() == 4 + 4 + 3 * (4 * 2 + 4));
static_assert(psio::max_encoded_size<GridW>() == psio::max_encoded_size<GridA>());
static_assert(psio::auto_pssz_width_v<GridA> == 1 && psio::auto_pssz_width_v<GridW> == 1);
// 4 + 4 + 2 x (4 + 2 x (4 + 2 x (4 + 3)))
static_assert(psio::max_encoded_size<DeepA>() == 4 + 4 + 2 * (4 + 2 * (4 + 2 * (4 + 3))));
static_assert(psio::auto_pssz_width_v<DeepA> == 1);

// header 4 + slot 4 + selector 1 + 2 x (16 + 4)
static_assert(psio::max_encoded_size<OptA>() == 4 + 4 + 1 + 2 * (16 + 4));
static_assert(psio::max_encoded_size<OptW>() == psio::max_encoded_size<OptA>());
// header 4 + slot 4 + 3 x (16 + 4)
static_assert(psio::max_encoded_size<ArrA>() == 4 + 4 + 3 * (16 + 4));
static_assert(psio::max_encoded_size<ArrW>() == psio::max_encoded_size<ArrA>());
static_assert(psio::max_encoded_size<std::array<Text16, 3>>() == 3 * (16 + 4));
static_assert(psio::auto_pssz_width_v<std::array<Text16, 3>> == 1);
static_assert(psio::auto_pssz_width_v<std::array<std::string, 3>> == 4);

// entry: header 4 + (16 + 4) + (2 x 4 + 4) = 36; map: 4 x (36 + 4)
static_assert(psio::max_encoded_size<MapA>() == 4 + 4 + 4 * (4 + (16 + 4) + (2 * 4 + 4) + 4));
static_assert(psio::max_encoded_size<MapW>() == psio::max_encoded_size<MapA>());
static_assert(psio::max_encoded_size<SetA>() == 4 + 4 + 4 * (16 + 4));
static_assert(psio::max_encoded_size<SetW>() == psio::max_encoded_size<SetA>());

// t: 4 + (16 + 4) + 4 + (5 + 4) = 37; p: 4 + 20 + 20 = 44
static_assert(psio::max_encoded_size<TupA>() == 4 + (37 + 4) + (44 + 4));
static_assert(psio::max_encoded_size<TupW>() == psio::max_encoded_size<TupA>());
// selector 1 + widest alternative: 2 x (16 + 4) = 40
static_assert(psio::max_encoded_size<VarA>() == 4 + 4 + 1 + 2 * (16 + 4));
static_assert(psio::max_encoded_size<VarW>() == psio::max_encoded_size<VarA>());

// A wrapper type is bounded wherever it appears, as a bare type too.
static_assert(psio::max_encoded_size<psio::bounded<std::uint32_t, 5>>() == 20);
static_assert(psio::max_encoded_size<psio::bounded<Text16, 8>>() == 8 * 20);
// A list writes its offset table 4 wide whatever bounds it.
static_assert(psio::auto_pssz_width_v<psio::bounded<Text16, 8>> == 4);
static_assert(psio::auto_pssz_width_v<std::vector<Text16>> == 4);

namespace
{
   template <typename T>
   std::vector<char> pssz_bytes(const T& value)
   {
      return psio::encode(psio::pssz{}, value);
   }
   template <typename T>
   bool pssz_valid(const std::vector<char>& bytes)
   {
      return psio::validate<T>(psio::pssz{}, std::span<const char>(bytes)).ok();
   }
   template <typename T>
   bool pssz_decodes(const std::vector<char>& bytes)
   {
      try
      {
         (void)psio::decode<T>(psio::pssz{}, std::span<const char>(bytes));
         return true;
      }
      catch (const psio::codec_exception&)
      {
         return false;
      }
   }
   template <typename T>
   bool pssz_encodes(const T& value)
   {
      std::vector<char> out;
      const bool checked = psio::encode_pssz_checked(psio::pssz{}, value, out).ok();
      bool       plain   = true;
      try
      {
         (void)psio::encode(psio::pssz{}, value);
      }
      catch (const psio::codec_exception&)
      {
         plain = false;
      }
      REQUIRE(checked == plain);
      return checked;
   }
   // The frame a writer that skips every bound check would produce.
   template <typename T>
   std::vector<char> unchecked(const T& value)
   {
      return psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, value);
   }
   // A value at its bounds is accepted by every entry point; `over` (one
   // past a bound somewhere in the tree) is refused by every entry point.
   template <typename T>
   void check_bound(const T& at_bound, const T& over)
   {
      REQUIRE(pssz_encodes(at_bound));
      const auto good = pssz_bytes(at_bound);
      CHECK(pssz_valid<T>(good));
      CHECK(pssz_decodes<T>(good));
      CHECK(psio::validate_strict<T>(psio::pssz{}, std::span<const char>(good)).ok());

      CHECK_FALSE(pssz_encodes(over));
      const auto bad = unchecked(over);
      CHECK_FALSE(pssz_valid<T>(bad));
      CHECK_FALSE(pssz_decodes<T>(bad));
      CHECK_FALSE(psio::validate_strict<T>(psio::pssz{}, std::span<const char>(bad)).ok());
      T    out{};
      bool unknown = false;
      CHECK_FALSE(
         psio::decode_pssz_checked(psio::pssz{}, std::span<const char>(bad), out, unknown).ok());

      // The schema-driven codec, given only the IR the builder emits for the
      // annotations (bounded list / string nodes), derives the same widths
      // and accepts and refuses the same frames.
      static const psio::Schema schema = psio::SchemaBuilder{}.insert<T>("T").build();
      std::size_t               width  = 0;
      CHECK(psio::pssz_ir::auto_width(schema, "T", width).ok());
      CHECK(width == psio::auto_pssz_width_v<T>);
      CHECK(psio::pssz_ir::validate_pssz(schema, "T", good).ok());
      CHECK_FALSE(psio::pssz_ir::validate_pssz(schema, "T", bad).ok());
      std::string text;
      CHECK(psio::pssz_ir::pssz_to_json(schema, "T", psio::ijson{}, good, text).ok());
      CHECK(text == psio::encode(psio::ijson{}, at_bound));
      std::vector<char> again;
      CHECK(psio::pssz_ir::json_to_pssz(schema, "T", psio::ijson{}, std::span<const char>{text},
                                        again)
               .ok());
      CHECK(again == good);

      // The typed text codecs: a value outside its tree has no JSON, and
      // the JSON of one (written by a twin without the annotations: here the
      // schema-free text of the frame) is refused by the reader.
      CHECK(psio::decode<T>(psio::ijson{}, std::span<const char>{text}) == at_bound);
      CHECK_THROWS_AS(psio::encode(psio::json{}, over), psio::codec_exception);
      CHECK_THROWS_AS(psio::encode(psio::ijson{}, over), psio::codec_exception);
   }
   const std::string s16(16, 'a');
   const std::string s17(17, 'a');
}  // namespace

TEST_CASE("annotation tree: the annotation and the wrapper spelling give the same bytes",
          "[annotation_tree]")
{
   {
      TagsA a{{"alpha", "beta", s16}};
      TagsW w;
      for (const auto& t : a.tags)
         w.tags.push_back(Text16{t});
      CHECK(pssz_bytes(a) == pssz_bytes(w));
      CHECK(pssz_bytes(TagsRaw{a.tags}) == pssz_bytes(w));
      CHECK(psio::decode<TagsA>(psio::pssz{}, std::span<const char>(pssz_bytes(w))).tags ==
            a.tags);
   }
   {
      GridA a{{{1, 2, 3, 4}, {}, {5}}};
      GridW w;
      for (const auto& row : a.rows)
         w.rows.push_back(psio::bounded<std::uint16_t, 4>{row});
      CHECK(pssz_bytes(a) == pssz_bytes(w));
   }
   {
      OptA a{std::vector<std::string>{"x", s16}};
      OptW w{psio::bounded<Text16, 2>{{Text16{"x"}, Text16{s16}}}};
      CHECK(pssz_bytes(a) == pssz_bytes(w));
      CHECK(pssz_bytes(OptA{}) == pssz_bytes(OptW{}));
   }
   {
      ArrA a{{"x", "", s16}};
      ArrW w{{Text16{"x"}, Text16{""}, Text16{s16}}};
      // The array's own offset table is 1 wide in both spellings.
      CHECK(pssz_bytes(a) == pssz_bytes(w));
      CHECK(pssz_bytes(a).size() == 1 + 1 + 3 + 17);
   }
   {
      MapA a{{{"a", {1, 2}}, {s16, {}}}};
      MapW w;
      for (const auto& [k, v] : a.m)
         w.m.emplace(Text16{k}, psio::bounded<std::uint32_t, 2>{v});
      CHECK(pssz_bytes(a) == pssz_bytes(w));
   }
   {
      SetA a{{"a", "b", s16}};
      SetW w;
      for (const auto& k : a.s)
         w.s.insert(Text16{k});
      CHECK(pssz_bytes(a) == pssz_bytes(w));
   }
   {
      TupA a{{"t", 7, {1, 2, 3}}, {"p", s16}};
      TupW w{{Text16{"t"}, 7, psio::bounded<std::uint8_t, 5>{{1, 2, 3}}},
             {Text16{"p"}, Text16{s16}}};
      CHECK(pssz_bytes(a) == pssz_bytes(w));
   }
   {
      VarA a{std::vector<std::string>{"x", s16}};
      VarW w{psio::bounded<Text16, 2>{{Text16{"x"}, Text16{s16}}}};
      CHECK(pssz_bytes(a) == pssz_bytes(w));
      CHECK(pssz_bytes(VarA{std::string("y")}) == pssz_bytes(VarW{Text16{"y"}}));
   }
}

TEST_CASE("annotation tree: every depth is enforced on encode, validate and decode",
          "[annotation_tree]")
{
   // The list's count, then an element's bytes.
   check_bound(TagsA{std::vector<std::string>(8, s16)}, TagsA{std::vector<std::string>(9, "a")});
   check_bound(TagsA{{s16}}, TagsA{{"a", s17}});
   check_bound(TagsRaw{{s16}}, TagsRaw{{s17}});
   // UTF-8 of an element.
   check_bound(TagsA{{"\xc3\xa9"}}, TagsA{{"ok", "\xff"}});
   CHECK(pssz_encodes(TagsRaw{{"\xff"}}));  // raw strings carry any bytes

   check_bound(GridA{{{1, 2, 3, 4}, {}, {5}}}, GridA{{{1}, {1}, {1}, {1}}});
   check_bound(GridA{{{1, 2, 3, 4}}}, GridA{{{}, {1, 2, 3, 4, 5}}});

   using L3 = std::vector<std::vector<std::vector<std::string>>>;
   check_bound(DeepA{L3{{{"abc", "abc"}, {"abc"}}, {{}}}}, DeepA{L3{{{"abc", "abcd"}}}});
   check_bound(DeepA{L3{{{"abc"}}}}, DeepA{L3{{{"a", "b", "c"}}}});
   check_bound(DeepA{L3{{{"abc"}}}}, DeepA{L3{{{}, {}, {}}}});

   check_bound(OptA{std::vector<std::string>{s16, s16}},
               OptA{std::vector<std::string>{"a", "b", "c"}});
   check_bound(OptA{}, OptA{std::vector<std::string>{s17}});
   check_bound(ArrA{{s16, s16, s16}}, ArrA{{"", s17, ""}});

   check_bound(MapA{{{s16, {1, 2}}}}, MapA{{{s17, {}}}});
   check_bound(MapA{{{"k", {1, 2}}}}, MapA{{{"k", {1, 2, 3}}}});
   check_bound(MapA{{{"a", {}}, {"b", {}}, {"c", {}}, {"d", {}}}},
               MapA{{{"a", {}}, {"b", {}}, {"c", {}}, {"d", {}}, {"e", {}}}});
   check_bound(SetA{{s16}}, SetA{{s17}});

   check_bound(TupA{{s16, 1, {1, 2, 3, 4, 5}}, {s16, s16}}, TupA{{s17, 1, {}}, {"", ""}});
   check_bound(TupA{{"", 1, {}}, {"", ""}}, TupA{{"", 1, {1, 2, 3, 4, 5, 6}}, {"", ""}});
   check_bound(TupA{{"", 1, {}}, {"", s16}}, TupA{{"", 1, {}}, {"", s17}});
   check_bound(VarA{std::string(s16)}, VarA{std::string(s17)});
   check_bound(VarA{std::vector<std::string>{s16, s16}},
               VarA{std::vector<std::string>{"a", "b", "c"}});
   check_bound(VarA{std::uint32_t{7}}, VarA{std::vector<std::string>{s17}});
}

TEST_CASE("annotation tree: psio::bounded is a pssz list with its bound enforced",
          "[annotation_tree]")
{
   using B = psio::bounded<std::uint32_t, 3>;
   B three{{1, 2, 3}};
   CHECK(pssz_bytes(three) == pssz_bytes(std::vector<std::uint32_t>{1, 2, 3}));
   CHECK(psio::decode<B>(psio::pssz{}, std::span<const char>(pssz_bytes(three))).storage() ==
         three.storage());
   const auto four = pssz_bytes(std::vector<std::uint32_t>{1, 2, 3, 4});
   CHECK_FALSE(pssz_valid<B>(four));
   CHECK_FALSE(pssz_decodes<B>(four));
   B over;
   over.storage() = {1, 2, 3, 4};  // past the bound behind the wrapper's back
   CHECK_FALSE(pssz_encodes(over));

   // As an element and under a member annotation of its own.
   check_bound(GridW{}, [] {
      GridW g;
      g.rows.push_back(psio::bounded<std::uint16_t, 4>{});
      g.rows[0].storage() = {1, 2, 3, 4, 5};
      return g;
   }());
}

// ── Schema IR ─────────────────────────────────────────────────────────────
namespace
{
   using namespace psio::schema_types;

   // `type` with every interned (@N) reference replaced by what it names:
   // the builder interns a wrapper type and writes an annotated type inline.
   AnyType inlined(const psio::Schema& schema, const AnyType& type);
   Box<AnyType> inlined(const psio::Schema& schema, const Box<AnyType>& type)
   {
      return Box<AnyType>{inlined(schema, *type)};
   }
   AnyType inlined(const psio::Schema& schema, const AnyType& type)
   {
      return std::visit(
         [&](const auto& t) -> AnyType {
            using N = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<N, Type>)
            {
               if (!t.type.starts_with("@"))
                  return t;
               const AnyType* target = schema.get(t.type);
               REQUIRE(target != nullptr);
               return inlined(schema, *target);
            }
            else if constexpr (std::is_same_v<N, Object> || std::is_same_v<N, Struct> ||
                               std::is_same_v<N, Variant>)
            {
               N out = t;
               for (auto& m : out.members)
                  m.type = inlined(schema, m.type);
               return out;
            }
            else if constexpr (std::is_same_v<N, Tuple>)
            {
               N out = t;
               for (auto& m : out.members)
                  m = inlined(schema, m);
               return out;
            }
            else if constexpr (requires { t.type; })
            {
               N out    = t;
               out.type = inlined(schema, t.type);
               return out;
            }
            else
               return t;
         },
         type.value);
   }
   template <typename T>
   AnyType member_type(std::string_view member)
   {
      const psio::Schema schema = psio::SchemaBuilder{}.insert<T>("T").build();
      const AnyType*     root   = schema.get(std::string{psio::reflect<T>::name});
      REQUIRE(root != nullptr);
      const auto& object = std::get<Object>(root->value);
      for (const auto& m : object.members)
         if (m.name == member)
         {
            // The attributes carry nothing for a bounded type form.
            CHECK(m.attributes.empty());
            return inlined(schema, *m.type);
         }
      FAIL("no such member");
      return {};
   }
}  // namespace

TEST_CASE("annotation tree: the annotation and the wrapper spelling give the same schema IR",
          "[annotation_tree][schema]")
{
   CHECK(member_type<TagsA>("tags") == member_type<TagsW>("tags"));
   CHECK(member_type<GridA>("rows") == member_type<GridW>("rows"));
   CHECK(member_type<OptA>("names") == member_type<OptW>("names"));
   CHECK(member_type<ArrA>("names") == member_type<ArrW>("names"));
   CHECK(member_type<MapA>("m") == member_type<MapW>("m"));
   CHECK(member_type<SetA>("s") == member_type<SetW>("s"));
   CHECK(member_type<TupA>("t") == member_type<TupW>("t"));
   CHECK(member_type<TupA>("p") == member_type<TupW>("p"));
   // A variant alternative is named in I-JSON by its C++ type name, which
   // the two spellings do not share: equal apart from that attribute.
   const auto unnamed = [](AnyType type) {
      for (auto& m : std::get<Variant>(type.value).members)
         m.attributes.clear();
      return type;
   };
   CHECK(unnamed(member_type<VarA>("v")) == unnamed(member_type<VarW>("v")));

   // The nodes themselves: a bounded list of bounded UTF-8 strings, and of
   // bounded raw strings.
   const AnyType u8   = Int{8, false};
   const AnyType text = Custom{Box<AnyType>{AnyType{BoundedList{Box<AnyType>{u8}, 16}}}, "utf8"};
   const AnyType raw  = Custom{Box<AnyType>{AnyType{BoundedList{Box<AnyType>{u8}, 16}}}, "string"};
   CHECK(member_type<TagsA>("tags") == AnyType{BoundedList{Box<AnyType>{text}, 8}});
   CHECK(member_type<TagsRaw>("tags") == AnyType{BoundedList{Box<AnyType>{raw}, 8}});
   // The count alone: the elements stay plain strings.
   const AnyType plain = Custom{Box<AnyType>{AnyType{List{Box<AnyType>{u8}}}}, "string"};
   CHECK(member_type<TagsCountOnly>("tags") == AnyType{BoundedList{Box<AnyType>{plain}, 8}});
}

// ── The key codec ─────────────────────────────────────────────────────────
TEST_CASE("annotation tree: psio::key refuses a key outside the tree", "[annotation_tree][key]")
{
   const auto key_of = [](const auto& v) { return psio::encode(psio::key{}, v); };
   const auto good   = key_of(TagsA{{"a", s16}});
   CHECK(psio::validate<TagsA>(psio::key{}, std::span<const char>{good}).ok());
   CHECK(psio::decode<TagsA>(psio::key{}, std::span<const char>{good}) == TagsA{{"a", s16}});
   // (The same key bytes, written for the twin without annotations.)
   const auto long_element = key_of(TagsCountOnly{{"a", s17}});
   CHECK(psio::validate<TagsCountOnly>(psio::key{}, std::span<const char>{long_element}).ok());
   CHECK_FALSE(psio::validate<TagsA>(psio::key{}, std::span<const char>{long_element}).ok());
   const auto bad_utf8 = key_of(TagsCountOnly{{"\xff"}});
   CHECK_FALSE(psio::validate<TagsA>(psio::key{}, std::span<const char>{bad_utf8}).ok());
   CHECK(psio::validate<TagsRaw>(psio::key{}, std::span<const char>{bad_utf8}).ok());
}

// ── JSON readers ──────────────────────────────────────────────────────────
TEST_CASE("annotation tree: the JSON readers refuse text outside the tree",
          "[annotation_tree][json]")
{
   const auto reads = []<typename T, typename Fmt>(Fmt fmt, std::string_view text) {
      const bool valid = psio::validate<T>(fmt, std::span<const char>{text}).ok();
      bool       decoded = true;
      try
      {
         (void)psio::decode<T>(fmt, std::span<const char>{text});
      }
      catch (const psio::codec_exception&)
      {
         decoded = false;
      }
      CHECK(valid == decoded);
      return valid;
   };
   for (bool i_json : {false, true})
   {
      const auto read = [&]<typename T>(std::string_view text) {
         return i_json ? reads.template operator()<T>(psio::ijson{}, text)
                       : reads.template operator()<T>(psio::json{}, text);
      };
      CHECK(read.template operator()<TagsA>(R"({"tags":["a","aaaaaaaaaaaaaaaa"]})"));
      CHECK_FALSE(read.template operator()<TagsA>(R"({"tags":["a","aaaaaaaaaaaaaaaaa"]})"));
      CHECK_FALSE(
         read.template operator()<TagsA>(R"({"tags":["1","2","3","4","5","6","7","8","9"]})"));
      CHECK(read.template operator()<GridA>(R"({"rows":[[1,2,3,4],[],[5]]})"));
      CHECK_FALSE(read.template operator()<GridA>(R"({"rows":[[1,2,3,4,5]]})"));
      CHECK_FALSE(read.template operator()<GridA>(R"({"rows":[[],[],[],[]]})"));
      CHECK(read.template operator()<OptA>(R"({"names":null})"));
      CHECK_FALSE(read.template operator()<OptA>(R"({"names":["a","b","c"]})"));
      CHECK_FALSE(read.template operator()<ArrA>(R"({"names":["","aaaaaaaaaaaaaaaaa",""]})"));
      CHECK(read.template operator()<ArrA>(R"({"names":["","aaaaaaaaaaaaaaaa",""]})"));
   }
}
