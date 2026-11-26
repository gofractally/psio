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
#include <psio/mutable_view.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
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
// In the bound a header, a slot and a list / array table entry count at the
// width of the type that writes them (docs/pssz-spec.md 3.2 `fit`): all of
// the types below are one byte wide at every level.

// header 1 + slot 1 + 8 x (table slot 1 + 16)
static_assert(psio::inferred_bound<TagsA>() == 1 + 1 + 8 * (1 + 16));
static_assert(psio::inferred_bound<TagsW>() == psio::inferred_bound<TagsA>());
static_assert(psio::inferred_bound<TagsRaw>() == psio::inferred_bound<TagsA>());
static_assert(psio::auto_pssz_width_v<TagsA> == 1);
static_assert(psio::auto_pssz_width_v<TagsW> == 1);
static_assert(psio::auto_pssz_width_v<TagsRaw> == 1);
// The count alone bounds nothing: the strings are unbounded.
static_assert(!psio::inferred_bound<TagsCountOnly>().has_value());
static_assert(psio::auto_pssz_width_v<TagsCountOnly> == 4);

// header 1 + slot 1 + 3 x (table slot 1 + 4 x 2)
static_assert(psio::inferred_bound<GridA>() == 1 + 1 + 3 * (1 + 4 * 2));
static_assert(psio::inferred_bound<GridW>() == psio::inferred_bound<GridA>());
static_assert(psio::auto_pssz_width_v<GridA> == 1 && psio::auto_pssz_width_v<GridW> == 1);
// 1 + 1 + 2 x (1 + 2 x (1 + 2 x (1 + 3)))
static_assert(psio::inferred_bound<DeepA>() == 1 + 1 + 2 * (1 + 2 * (1 + 2 * (1 + 3))));
static_assert(psio::auto_pssz_width_v<DeepA> == 1);

// header 1 + slot 1 + selector 1 + 2 x (1 + 16)
static_assert(psio::inferred_bound<OptA>() == 1 + 1 + 1 + 2 * (1 + 16));
static_assert(psio::inferred_bound<OptW>() == psio::inferred_bound<OptA>());
// header 1 + slot 1 + 3 x (1 + 16)
static_assert(psio::inferred_bound<ArrA>() == 1 + 1 + 3 * (1 + 16));
static_assert(psio::inferred_bound<ArrW>() == psio::inferred_bound<ArrA>());
static_assert(psio::inferred_bound<std::array<Text16, 3>>() == 3 * (1 + 16));
static_assert(psio::auto_pssz_width_v<std::array<Text16, 3>> == 1);
static_assert(psio::auto_pssz_width_v<std::array<std::string, 3>> == 4);

// entry: header 1 + (1 + 16) + (1 + 2 x 4) = 27; map: 4 x (1 + 27)
static_assert(psio::inferred_bound<MapA>() == 1 + 1 + 4 * (1 + 1 + (1 + 16) + (1 + 2 * 4)));
static_assert(psio::inferred_bound<MapW>() == psio::inferred_bound<MapA>());
static_assert(psio::inferred_bound<SetA>() == 1 + 1 + 4 * (1 + 16));
static_assert(psio::inferred_bound<SetW>() == psio::inferred_bound<SetA>());

// t: 1 + (1 + 16) + 4 + (1 + 5) = 28; p: 1 + 17 + 17 = 35
static_assert(psio::inferred_bound<TupA>() == 1 + (1 + 28) + (1 + 35));
static_assert(psio::inferred_bound<TupW>() == psio::inferred_bound<TupA>());
// selector 1 + widest alternative: 2 x (1 + 16) = 34
static_assert(psio::inferred_bound<VarA>() == 1 + 1 + 1 + 2 * (1 + 16));
static_assert(psio::inferred_bound<VarW>() == psio::inferred_bound<VarA>());

// A wrapper type is bounded wherever it appears, as a bare type too.
static_assert(psio::inferred_bound<psio::bounded<std::uint32_t, 5>>() == 20);
static_assert(psio::inferred_bound<psio::bounded<Text16, 8>>() == 8 * (1 + 16));
// A list's offset table takes the width of the list's own bound: one byte
// here, four for the unbounded vector.
static_assert(psio::auto_pssz_width_v<psio::bounded<Text16, 8>> == 1);
static_assert(psio::auto_pssz_width_v<std::vector<Text16>> == 4);

// ── A list's table at the width boundaries ───────────────────────────────
//
// N elements of at most M bytes: the least W of 1, 2, 4 with
// N * (W + M) <= 2^(8W) - 1, and N * (W + M) is the bound.
template <std::size_t M, std::size_t N>
using BList = psio::bounded<psio::utf8_string<M>, N>;
template <typename T>
constexpr auto bound_of = psio::inferred_bound<T>();
template <typename T>
constexpr std::size_t width_of = psio::auto_pssz_width_v<T>;
// 255 / 256: 15 * (1 + 16) = 255 is the last 1-byte table.
static_assert(bound_of<BList<16, 15>> == 255 && width_of<BList<16, 15>> == 1);
static_assert(bound_of<BList<17, 15>> == 15 * (2 + 17) && width_of<BList<17, 15>> == 2);
static_assert(bound_of<BList<15, 16>> == 16 * (2 + 15) && width_of<BList<15, 16>> == 2);  // 256
static_assert(bound_of<BList<1, 127>> == 254 && width_of<BList<1, 127>> == 1);
static_assert(bound_of<BList<1, 128>> == 128 * 3 && width_of<BList<1, 128>> == 2);  // 256
// 65535 / 65536: 255 * (2 + 255) = 65535 is the last 2-byte table.
static_assert(bound_of<BList<255, 255>> == 65535 && width_of<BList<255, 255>> == 2);
static_assert(bound_of<BList<255, 256>> == 256 * (4 + 255) && width_of<BList<255, 256>> == 4);
static_assert(bound_of<BList<254, 256>> == 65536 + 2 * 256 && width_of<BList<254, 256>> == 4);
// A list of fixed-size elements has no table: its bound is count * size, and
// a list of unbounded elements is unbounded whatever its count.
static_assert(bound_of<psio::bounded<std::uint64_t, 31>> == 248);
static_assert(!bound_of<psio::bounded<std::string, 8>> &&
              width_of<psio::bounded<std::string, 8>> == 4);
// Nested: each level has its own width. Rows of 255 bytes (1-byte tables);
// 20 of them are 20 * (2 + 255) = 5140 (a 2-byte table); a single row is
// 1 * (2 + 255) = 257 — one byte over what a 1-byte table can address.
static_assert(bound_of<psio::bounded<BList<16, 15>, 20>> == 5140 &&
              width_of<psio::bounded<BList<16, 15>, 20>> == 2);
static_assert(bound_of<psio::bounded<BList<16, 15>, 1>> == 257 &&
              width_of<psio::bounded<BList<16, 15>, 1>> == 2);
static_assert(bound_of<psio::bounded<BList<16, 14>, 1>> == 1 + 14 * 17 &&
              width_of<psio::bounded<BList<16, 14>, 1>> == 1);
// 255 rows of 255 bytes: 255 * (2 + 255) = 65535; one more row needs 4.
static_assert(bound_of<psio::bounded<BList<16, 15>, 255>> == 65535 &&
              width_of<psio::bounded<BList<16, 15>, 255>> == 2);
static_assert(width_of<psio::bounded<BList<16, 15>, 256>> == 4);
// An array of variable elements, and an optional list (one selector byte).
static_assert(bound_of<std::array<psio::utf8_string<16>, 15>> == 255 &&
              width_of<std::array<psio::utf8_string<16>, 15>> == 1);
static_assert(bound_of<std::array<psio::utf8_string<17>, 15>> == 285 &&
              width_of<std::array<psio::utf8_string<17>, 15>> == 2);
static_assert(bound_of<std::optional<BList<16, 15>>> == 256);

namespace at_width
{
   // The same boundaries by annotation, and for maps and sets (which have
   // no bounded type: the count is always a member annotation). The record
   // has its own width: 2 * W + the member.
   struct L255
   {
      friend bool operator==(const L255&, const L255&) = default;
      std::vector<std::string> v;
      PSIO_REFLECT(L255, attr(v, max<15> | element(max<16>)))
   };
   struct L256
   {
      friend bool operator==(const L256&, const L256&) = default;
      std::vector<std::string> v;
      PSIO_REFLECT(L256, attr(v, max<16> | element(max<15>)))
   };
   struct L65535
   {
      friend bool operator==(const L65535&, const L65535&) = default;
      std::vector<std::string> v;
      PSIO_REFLECT(L65535, attr(v, max<255> | element(max<255>)))
   };
   struct L65536
   {
      friend bool operator==(const L65536&, const L65536&) = default;
      std::vector<std::string> v;
      PSIO_REFLECT(L65536, attr(v, max<256> | element(max<255>)))
   };
   // An entry (u8 key, string value of <= M bytes) is the tuple: header 1,
   // key 1, slot 1, value: 3 + M (one byte wide through M = 252).
   struct M255  // 15 entries of 3 + 13 = 16: 15 * (1 + 16) = 255
   {
      friend bool operator==(const M255&, const M255&) = default;
      std::map<std::uint8_t, std::string> m;
      PSIO_REFLECT(M255, attr(m, max<15> | values(max<13>)))
   };
   struct M256  // 16 entries of 3 + 12 = 15: 16 * (1 + 15) = 256 → 16 * (2 + 15)
   {
      friend bool operator==(const M256&, const M256&) = default;
      std::map<std::uint8_t, std::string> m;
      PSIO_REFLECT(M256, attr(m, max<16> | values(max<12>)))
   };
   struct M65535  // 255 entries of 3 + 252 = 255: 255 * (2 + 255) = 65535
   {
      friend bool operator==(const M65535&, const M65535&) = default;
      std::map<std::uint8_t, std::string> m;
      PSIO_REFLECT(M65535, attr(m, max<255> | values(max<252>)))
   };
   struct M65536  // 256 such entries: 256 * (2 + 255) = 65792 → 256 * (4 + 255)
   {
      friend bool operator==(const M65536&, const M65536&) = default;
      std::map<std::uint8_t, std::string> m;
      PSIO_REFLECT(M65536, attr(m, max<256> | values(max<252>)))
   };
   struct S255
   {
      friend bool operator==(const S255&, const S255&) = default;
      std::set<std::string> s;
      PSIO_REFLECT(S255, attr(s, max<15> | element(max<16>)))
   };
   struct S256
   {
      friend bool operator==(const S256&, const S256&) = default;
      std::set<std::string> s;
      PSIO_REFLECT(S256, attr(s, max<16> | element(max<15>)))
   };

   // exact<N> is a member-only bound: the schema keeps it as the member
   // attribute lengthBound over a plain List, and the list's table follows
   // it all the same (3 * (1 + 4) = 15).
   struct LExact
   {
      friend bool operator==(const LExact&, const LExact&) = default;
      std::vector<std::string> v;
      PSIO_REFLECT(LExact, attr(v, exact<3> | element(max<4>)))
   };
   // The same through an optional.
   struct LOpt
   {
      friend bool operator==(const LOpt&, const LOpt&) = default;
      std::optional<std::vector<std::string>> v;
      PSIO_REFLECT(LOpt, attr(v, exact<3> | element(max<4>)))
   };
   // Rows of records: fixed-size members of an element are writable in
   // place through a mutable view.
   struct Cell
   {
      friend bool operator==(const Cell&, const Cell&) = default;
      std::uint16_t n = 0;
      std::string   s;
      PSIO_REFLECT(Cell, n, attr(s, max<4>))
   };
   struct Cells
   {
      friend bool operator==(const Cells&, const Cells&) = default;
      std::vector<Cell> v;
      PSIO_REFLECT(Cells, attr(v, max<8>))
   };

   struct Plain
   {
      std::vector<std::string> v;
      PSIO_REFLECT(Plain, v)
   };

   // The width a record's member has on the wire: that of the member's type
   // at a position carrying the member's annotations.
   template <typename R>
   constexpr std::size_t member_width = psio::detail::pssz_impl::width_for<
      typename psio::detail::pssz_impl::record_traits<R>::template member_type<0>,
      psio::detail::pssz_impl::member_ann<R, 0>>();
}  // namespace at_width
static_assert(at_width::member_width<at_width::L255> == 1 && bound_of<at_width::L255> == 4 + 255);
static_assert(at_width::member_width<at_width::L256> == 2 &&
              bound_of<at_width::L256> == 4 + 16 * 17);
static_assert(at_width::member_width<at_width::L65535> == 2 &&
              bound_of<at_width::L65535> == 8 + 65535);
static_assert(at_width::member_width<at_width::L65536> == 4 &&
              bound_of<at_width::L65536> == 8 + 256 * 259);
static_assert(at_width::member_width<at_width::M255> == 1 && bound_of<at_width::M255> == 4 + 255);
static_assert(at_width::member_width<at_width::M256> == 2 &&
              bound_of<at_width::M256> == 4 + 16 * 17);
static_assert(at_width::member_width<at_width::M65535> == 2 &&
              bound_of<at_width::M65535> == 8 + 65535);
static_assert(at_width::member_width<at_width::M65536> == 4 &&
              bound_of<at_width::M65536> == 8 + 256 * 259);
static_assert(at_width::member_width<at_width::S255> == 1 && at_width::member_width<at_width::S256> == 2);
// The unannotated member types are unbounded: 4.
static_assert(width_of<std::vector<std::string>> == 4 &&
              width_of<std::map<std::uint8_t, std::string>> == 4 &&
              width_of<std::set<std::string>> == 4);

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
         (void)psio::to_pssz_checked(value);   // the validated write, raising
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
      CHECK_THROWS_AS(psio::to_json_checked(over), psio::codec_exception);
      CHECK_THROWS_AS(psio::to_ijson_checked(over), psio::codec_exception);
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
   // A variant alternative is labelled by its type's name, and a wrapper
   // type is named as what it bounds: the two spellings are one node.
   CHECK(member_type<VarA>("v") == member_type<VarW>("v"));

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

// ── A list's own offset-table width ───────────────────────────────────────
namespace
{
   std::string hex_of(const std::vector<char>& bytes)
   {
      static const char* digits = "0123456789abcdef";
      std::string        out;
      for (unsigned char c : bytes)
      {
         out += digits[c >> 4];
         out += digits[c & 15];
      }
      return out;
   }
}  // namespace

TEST_CASE("list width: a bounded list writes its table at its own width", "[annotation_tree][list_width]")
{
   using namespace at_width;
   // The same two strings behind a 1-, 2- and 4-byte table. The record in
   // front has its own width (2 or 4 bytes: header, slot).
   CHECK(hex_of(pssz_bytes(L255{{"ab", "c"}})) == "02000200" "0204" "616263");
   CHECK(hex_of(pssz_bytes(L256{{"ab", "c"}})) == "02000200" "04000600" "616263");
   CHECK(hex_of(pssz_bytes(L65535{{"ab", "c"}})) == "0400000004000000" "04000600" "616263");
   CHECK(hex_of(pssz_bytes(L65536{{"ab", "c"}})) ==
         "0400000004000000" "080000000a000000" "616263");
   // Wrapper types, bare: the table alone.
   CHECK(hex_of(pssz_bytes(BList<16, 15>{{Text16{"ab"}, Text16{"c"}}})) == "0204" "616263");
   CHECK(hex_of(pssz_bytes(BList<17, 15>{{psio::utf8_string<17>{"ab"}, psio::utf8_string<17>{"c"}}})) ==
         "04000600" "616263");
   CHECK(hex_of(pssz_bytes(std::vector<Text16>{Text16{"ab"}, Text16{"c"}})) ==
         "080000000a000000" "616263");
   // A map: record 02000200, table 02 (one entry), entry (header 02, key,
   // slot 02, value).
   CHECK(hex_of(pssz_bytes(M255{{{7, "v"}}})) == "02000200" "01" "02" "07" "02" "76");
   CHECK(hex_of(pssz_bytes(M256{{{7, "v"}}})) == "02000200" "0200" "02" "07" "02" "76");
   // An empty list is empty at every width.
   CHECK(hex_of(pssz_bytes(L255{})) == "02000200");
   CHECK(pssz_bytes(BList<16, 15>{}).empty());
   // exact<3>: a 1-byte table (03 03 04), directly and behind an optional's
   // selector.
   CHECK(hex_of(pssz_bytes(LExact{{"", "a", ""}})) == "0101" "030304" "61");
   CHECK(hex_of(pssz_bytes(LOpt{std::vector<std::string>{"", "a", ""}})) ==
         "0101" "01" "030304" "61");

   // The frame with the other table width is not a frame of the type.
   CHECK_FALSE(pssz_valid<BList<16, 15>>(pssz_bytes(std::vector<Text16>{Text16{"ab"}, Text16{"c"}})));
   CHECK_FALSE(pssz_valid<std::vector<Text16>>(pssz_bytes(BList<16, 15>{{Text16{"ab"}, Text16{"c"}}})));
   CHECK_FALSE(pssz_valid<L256>(pssz_bytes(L255{{"ab", "c"}})));
}

TEST_CASE("list width: values at the bounds, typed and schema-driven", "[annotation_tree][list_width]")
{
   using namespace at_width;
   const std::string s12(12, 'v'), s13(13, 'v'), s15(15, 'x');
   // The largest value of each type: 255 bytes behind a 1-byte table (the
   // last offset is 239), 272 behind a 2-byte one.
   {
      const L255 full{std::vector<std::string>(15, s16)};
      CHECK(pssz_bytes(full).size() == 4 + 255);
      check_bound(full, L255{std::vector<std::string>(16, "")});
      check_bound(full, L255{{s17}});
   }
   check_bound(L256{std::vector<std::string>(16, s15)}, L256{std::vector<std::string>(17, "")});
   check_bound(L65535{std::vector<std::string>(255, std::string(255, 'z'))},
               L65535{std::vector<std::string>(256, "")});
   check_bound(L65536{std::vector<std::string>(256, std::string(255, 'z'))},
               L65536{std::vector<std::string>(257, "")});
   {
      M255 full, over;
      for (std::uint8_t k = 0; k < 15; ++k)
         full.m.emplace(k, s13);
      // (A sixteenth entry would not even fit the 1-byte table: an encoder
      // that skips the bound check refuses it for its size.)
      over.m.emplace(std::uint8_t{0}, std::string(14, 'v'));
      CHECK(pssz_bytes(full).size() == 4 + 255);
      CHECK_THROWS_AS(unchecked([&] {
                         M255 sixteen = full;
                         sixteen.m.emplace(std::uint8_t{15}, s13);
                         return sixteen;
                      }()),
                      psio::codec_exception);
      check_bound(full, over);
   }
   {
      M256 full, over;
      for (std::uint8_t k = 0; k < 16; ++k)
         full.m.emplace(k, s12);
      over.m.emplace(std::uint8_t{0}, s13);
      CHECK(pssz_bytes(full).size() == 4 + 16 * 17);
      check_bound(full, over);
   }
   {
      S255 full{{}}, over;
      for (char c = 'a'; c < 'a' + 15; ++c)
         full.s.insert(std::string(16, c));
      over.s.insert(s17);
      check_bound(full, over);
   }
   check_bound(LExact{{"aaaa", "bbbb", "cccc"}}, LExact{{"a", "b"}});
   check_bound(LOpt{std::vector<std::string>{"aaaa", "bbbb", "cccc"}},
               LOpt{std::vector<std::string>{"a", "b", "c", "d"}});
   check_bound(LOpt{}, LOpt{std::vector<std::string>{"aaaaa", "", ""}});
}

TEST_CASE("list width: views index a narrow table", "[annotation_tree][list_width][view]")
{
   using namespace at_width;
   {
      const auto bytes = pssz_bytes(L255{{"ab", "", "c"}});
      psio::view<L255, psio::pssz> v{std::span<const char>(bytes)};
      const auto                   list = v.get<0>();
      // The member's view carries the annotations that narrow its table.
      static_assert(!std::is_same_v<decltype(list), const psio::view<std::vector<std::string>, psio::pssz>>);
      REQUIRE(list.size() == 3);
      CHECK(list[0].view_() == "ab");
      CHECK(list[1].view_() == "");
      CHECK(list[2].view_() == "c");
      CHECK(list.at(3)._psio_data().empty());
   }
   {
      const auto bytes = pssz_bytes(L256{{"ab", "", "c"}});
      psio::view<L256, psio::pssz> v{std::span<const char>(bytes)};
      REQUIRE(v.get<0>().size() == 3);
      CHECK(v.get<0>()[2].view_() == "c");
   }
   {
      // An unannotated list member is viewed as before.
      const auto                    bytes = pssz_bytes(Plain{{"ab", "c"}});
      psio::view<Plain, psio::pssz> v{std::span<const char>(bytes)};
      static_assert(std::is_same_v<decltype(v.get<0>()), psio::view<std::vector<std::string>, psio::pssz>>);
      CHECK(v.get<0>()[1].view_() == "c");
   }
   {
      // Through an optional, and two levels deep (rows with their own
      // 1-byte tables behind a 1-byte table of rows).
      const auto bytes = pssz_bytes(LOpt{std::vector<std::string>{"x", "yy", ""}});
      psio::view<LOpt, psio::pssz> v{std::span<const char>(bytes)};
      REQUIRE(v.get<0>().has_value());
      CHECK((*v.get<0>())[1].view_() == "yy");

      const DeepA deep{{{{"a", "b"}, {"c"}}, {{}, {"d", "e"}}}};
      const auto  wire = pssz_bytes(deep);
      psio::view<DeepA, psio::pssz> d{std::span<const char>(wire)};
      REQUIRE(d.get<0>().size() == 2);
      CHECK(d.get<0>()[0][0][1].view_() == "b");
      CHECK(d.get<0>()[1][1][0].view_() == "d");
      CHECK(d.get<0>()[1][0].size() == 0);
   }
   {
      // A map bounded by its member: the list of its entries.
      const auto bytes = pssz_bytes(M255{{{3, "three"}, {7, "seven"}}});
      psio::view<M255, psio::pssz> v{std::span<const char>(bytes)};
      const auto                   map = v.get<0>();
      REQUIRE(map.size() == 2);
      CHECK(map[1].get<0>() == 7);
      CHECK(map[1].get<1>().view_() == "seven");
   }
   {
      // In place: a fixed-size member of an element of a bounded list.
      auto bytes = pssz_bytes(Cells{{{1, "a"}, {2, "bb"}, {3, ""}}});
      psio::mutable_view<Cells, psio::pssz> mv{std::span<char>(bytes)};
      REQUIRE(mv.get<0>().size() == 3);
      REQUIRE(mv.get<0>()[1].get<0>().set(std::uint16_t{700}));
      const auto back = psio::decode<Cells>(psio::pssz{}, std::span<const char>(bytes));
      CHECK(back == Cells{{{1, "a"}, {700, "bb"}, {3, ""}}});
      CHECK(pssz_valid<Cells>(bytes));
   }
}

// ── A record's cap bounds what lies beneath it (docs/pssz-spec.md 3.6) ────
namespace cap_test
{
   namespace pz = psio::detail::pssz_impl;
   // The action the rule was written for: every member type is the plain
   // unbounded one. 255 is the largest cap with one-byte offsets.
   struct Transfer
   {
      friend bool operator==(const Transfer&, const Transfer&) = default;
      std::uint32_t                    token = 0;
      std::uint64_t                    from  = 0;
      std::uint64_t                    amount = 0;
      psio::utf8_string<>              memo;
      std::optional<std::vector<char>> data;
      PSIO_REFLECT(Transfer, token, from, amount, memo, data, maxDynamicData(255))
   };
   struct Transfer256  // one more byte of cap: two-byte offsets
   {
      std::uint32_t                    token = 0;
      std::uint64_t                    from  = 0;
      std::uint64_t                    amount = 0;
      psio::utf8_string<>              memo;
      std::optional<std::vector<char>> data;
      PSIO_REFLECT(Transfer256, token, from, amount, memo, data, maxDynamicData(256))
   };
   struct Child  // unbounded: 4-byte offsets, wherever it appears
   {
      friend bool operator==(const Child&, const Child&) = default;
      std::uint32_t id = 0;
      std::string   name;
      PSIO_REFLECT(Child, id, name)
   };
   template <std::size_t Cap>
   struct Capped
   {
      friend bool operator==(const Capped&, const Capped&) = default;
      std::vector<std::string>                                       list;
      std::vector<std::vector<std::string>>                          rows;
      std::map<std::string, std::vector<std::uint32_t>>              map;
      std::tuple<std::string, std::vector<std::string>>              tuple;
      std::optional<std::vector<std::string>>                        opt;
      std::variant<std::uint32_t, std::vector<std::string>>          var;
      std::array<std::string, 2>                                     arr;
      Child                                                          child;
      std::vector<Child>                                             kids;
   };
   using Cap255   = Capped<255>;
   using Cap256   = Capped<256>;
   using Cap65535 = Capped<65535>;
   using Cap65536 = Capped<65536>;
   PSIO_REFLECT(Cap255, list, rows, map, tuple, opt, var, arr, child, kids, maxDynamicData(255))
   PSIO_REFLECT(Cap256, list, rows, map, tuple, opt, var, arr, child, kids, maxDynamicData(256))
   PSIO_REFLECT(Cap65535, list, rows, map, tuple, opt, var, arr, child, kids,
                maxDynamicData(65535))
   PSIO_REFLECT(Cap65536, list, rows, map, tuple, opt, var, arr, child, kids,
                maxDynamicData(65536))
   struct Free  // the same members with no cap
   {
      std::vector<std::string> list;
      std::tuple<std::string, std::vector<std::string>> tuple;
      PSIO_REFLECT(Free, list, tuple)
   };
   // A tighter bound of the member's own wins; a looser one is clamped.
   struct Tight
   {
      std::vector<std::string> list;
      PSIO_REFLECT(Tight, attr(list, max<8> | element(max<16>)), maxDynamicData(60000))
   };
   struct Loose
   {
      std::vector<std::string> list;
      PSIO_REFLECT(Loose, attr(list, max<1000> | element(max<1000>)), maxDynamicData(255))
   };

   // The width of member I of R on the wire, and of what lies beneath it.
   template <typename R, std::size_t I>
   using member_t = std::remove_cvref_t<typename pz::record_traits<R>::template member_type<I>>;
   template <typename R, std::size_t I>
   constexpr std::size_t member_w = pz::width_for<member_t<R, I>, pz::member_ann<R, I>>();
   template <typename R, std::size_t I>
   constexpr std::size_t element_w = pz::width_for<
      typename member_t<R, I>::value_type, pz::ann::element_ann<pz::member_ann<R, I>>>();

   template <typename R, std::size_t W>
   constexpr bool all_members_are =
      member_w<R, 0> == W && member_w<R, 1> == W && element_w<R, 1> == W &&  // list, rows, a row
      member_w<R, 2> == W &&                                                 // the map's table
      pz::width_for<std::pair<std::string, std::vector<std::uint32_t>>,
                    pz::ann::entry_ann<member_t<R, 2>, pz::member_ann<R, 2>>>() == W &&  // an entry
      member_w<R, 3> == W &&  // the tuple
      pz::width_for<std::vector<std::string>, pz::ann::at_ann<pz::member_ann<R, 3>, 1>>() == W &&
      element_w<R, 4> == W &&  // the optional's list
      pz::width_for<std::vector<std::string>, pz::ann::at_ann<pz::member_ann<R, 5>, 1>>() == W &&
      member_w<R, 6> == W &&   // the array's table
      member_w<R, 8> == W;     // the table of the list of records
   static_assert(psio::auto_pssz_width_v<Cap255> == 1 && all_members_are<Cap255, 1>);
   static_assert(psio::auto_pssz_width_v<Cap256> == 2 && all_members_are<Cap256, 2>);
   static_assert(psio::auto_pssz_width_v<Cap65535> == 2 && all_members_are<Cap65535, 2>);
   static_assert(psio::auto_pssz_width_v<Cap65536> == 4 && all_members_are<Cap65536, 4>);
   // A record keeps its own width beneath any cap, and what is beneath it
   // takes that record's cap (none).
   static_assert(member_w<Cap255, 7> == 4 && psio::auto_pssz_width_v<Child> == 4);
   static_assert(element_w<Cap255, 8> == 4);  // an element of kids: a Child
   // No cap: the member types alone.
   static_assert(member_w<Free, 0> == 4 && member_w<Free, 1> == 4);
   static_assert(member_w<Tight, 0> == 1 && psio::auto_pssz_width_v<Tight> == 2);
   static_assert(member_w<Loose, 0> == 1);
   // The cap does not change any bound: a capped record is its cap.
   static_assert(psio::inferred_bound<std::vector<std::string>>() == std::nullopt);
   static_assert(psio::inferred_bound<std::tuple<Cap255, std::uint8_t>>() == 2 + 2 + 255 + 1);

   // The query for a child record that cannot use its width.
   static_assert(psio::pssz_child_wider_than_cap_v<Cap255>);   // Child (4) beneath a cap of 255
   static_assert(!psio::pssz_child_wider_than_cap_v<Cap65536>);
   static_assert(!psio::pssz_child_wider_than_cap_v<Transfer>);
   static_assert(!psio::pssz_child_wider_than_cap_v<Free>);    // no cap
   static_assert(!psio::pssz_child_wider_than_cap_v<Tight>);
}  // namespace cap_test

namespace
{
   // The schema-driven codec, given only the IR, accepts the typed bytes,
   // gives the same text, and writes the same bytes back.
   template <typename T>
   void ir_agrees(const T& value)
   {
      static const psio::Schema schema = psio::SchemaBuilder{}.insert<T>("T").build();
      const auto                bytes  = pssz_bytes(value);
      REQUIRE(pssz_valid<T>(bytes));
      CHECK(psio::validate_strict<T>(psio::pssz{}, std::span<const char>(bytes)).ok());
      CHECK(psio::pssz_ir::validate_pssz(schema, "T", bytes).ok());
      std::string text;
      REQUIRE(psio::pssz_ir::pssz_to_json(schema, "T", psio::ijson{}, bytes, text).ok());
      CHECK(text == psio::encode(psio::ijson{}, value));
      std::vector<char> again;
      REQUIRE(psio::pssz_ir::json_to_pssz(schema, "T", psio::ijson{}, std::span<const char>{text},
                                          again)
                 .ok());
      CHECK(again == bytes);
   }
}  // namespace

TEST_CASE("cap: an unbounded string or byte list costs its slot and nothing else",
          "[annotation_tree][cap]")
{
   using namespace cap_test;
   static_assert(psio::auto_pssz_width_v<Transfer> == 1 && psio::auto_pssz_width_v<Transfer256> == 2);
   // header 16 (the fixed region: 4 + 8 + 8 + two 1-byte slots = 22), the
   // three numbers, slot(memo) = 22, slot(data) = 24, "hi", then data:
   // selector 01 and its one byte. No length prefix anywhere.
   const Transfer t{1, 2, 3, psio::utf8_string<>{"hi"}, std::vector<char>{'\xaa'}};
   CHECK(hex_of(pssz_bytes(t)) ==
         "16" "01000000" "0200000000000000" "0300000000000000" "16" "18" "6869" "01aa");
   // With no data its slot is pruned.
   CHECK(hex_of(pssz_bytes(Transfer{1, 2, 3, psio::utf8_string<>{"hi"}, std::nullopt})) ==
         "15" "01000000" "0200000000000000" "0300000000000000" "15" "6869");
   // One byte of cap more and every offset is two bytes.
   CHECK(hex_of(pssz_bytes(Transfer256{1, 2, 3, psio::utf8_string<>{"hi"}, std::vector<char>{'\xaa'}})) ==
         "1800" "01000000" "0200000000000000" "0300000000000000" "1800" "1a00" "6869" "01aa");

   // The budget: 255 - (header 1 + 20 + 2 slots) = 232 bytes for memo and
   // data together, one of which is the selector of a present data.
   const auto with = [](std::size_t memo, std::optional<std::size_t> data) {
      Transfer v{1, 2, 3, psio::utf8_string<>{std::string(memo, 'm')}, std::nullopt};
      if (data)
         v.data = std::vector<char>(*data, 'd');
      return v;
   };
   CHECK(pssz_bytes(with(231, 0)).size() == 255);
   CHECK(pssz_bytes(with(0, 231)).size() == 255);
   CHECK(pssz_bytes(with(100, 131)).size() == 255);
   CHECK(pssz_bytes(with(233, std::nullopt)).size() == 255);  // no data: its slot is pruned too
   CHECK_FALSE(pssz_encodes(with(232, 0)));
   CHECK_FALSE(pssz_encodes(with(0, 232)));
   CHECK_FALSE(pssz_encodes(with(100, 132)));
   CHECK_FALSE(pssz_encodes(with(234, std::nullopt)));
   // And a reader refuses the 256-byte frame.
   auto over = pssz_bytes(with(100, 131));
   over.push_back('d');
   CHECK_FALSE(pssz_valid<Transfer>(over));
   CHECK_FALSE(pssz_decodes<Transfer>(over));
   ir_agrees(t);
   ir_agrees(with(100, 131));
   ir_agrees(with(233, std::nullopt));
}

TEST_CASE("cap: lists, maps and tuples beneath a capped record take the cap's width",
          "[annotation_tree][cap]")
{
   using namespace cap_test;
   const auto fill = []<typename R>() {
      R v;
      v.list  = {"ab", "c"};
      v.rows  = {{"d"}, {}};
      v.map   = {{"k", {7}}};
      v.tuple = {"t", {"u"}};
      v.opt   = std::vector<std::string>{"o"};
      v.var   = std::vector<std::string>{"v"};
      v.arr   = {"x", "y"};
      v.child = {5, "n"};
      v.kids  = {{6, "m"}};
      return v;
   };
   // header, nine slots; then each member with one-byte offsets — except
   // the two Child records, which keep their four-byte ones.
   CHECK(hex_of(pssz_bytes(fill.template operator()<Cap255>())) ==
         "09" "090e121b" "2124272b" "38"
         "0204" "616263"                              // list
         "0204" "01" "64"                             // rows: a row with its own table
         "01" "02" "0203" "6b" "07000000"             // map: table, entry (header, slots)
         "02" "0203" "74" "01" "75"                   // tuple, and the list in it
         "01" "01" "6f"                               // optional: selector, table
         "01" "01" "76"                               // variant: index, table
         "0203" "78" "79"                             // array
         "08000000" "05000000" "08000000" "6e"        // child: 4-byte offsets
         "01" "08000000" "06000000" "08000000" "6d"); // kids: 1-byte table, 4-byte record
   ir_agrees(fill.template operator()<Cap255>());
   ir_agrees(fill.template operator()<Cap256>());
   ir_agrees(fill.template operator()<Cap65535>());
   ir_agrees(fill.template operator()<Cap65536>());
   ir_agrees(Cap255{});
   ir_agrees(Tight{{"ab", "c"}});
   ir_agrees(Loose{{"ab", "c"}});
   CHECK(hex_of(pssz_bytes(Tight{{"ab", "c"}})) == "02000200" "0204" "616263");
   CHECK(hex_of(pssz_bytes(Loose{{"ab", "c"}})) == "0101" "0204" "616263");
   CHECK(hex_of(pssz_bytes(Free{{"ab", "c"}, {"t", {"u"}}})) ==
         "08000000" "08000000" "13000000" "080000000a000000" "616263"
         "08000000" "08000000" "09000000" "74" "04000000" "75");

   // Views index the narrowed tables; the record child is the plain view.
   {
      const auto bytes = pssz_bytes(fill.template operator()<Cap255>());
      psio::view<Cap255, psio::pssz> v{std::span<const char>(bytes)};
      REQUIRE(v.get<0>().size() == 2);
      CHECK(v.get<0>()[1].view_() == "c");
      CHECK(v.get<1>()[0][0].view_() == "d");
      CHECK(v.get<1>()[1].size() == 0);
      CHECK(v.get<2>()[0].get<0>().view_() == "k");
      CHECK(v.get<2>()[0].get<1>().size() == 1);
      CHECK(v.get<3>().get<1>()[0].view_() == "u");
      CHECK((*v.get<4>())[0].view_() == "o");
      CHECK(v.get<6>()[1].view_() == "y");
      CHECK(v.get<7>().get<1>().view_() == "n");
      static_assert(std::is_same_v<decltype(v.get<7>()), psio::view<Child, psio::pssz>>);
      CHECK(v.get<8>()[0].get<0>() == 6);
   }
   {
      // In place: a number in a record in a list beneath the cap.
      auto bytes = pssz_bytes(fill.template operator()<Cap255>());
      psio::mutable_view<Cap255, psio::pssz> mv{std::span<char>(bytes)};
      REQUIRE(mv.get<8>()[0].get<0>().set(std::uint32_t{77}));
      auto expected = fill.template operator()<Cap255>();
      expected.kids[0].id = 77;
      CHECK(psio::decode<Cap255>(psio::pssz{}, std::span<const char>(bytes)) == expected);
   }
}
