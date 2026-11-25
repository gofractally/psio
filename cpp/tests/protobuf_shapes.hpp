#pragma once
// The record types the protobuf tests share: every shape psio::protobuf
// maps (docs/protobuf.md "The mapping"). protobuf_schema_emit.cpp writes
// psio::emit_protobuf of them; protoc compiles that, and
// protobuf_interop_tests.cpp exchanges bytes with the generated code.
//
// At global scope: an annotation specializes a psio template, which C++
// only allows from an enclosing namespace.

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

enum class PbColor : std::uint8_t
{
   red,
   green,
   blue
};

// No zero value: the emitted enum gets one (what an absent field reads as).
enum class PbLevel : std::int32_t
{
   low  = -1,
   mid  = 5,
   high = 100
};

PSIO_REFLECT_ENUM(PbColor, red, green, blue)
PSIO_REFLECT_ENUM(PbLevel, low, mid, high)

// Without PSIO_REFLECT_ENUM: its integer.
enum class PbPlain : std::uint32_t
{
   none = 0,
   big  = 0xfffffff0u
};

struct PbPoint
{
   std::int32_t x = 0, y = 0;
   friend bool  operator==(const PbPoint&, const PbPoint&) = default;
   friend auto  operator<=>(const PbPoint&, const PbPoint&) = default;
};
PSIO_REFLECT(PbPoint, x, y)

struct PbScalars
{
   bool          b   = false;
   std::int8_t   i8  = 0;
   std::int16_t  i16 = 0;
   std::int32_t  i32 = 0;
   std::int64_t  i64 = 0;
   std::uint8_t  u8  = 0;
   std::uint16_t u16 = 0;
   std::uint32_t u32 = 0;
   std::uint64_t u64 = 0;
   float         f   = 0;
   double        d   = 0;
   char          c   = 0;
   PbColor       color{};
   PbLevel       level{};
   PbPlain       plain{};
   friend bool   operator==(const PbScalars&, const PbScalars&) = default;
};
PSIO_REFLECT(PbScalars, b, i8, i16, i32, i64, u8, u16, u32, u64, f, d, c, color, level, plain)

struct PbEncodings
{
   std::int32_t               s32  = 0;
   std::int64_t               s64  = 0;
   std::int32_t               f32  = 0;
   std::uint32_t              uf32 = 0;
   std::int64_t               f64  = 0;
   std::uint64_t              uf64 = 0;
   std::int16_t               s16  = 0;
   std::vector<std::int32_t>  zs;
   std::vector<std::uint64_t> fs;
   std::optional<std::int64_t> os;
   std::vector<std::int32_t>  sf;
   friend bool operator==(const PbEncodings&, const PbEncodings&) = default;
};
PSIO_REFLECT(PbEncodings,
             attr(s32, pb_sint),
             attr(s64, pb_sint),
             attr(f32, pb_fixed),
             attr(uf32, pb_fixed),
             attr(f64, pb_fixed),
             attr(uf64, pb_fixed),
             attr(s16, pb_sint),
             attr(zs, pb_sint),
             attr(fs, pb_fixed),
             attr(os, pb_sint),
             attr(sf, pb_fixed))

struct PbText
{
   std::string                 s;
   std::vector<std::uint8_t>   bytes;
   std::vector<char>           chars;
   std::array<std::uint8_t, 4> quad{};
   friend bool operator==(const PbText&, const PbText&) = default;
};
PSIO_REFLECT(PbText, s, bytes, chars, quad)

struct PbOptionals
{
   std::optional<std::int32_t>              i;
   std::optional<bool>                      b;
   std::optional<std::string>               s;
   std::optional<double>                    d;
   std::optional<PbPoint>                   p;
   std::optional<PbColor>                   c;
   std::optional<std::vector<std::uint8_t>> y;
   friend bool operator==(const PbOptionals&, const PbOptionals&) = default;
};
PSIO_REFLECT(PbOptionals, i, b, s, d, p, c, y)

struct PbRepeated
{
   std::vector<std::int32_t>              ints;
   std::vector<bool>                      bools;
   std::vector<double>                    doubles;
   std::vector<float>                     floats;
   std::vector<std::string>               strings;
   std::vector<PbPoint>                   points;
   std::vector<PbColor>                   colors;
   std::vector<std::vector<std::uint8_t>> blobs;
   std::set<std::uint32_t>                ids;
   std::vector<std::uint64_t>             u64s;
   std::vector<std::int64_t>              i64s;
   friend bool operator==(const PbRepeated&, const PbRepeated&) = default;
};
PSIO_REFLECT(PbRepeated, ints, bools, doubles, floats, strings, points, colors, blobs, ids, u64s, i64s)

struct PbMaps
{
   std::map<std::string, std::int32_t>               counts;
   std::map<std::int32_t, std::string>               names;
   std::map<std::uint64_t, PbPoint>                  points;
   std::map<bool, double>                            flags;
   std::map<std::string, PbColor>                    colors;
   std::map<std::int64_t, std::vector<std::uint8_t>> blobs;
   friend bool operator==(const PbMaps&, const PbMaps&) = default;
};
PSIO_REFLECT(PbMaps, counts, names, points, flags, colors, blobs)

// A oneof that may be unset (std::monostate), one that is always set, and
// plain fields between and after them: numbers 1, 5..7, 10..11, 8, 20.
struct PbChoice
{
   std::int32_t                                                  before = 0;
   std::variant<std::monostate, std::int32_t, std::string, PbPoint> pick;
   std::variant<bool, double>                                    always;
   std::uint32_t                                                 between = 0;
   std::string                                                   after;
   friend bool operator==(const PbChoice&, const PbChoice&) = default;
};
PSIO_REFLECT(PbChoice,
             before,
             attr(pick, field<5>),
             attr(always, field<10>),
             attr(between, field<8>),
             attr(after, field<20>))

struct PbRenumbered
{
   std::int32_t  a = 0;
   std::string   b;
   std::uint64_t c = 0;
   bool          d = false;
   std::int32_t  old = 0;
   friend bool   operator==(const PbRenumbered&, const PbRenumbered&) = default;
};
PSIO_REFLECT(PbRenumbered,
             attr(a, field<7>),
             attr(b, field<3>),
             attr(c, field<1000>),
             attr(d, field<16>),
             attr(old, field<2> | deprecated_spec{.version = "2"}))

struct PbTree
{
   std::string                 label;
   std::vector<PbTree>         children;
   std::optional<std::int32_t> weight;
   friend bool operator==(const PbTree&, const PbTree&) = default;
};
PSIO_REFLECT(PbTree, label, children, weight)

// Member initialisers that are not zero: no part of the wire (proto3).
struct PbDefaults
{
   std::int32_t n     = 7;
   bool         on    = true;
   double       ratio = 0.5;
   PbColor      color = PbColor::blue;
   std::string  name  = "anon";
   friend bool  operator==(const PbDefaults&, const PbDefaults&) = default;
};
PSIO_REFLECT(PbDefaults, n, on, ratio, color, name)

struct PbEmpty
{
   friend bool operator==(const PbEmpty&, const PbEmpty&) = default;
};
PSIO_REFLECT(PbEmpty)

struct PbOrder
{
   std::uint64_t            id = 0;
   PbPoint                  origin;
   PbScalars                scalars;
   PbText                   text;
   std::vector<PbChoice>    choices;
   PbMaps                   maps;
   std::optional<PbTree>    tree;
   PbEmpty                  nothing;
   std::map<std::string, PbRepeated> lists;
   friend bool operator==(const PbOrder&, const PbOrder&) = default;
};
PSIO_REFLECT(PbOrder, id, origin, scalars, text, choices, maps, tree, nothing, lists)

// A record whose declaration constrains its values (docs/annotations.md): a
// validating read path refuses a message that holds a value outside it, in
// protobuf as in every other format. (At most one of an order spec and a
// bound below the member on one member: for a value that violates both,
// which of the two is reported is not the same in every language.)
struct PbSpan
{
   std::uint32_t             lo = 0, hi = 0;
   friend bool               operator==(const PbSpan&, const PbSpan&) = default;
   static psio::codec_status ordered(const PbSpan& s) noexcept
   {
      return s.lo <= s.hi ? psio::codec_ok()
                          : psio::codec_fail("PbSpan: lo is greater than hi", 0, "PbSpan");
   }
   PSIO_REFLECT(PbSpan, lo, hi, invariant(ordered))
};

struct PbSlot
{
   std::string   code;
   std::uint32_t qty = 0;
   friend bool   operator==(const PbSlot&, const PbSlot&) = default;
};
PSIO_REFLECT(PbSlot, attr(code, max<4>), qty)

struct PbBounded
{
   std::vector<std::uint32_t>         ids;
   std::string                        name;
   std::vector<std::string>           tags;
   std::map<std::string, std::string> attrs;
   std::vector<std::uint32_t>         ranks;
   std::vector<std::uint32_t>         codes;
   std::vector<PbSlot>                slots;
   PbSpan                             span;
   std::optional<PbSlot>              spare;
   friend bool                        operator==(const PbBounded&, const PbBounded&) = default;
};
PSIO_REFLECT(PbBounded,
             attr(ids, max<2>),
             attr(name, max<8>),
             attr(tags, max<3> | element(max<4>)),
             attr(attrs, max<2> | keys(max<3>) | values(max<5>)),
             attr(ranks, sorted),
             attr(codes, unique),
             slots,
             span,
             spare)

// The same members without the declarations: what a writer that does not
// know them writes. The bytes of a value are the same for both.
struct PbLooseSpan
{
   std::uint32_t lo = 0, hi = 0;
};
PSIO_REFLECT(PbLooseSpan, lo, hi)

struct PbLooseSlot
{
   std::string   code;
   std::uint32_t qty = 0;
};
PSIO_REFLECT(PbLooseSlot, code, qty)

struct PbLoose
{
   std::vector<std::uint32_t>         ids;
   std::string                        name;
   std::vector<std::string>           tags;
   std::map<std::string, std::string> attrs;
   std::vector<std::uint32_t>         ranks;
   std::vector<std::uint32_t>         codes;
   std::vector<PbLooseSlot>           slots;
   PbLooseSpan                        span;
   std::optional<PbLooseSlot>         spare;
};
PSIO_REFLECT(PbLoose, ids, name, tags, attrs, ranks, codes, slots, span, spare)

// A value at every one of its limits.
inline PbBounded pb_bounded_at_limits()
{
   return PbBounded{{7, 9},
                    "eightchr",
                    {"a", "bb", "cccc"},
                    {{"k1", "v"}, {"key", "value"}},
                    {1, 1, 5},
                    {2, 3, 8},
                    {{"slot", 1}, {"", 2}},
                    {3, 3},
                    PbSlot{"sp", 4}};
}
inline PbLoose pb_loose_at_limits()
{
   return PbLoose{{7, 9},
                  "eightchr",
                  {"a", "bb", "cccc"},
                  {{"k1", "v"}, {"key", "value"}},
                  {1, 1, 5},
                  {2, 3, 8},
                  {{"slot", 1}, {"", 2}},
                  {3, 3},
                  PbLooseSlot{"sp", 4}};
}

inline psio::Schema protobuf_shapes_schema()
{
   psio::SchemaBuilder b;
   b.insert<PbOrder>("PbOrder");
   b.insert<PbPoint>("PbPoint");
   b.insert<PbScalars>("PbScalars");
   b.insert<PbEncodings>("PbEncodings");
   b.insert<PbText>("PbText");
   b.insert<PbOptionals>("PbOptionals");
   b.insert<PbRepeated>("PbRepeated");
   b.insert<PbMaps>("PbMaps");
   b.insert<PbChoice>("PbChoice");
   b.insert<PbRenumbered>("PbRenumbered");
   b.insert<PbTree>("PbTree");
   b.insert<PbDefaults>("PbDefaults");
   b.insert<PbEmpty>("PbEmpty");
   auto s         = b.build();
   s.package.name = "pbe";
   return s;
}
