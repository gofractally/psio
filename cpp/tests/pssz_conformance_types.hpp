#pragma once
// Types of the shared cross-language PSSZ conformance fixture
// (conformance/pssz-vectors.json).
//
// Every type the fixture names is listed in `pssz_conf::registry`, with a
// stable string id and a `shape` string that both languages can use to check
// that they mapped an id to the same logical type:
//
//   - a reflected record or an enum is a NAMED type: its id is its C++ name,
//     and other shapes refer to it by that id;
//   - any other type is STRUCTURAL: its id is its shape string
//     (for example `list<R2>` or `optional<raw_string>`).
//
// The shape grammar is described in conformance/pssz-vectors.md. Shapes are
// derived from the C++ types here, never written by hand.

#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_ir.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pssz_conf
{
   using u8   = std::uint8_t;
   using u16  = std::uint16_t;
   using u32  = std::uint32_t;
   using u64  = std::uint64_t;
   using Raw  = std::string;            // raw bytes        (Rust RawString)
   using Text = psio::utf8_string<>;    // unbounded UTF-8  (Rust String)
   using Text4 = psio::utf8_string<4>;  // UTF-8, at most 4 bytes

   // ── Enums ─────────────────────────────────────────────────────────────
   enum class Color : std::uint8_t { Red = 0, Green = 1, Blue = 2 };
   enum class E16 : std::uint16_t { Lo = 1, Hi = 258 };
   enum class E32 : std::int32_t { Neg = -2, Zero = 0, Pos = 16909060 };
   enum class E64 : std::uint64_t { Zero = 0, Big = 72623859790382856ull };
   // Enumerator names: the Schema IR records these as Enum (names + values)
   // over the underlying integer; the bytes are the integer's.
   PSIO_REFLECT_ENUM(Color, Red, Green, Blue)
   PSIO_REFLECT_ENUM(E16, Lo, Hi)
   PSIO_REFLECT_ENUM(E32, Neg, Zero, Pos)
   PSIO_REFLECT_ENUM(E64, Zero, Big)

   // ── Scalars ───────────────────────────────────────────────────────────
   struct Scalars
   {
      bool          t;
      u8            a;
      u16           b;
      u32           c;
      u64           d;
      std::int8_t   e;
      std::int16_t  f;
      std::int32_t  g;
      std::int64_t  h;
      float         x;
      double        y;
   };
   PSIO_REFLECT(Scalars, t, a, b, c, d, e, f, g, h, x, y)

   // ── Evolution lineage A: fixed scalar, string, vector ─────────────────
   struct R0 { u32 a; Raw s; };
   PSIO_REFLECT(R0, a, s)
   struct R1 { u32 a; Raw s; std::optional<u32> x; };
   PSIO_REFLECT(R1, a, s, x)
   struct R2 { u32 a; Raw s; std::optional<u32> x; std::optional<Raw> y; };
   PSIO_REFLECT(R2, a, s, x, y)
   struct R3
   {
      u32 a; Raw s; std::optional<u32> x; std::optional<Raw> y;
      std::optional<std::vector<u16>> z;
   };
   PSIO_REFLECT(R3, a, s, x, y, z)

   // ── Lineage B: nested record, then optional<optional<T>> ──────────────
   struct Inner { u8 k; Raw t; };
   PSIO_REFLECT(Inner, k, t)
   struct N1 { u32 a; Raw s; std::optional<Inner> in; };
   PSIO_REFLECT(N1, a, s, in)
   struct N2 { u32 a; Raw s; std::optional<Inner> in; std::optional<std::optional<u32>> oo; };
   PSIO_REFLECT(N2, a, s, in, oo)
   struct P1 { u32 a; Raw s; std::optional<std::optional<u32>> oo; };
   PSIO_REFLECT(P1, a, s, oo)

   // ── Lineage C: enum, variant, tuple ───────────────────────────────────
   using VarUS = std::variant<u32, Raw>;
   using TupBS = std::tuple<u8, Raw>;
   struct E1 { u32 a; Raw s; std::optional<Color> e; };
   PSIO_REFLECT(E1, a, s, e)
   struct E2 { u32 a; Raw s; std::optional<Color> e; std::optional<VarUS> v; };
   PSIO_REFLECT(E2, a, s, e, v)
   struct E3
   {
      u32 a; Raw s; std::optional<Color> e; std::optional<VarUS> v; std::optional<TupBS> t;
   };
   PSIO_REFLECT(E3, a, s, e, v, t)

   // A non-optional appended field: NOT a compatible revision of R0.
   struct Bad1 { u32 a; Raw s; u32 b; };
   PSIO_REFLECT(Bad1, a, s, b)

   // The evolving record as a non-last field of a parent.
   template <typename T>
   struct Parent { T child; Raw tail; };
   using Parent0 = Parent<R0>;
   using Parent1 = Parent<R1>;
   using Parent2 = Parent<R2>;
   using Parent3 = Parent<R3>;
   PSIO_REFLECT(Parent0, child, tail)
   PSIO_REFLECT(Parent1, child, tail)
   PSIO_REFLECT(Parent2, child, tail)
   PSIO_REFLECT(Parent3, child, tail)

   // ── Capped lineages: explicit maxDynamicData keeps the width fixed ────
   // cap 200 -> one-byte offsets; cap 1000 -> two-byte offsets.
   struct C1a { u32 a; Raw s; };
   PSIO_REFLECT(C1a, a, s)
   struct C1b { u32 a; Raw s; std::optional<u32> x; };
   PSIO_REFLECT(C1b, a, s, x)
   struct C1c { u32 a; Raw s; std::optional<u32> x; std::optional<Raw> y; };
   PSIO_REFLECT(C1c, a, s, x, y)
   struct C2a { u32 a; Raw s; };
   PSIO_REFLECT(C2a, a, s)
   struct C2b { u32 a; Raw s; std::optional<u32> x; };
   PSIO_REFLECT(C2b, a, s, x)
   struct C2c { u32 a; Raw s; std::optional<u32> x; std::optional<Raw> y; };
   PSIO_REFLECT(C2c, a, s, x, y)
   // Inferred (uncapped) one-byte lineage: the appended field keeps the bound
   // under the threshold, so the step is legal without a cap.
   struct I0 { u32 a; u8 b; };
   PSIO_REFLECT(I0, a, b)
   struct I1 { u32 a; u8 b; std::optional<u16> x; };
   PSIO_REFLECT(I1, a, b, x)

   // ── Optional fixtures ─────────────────────────────────────────────────
   struct Base { Raw name; };
   PSIO_REFLECT(Base, name)
   struct Extended { Raw name; std::optional<Raw> extra; };
   PSIO_REFLECT(Extended, name, extra)
   struct Twice { Raw name; std::optional<u32> middle; std::optional<Raw> last; };
   PSIO_REFLECT(Twice, name, middle, last)
   struct OddBase { bool flag; Raw name; };
   PSIO_REFLECT(OddBase, flag, name)
   struct OddExtended { bool flag; Raw name; std::optional<Raw> extra; };
   PSIO_REFLECT(OddExtended, flag, name, extra)
   struct OldOuter { Base inner; std::vector<Base> values; };
   PSIO_REFLECT(OldOuter, inner, values)
   struct NewOuter { Extended inner; std::vector<Extended> values; };
   PSIO_REFLECT(NewOuter, inner, values)
   struct DepthNode { u8 value; std::optional<psio::schema_types::Box<DepthNode>> next; };
   PSIO_REFLECT(DepthNode, value, next)

   // ── Boxed values ──────────────────────────────────────────────────────
   // A Box<T> is T on the wire, and the typed codecs infer no size bound
   // through it: a record holding a boxed bounded value uses 4-byte offsets
   // unless it carries an explicit cap. The Schema IR marks the box (Custom
   // "box") so that a schema-only reader derives the same widths.
   template <typename T> using Boxed = psio::schema_types::Box<T>;
   struct BLeaf { u16 n; psio::utf8_string<4> t; };  // bounded: at most 11 bytes
   PSIO_REFLECT(BLeaf, n, t)
   struct BDirect { u32 id; BLeaf leaf; };  // no box: 1-byte offsets
   PSIO_REFLECT(BDirect, id, leaf)
   struct BoxRec { u32 id; Boxed<BLeaf> leaf; };  // boxed: no bound, 4-byte offsets
   PSIO_REFLECT(BoxRec, id, leaf)
   struct BoxCapRec { u32 id; Boxed<BLeaf> leaf; };  // boxed under a cap of 200
   PSIO_REFLECT(BoxCapRec, id, leaf)
   struct BoxOptRec { u32 id; std::optional<Boxed<BLeaf>> leaf; u8 tail; };
   PSIO_REFLECT(BoxOptRec, id, leaf, tail)
   struct BoxVecRec { u8 id; std::vector<Boxed<BLeaf>> leaves; };  // leaves <= 3
   PSIO_REFLECT(BoxVecRec, id, leaves)
   struct BoxVecCapRec { u8 id; std::vector<Boxed<BLeaf>> leaves; };  // cap 60000
   PSIO_REFLECT(BoxVecCapRec, id, leaves)
   struct BoxFixedRec { u8 id; Boxed<u32> word; Boxed<std::array<u8, 2>> pair; };
   PSIO_REFLECT(BoxFixedRec, id, word, pair)
   struct BoxOuter { u8 k; BoxCapRec capped; BoxRec open; std::optional<BDirect> direct; };
   PSIO_REFLECT(BoxOuter, k, capped, open, direct)

   // ── Nested values ─────────────────────────────────────────────────────
   // A nested<T> is a byte list holding the automatic-profile encoding of a
   // T (psio/nested.hpp): on the wire exactly list<u8>, and every checking
   // reader also validates the bytes as T. The deferred form has the same
   // bytes and schema, and its bytes are opaque to an enclosing reader.
   template <typename T> using Nested = psio::nested<T>;
   template <typename T>
   using NestedLater = psio::nested<T, psio::nested_validation::deferred>;
   struct NArgs { Raw to; u64 amount; std::optional<Raw> memo; };
   PSIO_REFLECT(NArgs, to, amount, memo)
   struct NArgs2 { Raw to; u64 amount; std::optional<Raw> memo; std::optional<u32> fee; };
   PSIO_REFLECT(NArgs2, to, amount, memo, fee)  // what a newer writer of NArgs sends
   struct NAction { u32 method; Nested<NArgs> args; };
   PSIO_REFLECT(NAction, method, args)
   struct NAction2 { u32 method; Nested<NArgs2> args; };  // the newer writer of NAction
   PSIO_REFLECT(NAction2, method, args)
   struct NActionRaw { u32 method; std::vector<u8> args; };  // what a forwarder declares
   PSIO_REFLECT(NActionRaw, method, args)
   struct NActionLater { u32 method; NestedLater<NArgs> args; };
   PSIO_REFLECT(NActionLater, method, args)
   struct NShapes
   {
      Nested<u32>                  scalar;
      Nested<Nested<NArgs>>        twice;
      std::optional<Nested<NArgs>> maybe;
      std::vector<Nested<NArgs>>   many;
   };
   PSIO_REFLECT(NShapes, scalar, twice, maybe, many)
   struct NCapped { u8 id; Nested<Raw> note; };  // note <= 8 bytes of encoding
   PSIO_REFLECT(NCapped, id, note)
   struct NChain { std::optional<Nested<NChain>> next; };  // depth across the boundary
   PSIO_REFLECT(NChain, next)

   // ── Zero-size and fixed records ───────────────────────────────────────
   struct Unit {};  // zero-field DWNC: encodes to zero bytes
   PSIO_REFLECT(Unit)
   struct Empty {};  // zero-field extensible: header only
   PSIO_REFLECT(Empty)
   struct Pair { u8 x; u8 y; };  // DWNC all-fixed
   PSIO_REFLECT(Pair, x, y)
   struct DwncVar { u8 k; Raw s; };  // DWNC with a variable field
   PSIO_REFLECT(DwncVar, k, s)

   // ── Optional presence: every presence class as a record field ─────────
   using VarBS = std::variant<u8, Raw>;
   using Bits12 = psio::bitlist<12>;
#define PSSZ_CONF_PRESENCE_FIELDS                    \
   std::optional<u32>                    o_u32;      \
   std::optional<Raw>                    o_str;      \
   std::optional<std::vector<u16>>       o_vec;      \
   std::optional<Unit>                   o_unit;     \
   std::optional<Empty>                  o_empty;    \
   std::optional<Pair>                   o_pair;     \
   std::optional<Inner>                  o_rec;      \
   std::optional<VarBS>                  o_var;      \
   std::optional<Bits12>                 o_bits;     \
   std::optional<std::array<Raw, 2>>     o_arr;      \
   std::optional<std::array<u32, 0>>     o_arr0;     \
   std::optional<std::optional<u32>>     o_oo;       \
   std::optional<std::optional<Raw>>     o_oos;      \
   std::optional<Color>                  o_enum;
   struct PresMid { u8 head; PSSZ_CONF_PRESENCE_FIELDS u8 tail; };
   PSIO_REFLECT(PresMid, head, o_u32, o_str, o_vec, o_unit, o_empty, o_pair, o_rec, o_var, o_bits,
                o_arr, o_arr0, o_oo, o_oos, o_enum, tail)
   struct PresTail { u8 head; PSSZ_CONF_PRESENCE_FIELDS };
   PSIO_REFLECT(PresTail, head, o_u32, o_str, o_vec, o_unit, o_empty, o_pair, o_rec, o_var,
                o_bits, o_arr, o_arr0, o_oo, o_oos, o_enum)
#undef PSSZ_CONF_PRESENCE_FIELDS

   // ── Type-derived widths (mirrors pssz_type_width_fixture.hpp) ─────────
   struct WidthCheckpoint { u32 height; Raw label; };  // cap 64, label <= 16
   PSIO_REFLECT(WidthCheckpoint, height, label)
   struct WidthRelayBatch
   {
      WidthCheckpoint                    checkpoint;
      std::vector<WidthCheckpoint>       entries;
      std::optional<WidthCheckpoint>     latest;
      std::array<WidthCheckpoint, 2>     pair;
      std::variant<WidthCheckpoint, u32> proof;
      Raw                                context;
   };
   PSIO_REFLECT(WidthRelayBatch, checkpoint, entries, latest, pair, proof, context)
   struct WidthLargeBudget { u8 value; };  // cap 1000000: larger than needed
   PSIO_REFLECT(WidthLargeBudget, value)
   struct WidthNarrowEnvelope { WidthLargeBudget child; u32 sequence; };  // cap 64
   PSIO_REFLECT(WidthNarrowEnvelope, child, sequence)
   struct WidthReserveParent { WidthLargeBudget child; };
   PSIO_REFLECT(WidthReserveParent, child)
   struct WidthGrowV1 { u32 id; };  // cap 512
   PSIO_REFLECT(WidthGrowV1, id)
   struct WidthGrowV2 { u32 id; std::optional<u32> time; };  // cap 512
   PSIO_REFLECT(WidthGrowV2, id, time)
   struct WidthGrowParentV1 { WidthGrowV1 child; };
   PSIO_REFLECT(WidthGrowParentV1, child)
   struct WidthGrowParentV2 { WidthGrowV2 child; };
   PSIO_REFLECT(WidthGrowParentV2, child)
   struct WidthThreshold251 { std::array<u8, 251> value; };  // bound 255
   PSIO_REFLECT(WidthThreshold251, value)
   struct WidthThreshold252 { std::array<u8, 252> value; };  // bound 256
   PSIO_REFLECT(WidthThreshold252, value)
   struct WidthBoundedBatch { std::vector<WidthCheckpoint> entries; };  // entries <= 4
   PSIO_REFLECT(WidthBoundedBatch, entries)
   struct WidthStringPair { Raw first; Raw second; };
   PSIO_REFLECT(WidthStringPair, first, second)
   struct WidthCappedPair { Raw first; Raw second; };  // cap 255
   PSIO_REFLECT(WidthCappedPair, first, second)

   // Inferred-bound thresholds driven by a bounded string (small values).
   struct S247 { Raw s; };  // bound 4 + 4 + 247 = 255
   PSIO_REFLECT(S247, s)
   struct S248 { Raw s; };  // 256
   PSIO_REFLECT(S248, s)
   struct S65527 { Raw s; };  // 65535
   PSIO_REFLECT(S65527, s)
   struct S65528 { Raw s; };  // 65536
   PSIO_REFLECT(S65528, s)

   // A bounded UTF-8 string member is what bounds these records.
   struct T16 { u32 a; psio::utf8_string<16> s; };  // bound 4 + 4 + 4 + 16 = 28
   PSIO_REFLECT(T16, a, s)
   struct T300 { u32 a; psio::utf8_string<300> s; };  // bound 312
   PSIO_REFLECT(T300, a, s)

   // A bounded vector of variable elements inside a narrow parent.
   struct Tiny { u8 v; };  // cap 16
   PSIO_REFLECT(Tiny, v)
   struct NarrowBatch { std::vector<Tiny> entries; };  // entries <= 2
   PSIO_REFLECT(NarrowBatch, entries)

   // Parent width P over child width C, the child in every position.
   template <int P, typename C>
   struct Mix
   {
      C                    child;
      std::vector<C>       kids;
      std::array<C, 2>     pair;
      std::optional<C>     maybe;
      std::variant<u32, C> alt;
   };
   using Mix1x1 = Mix<1, C1a>;
   using Mix1x2 = Mix<1, C2a>;
   using Mix1x4 = Mix<1, R0>;
   using Mix2x1 = Mix<2, C1a>;
   using Mix2x2 = Mix<2, C2a>;
   using Mix2x4 = Mix<2, R0>;
   using Mix4x1 = Mix<4, C1a>;
   using Mix4x2 = Mix<4, C2a>;
   using Mix4x4 = Mix<4, R0>;
   PSIO_REFLECT(Mix1x1, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix1x2, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix1x4, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix2x1, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix2x2, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix2x4, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix4x1, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix4x2, child, kids, pair, maybe, alt)
   PSIO_REFLECT(Mix4x4, child, kids, pair, maybe, alt)

   // ── Strings, bounds, bytes, enums, tuples, variants ───────────────────
   struct StrRec { Raw r; Text t; Text4 b; };
   PSIO_REFLECT(StrRec, r, t, b)
   struct Bounded { Raw s; std::vector<u16> v; };  // s <= 4 bytes, v <= 2 elements
   PSIO_REFLECT(Bounded, s, v)
   struct BytesRec
   {
      std::vector<u8>     b;
      std::array<u8, 4>   a;
      psio::bitvector<12> bv;
      Bits12              bl;
      psio::uint256       u;
      psio::uint128       x;
      psio::int128        y;
   };
   PSIO_REFLECT(BytesRec, b, a, bv, bl, u, x, y)
   struct EnumRec
   {
      Color c; E16 w; E32 i; E64 q; std::vector<Color> list; std::optional<E32> opt;
   };
   PSIO_REFLECT(EnumRec, c, w, i, q, list, opt)
   struct TupRec { TupBS t; u8 n; };
   PSIO_REFLECT(TupRec, t, n)
   using Var5 = std::variant<u32, Raw, Inner, TupBS, Unit>;
}  // namespace pssz_conf

// Annotations must be declared at namespace scope outside pssz_conf.
PSIO_TYPE_ATTRS(pssz_conf::Unit, psio::definition_will_not_change{})
PSIO_TYPE_ATTRS(pssz_conf::Pair, psio::definition_will_not_change{})
PSIO_TYPE_ATTRS(pssz_conf::DwncVar, psio::definition_will_not_change{})
PSIO_TYPE_ATTRS(pssz_conf::BoxCapRec, psio::max_dynamic_data_spec{200})
PSIO_FIELD_ATTRS(pssz_conf::BoxVecRec, leaves, psio::length_bound{.max = 3})
PSIO_TYPE_ATTRS(pssz_conf::BoxVecCapRec, psio::max_dynamic_data_spec{60000})
PSIO_TYPE_ATTRS(pssz_conf::C1a, psio::max_dynamic_data_spec{200})
PSIO_TYPE_ATTRS(pssz_conf::C1b, psio::max_dynamic_data_spec{200})
PSIO_TYPE_ATTRS(pssz_conf::C1c, psio::max_dynamic_data_spec{200})
PSIO_TYPE_ATTRS(pssz_conf::C2a, psio::max_dynamic_data_spec{1000})
PSIO_TYPE_ATTRS(pssz_conf::C2b, psio::max_dynamic_data_spec{1000})
PSIO_TYPE_ATTRS(pssz_conf::C2c, psio::max_dynamic_data_spec{1000})
PSIO_TYPE_ATTRS(pssz_conf::WidthCheckpoint, psio::max_dynamic_data_spec{64})
PSIO_FIELD_ATTRS(pssz_conf::WidthCheckpoint, label, psio::length_bound{.max = 16})
PSIO_TYPE_ATTRS(pssz_conf::WidthLargeBudget, psio::max_dynamic_data_spec{1000000})
PSIO_TYPE_ATTRS(pssz_conf::WidthNarrowEnvelope, psio::max_dynamic_data_spec{64})
PSIO_TYPE_ATTRS(pssz_conf::WidthGrowV1, psio::max_dynamic_data_spec{512})
PSIO_TYPE_ATTRS(pssz_conf::WidthGrowV2, psio::max_dynamic_data_spec{512})
PSIO_FIELD_ATTRS(pssz_conf::WidthBoundedBatch, entries, psio::length_bound{.max = 4})
PSIO_TYPE_ATTRS(pssz_conf::WidthCappedPair, psio::max_dynamic_data_spec{255})
PSIO_FIELD_ATTRS(pssz_conf::S247, s, psio::length_bound{.max = 247})
PSIO_FIELD_ATTRS(pssz_conf::S248, s, psio::length_bound{.max = 248})
PSIO_FIELD_ATTRS(pssz_conf::S65527, s, psio::length_bound{.max = 65527})
PSIO_FIELD_ATTRS(pssz_conf::S65528, s, psio::length_bound{.max = 65528})
PSIO_TYPE_ATTRS(pssz_conf::Tiny, psio::max_dynamic_data_spec{16})
PSIO_FIELD_ATTRS(pssz_conf::NarrowBatch, entries, psio::length_bound{.max = 2})
PSIO_TYPE_ATTRS(pssz_conf::Mix1x1, psio::max_dynamic_data_spec{250})
PSIO_TYPE_ATTRS(pssz_conf::Mix1x2, psio::max_dynamic_data_spec{250})
PSIO_TYPE_ATTRS(pssz_conf::Mix1x4, psio::max_dynamic_data_spec{250})
PSIO_TYPE_ATTRS(pssz_conf::Mix2x1, psio::max_dynamic_data_spec{60000})
PSIO_TYPE_ATTRS(pssz_conf::Mix2x2, psio::max_dynamic_data_spec{60000})
PSIO_TYPE_ATTRS(pssz_conf::Mix2x4, psio::max_dynamic_data_spec{60000})
PSIO_FIELD_ATTRS(pssz_conf::Bounded, s, psio::length_bound{.max = 4})
PSIO_FIELD_ATTRS(pssz_conf::Bounded, v, psio::length_bound{.max = 2})
PSIO_FIELD_ATTRS(pssz_conf::NCapped, note, psio::length_bound{.max = 8})

namespace pssz_conf
{
   // ── Enum metadata (names are part of the shape) ───────────────────────
   struct Enumerator
   {
      std::string_view name;
      long long        value;
   };
   template <typename E>
   struct enum_info;
   template <>
   struct enum_info<Color>
   {
      static constexpr std::string_view id = "Color";
      static constexpr Enumerator       items[]{{"Red", 0}, {"Green", 1}, {"Blue", 2}};
   };
   template <>
   struct enum_info<E16>
   {
      static constexpr std::string_view id = "E16";
      static constexpr Enumerator       items[]{{"Lo", 1}, {"Hi", 258}};
   };
   template <>
   struct enum_info<E32>
   {
      static constexpr std::string_view id = "E32";
      static constexpr Enumerator       items[]{{"Neg", -2}, {"Zero", 0}, {"Pos", 16909060}};
   };
   template <>
   struct enum_info<E64>
   {
      static constexpr std::string_view id = "E64";
      static constexpr Enumerator       items[]{{"Zero", 0}, {"Big", 72623859790382856ll}};
   };

   // ── Shape strings ─────────────────────────────────────────────────────
   namespace shape_detail
   {
      namespace impl = psio::detail::pssz_impl;

      template <typename T> struct is_bitvector : std::false_type {};
      template <std::size_t N> struct is_bitvector<psio::bitvector<N>> : std::true_type
      { static constexpr std::size_t n = N; };
      template <typename T> struct is_bitlist : std::false_type {};
      template <std::size_t N> struct is_bitlist<psio::bitlist<N>> : std::true_type
      { static constexpr std::size_t n = N; };

      template <typename T>
      inline constexpr bool is_named_v =
         std::is_enum_v<T> || (psio::Reflected<T> && !std::is_enum_v<T>);

      template <typename T> std::string ref();

      template <typename T>
      std::string primitive()
      {
         if constexpr (std::is_same_v<T, bool>) return "bool";
         else if constexpr (std::is_same_v<T, psio::uint256>) return "u256";
         else if constexpr (std::is_same_v<T, psio::uint128>) return "u128";
         else if constexpr (std::is_same_v<T, psio::int128>) return "i128";
         else if constexpr (std::is_same_v<T, float>) return "f32";
         else if constexpr (std::is_same_v<T, double>) return "f64";
         else if constexpr (std::is_integral_v<T>)
            return std::string(std::is_signed_v<T> ? "i" : "u") + std::to_string(8 * sizeof(T));
         else
         {
            static_assert(sizeof(T) == 0, "pssz_conf: type has no shape");
            return {};
         }
      }

      template <typename T>
      std::string structural()
      {
         if constexpr (std::is_same_v<T, std::string>) return "raw_string";
         else if constexpr (impl::is_utf8_string_v<T>)
            return T::is_bounded ? "string<=" + std::to_string(T::max_size_value)
                                 : std::string("string");
         else if constexpr (impl::is_std_vector_v<T>)
            return "list<" + ref<typename T::value_type>() + ">";
         else if constexpr (impl::is_std_array_v<T>)
            return "array<" + ref<typename T::value_type>() + "," +
                   std::to_string(std::tuple_size_v<T>) + ">";
         else if constexpr (impl::is_std_optional_v<T>)
            return "optional<" + ref<typename T::value_type>() + ">";
         else if constexpr (impl::is_std_variant_v<T>)
            return []<std::size_t... I>(std::index_sequence<I...>) {
               std::string out = "variant<";
               ((out += (I ? "," : "") + ref<std::variant_alternative_t<I, T>>()), ...);
               return out + ">";
            }(std::make_index_sequence<std::variant_size_v<T>>{});
         else if constexpr (impl::is_std_tuple_v<T>)
            return []<std::size_t... I>(std::index_sequence<I...>) {
               std::string out = "tuple<";
               ((out += (I ? "," : "") + ref<std::tuple_element_t<I, T>>()), ...);
               return out + ">";
            }(std::make_index_sequence<std::tuple_size_v<T>>{});
         else if constexpr (is_bitvector<T>::value)
            return "bitvector<" + std::to_string(is_bitvector<T>::n) + ">";
         else if constexpr (is_bitlist<T>::value)
            return "bitlist<" + std::to_string(is_bitlist<T>::n) + ">";
         else if constexpr (impl::is_psio_box_v<T>)
            return "box<" + ref<typename impl::is_psio_box<T>::elem>() + ">";
         else if constexpr (impl::is_nested_v<T>)
            return "nested<" + ref<typename impl::is_nested<T>::inner>() +
                   (impl::is_nested<T>::descends ? "" : ",deferred") + ">";
         else
            return primitive<T>();
      }

      template <typename T>
      std::string named_id()
      {
         if constexpr (std::is_enum_v<T>) return std::string(enum_info<T>::id);
         else return std::string(psio::reflect<T>::name);
      }

      // A type in nested position: named types by id, everything else inline.
      template <typename T>
      std::string ref()
      {
         if constexpr (is_named_v<T>) return named_id<T>();
         else return structural<T>();
      }

      // Member I of record T, with its length_bound annotation if any. Only
      // std::string and std::vector members may carry one.
      template <typename T, std::size_t I>
      std::string member()
      {
         using R = psio::reflect<T>;
         using F = typename R::template member_type<I>;
         using Annots = typename psio::effective_annotations_for<
            T, F, R::template member_pointer<I>>::value_t;
         std::string out = std::string(R::template member_name<I>) + ":" + ref<F>();
         if constexpr (psio::has_spec_v<psio::length_bound, Annots> &&
                       !impl::is_utf8_string_v<F>)
         {
            static_assert(std::is_same_v<F, std::string> || impl::is_std_vector_v<F> ||
                             impl::is_nested_v<F>,
                          "pssz_conf: length_bound only on string, vector and nested members");
            const auto bound = std::get<psio::length_bound>(
               psio::effective_annotations_for<T, F, R::template member_pointer<I>>::value);
            out += "<=" + std::to_string(*bound.max);
         }
         return out;
      }

      // The definition of a type: what `shape` holds in the fixture.
      template <typename T>
      std::string definition()
      {
         if constexpr (std::is_enum_v<T>)
         {
            std::string out = "enum<" + primitive<std::underlying_type_t<T>>() + ">{";
            bool        first = true;
            for (const auto& item : enum_info<T>::items)
            {
               out += (first ? "" : ",") + std::string(item.name) + "=" +
                      std::to_string(item.value);
               first = false;
            }
            return out + "}";
         }
         else if constexpr (psio::Reflected<T>)
         {
            return []<std::size_t... I>(std::index_sequence<I...>) {
               std::string out = psio::is_dwnc_v<T> ? "dwnc{" : "record{";
               ((out += (I ? "," : "") + member<T, I>()), ...);
               out += "}";
               if (psio::max_dynamic_data_v<T>.has_value())
                  out += "@cap=" + std::to_string(*psio::max_dynamic_data_v<T>);
               return out;
            }(std::make_index_sequence<psio::reflect<T>::member_count>{});
         }
         else
            return structural<T>();
      }
   }  // namespace shape_detail

   template <typename T>
   std::string type_id()
   {
      return shape_detail::ref<T>();
   }
   template <typename T>
   std::string type_shape()
   {
      return shape_detail::definition<T>();
   }

   // ── Registry ──────────────────────────────────────────────────────────
   template <typename... Ts>
   struct types
   {
   };
   template <typename... Ls>
   struct concat;
   template <typename... Ts>
   struct concat<types<Ts...>>
   {
      using type = types<Ts...>;
   };
   template <typename... As, typename... Bs, typename... Rest>
   struct concat<types<As...>, types<Bs...>, Rest...> : concat<types<As..., Bs...>, Rest...>
   {
   };

   template <typename T> using Vec  = std::vector<T>;
   template <typename T> using Opt  = std::optional<T>;
   template <typename T> using VarU = std::variant<u32, T>;
   template <typename T> using TupS = std::tuple<T, Raw>;
   template <typename T> using Arr2 = std::array<T, 2>;

   // The evolving record in a vector, an optional, a variant alternative, a
   // tuple and an array.
   template <typename T>
   using in_all = types<Vec<T>, Opt<T>, VarU<T>, TupS<T>, Arr2<T>>;
   template <typename T>
   using in_two = types<Vec<T>, Opt<T>>;

   using OptU32 = std::optional<u32>;
   using registry = concat<
      // scalars and enums
      types<bool, u8, u16, u32, u64, std::int8_t, std::int16_t, std::int32_t, std::int64_t,
            float, double, psio::uint128, psio::int128, psio::uint256, Scalars,
            Color, E16, E32, E64, EnumRec, Vec<Color>, Vec<E16>, Opt<Color>, Opt<E32>,
            std::variant<Color, E64>, Arr2<E32>>,
      // evolution lineages
      types<R0, R1, R2, R3, Inner, N1, N2, P1, E1, E2, E3, Bad1,
            Parent0, Parent1, Parent2, Parent3>,
      in_all<R0>, in_all<R1>, in_all<R2>, in_all<R3>,
      in_two<N1>, in_two<N2>, in_two<P1>, in_two<E1>, in_two<E2>, in_two<E3>,
      types<C1a, C1b, C1c, C2a, C2b, C2c, I0, I1>,
      in_two<C1a>, in_two<C1b>, in_two<C1c>, in_two<C2a>, in_two<C2b>, in_two<C2c>,
      in_two<I0>, in_two<I1>,
      types<Base, Extended, Twice, OddBase, OddExtended, OldOuter, NewOuter,
            DepthNode, Opt<DepthNode>>,
      // zero-size and fixed records, optional presence
      types<Unit, Empty, Pair, DwncVar,
            Vec<Unit>, Arr2<Unit>, Opt<Unit>, Vec<Empty>, Arr2<Empty>, Opt<Empty>,
            Vec<Pair>, Arr2<Pair>, Opt<Pair>, Vec<DwncVar>, Arr2<DwncVar>, Opt<DwncVar>,
            Opt<Inner>, Opt<Raw>, Opt<Vec<u16>>, VarBS, Opt<VarBS>, Opt<Bits12>,
            Arr2<Raw>, Opt<Arr2<Raw>>, std::array<u32, 0>, Opt<std::array<u32, 0>>,
            OptU32, Opt<OptU32>, Opt<Opt<Raw>>, PresMid, PresTail>,
      // widths
      types<WidthCheckpoint, WidthRelayBatch, WidthLargeBudget, WidthNarrowEnvelope,
            WidthReserveParent, WidthGrowV1, WidthGrowV2, WidthGrowParentV1, WidthGrowParentV2,
            WidthThreshold251, WidthThreshold252, WidthBoundedBatch, WidthStringPair,
            WidthCappedPair, S247, S248, S65527, S65528, T16, T300, Tiny, NarrowBatch,
            Mix1x1, Mix1x2, Mix1x4, Mix2x1, Mix2x2, Mix2x4, Mix4x1, Mix4x2, Mix4x4,
            Vec<WidthCheckpoint>, Opt<WidthCheckpoint>, std::variant<WidthCheckpoint, u32>,
            Arr2<WidthCheckpoint>, Vec<Tiny>, VarU<C1a>, TupS<C1a>>,
      // strings and bounds
      types<Raw, Text, Text4, StrRec, Vec<Raw>, Vec<Text>, Vec<Text4>, Opt<Text>, Opt<Text4>,
            Bounded>,
      // bytes
      types<Vec<u8>, std::array<u8, 4>, psio::bitvector<12>, Bits12, BytesRec, Vec<u32>,
            Arr2<bool>>,
      // boxed values
      types<BLeaf, BDirect, BoxRec, BoxCapRec, BoxOptRec, BoxVecRec, BoxVecCapRec, BoxFixedRec,
            BoxOuter, Boxed<BLeaf>, Vec<Boxed<BLeaf>>, Opt<Boxed<BLeaf>>, Vec<BoxRec>,
            Opt<BoxCapRec>, std::variant<Boxed<BLeaf>, u8>, std::tuple<Boxed<BLeaf>, u8>>,
      // tuples and variants
      types<std::tuple<>, std::tuple<u32>, std::tuple<Raw>, TupBS,
            std::tuple<u32, Raw, std::optional<u16>>,
            std::tuple<u8, std::optional<u32>, std::optional<Raw>>, TupRec, Vec<TupBS>,
            std::variant<std::tuple<u8, std::optional<u16>>, u32>, VarUS, Var5>,
      // nested values (appended: the ids above keep their positions)
      types<NArgs, NArgs2, NAction, NAction2, NActionRaw, NActionLater, NShapes, NCapped, NChain,
            Nested<u32>, Nested<NArgs>, NestedLater<NArgs>, Opt<Nested<Raw>>, Vec<Nested<u32>>,
            std::variant<u8, Nested<NArgs>>, std::tuple<Nested<Raw>, u8>, Opt<NChain>,
            Opt<Opt<NChain>>>>::type;

   template <typename F, typename... Ts>
   void each_type(types<Ts...>, F&& f)
   {
      (f(std::type_identity<Ts>{}), ...);
   }
   // Calls f(std::type_identity<T>{}) for every fixture type, in fixture order.
   template <typename F>
   void for_each_type(F&& f)
   {
      each_type(registry{}, std::forward<F>(f));
   }

   // ── Schema IR coverage ────────────────────────────────────────────────
   //
   // Types the Schema IR cannot describe faithfully; the fixture records
   // "ir": false for them and the schema-driven tests skip them.
   template <typename T>
   struct ir_inexpressible : std::false_type
   {
   };
   template <typename T>
   constexpr bool ir_expressible()
   {
      return !ir_inexpressible<T>::value;
   }

   // ── Shared helpers for the emitter and the consuming test ─────────────
   inline std::string to_hex(std::span<const char> bytes)
   {
      static constexpr char digits[] = "0123456789abcdef";
      std::string           out;
      out.reserve(bytes.size() * 2);
      for (char c : bytes)
      {
         const auto b = static_cast<unsigned char>(c);
         out.push_back(digits[b >> 4]);
         out.push_back(digits[b & 15]);
      }
      return out;
   }
   inline std::vector<char> from_hex(std::string_view hex)
   {
      auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
      std::vector<char> out;
      out.reserve(hex.size() / 2);
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
         out.push_back(static_cast<char>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
      return out;
   }

   template <typename F>
   constexpr std::string_view format_name()
   {
      if constexpr (std::is_same_v<F, psio::pssz>) return "auto";
      else if constexpr (std::is_same_v<F, psio::pssz8>) return "pssz8";
      else if constexpr (std::is_same_v<F, psio::pssz16>) return "pssz16";
      else return "pssz32";
   }

   // The unchecked `psio::decode` adopts the bytes of a nested value without
   // reading them, so for the nested classes it is asserted to throw for no
   // rejection at all.
   inline bool unchecked_decode_reads_all(std::string_view cls)
   {
      return !cls.starts_with("nested");
   }

   // Validation errors that the unchecked fail-fast `psio::decode` is also
   // guaranteed to raise. decode() does not re-run the validator: for other
   // malformed input (bad bool, variant index, offset tables, bit padding,
   // depth) it is not a validating entry point and must not be called on
   // untrusted bytes. Explicit-width decode does not check maxDynamicData.
   template <typename F>
   bool decode_also_rejects(std::string_view reason)
   {
      static constexpr std::string_view mirrored[]{
         "pssz: invalid record span",
         "pssz: record header truncated",
         "pssz: record fixed region truncated",
         "pssz: partial unknown optional slot",
         "pssz: invalid record payload offset",
         "pssz: header cuts a known field",
         "pssz: required record field missing",
         "pssz: fixed record has trailing payload",
         "pssz: trailing absent optional field was not pruned",
         "pssz: fixed primitive span mismatch",
         "pssz: optional<fixed> span mismatch",
         "pssz: zero-size fixed payload must be empty",
         "pssz: optional<var> missing selector",
         "pssz: None has trailing bytes",
         "pssz: optional selector not 0/1",
         "pssz: utf8_string exceeds its bound",
         "pssz: nonempty zero-size vector is unsupported",
         "pssz: vector span not a multiple of element size",
      };
      for (auto m : mirrored)
         if (m == reason) return true;
      if (reason.starts_with("utf8_spec:")) return true;
      if (std::is_same_v<F, psio::pssz> &&
          reason == "pssz: encoded size exceeds maxDynamicData cap")
         return true;
      return false;
   }
}  // namespace pssz_conf
