// psio::protobuf against the official implementation: the schema psio
// emits for tests/protobuf_shapes.hpp is compiled by protoc (the build
// does that), and this test exchanges bytes with the generated code.
//
//   psio → official   psio's bytes parse with no unknown field, and the
//                     official deterministic serialisation of what was
//                     parsed is the same bytes
//   official → psio   messages the official library builds (from text
//                     format) decode to the C++ value written here
//   differential      on corrupted bytes, what psio accepts the official
//                     parser accepts, as the same value

#include <psio/protobuf.hpp>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/unknown_field_set.h>

#include <absl/log/globals.h>

#include <catch.hpp>

#include <map>
#include <span>
#include <string>
#include <vector>

#include "emitted.pb.h"
#include "protobuf_random.hpp"
#include "protobuf_shapes.hpp"

namespace
{
   namespace gp = google::protobuf;

   // The official parser logs every string that is not UTF-8.
   const bool quiet = (absl::SetStderrThreshold(absl::LogSeverityAtLeast::kFatal), true);

   std::vector<char> official_bytes(const gp::Message& m)
   {
      std::string out;
      {
         gp::io::StringOutputStream s(&out);
         gp::io::CodedOutputStream  c(&s);
         c.SetSerializationDeterministic(true);
         m.SerializeWithCachedSizes(&c);
      }
      return {out.begin(), out.end()};
   }
   std::vector<char> official(const gp::Message& m)
   {
      (void)m.ByteSizeLong();
      return official_bytes(m);
   }

   bool has_unknown(const gp::Message& m)
   {
      const gp::Reflection* r = m.GetReflection();
      if (!r->GetUnknownFields(m).empty())
         return true;
      std::vector<const gp::FieldDescriptor*> fields;
      r->ListFields(m, &fields);
      for (const auto* f : fields)
      {
         if (f->cpp_type() != gp::FieldDescriptor::CPPTYPE_MESSAGE)
            continue;
         if (f->is_repeated())
         {
            for (int i = 0; i < r->FieldSize(m, f); ++i)
               if (has_unknown(r->GetRepeatedMessage(m, f, i)))
                  return true;
         }
         else if (has_unknown(r->GetMessage(m, f)))
            return true;
      }
      return false;
   }

   template <typename T, typename M>
   void exchange(std::uint64_t seed, unsigned count)
   {
      pb_rng r{seed};
      for (unsigned i = 0; i < count; ++i)
      {
         const T                 v = pb_random<T>(r);
         const std::vector<char> b = psio::to_protobuf(v);
         REQUIRE(psio::protobuf_size(v) == b.size());
         REQUIRE(psio::validate_protobuf<T>(std::span<const char>{b}).ok());
         M m;
         REQUIRE(m.ParseFromArray(b.data(), static_cast<int>(b.size())));
         REQUIRE(!has_unknown(m));
         const std::vector<char> o = official(m);
         REQUIRE(o == b);
         const T back = psio::from_protobuf<T>(std::span<const char>{o});
         REQUIRE(back == v);
      }
   }

   // The official message of a text, and psio's value of its bytes.
   template <typename T, typename M>
   T through_official(const char* text, const T& expected)
   {
      M m;
      REQUIRE(gp::TextFormat::ParseFromString(text, &m));
      const std::string       plain = m.SerializeAsString();
      const std::vector<char> o     = official(m);
      const T                 a     = psio::from_protobuf<T>(std::span<const char>{plain});
      const T                 b     = psio::from_protobuf<T>(std::span<const char>{o});
      CHECK(a == expected);
      CHECK(b == expected);
      CHECK(psio::to_protobuf(expected) == o);
      return b;
   }

   struct tally
   {
      std::uint64_t both = 0, neither = 0, official_only = 0;
      std::map<std::string, std::uint64_t> why;  // psio's reason for the official-only ones
   };

   std::vector<char> corrupt(const std::vector<char>& b, const std::vector<char>& other, pb_rng& r)
   {
      std::vector<char> c = b;
      const unsigned    edits = 1 + static_cast<unsigned>(r.below(3));
      for (unsigned e = 0; e < edits; ++e)
      {
         switch (r.below(7))
         {
            case 0:  // replace a byte
               if (!c.empty())
                  c[r.below(c.size())] = static_cast<char>(r.below(256));
               break;
            case 1:  // flip a bit
               if (!c.empty())
                  c[r.below(c.size())] ^= static_cast<char>(1u << r.below(8));
               break;
            case 2:  // truncate
               c.resize(r.below(c.size() + 1));
               break;
            case 3:  // insert a byte
               c.insert(c.begin() + static_cast<std::ptrdiff_t>(r.below(c.size() + 1)),
                        static_cast<char>(r.below(256)));
               break;
            case 4:  // remove a byte
               if (!c.empty())
                  c.erase(c.begin() + static_cast<std::ptrdiff_t>(r.below(c.size())));
               break;
            case 5:  // append another message's bytes (fields repeat: last wins, merge)
               c.insert(c.end(), other.begin(), other.end());
               break;
            default:  // a small tag and value somewhere
            {
               static const unsigned char fields[][4] = {
                  {0x08, 0x96, 0x01, 0},  {0x12, 0x01, 0x41, 0}, {0x1d, 1, 2, 3},
                  {0x0a, 0x00, 0, 0},     {0x2a, 0x02, 0x08, 0x01}, {0x10, 0xff, 0xff, 0x03}};
               const auto& f  = fields[r.below(6)];
               const auto  at = c.begin() + static_cast<std::ptrdiff_t>(r.below(c.size() + 1));
               c.insert(at, f, f + 3 + (f[3] ? 1 : 0));
            }
         }
      }
      return c;
   }

   template <typename T, typename M>
   void differential(std::uint64_t seed, unsigned values, unsigned corruptions, tally& t)
   {
      pb_rng            r{seed};
      std::vector<char> previous;
      for (unsigned i = 0; i < values; ++i)
      {
         const std::vector<char> b = psio::to_protobuf(pb_random<T>(r));
         for (unsigned k = 0; k < corruptions; ++k)
         {
            const std::vector<char> c  = corrupt(b, previous, r);
            const auto              st = psio::validate_protobuf<T>(std::span<const char>{c});
            M                       m;
            const bool off = m.ParseFromArray(c.data(), static_cast<int>(c.size()));
            if (st.ok())
            {
               REQUIRE(off);
               // The same value: psio's reading of the official
               // serialisation of what the official parser read.
               T v{};
               REQUIRE(psio::protobuf::decode_checked(std::span<const char>{c}, v).ok());
               m.DiscardUnknownFields();
               const std::vector<char> o = official(m);
               T                       w{};
               REQUIRE(psio::protobuf::decode_checked(std::span<const char>{o}, w).ok());
               REQUIRE(psio::to_protobuf(v) == psio::to_protobuf(w));
               ++t.both;
            }
            else if (off)
            {
               ++t.official_only;
               ++t.why[std::string{st.error().what}];
            }
            else
               ++t.neither;
         }
         previous = b;
      }
   }
}  // namespace

TEST_CASE("protobuf interop: psio's bytes are the official library's", "[protobuf][interop]")
{
   exchange<PbPoint, pbe::PbPoint>(1, 2000);
   exchange<PbScalars, pbe::PbScalars>(2, 4000);
   exchange<PbEncodings, pbe::PbEncodings>(3, 4000);
   exchange<PbText, pbe::PbText>(4, 3000);
   exchange<PbOptionals, pbe::PbOptionals>(5, 4000);
   exchange<PbRepeated, pbe::PbRepeated>(6, 3000);
   exchange<PbMaps, pbe::PbMaps>(7, 3000);
   exchange<PbChoice, pbe::PbChoice>(8, 4000);
   exchange<PbRenumbered, pbe::PbRenumbered>(9, 2000);
   exchange<PbTree, pbe::PbTree>(10, 2000);
   exchange<PbDefaults, pbe::PbDefaults>(11, 2000);
   exchange<PbEmpty, pbe::PbEmpty>(12, 3);
   exchange<PbOrder, pbe::PbOrder>(13, 1500);
}

TEST_CASE("protobuf interop: what the official library writes, psio reads", "[protobuf][interop]")
{
   {
      PbScalars e;
      e.b = true, e.i8 = -8, e.i16 = -1600, e.i32 = -32, e.i64 = -64, e.u8 = 200, e.u16 = 60000;
      e.u32 = 4000000000u, e.u64 = 18000000000000000000ull, e.f = 1.5f, e.d = -2.25, e.c = 'A';
      e.color = PbColor::blue, e.level = PbLevel::low, e.plain = PbPlain::big;
      through_official<PbScalars, pbe::PbScalars>(
         "b: true i8: -8 i16: -1600 i32: -32 i64: -64 u8: 200 u16: 60000 u32: 4000000000 "
         "u64: 18000000000000000000 f: 1.5 d: -2.25 c: 65 color: blue level: low plain: 4294967280",
         e);
      // An enum value the schema does not name (proto3: open).
      PbScalars open;
      open.color = static_cast<PbColor>(77);
      open.level = static_cast<PbLevel>(-123456);
      through_official<PbScalars, pbe::PbScalars>("color: 77 level: -123456", open);
   }
   {
      PbEncodings e;
      e.s32 = -3, e.s64 = -5000000000, e.f32 = -7, e.uf32 = 4000000000u, e.f64 = -9;
      e.uf64 = 18000000000000000000ull, e.s16 = -300;
      e.zs = {-1, 1, -2147483647 - 1, 2147483647};
      e.fs = {0, 1, 18000000000000000000ull};
      e.os = 0;
      e.sf = {-1, 0, 5};
      through_official<PbEncodings, pbe::PbEncodings>(
         "s32: -3 s64: -5000000000 f32: -7 uf32: 4000000000 f64: -9 uf64: 18000000000000000000 "
         "s16: -300 zs: [-1, 1, -2147483648, 2147483647] fs: [0, 1, 18000000000000000000] os: 0 "
         "sf: [-1, 0, 5]",
         e);
   }
   {
      PbText e;
      e.s = "h\xc3\xa9llo", e.bytes = {0, 255, 7}, e.chars = {'a', '\0', 'b'}, e.quad = {1, 2, 3, 4};
      through_official<PbText, pbe::PbText>(
         "s: \"h\\303\\251llo\" bytes: \"\\000\\377\\007\" chars: \"a\\000b\" quad: \"\\001\\002\\003\\004\"", e);
   }
   {
      PbOptionals e;
      e.i = 0, e.b = false, e.s = "", e.d = 0.0, e.p = PbPoint{}, e.c = PbColor::red;
      e.y = std::vector<std::uint8_t>{};
      // Present and zero: explicit presence.
      through_official<PbOptionals, pbe::PbOptionals>("i: 0 b: false s: \"\" d: 0 p {} c: red y: \"\"", e);
      through_official<PbOptionals, pbe::PbOptionals>("", PbOptionals{});
   }
   {
      PbRepeated e;
      e.ints = {1, -2, 0}, e.bools = {true, false, true}, e.doubles = {0.5}, e.floats = {-1, 0};
      e.strings = {"", "a"}, e.points = {{1, 2}, {}, {0, -1}};
      e.colors = {PbColor::green, static_cast<PbColor>(9)}, e.blobs = {{}, {1}};
      e.ids = {3, 5}, e.u64s = {18000000000000000000ull}, e.i64s = {-1};
      through_official<PbRepeated, pbe::PbRepeated>(
         "ints: [1, -2, 0] bools: [true, false, true] doubles: 0.5 floats: [-1, 0] "
         "strings: [\"\", \"a\"] points { x: 1 y: 2 } points {} points { y: -1 } "
         "colors: [green, 9] blobs: \"\" blobs: \"\\001\" ids: [3, 5] u64s: 18000000000000000000 i64s: -1",
         e);
   }
   {
      PbMaps e;
      e.counts = {{"", 0}, {"a", -1}}, e.names = {{-5, "m"}, {0, ""}}, e.points = {{7, {1, 2}}, {9, {}}};
      e.flags = {{false, 0.0}, {true, 1.5}}, e.colors = {{"k", PbColor::blue}}, e.blobs = {{-1, {1, 2}}};
      through_official<PbMaps, pbe::PbMaps>(
         "counts { key: \"a\" value: -1 } counts { key: \"\" value: 0 } "
         "names { key: 0 value: \"\" } names { key: -5 value: \"m\" } "
         "points { key: 9 value {} } points { key: 7 value { x: 1 y: 2 } } "
         "flags { key: true value: 1.5 } flags { key: false value: 0 } "
         "colors { key: \"k\" value: blue } blobs { key: -1 value: \"\\001\\002\" }",
         e);
   }
   {
      PbChoice e;
      e.before = 3, e.pick = std::string{"x"}, e.always = 2.5, e.between = 8, e.after = "z";
      through_official<PbChoice, pbe::PbChoice>(
         "before: 3 string: \"x\" double: 2.5 between: 8 after: \"z\"", e);
      PbChoice p;
      p.pick = PbPoint{0, 0}, p.always = false;
      // A set alternative that is zero is written: a oneof has presence.
      const PbChoice got = through_official<PbChoice, pbe::PbChoice>("PbPoint {} bool: false", p);
      CHECK(got.pick.index() == 3);
      PbChoice n;
      n.pick = std::int32_t{0};
      through_official<PbChoice, pbe::PbChoice>("int32: 0 bool: false", n);
   }
   {
      PbRenumbered e;
      e.a = 1, e.b = "b", e.c = 2, e.d = true, e.old = 9;
      through_official<PbRenumbered, pbe::PbRenumbered>("a: 1 b: \"b\" c: 2 d: true old: 9", e);
   }
   {
      PbOrder e;
      e.id = 7, e.origin = {1, 0}, e.scalars.i8 = -1, e.text.s = "t";
      e.choices.resize(2);
      e.choices[1].pick = std::int32_t{4};
      e.maps.counts = {{"n", 1}};
      e.tree = PbTree{"root", {PbTree{"leaf", {}, 5}, PbTree{}}, std::nullopt};
      e.lists["k"].ints = {1};
      e.lists[""];
      through_official<PbOrder, pbe::PbOrder>(
         "id: 7 origin { x: 1 } scalars { i8: -1 } text { s: \"t\" } "
         "choices { bool: false } choices { int32: 4 bool: false } maps { counts { key: \"n\" value: 1 } } "
         "tree { label: \"root\" children { label: \"leaf\" weight: 5 } children {} } "
         "lists { key: \"k\" value { ints: 1 } } lists { key: \"\" value {} }",
         e);
   }
}

TEST_CASE("protobuf interop: a member initialiser is not a default of the wire", "[protobuf][interop]")
{
   // PbDefaults{} holds 7 / true / 0.5 / blue / "anon"; an empty message is
   // all zero for the official library, and for psio.
   pbe::PbDefaults m;
   REQUIRE(m.ParseFromString(""));
   CHECK(m.n() == 0);
   const PbDefaults zero = psio::from_protobuf<PbDefaults>(std::span<const char>{});
   CHECK(zero.n == 0);
   CHECK(zero.on == false);
   CHECK(zero.ratio == 0.0);
   CHECK(zero.color == PbColor::red);
   CHECK(zero.name.empty());
   // And the initialisers are written: they are not zero.
   const auto b = psio::to_protobuf(PbDefaults{});
   REQUIRE(m.ParseFromArray(b.data(), static_cast<int>(b.size())));
   CHECK(m.n() == 7);
   CHECK(m.on());
   CHECK(m.ratio() == 0.5);
   CHECK(m.color() == pbe::blue);
   CHECK(m.name() == "anon");
}

TEST_CASE("protobuf interop: the nesting limit is the official one", "[protobuf][interop]")
{
   const auto nested = [](unsigned depth) {
      // children { children { … } }: field 2, length-delimited.
      std::vector<char> b;
      for (unsigned i = 0; i < depth; ++i)
      {
         std::vector<char> outer{0x12};
         std::size_t       n = b.size();
         while (n >= 0x80)
         {
            outer.push_back(static_cast<char>(n | 0x80));
            n >>= 7;
         }
         outer.push_back(static_cast<char>(n));
         outer.insert(outer.end(), b.begin(), b.end());
         b = std::move(outer);
      }
      return b;
   };
   for (unsigned depth : {1u, 50u, 99u, 100u, 101u, 150u})
   {
      const auto  b = nested(depth);
      pbe::PbTree m;
      const bool  off = m.ParseFromArray(b.data(), static_cast<int>(b.size()));
      const bool  ours = psio::validate_protobuf<PbTree>(std::span<const char>{b}).ok();
      INFO("depth " << depth << ": official " << off << ", psio " << ours);
      CHECK(ours == off);
   }
}

TEST_CASE("protobuf interop: differential fuzz of the two readers", "[protobuf][interop][fuzz]")
{
   tally t;
   differential<PbScalars, pbe::PbScalars>(21, 300, 100, t);
   differential<PbEncodings, pbe::PbEncodings>(22, 300, 100, t);
   differential<PbText, pbe::PbText>(23, 300, 100, t);
   differential<PbOptionals, pbe::PbOptionals>(24, 300, 100, t);
   differential<PbRepeated, pbe::PbRepeated>(25, 300, 100, t);
   differential<PbMaps, pbe::PbMaps>(26, 300, 100, t);
   differential<PbChoice, pbe::PbChoice>(27, 300, 100, t);
   differential<PbTree, pbe::PbTree>(28, 300, 100, t);
   differential<PbOrder, pbe::PbOrder>(29, 600, 100, t);
   std::string why;
   for (const auto& [reason, count] : t.why)
      why += "\n   " + std::to_string(count) + "  " + reason;
   WARN("accepted by both " << t.both << ", by neither " << t.neither
                            << ", by the official parser only " << t.official_only << why);
   // What psio refuses and the official parser accepts is what a C++ member
   // cannot hold (an int8 / int16 / narrow enum out of range, bytes for a
   // std::array of another length) …
   for (const auto& [reason, count] : t.why)
   {
      INFO(reason << ": " << count);
      CHECK((reason == "protobuf: a value does not fit its type" ||
             reason == "protobuf: bytes of a fixed size have another length" ||
             // … and two encodings no writer produces, which the official
             // C++ parser reads by dropping the excess bits:
             reason == "protobuf: varint exceeds 64 bits" ||
             reason == "protobuf: tag exceeds 32 bits"));
   }
   CHECK(t.both > 20000);
}
