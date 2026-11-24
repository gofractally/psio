// Cap'n Proto schemas against the official compiler.
//
//   import      the layout psio::capnp_layout computes from parse_capnp(text)
//               is the one in the CodeGeneratorRequest `capnp compile -o-`
//               writes for the same text: data and pointer section sizes,
//               every field's ordinal, offset, size and default, every
//               union's discriminant position and member values, groups;
//   round trip  emit_capnp(parse_capnp(text)) declares what `text` declares
//               (the compiler's own listing of the two, `-ocapnp`, has the
//               same lines), is a fixed point, and parses back to the same
//               IR;
//   export      for reflected C++ types the compiler accepts the emitted
//               schema and computes the layout of psio::capnp::layout<T>.
//
// On hand-written schemas (tests/capnp/*.capnp) and on generated ones.
// Messages are exchanged with the compiled schema in
// capnp_schema_interop_tests.cpp.

#include <psio/capnp.hpp>
#include <psio/capnp_layout.hpp>
#include <psio/capnp_parser.hpp>
#include <psio/emit_capnp.hpp>
#include <psio/schema_builder.hpp>

#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <catch.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "capnp_shapes.hpp"

namespace
{
   namespace fs = std::filesystem;
   using namespace psio::schema_types;

   std::string read_file(const fs::path& path)
   {
      std::ifstream     in(path, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      REQUIRE(in);
      return text.str();
   }

   struct scratch
   {
      fs::path dir;
      scratch()
      {
         dir = fs::temp_directory_path() /
               ("psio_capnp_schema_" + std::to_string(std::random_device{}()));
         fs::create_directories(dir);
      }
      ~scratch()
      {
         std::error_code ec;
         fs::remove_all(dir, ec);
      }
   };

   // What `capnp compile -o<output>` writes for `text`; fails the test with
   // the compiler's message when it refuses the text.
   std::string compile(const scratch& tmp, const std::string& text, const std::string& name,
                       const char* output)
   {
      const fs::path file = tmp.dir / (name + ".capnp");
      {
         std::ofstream out(file, std::ios::binary);
         out << text;
      }
      const fs::path    result  = tmp.dir / (name + ".out");
      const fs::path    log     = tmp.dir / (name + ".log");
      // CAPNP_IGNORE_ISSUE_344: for a few shapes (a union member that grows
      // inside a group's location) the compiler stops by default, because
      // Cap'n Proto 0.5 and older laid them out wrongly; with the variable
      // set it compiles them as it compiles everything else, which is the
      // layout psio computes.
      const std::string command = std::string{"CAPNP_IGNORE_ISSUE_344=1 "} + PSIO_CAPNP +
                                  " compile -I\"" + PSIO_CAPNP_INCLUDE +
                                  "\" --src-prefix=\"" + tmp.dir.string() + "\" -o" + output +
                                  " \"" + file.string() + "\" > \"" + result.string() + "\" 2> \"" +
                                  log.string() + "\"";
      if (std::system(command.c_str()) != 0)
         FAIL("capnp refuses:\n" << read_file(log) << "\n" << text);
      return read_file(result);
   }

   // ── The layout, as text: from the compiler's CodeGeneratorRequest … ────

   constexpr std::uint64_t json_name_annotation = 0xfa5b1fd61c2e7c3dull;

   struct request
   {
      kj::Array<capnp::word>                                     words;
      kj::Own<capnp::FlatArrayMessageReader>                     reader;
      std::map<std::uint64_t, capnp::schema::Node::Reader>       nodes;
      std::string                                                file;

      explicit request(const std::string& bytes)
      {
         REQUIRE(bytes.size() % sizeof(capnp::word) == 0);
         words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
         std::memcpy(words.begin(), bytes.data(), bytes.size());
         capnp::ReaderOptions options;
         options.traversalLimitInWords = 1ull << 30;
         reader = kj::heap<capnp::FlatArrayMessageReader>(words.asPtr(), options);
         const auto root = reader->getRoot<capnp::schema::CodeGeneratorRequest>();
         for (const auto node : root.getNodes())
            nodes.emplace(node.getId(), node);
         REQUIRE(root.getRequestedFiles().size() == 1);
         file = root.getRequestedFiles()[0].getFilename().cStr();
      }
   };

   std::string hex(std::uint64_t v)
   {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(v));
      return buffer;
   }

   // The name a member has in the IR: its JSON name when it has one.
   template <typename Reader>
   std::string ir_name(const Reader& r)
   {
      for (const auto a : r.getAnnotations())
         if (a.getId() == json_name_annotation)
            return a.getValue().getText().cStr();
      return r.getName().cStr();
   }

   void scope_text(const request& rq, capnp::schema::Node::Struct::Reader st, unsigned indent,
                   std::string& out)
   {
      const std::string pad(2 * indent, ' ');
      if (st.getDiscriminantCount() != 0)
         out += pad + "union of " + std::to_string(st.getDiscriminantCount()) + " at " +
                std::to_string(st.getDiscriminantOffset()) + "\n";
      std::vector<capnp::schema::Field::Reader> fields;
      for (const auto f : st.getFields())
         fields.push_back(f);
      std::sort(fields.begin(), fields.end(),
                [](const auto& a, const auto& b) { return a.getCodeOrder() < b.getCodeOrder(); });
      for (const auto& f : fields)
      {
         out += pad + ir_name(f);
         if (f.getOrdinal().isExplicit())
            out += " @" + std::to_string(f.getOrdinal().getExplicit());
         if (f.getDiscriminantValue() != capnp::schema::Field::NO_DISCRIMINANT)
            out += " tag " + std::to_string(f.getDiscriminantValue());
         if (f.isGroup())
         {
            out += " group\n";
            scope_text(rq, rq.nodes.at(f.getGroup().getTypeId()).getStruct(), indent + 1, out);
            continue;
         }
         const auto slot  = f.getSlot();
         const auto type  = slot.getType();
         const auto value = slot.getDefaultValue();
         using T          = capnp::schema::Type;
         int           lg   = -1;
         std::uint64_t bits = 0;
         switch (type.which())
         {
            case T::VOID: break;
            case T::BOOL: lg = 0, bits = value.getBool(); break;
            case T::INT8: lg = 3, bits = static_cast<std::uint8_t>(value.getInt8()); break;
            case T::UINT8: lg = 3, bits = value.getUint8(); break;
            case T::INT16: lg = 4, bits = static_cast<std::uint16_t>(value.getInt16()); break;
            case T::UINT16: lg = 4, bits = value.getUint16(); break;
            case T::ENUM: lg = 4, bits = value.getEnum(); break;
            case T::INT32: lg = 5, bits = static_cast<std::uint32_t>(value.getInt32()); break;
            case T::UINT32: lg = 5, bits = value.getUint32(); break;
            case T::FLOAT32: lg = 5, bits = std::bit_cast<std::uint32_t>(value.getFloat32()); break;
            case T::INT64: lg = 6, bits = static_cast<std::uint64_t>(value.getInt64()); break;
            case T::UINT64: lg = 6, bits = value.getUint64(); break;
            case T::FLOAT64: lg = 6, bits = std::bit_cast<std::uint64_t>(value.getFloat64()); break;
            default: lg = -2;
         }
         if (lg == -1)
            out += " void\n";
         else if (lg == -2)
            out += " pointer " + std::to_string(slot.getOffset()) + "\n";
         else
            out += " data " + std::to_string(1u << lg) + " bits at " +
                   std::to_string(slot.getOffset()) + " default " + hex(bits) + "\n";
      }
   }

   std::string layout_text(const request& rq)
   {
      std::map<std::string, std::string> structs;
      for (const auto& [id, node] : rq.nodes)
      {
         if (!node.isStruct() || node.getStruct().getIsGroup())
            continue;
         const std::string display = node.getDisplayName().cStr();
         if (!display.starts_with(rq.file + ":"))
            continue;  // (of an imported file)
         const std::string name = display.substr(rq.file.size() + 1);
         // (the parameter and result structs of methods are not declared)
         if (name.find('$') != std::string::npos)
            continue;
         const auto  st  = node.getStruct();
         std::string out = "struct " + name + ": " + std::to_string(st.getDataWordCount()) +
                           " words, " + std::to_string(st.getPointerCount()) + " pointers\n";
         scope_text(rq, st, 1, out);
         structs[name] = out;
      }
      std::string out;
      for (const auto& [name, text] : structs)
         out += text;
      return out;
   }

   // … and from psio's. `names`: with the members' names (the IR's).
   void scope_text(const capnp_scope_layout& scope, unsigned indent, bool names, std::string& out)
   {
      const std::string pad(2 * indent, ' ');
      if (scope.discriminant_count != 0)
         out += pad + "union of " + std::to_string(scope.discriminant_count) + " at " +
                std::to_string(scope.discriminant_offset) + "\n";
      for (const auto& f : scope.fields)
      {
         out += pad + (names ? f.name : std::string{"."});
         if (f.ordinal)
            out += " @" + std::to_string(*f.ordinal);
         if (f.discriminant)
            out += " tag " + std::to_string(*f.discriminant);
         switch (f.kind)
         {
            case capnp_slot_kind::group:
               out += " group\n";
               scope_text(f.group.at(0), indent + 1, names, out);
               break;
            case capnp_slot_kind::none:
               out += " void\n";
               break;
            case capnp_slot_kind::pointer:
               out += " pointer " + std::to_string(f.offset) + "\n";
               break;
            case capnp_slot_kind::data:
               out += " data " + std::to_string(1u << f.lg) + " bits at " +
                      std::to_string(f.offset) + " default " + hex(f.default_bits) + "\n";
               break;
         }
      }
   }

   std::string struct_text(const capnp_struct_layout& t, const std::string& name, bool names = true)
   {
      std::string out = "struct " + name + ": " + std::to_string(t.data_words) + " words, " +
                        std::to_string(t.pointers) + " pointers\n";
      scope_text(t, 1, names, out);
      return out;
   }

   std::string layout_text(const Schema& schema)
   {
      std::map<std::string, std::string> structs;
      for (const auto& t : psio::capnp_layout(schema).structs)
         structs[t.name] = struct_text(t, t.name);
      std::string out;
      for (const auto& [name, text] : structs)
         out += text;
      return out;
   }

   void same_text(const std::string& ours, const std::string& theirs, const char* what)
   {
      if (ours == theirs)
      {
         SUCCEED(what);
         return;
      }
      std::istringstream       a(ours), b(theirs);
      std::vector<std::string> la, lb;
      for (std::string line; std::getline(a, line);)
         la.push_back(line);
      for (std::string line; std::getline(b, line);)
         lb.push_back(line);
      std::string report;
      for (std::size_t i = 0; i < std::max(la.size(), lb.size()); ++i)
      {
         const std::string x = i < la.size() ? la[i] : "<end>";
         const std::string y = i < lb.size() ? lb[i] : "<end>";
         if (x != y)
         {
            report += "line " + std::to_string(i + 1) + "\n   psio : " + x + "\n   capnp: " + y + "\n";
            if (report.size() > 2500)
               break;
         }
      }
      FAIL(what << "\n" << report);
   }

   // The compiler's own listing of a schema, as a sorted set of lines (a
   // declaration kept as written may move within its scope), without the
   // line that names the file.
   std::string listing(const scratch& tmp, const std::string& text, const std::string& name)
   {
      std::istringstream       in(compile(tmp, text, name, "capnp"));
      std::vector<std::string> lines;
      for (std::string line; std::getline(in, line);)
         if (!line.starts_with("# "))
            lines.push_back(line);
      std::sort(lines.begin(), lines.end());
      std::string out;
      for (const auto& line : lines)
         out += line + "\n";
      return out;
   }

   void check_text(const std::string& text)
   {
      scratch       tmp;
      const request official{compile(tmp, text, "original", "-")};

      // Import.
      const Schema parsed = psio::parse_capnp(text);
      same_text(layout_text(parsed), layout_text(official), "the layout of the parsed text");

      // Round trip.
      const std::string emitted = psio::emit_capnp(parsed);
      INFO(emitted);
      same_text(listing(tmp, emitted, "emitted"), listing(tmp, text, "listed"),
                "the emitted text declares the same");
      const Schema reparsed = psio::parse_capnp(emitted);
      CHECK(reparsed == parsed);
      CHECK(psio::emit_capnp(reparsed) == emitted);
   }
}  // namespace

TEST_CASE("capnp schema: hand-written schemas against the compiler", "[capnp][schema][compiler]")
{
   const fs::path dir = PSIO_CAPNP_SCHEMA_DIR;
   SECTION("interop.capnp") { check_text(read_file(dir / "interop.capnp")); }
   SECTION("schema_all.capnp") { check_text(read_file(dir / "schema_all.capnp")); }
}

// ── Export: reflected C++ types ──────────────────────────────────────────

namespace
{
   // The layout psio::capnp_layout computes for IR type `name`, against the
   // compile-time layout psio::capnp uses for the C++ type T.
   template <typename T>
   void same_as_codec(const psio::capnp_layouts& layouts, const std::string& name)
   {
      INFO(name);
      const auto* t = layouts.find(name);
      REQUIRE(t != nullptr);
      using L = psio::capnp::layout<T>;
      using R = psio::reflect<T>;
      CHECK(t->data_words == L::data_words);
      CHECK(t->pointers == L::ptr_count);
      REQUIRE(t->fields.size() == R::member_count);
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (([&] {
             const auto& f    = t->fields[Is];
             const auto  slot = L::field(Is);
             INFO(f.name);
             CHECK(f.name == R::template member_name<Is>);
             using M = std::remove_cvref_t<typename R::template member_type<Is>>;
             if constexpr (psio::detail::capnp_impl::is_union_v<M>)
             {
                REQUIRE(f.kind == capnp_slot_kind::group);
                const auto& g = f.group.at(0);
                CHECK(g.discriminant_offset == slot.offset);
                for (std::size_t a = 0; a < g.fields.size(); ++a)
                {
                   const auto alt = L::alt(Is, a);
                   CHECK(static_cast<int>(g.fields[a].kind) == static_cast<int>(alt.kind));
                   CHECK(*g.fields[a].discriminant == a);
                   if (alt.kind != psio::detail::capnp_impl::slot_kind::none)
                   {
                      CHECK(g.fields[a].offset == alt.offset);
                      CHECK(g.fields[a].lg == alt.lg);
                   }
                }
             }
             else
             {
                CHECK(static_cast<int>(f.kind) == static_cast<int>(slot.kind));
                CHECK(f.offset == slot.offset);
                CHECK(f.lg == slot.lg);
             }
          }()),
          ...);
      }(std::make_index_sequence<R::member_count>{});
   }

#define CAPNP_SCHEMA_FIXTURE(X)                                                                    \
   X(S0) X(S1) X(S2) X(S3) X(S4) X(S5) X(S6) X(S7) X(S8) X(S9) X(S10) X(S11) X(S12) X(S13) X(S14)  \
   X(S15) X(S16) X(S17) X(S18) X(S19) X(S20) X(S21) X(S22) X(S23) X(S24) X(S25) X(S26) X(S27)      \
   X(S28) X(S29) X(S30) X(S31) X(S32) X(S33) X(S34) X(S35) X(S36) X(S37) X(S38) X(S39) X(S40)      \
   X(S41) X(S42) X(S43) X(S44) X(S45) X(S46) X(S47)
}  // namespace

TEST_CASE("capnp schema: the compiler lays the emitted schema of reflected types out as the codec does",
          "[capnp][schema][compiler]")
{
   // The types exchanged with libcapnp in capnp_schema_interop_tests.cpp,
   // and 48 records with unions of mixed sizes and shuffled field numbers
   // (capnp_layout_fixture.hpp).
   Schema reflected = capnp_shapes_schema();
   {
      psio::SchemaBuilder b;
      using namespace capnp_layout_fixture;
#define X(T) b.insert<T>(#T);
      CAPNP_SCHEMA_FIXTURE(X)
#undef X
      Schema more = b.build();
      // (two builders: the interned `@N` names of the second are its own)
      const std::string text = psio::emit_capnp(more);
      INFO(text);
      const auto layouts = psio::capnp_layout(more);
#define X(T) same_as_codec<T>(layouts, #T);
      CAPNP_SCHEMA_FIXTURE(X)
#undef X
      scratch       tmp;
      const request official{compile(tmp, text, "fixture", "-")};
      same_text(layout_text(more), layout_text(official), "the layout of the fixture records");
      const Schema parsed = psio::parse_capnp(text);
      same_text(layout_text(parsed), layout_text(official), "the layout of the text read back");
      CHECK(psio::emit_capnp(parsed) == text);
   }

   const std::string text = psio::emit_capnp(reflected);
   INFO(text);

   // The IR's layout is the codec's …
   const auto layouts = psio::capnp_layout(reflected);
   same_as_codec<CpProfile>(layouts, "CpProfile");
   same_as_codec<CpItem>(layouts, "CpItem");
   same_as_codec<CpOrder>(layouts, "CpOrder");
   same_as_codec<CpMixed>(layouts, "CpMixed");
   same_as_codec<CpRenumbered>(layouts, "CpRenumbered");
   same_as_codec<CpDefaults>(layouts, "CpDefaults");
   const auto* defaults = layouts.find("CpDefaults");
   CHECK(defaults->field("a")->default_bits == 0xfb);  // -5
   CHECK(defaults->field("b")->default_bits == ~std::uint64_t{0});
   CHECK(defaults->field("d")->default_bits == std::bit_cast<std::uint64_t>(-2.5));
   CHECK(defaults->field("f")->default_bits == std::bit_cast<std::uint32_t>(0.25f));
   CHECK(defaults->field("t")->default_bits == 1);
   CHECK(defaults->field("g")->default_bits == 1);
   CHECK(defaults->field("u")->default_bits == 65535);

   // … the compiler accepts the text and computes the same, member by
   // member under the members' IR names …
   scratch       tmp;
   const request official{compile(tmp, text, "shapes", "-")};
   same_text(layout_text(reflected), layout_text(official), "the layout of the emitted text");

   // … and the IR read back from the text has that layout too.
   const Schema parsed = psio::parse_capnp(text);
   same_text(layout_text(parsed), layout_text(official), "the layout of the text read back");
   const auto back = psio::capnp_layout(parsed);
   for (const auto& t : layouts.structs)
   {
      const auto* other = back.find(t.name);
      REQUIRE(other != nullptr);
      CHECK(struct_text(*other, t.name) == struct_text(t, t.name));
   }

   // The normal form: the text is a fixed point.
   CHECK(psio::emit_capnp(parsed) == text);
   CHECK(psio::parse_capnp(psio::emit_capnp(parsed)) == parsed);
}

// ── Generated schemas ────────────────────────────────────────────────────

namespace
{
   struct generator
   {
      std::mt19937_64 rng;
      Schema          s;
      std::vector<std::string>              enums;
      std::vector<std::size_t>              enum_sizes;
      std::vector<std::string>              structs;
      std::size_t                           next_name = 0;

      std::uint64_t pick(std::uint64_t n) { return rng() % n; }
      bool          chance(unsigned percent) { return pick(100) < percent; }

      static AnyType boolean() { return AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}}; }
      static AnyType text()
      {
         return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
      }
      static AnyType data()
      {
         return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "hex"}};
      }

      // A data type and (sometimes) a default for it.
      AnyType data_type(std::optional<std::string>& value)
      {
         const auto roll = pick(12);
         if (roll == 0)
         {
            if (chance(30))
               value = "true";
            return boolean();
         }
         if (roll <= 2)
         {
            static const char* values[] = {"1.5", "-0.25", "nan", "inf", "-inf", "1e+30", "3"};
            if (chance(30))
               value = values[pick(7)];
            return roll == 1 ? AnyType{Float{8, 24}} : AnyType{Float{11, 53}};
         }
         if (roll == 3 && !enums.empty())
         {
            const auto e = pick(enums.size());
            if (chance(30) && enum_sizes[e] > 1)
               value = std::to_string(1 + pick(enum_sizes[e] - 1));
            return AnyType{Type{enums[e]}};
         }
         static const std::uint32_t bits[] = {8, 16, 32, 64};
         const Int i{bits[pick(4)], chance(50)};
         if (chance(30))
         {
            if (i.isSigned)
            {
               const std::int64_t hi = i.bits == 64 ? std::numeric_limits<std::int64_t>::max()
                                                     : (std::int64_t{1} << (i.bits - 1)) - 1;
               value = chance(50) ? std::to_string(-hi - 1) : std::to_string(1 + pick(100));
            }
            else
               value = chance(30) ? std::to_string(i.bits == 64 ? ~std::uint64_t{0}
                                                                 : (std::uint64_t{1} << i.bits) - 1)
                                  : std::to_string(1 + pick(100));
         }
         return AnyType{i};
      }

      AnyType pointer_type()
      {
         switch (pick(7))
         {
            case 0: return text();
            case 1: return data();
            case 2: return AnyType{Type{structs[pick(structs.size())]}};
            case 3: return AnyType{Custom{Box<AnyType>{Tuple{}}, "AnyPointer"}};
            case 4:
            {
               std::optional<std::string> ignored;
               return AnyType{List{Box<AnyType>{data_type(ignored)}}};
            }
            case 5: return AnyType{List{Box<AnyType>{Type{structs[pick(structs.size())]}}}};
            default: return AnyType{List{Box<AnyType>{List{Box<AnyType>{text()}}}}};
         }
      }

      std::string name() { return "m" + std::to_string(next_name++); }

      // A field: Void (in a union), data or a pointer.
      Member field(bool in_union)
      {
         Member m;
         m.name = name();
         const auto roll = pick(10);
         if (in_union && roll == 0)
            *m.type = AnyType{Tuple{}};
         else if (roll < 7)
         {
            std::optional<std::string> value;
            *m.type = data_type(value);
            if (value)
               m.attributes.push_back({"default", *value});
         }
         else
            *m.type = pointer_type();
         return m;
      }

      std::vector<Member> members(unsigned depth, std::size_t count, bool top)
      {
         std::vector<Member> out;
         bool                unnamed = false;
         for (std::size_t i = 0; i < count; ++i)
         {
            const auto roll = pick(20);
            if (roll < 13 || depth >= 3)
               out.push_back(field(false));
            else if (roll < 17)
            {
               // A union: named, or (once per scope) the unnamed one.
               Member m;
               if (!unnamed && chance(35))
                  unnamed = true;
               else
                  m.name = name();
               Variant v;
               for (std::size_t a = 0, n = 2 + pick(4); a < n; ++a)
               {
                  if (chance(20))
                  {
                     Member g;
                     g.name  = name();
                     *g.type = AnyType{Object{members(depth + 1, 1 + pick(3), false), {}}};
                     v.members.push_back(std::move(g));
                  }
                  else
                     v.members.push_back(field(true));
               }
               *m.type = AnyType{std::move(v)};
               out.push_back(std::move(m));
            }
            else
            {
               Member g;
               g.name  = name();
               *g.type = AnyType{Object{members(depth + 1, 1 + pick(3), false), {}}};
               out.push_back(std::move(g));
            }
         }
         (void)top;
         return out;
      }

      // Every field of `members`, in declaration order; a union that is to
      // have its own ordinal right after its first member's.
      struct place
      {
         Member*  field = nullptr;
         Variant* own   = nullptr;
      };
      void collect(std::vector<Member>& members, std::vector<place>& out, bool allow_own)
      {
         for (auto& m : members)
         {
            if (auto* v = std::get_if<Variant>(&m.type->value))
            {
               const bool first_is_field = !std::holds_alternative<Object>(v->members[0].type->value);
               // (only a named union can have one)
               const bool own = allow_own && first_is_field && !m.name.empty() && chance(20);
               for (std::size_t a = 0; a < v->members.size(); ++a)
               {
                  if (auto* g = std::get_if<Object>(&v->members[a].type->value))
                     collect(g->members, out, allow_own);
                  else
                     out.push_back({&v->members[a], nullptr});
                  if (a == 0 && own)
                     out.push_back({nullptr, v});
               }
            }
            else if (auto* g = std::get_if<Object>(&m.type->value))
               collect(g->members, out, allow_own);
            else
               out.push_back({&m, nullptr});
         }
      }

      void make_struct(const std::string& struct_name)
      {
         Object o;
         o.members = members(0, 1 + pick(8), true);
         std::vector<place> places;
         const bool         shuffled = chance(40);
         collect(o.members, places, !shuffled);
         std::vector<std::size_t> ordinal(places.size());
         for (std::size_t i = 0; i < places.size(); ++i)
            ordinal[i] = i;
         if (shuffled)
            std::shuffle(ordinal.begin(), ordinal.end(), rng);
         for (std::size_t i = 0; i < places.size(); ++i)
         {
            const Attribute a{"ordinal", std::to_string(ordinal[i])};
            if (places[i].field)
               places[i].field->attributes.insert(places[i].field->attributes.begin(), a);
            else
               places[i].own->attributes.push_back(a);
         }
         for (auto& entry : s.types)
            if (entry.name == struct_name)
               *entry.type = AnyType{std::move(o)};
      }

      Schema run()
      {
         s.package.attributes.push_back({"capnp_id", "0xf00dfeedf00dfeed"});
         for (std::size_t i = 0, n = pick(3); i < n; ++i)
         {
            Enum e;
            e.type = Box<AnyType>{AnyType{Int{16, false}}};
            const std::size_t count = 1 + pick(5);
            for (std::size_t v = 0; v < count; ++v)
               e.members.push_back({"v" + std::to_string(v), v});
            enums.push_back("E" + std::to_string(i));
            enum_sizes.push_back(count);
            s.types.push_back(Member{enums.back(), Box<AnyType>{AnyType{std::move(e)}}, {}});
         }
         const std::size_t count = 1 + pick(4);
         for (std::size_t i = 0; i < count; ++i)
         {
            structs.push_back("S" + std::to_string(i));
            s.types.push_back(Member{structs.back(), Box<AnyType>{AnyType{Object{}}}, {}});
         }
         for (const auto& name : structs)
            make_struct(name);
         return std::move(s);
      }
   };
}  // namespace

TEST_CASE("capnp schema: generated schemas against the compiler", "[capnp][schema][compiler]")
{
   scratch tmp;
   for (std::uint64_t seed = 1; seed <= 300; ++seed)
   {
      generator g;
      g.rng.seed(seed);
      const Schema      schema = g.run();
      const std::string text   = psio::emit_capnp(schema);
      INFO("seed " << seed << "\n" << text);

      // Written from the normal form, read back as it.
      const Schema parsed = psio::parse_capnp(text);
      REQUIRE(parsed == schema);
      REQUIRE(psio::emit_capnp(parsed) == text);

      // The layout psio computes is the compiler's: groups in unions,
      // unions in groups in unions, ordinals in any order, a union's own
      // ordinal.
      const request official{compile(tmp, text, "g" + std::to_string(seed), "-")};
      same_text(layout_text(schema), layout_text(official), "the layout of a generated schema");
   }
}
