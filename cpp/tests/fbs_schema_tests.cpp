// FlatBuffers schemas against the official compiler.
//
//   import      the layout psio::fbs_layout computes from parse_fbs(text) is
//               the one in the binary schema `flatc --binary --schema`
//               writes for the same text: field ids, vtable and struct
//               offsets, struct size and alignment, defaults, optional /
//               required / key / deprecated, enum types and values, union
//               discriminants, root type, file identifier;
//   round trip  emit_fbs(parse_fbs(text)) declares what `text` declares
//               (flatc's binary schemas agree on everything but comments
//               and written-out ids), is a fixed point, and parses back to
//               the same IR;
//   export      for reflected C++ types, flatc accepts the emitted schema
//               and computes the layout of psio::flatbuf::layout<T>.
//
// On hand-written schemas (tests/flatbuf/*.fbs) and on generated ones.
// Buffers are exchanged with the generated code in
// flatbuf_schema_interop_tests.cpp.

#include <psio/emit_fbs.hpp>
#include <psio/fbs_layout.hpp>
#include <psio/fbs_parser.hpp>
#include <psio/flatbuf.hpp>
#include <psio/schema_builder.hpp>

#include <flatbuffers/reflection.h>

#include <catch.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuf_schema_shapes.hpp"

namespace
{
   namespace fs = std::filesystem;

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
               ("psio_fbs_schema_" + std::to_string(std::random_device{}()));
         fs::create_directories(dir);
      }
      ~scratch()
      {
         std::error_code ec;
         fs::remove_all(dir, ec);
      }
   };

   // The binary schema flatc writes for `text`; fails the test with flatc's
   // message when it refuses the text.
   std::string flatc_schema(const scratch& tmp, const std::string& text,
                            const std::string& name = "schema")
   {
      const fs::path file = tmp.dir / (name + ".fbs");
      {
         std::ofstream out(file, std::ios::binary);
         out << text;
      }
      const fs::path    log     = tmp.dir / (name + ".log");
      const std::string command = std::string{PSIO_FLATC} + " --binary --schema -o \"" +
                                  tmp.dir.string() + "\" \"" + file.string() + "\" > \"" +
                                  log.string() + "\" 2>&1";
      if (std::system(command.c_str()) != 0)
         FAIL("flatc refuses:\n" << read_file(log) << "\n" << text);
      return read_file(tmp.dir / (name + ".bfbs"));
   }

   const reflection::Schema& schema_of(const std::string& bfbs)
   {
      flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bfbs.data()),
                                     bfbs.size());
      REQUIRE(reflection::VerifySchemaBuffer(verifier));
      return *reflection::GetSchema(bfbs.data());
   }

   std::string number(double v)
   {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%a", v);
      return v != v ? "nan" : buffer;
   }

   std::uint32_t scalar_size(reflection::BaseType t)
   {
      using namespace reflection;
      switch (t)
      {
         case Bool: case Byte: case UByte: case UType: return 1;
         case Short: case UShort: return 2;
         case Int: case UInt: case Float: return 4;
         case Long: case ULong: case Double: return 8;
         default: return 0;
      }
   }

   // ── The layout, as text: from flatc's binary schema … ──────────────────

   // Whether a declaration is of the file itself (a binary schema also
   // holds the declarations of the files it includes).
   template <typename Declaration>
   bool own(const reflection::Schema& s, const Declaration& d)
   {
      if (!d.declaration_file() || !s.fbs_files() || s.fbs_files()->size() == 0)
         return true;
      // The file compiled is the one that includes and is not included.
      for (const auto* file : *s.fbs_files())
         if (file->included_filenames())
            for (const auto* included : *file->included_filenames())
               if (included->str() == d.declaration_file()->str())
                  return false;
      return true;
   }

   std::string layout_text(const reflection::Schema& s)
   {
      std::map<std::string, std::string> lines;  // by name
      for (const auto* e : *s.enums())
      {
         if (!own(s, *e))
            continue;
         std::ostringstream out;
         const auto         base = e->underlying_type()->base_type();
         const bool is_signed = base == reflection::Byte || base == reflection::Short ||
                                base == reflection::Int || base == reflection::Long;
         out << (e->is_union() ? "union " : "enum ") << e->name()->str() << " size "
             << scalar_size(base) << (is_signed && !e->is_union() ? " signed" : "") << " :";
         for (const auto* v : *e->values())
            out << " " << v->name()->str() << "=" << v->value();
         lines[e->name()->str()] = out.str();
      }
      for (const auto* o : *s.objects())
      {
         if (!own(s, *o))
            continue;
         std::ostringstream out;
         if (o->is_struct())
            out << "struct " << o->name()->str() << " size " << o->bytesize() << " align "
                << o->minalign();
         else
            out << "table " << o->name()->str();
         // In id order (structs: declaration order is id order).
         std::map<int, const reflection::Field*> fields;
         for (const auto* f : *o->fields())
            fields[f->id()] = f;
         std::uint32_t slots = 0;
         for (const auto& [id, f] : fields)
         {
            const auto base = f->type()->base_type();
            const auto elem = f->type()->element();
            slots           = id + 1;
            // The type half of a union (or of a vector of unions): the
            // value half reports both.
            if (base == reflection::UType || (base == reflection::Vector && elem == reflection::UType))
               continue;
            const bool is_union = base == reflection::Union ||
                                  (base == reflection::Vector && elem == reflection::Union);
            out << "\n  " << f->name()->str();
            if (o->is_struct())
               out << " at " << f->offset();
            else
            {
               out << " slot " << (is_union ? id - 1 : id) << " vt "
                   << (is_union ? f->offset() - 2 : f->offset());
               if (is_union)
                  out << " union";
               if (scalar_size(base) != 0)
               {
                  out << " size " << scalar_size(base);
                  // (a binary schema holds the default of a `float` as the
                  // double the text reads as; the field's default is that
                  // rounded to a float)
                  if (base == reflection::Float)
                     out << " default " << number(static_cast<float>(f->default_real()));
                  else if (base == reflection::Double)
                     out << " default " << number(f->default_real());
                  else
                     out << " default " << f->default_integer();
               }
               if (f->optional() && scalar_size(base) != 0)
                  out << " optional";
               if (f->required())
                  out << " required";
               if (f->key())
                  out << " key";
            }
            if (f->deprecated())
               out << " deprecated";
         }
         if (!o->is_struct())
            out << "\n  slots " << slots;
         lines[o->name()->str()] = out.str();
      }
      std::string out;
      for (const auto& [name, line] : lines)
         out += line + "\n";
      out += "root " + (s.root_table() ? s.root_table()->name()->str() : std::string{}) + "\n";
      out += "identifier " + (s.file_ident() ? s.file_ident()->str() : std::string{}) + "\n";
      return out;
   }

   // … and from psio's. `full` gives an IR name its FlatBuffers name.
   std::string full_name(const psio::Schema& schema, const std::string& ir)
   {
      if (ir.find('.') != std::string::npos || schema.package.name.empty())
         return ir;
      std::string ns = schema.package.name;
      for (char& c : ns)
         if (c == ':')
            c = '.';
      return ns + "." + ir;
   }

   std::string layout_text(const psio::Schema& schema)
   {
      const auto                         layouts = psio::fbs_layout(schema);
      std::map<std::string, std::string> lines;
      psio::schema_types::fbs_layout_detail::model types{schema};
      for (const auto& e : layouts.enums)
      {
         std::ostringstream out;
         out << (e.is_union ? "union " : "enum ") << full_name(schema, e.name) << " size "
             << e.size << (e.is_signed ? " signed" : "") << " :";
         for (const auto& v : e.values)
            out << " " << v.name << "=" << v.value;
         lines[full_name(schema, e.name)] = out.str();
      }
      for (const auto& t : layouts.types)
      {
         std::ostringstream out;
         if (t.is_struct)
            out << "struct " << full_name(schema, t.name) << " size " << t.size << " align "
                << t.align;
         else
            out << "table " << full_name(schema, t.name);
         std::map<std::uint32_t, const psio::schema_types::fbs_field_layout*> fields;
         std::uint32_t                                                         n = 0;
         for (const auto& f : t.fields)
            fields[t.is_struct ? n++ : f.slot] = &f;
         const psio::AnyType* node = schema.get(t.name);
         for (const auto& [id, f] : fields)
         {
            out << "\n  " << f->name;
            if (t.is_struct)
               out << " at " << f->offset;
            else
            {
               out << " slot " << f->slot << " vt " << f->vtable_offset;
               if (f->is_union)
                  out << " union";
               // Whether the member is a scalar (the layout's size alone
               // does not say: an offset is 4 bytes too).
               const psio::Member* member = nullptr;
               if (const auto* o = std::get_if<psio::schema_types::Object>(&node->value))
                  for (const auto& m : o->members)
                     if (m.name == f->name)
                        member = &m;
               if (const auto* st = std::get_if<psio::schema_types::Struct>(&node->value))
                  for (const auto& m : st->members)
                     if (m.name == f->name)
                        member = &m;
               REQUIRE(member != nullptr);
               const auto shape = types.shape_of(*member->type);
               if (shape.scalar())
               {
                  out << " size " << f->size;
                  if (shape.k == psio::schema_types::fbs_layout_detail::kind::real)
                     out << " default " << number(f->default_real);
                  // (flatc's binary schema holds a default as a signed
                  // 64-bit number and writes 0 for one above its range)
                  else if (!shape.is_signed && shape.size == 8 && f->default_integer < 0)
                     out << " default 0";
                  else
                     out << " default " << f->default_integer;
               }
               if (f->optional)
                  out << " optional";
               if (f->required)
                  out << " required";
               if (f->key)
                  out << " key";
            }
            if (f->deprecated)
               out << " deprecated";
         }
         if (!t.is_struct)
            out << "\n  slots " << t.slots;
         lines[full_name(schema, t.name)] = out.str();
      }
      std::string out;
      for (const auto& [name, line] : lines)
         out += line + "\n";
      out += "root " +
             (layouts.root_type.empty() ? std::string{} : full_name(schema, layouts.root_type)) +
             "\n";
      out += "identifier " + layouts.file_identifier + "\n";
      return out;
   }

   // ── Everything a binary schema says, as text ───────────────────────────
   // (but comments, and the `id` attribute, which says what the field's id
   // already does)

   std::string type_text(const reflection::Schema& s, const reflection::Type& t)
   {
      const auto named = [&](reflection::BaseType base, int index) -> std::string {
         if (index < 0)
            return reflection::EnumNameBaseType(base);
         if (base == reflection::Obj)
            return s.objects()->Get(index)->name()->str();
         return std::string{reflection::EnumNameBaseType(base)} + ":" +
                s.enums()->Get(index)->name()->str();
      };
      if (t.base_type() == reflection::Vector)
         return "[" + named(t.element(), t.index()) + "]";
      if (t.base_type() == reflection::Array)
         return "[" + named(t.element(), t.index()) + ":" + std::to_string(t.fixed_length()) + "]";
      return named(t.base_type(), t.index());
   }

   template <typename Attributes>
   std::string attributes_text(const Attributes* attrs)
   {
      std::string out;
      if (attrs)
         for (const auto* kv : *attrs)
            if (kv->key()->str() != "id")
               out += " (" + kv->key()->str() + ":" + (kv->value() ? kv->value()->str() : "") + ")";
      return out;
   }

   std::string full_text(const reflection::Schema& s)
   {
      std::string out = layout_text(s);
      for (const auto* e : *s.enums())
      {
         if (!own(s, *e))
            continue;
         out += "enum " + e->name()->str() + attributes_text(e->attributes());
         for (const auto* v : *e->values())
            if (v->union_type() && v->union_type()->index() >= 0)
               out += " " + v->name()->str() + ":" + type_text(s, *v->union_type());
         out += "\n";
      }
      for (const auto* o : *s.objects())
      {
         if (!own(s, *o))
            continue;
         out += "object " + o->name()->str() + attributes_text(o->attributes()) + "\n";
         for (const auto* f : *o->fields())
            out += "  " + f->name()->str() + " " + type_text(s, *f->type()) +
                   attributes_text(f->attributes()) + "\n";
      }
      if (s.services())
         for (const auto* service : *s.services())
         {
            out += "service " + service->name()->str() + "\n";
            for (const auto* call : *service->calls())
               out += "  " + call->name()->str() + " " + call->request()->name()->str() + " " +
                      call->response()->name()->str() + attributes_text(call->attributes()) + "\n";
         }
      out += "extension " + (s.file_ext() ? s.file_ext()->str() : std::string{}) + "\n";
      return out;
   }

   // Requires two descriptions to be equal; reports the lines that are not.
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
            report += "line " + std::to_string(i + 1) + "\n   psio : " + x + "\n   flatc: " + y + "\n";
            if (report.size() > 2000)
               break;
         }
      }
      FAIL(what << "\n" << report);
   }

   // The three properties of a schema text.
   void check_text(const std::string& text, const fs::path& include_from = {})
   {
      scratch tmp;
      if (!include_from.empty())
         for (const auto& entry : fs::directory_iterator(include_from))
            if (entry.path().extension() == ".fbs")
               fs::copy_file(entry.path(), tmp.dir / entry.path().filename());
      const std::string official = flatc_schema(tmp, text, "original");

      // Import.
      const psio::Schema parsed = psio::parse_fbs(text);
      same_text(layout_text(parsed), layout_text(schema_of(official)), "the layout of the parsed text");

      // Round trip.
      const std::string emitted = psio::emit_fbs(parsed);
      INFO(emitted);
      const std::string again = flatc_schema(tmp, emitted, "emitted");
      same_text(full_text(schema_of(again)), full_text(schema_of(official)), "the emitted text declares the same");
      const psio::Schema reparsed = psio::parse_fbs(emitted);
      CHECK(reparsed == parsed);
      CHECK(psio::emit_fbs(reparsed) == emitted);
   }
}  // namespace

TEST_CASE("fbs schema: hand-written schemas against flatc", "[fbs][schema][flatc]")
{
   const fs::path dir = PSIO_FLATBUF_SCHEMA_DIR;
   SECTION("interop.fbs") { check_text(read_file(dir / "interop.fbs")); }
   SECTION("schema_all.fbs") { check_text(read_file(dir / "schema_all.fbs"), dir); }
   SECTION("monster.fbs") { check_text(read_file(dir / "monster.fbs")); }
}

TEST_CASE("fbs schema: what parse_fbs keeps of schema_all.fbs", "[fbs][schema]")
{
   const fs::path dir     = PSIO_FLATBUF_SCHEMA_DIR;
   const auto     schema  = psio::parse_fbs(read_file(dir / "schema_all.fbs"));
   const auto     layouts = psio::fbs_layout(schema);

   CHECK(schema.package.name == "all:types");
   CHECK(layouts.root_type == "Everything");
   CHECK(layouts.file_identifier == "ALL1");

   const auto* level = layouts.find_enum("Level");
   REQUIRE(level != nullptr);
   CHECK(level->size == 2);
   CHECK(level->is_signed);
   CHECK(level->value("low")->value == -2);
   CHECK(level->value("high")->value == 16);
   CHECK(level->value("top")->value == 17);
   CHECK(layouts.find_enum("Flags")->value("admin")->value == 0x8000);
   CHECK(layouts.find_enum("Wide")->value("big")->value == -1);  // all 64 bits

   const auto* aliased = layouts.find_enum("Aliased");
   REQUIRE(aliased != nullptr);
   CHECK(aliased->is_union);
   CHECK(aliased->value("NONE")->value == 0);
   CHECK(aliased->value("second")->value == 2);
   CHECK(aliased->value("Wire")->value == 7);
   CHECK(aliased->value("other_space_Far")->value == 8);

   const auto* aligned = layouts.find("Aligned");
   REQUIRE(aligned != nullptr);
   CHECK(aligned->is_struct);
   CHECK(aligned->align == 16);
   CHECK(aligned->size == 32);

   const auto* defaults = layouts.find("Defaults");
   REQUIRE(defaults != nullptr);
   CHECK(defaults->field("i8")->default_integer == -5);
   CHECK(defaults->field("u64")->default_integer == -1);
   CHECK(defaults->field("f")->default_real == 1.5);
   CHECK(defaults->field("d")->default_real == -2.25e-3);
   CHECK(defaults->field("not_a_number")->default_real != defaults->field("not_a_number")->default_real);
   CHECK(defaults->field("on")->default_integer == 1);
   CHECK(defaults->field("color")->default_integer == 2);
   CHECK(defaults->field("level")->default_integer == 17);
   CHECK(defaults->field("flags")->default_integer == 2);
   CHECK(defaults->field("maybe_i")->optional);
   CHECK_FALSE(defaults->field("zero")->optional);

   const auto* everything = layouts.find("Everything");
   REQUIRE(everything != nullptr);
   CHECK(everything->field("old")->deprecated);
   CHECK(everything->field("many")->is_union);  // a vector of unions: two slots
   CHECK(layouts.find("Card")->field("number")->required);
   CHECK(layouts.find("Entry")->field("key")->key);

   // Ids out of declaration order.
   const auto* renumbered = layouts.find("Renumbered");
   CHECK(renumbered->field("a")->slot == 0);
   CHECK(renumbered->field("c")->slot == 2);
   CHECK(renumbered->field("u")->slot == 3);  // the type; the value is (id: 4)
   CHECK(renumbered->field("last")->slot == 5);
   CHECK(renumbered->slots == 8);

   // A type of another namespace is named in full.
   CHECK(schema.get("other.space.Far") != nullptr);
   CHECK(layouts.find("other.space.Far")->field("value")->default_integer == 7);

   // Metadata psio gives no meaning to is kept.
   const auto& members =
      std::get<psio::schema_types::Object>(schema.get("Everything")->value).members;
   bool found = false;
   for (const auto& m : members)
      if (m.name == "score")
      {
         found = true;
         REQUIRE(m.attributes.size() == 2);
         CHECK(m.attributes[0] == psio::Attribute{"priority", "3"});
         CHECK(m.attributes[1] == psio::Attribute{"audit", std::nullopt});
      }
   CHECK(found);

   REQUIRE(schema.interfaces.size() == 1);
   CHECK(schema.interfaces[0].name == "other.space.Ledger");
   REQUIRE(schema.interfaces[0].funcs.size() == 2);
   CHECK(schema.interfaces[0].funcs[1].attributes[0] == psio::Attribute{"streaming", "server"});
}

// ── Export: reflected C++ types ──────────────────────────────────────────

namespace
{
   // The layout psio::fbs_layout computes for IR type `name`, against the
   // compile-time layout psio::flatbuf uses for the C++ type T.
   template <typename T>
   void same_as_codec(const psio::fbs_layouts& layouts, const std::string& name)
   {
      INFO(name);
      const auto* t = layouts.find(name);
      REQUIRE(t != nullptr);
      using L = psio::flatbuf::layout<T>;
      using R = psio::reflect<T>;
      REQUIRE(t->is_struct == L::is_struct);
      REQUIRE(t->fields.size() == R::member_count);
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (([&] {
             const auto* f = t->field(R::template member_name<Is>);
             INFO(R::template member_name<Is>);
             REQUIRE(f != nullptr);
             if constexpr (L::is_struct)
                CHECK(f->offset == L::offset(Is));
             else
             {
                CHECK(f->slot == L::slot(Is));
                CHECK(f->vtable_offset == L::vtable_offset(Is));
             }
          }()),
          ...);
      }(std::make_index_sequence<R::member_count>{});
      if constexpr (L::is_struct)
      {
         CHECK(t->size == L::size());
         CHECK(t->align == L::align());
      }
      else
         CHECK(t->slots == L::slots());
   }
}  // namespace

TEST_CASE("fbs schema: flatc lays the emitted schema of reflected types out as the codec does",
          "[fbs][schema][flatc]")
{
   const psio::Schema reflected = flatbuf_shapes_schema();
   const std::string  text      = psio::emit_fbs(reflected);
   INFO(text);

   // The IR's layout is the codec's …
   const auto layouts = psio::fbs_layout(reflected);
   same_as_codec<Vec3>(layouts, "Vec3");
   same_as_codec<Mixed>(layouts, "Mixed");
   same_as_codec<Point>(layouts, "Point");
   same_as_codec<Profile>(layouts, "Profile");
   same_as_codec<Item>(layouts, "Item");
   same_as_codec<Card>(layouts, "Card");
   same_as_codec<Order>(layouts, "Order");
   same_as_codec<Tree>(layouts, "Tree");
   same_as_codec<Scalars>(layouts, "Scalars");
   same_as_codec<Defaults>(layouts, "Defaults");
   same_as_codec<Renumbered>(layouts, "Renumbered");
   same_as_codec<WithDeprecated>(layouts, "WithDeprecated");
   same_as_codec<Sparse>(layouts, "Sparse");
   same_as_codec<Basket>(layouts, "Basket");
   CHECK(layouts.find("Defaults")->field("level")->default_integer == 7);
   CHECK(layouts.find("Defaults")->field("on")->default_integer == 1);
   CHECK(layouts.find("Defaults")->field("c")->default_integer == 1);
   CHECK(layouts.find("Defaults")->field("ratio")->default_real == 0.5);
   CHECK(layouts.find("Item")->field("qty")->default_integer == 1);
   CHECK(layouts.find("Order")->field("discount")->optional);
   CHECK(layouts.find("WithDeprecated")->field("old")->deprecated);

   // … flatc accepts the text and computes, for every record, the same …
   scratch            tmp;
   const std::string  official = flatc_schema(tmp, text);
   const psio::Schema parsed   = psio::parse_fbs(text);
   same_text(layout_text(parsed), layout_text(schema_of(official)), "the layout of the emitted text");

   // … and the IR read back from the text has the layout of the IR the
   // text was written from, record by record (the unions, enums and entry
   // tables the text had to name are the only other declarations).
   const auto back = psio::fbs_layout(parsed);
   for (const auto& t : layouts.types)
   {
      INFO(t.name);
      const auto* other = back.find(t.name);
      REQUIRE(other != nullptr);
      CHECK(other->is_struct == t.is_struct);
      CHECK(other->slots == t.slots);
      CHECK(other->size == t.size);
      CHECK(other->align == t.align);
      REQUIRE(other->fields.size() == t.fields.size());
      for (std::size_t i = 0; i < t.fields.size(); ++i)
      {
         const auto& a = t.fields[i];
         const auto& b = other->fields[i];
         INFO(a.name);
         CHECK(a.name == b.name);
         CHECK(a.slot == b.slot);
         CHECK(a.offset == b.offset);
         CHECK(a.size == b.size);
         CHECK(a.align == b.align);
         CHECK(a.is_union == b.is_union);
         CHECK(a.deprecated == b.deprecated);
         CHECK(a.optional == b.optional);
         CHECK(a.default_integer == b.default_integer);
         CHECK(a.default_real == b.default_real);
      }
   }
   CHECK(back.root_type == "Order");
   CHECK(back.file_identifier == "ORDR");

   // The normal form: the text is a fixed point.
   CHECK(psio::emit_fbs(parsed) == text);
   CHECK(psio::parse_fbs(psio::emit_fbs(parsed)) == parsed);
}

// ── Generated schemas ────────────────────────────────────────────────────

namespace
{
   using namespace psio::schema_types;

   struct generator
   {
      std::mt19937_64 rng;
      Schema          s;

      struct enum_info
      {
         std::string                name;
         bool                       has_zero = false;
         bool                       flags    = false;
         bool                       is_signed = false;
         std::vector<std::uint64_t> values;
      };
      std::vector<enum_info>   enums;
      std::vector<std::string> structs, tables, unions;

      std::uint64_t pick(std::uint64_t n) { return rng() % n; }
      bool          chance(unsigned percent) { return pick(100) < percent; }

      static AnyType boolean() { return AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}}; }
      static AnyType string()
      {
         return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
      }

      AnyType integer()
      {
         static const std::uint32_t bits[] = {8, 16, 32, 64};
         return AnyType{Int{bits[pick(4)], chance(50)}};
      }
      AnyType scalar()
      {
         switch (pick(4))
         {
            case 0: return boolean();
            case 1: return AnyType{Float{8, 24}};
            case 2: return AnyType{Float{11, 53}};
            default: return integer();
         }
      }

      void make_enum(std::size_t n)
      {
         enum_info info;
         info.name  = "E" + std::to_string(n);
         info.flags = chance(20);
         Int under  = std::get<Int>(integer().value);
         if (info.flags)
            under.isSigned = false;
         info.is_signed = under.isSigned;
         Enum e;
         e.type = Box<AnyType>{AnyType{under}};
         std::vector<Attribute> attrs;
         if (info.flags)
         {
            attrs.push_back({"bit_flags", std::nullopt});
            std::uint32_t bit = 0;
            for (std::size_t i = 0, count = 1 + pick(4); i < count && bit < under.bits; ++i)
            {
               e.members.push_back({"v" + std::to_string(i), std::uint64_t{1} << bit});
               bit += 1 + static_cast<std::uint32_t>(pick(3));
            }
         }
         else
         {
            const std::int64_t lo = under.isSigned ? -(std::int64_t{1} << (under.bits - 1)) : 0;
            const std::int64_t hi =
               under.bits == 64 ? std::numeric_limits<std::int64_t>::max()
                                : (under.isSigned ? (std::int64_t{1} << (under.bits - 1)) - 1
                                                  : (std::int64_t{1} << under.bits) - 1);
            std::int64_t v = chance(60) ? 0 : (under.isSigned && chance(50) ? -3 : 2);
            v              = std::max(v, lo);
            for (std::size_t i = 0, count = 1 + pick(5); i < count && v <= hi; ++i)
            {
               e.members.push_back({"v" + std::to_string(i), static_cast<std::uint64_t>(v)});
               if (v == 0)
                  info.has_zero = true;
               const std::int64_t step = 1 + (chance(30) ? static_cast<std::int64_t>(pick(5)) : 0);
               if (v > hi - step)
                  break;
               v += step;
            }
         }
         for (const auto& m : e.members)
            info.values.push_back(m.value);
         s.types.push_back(Member{info.name, Box<AnyType>{AnyType{std::move(e)}}, std::move(attrs)});
         enums.push_back(std::move(info));
      }

      // A type a struct may hold.
      AnyType fixed(std::size_t structs_so_far, bool allow_array = true)
      {
         const auto roll = pick(10);
         if (roll < 5 || (roll < 7 && enums.empty()) || (roll >= 7 && roll < 9 && structs_so_far == 0))
            return scalar();
         if (roll < 7)
         {
            // (flatc: an enum in a struct has a zero)
            const auto& e = enums[pick(enums.size())];
            return e.has_zero || e.flags ? AnyType{Type{e.name}} : scalar();
         }
         if (roll < 9)
            return AnyType{Type{structs[pick(structs_so_far)]}};
         if (!allow_array)
            return scalar();
         return AnyType{Array{Box<AnyType>{fixed(structs_so_far, false)}, 1 + pick(5)}};
      }

      void make_struct(std::size_t n)
      {
         Struct st;
         for (std::size_t i = 0, count = 1 + pick(6); i < count; ++i)
            st.members.push_back(
               Member{"m" + std::to_string(i), Box<AnyType>{fixed(structs.size())}, {}});
         if (chance(15))
            // (16: never below the struct's own alignment)
            st.attributes.push_back({"force_align", "16"});
         const std::string name = "S" + std::to_string(n);
         s.types.push_back(Member{name, Box<AnyType>{AnyType{std::move(st)}}, {}});
         structs.push_back(name);
      }

      void make_union(std::size_t n)
      {
         Variant      v;
         std::int64_t value = 0;
         for (std::size_t i = 0, count = 1 + pick(4); i < count; ++i)
         {
            const std::string& table = tables[pick(tables.size())];
            Member             m;
            m.name  = "a" + std::to_string(i);
            *m.type = AnyType{Type{table}};
            ++value;
            if (chance(15))
            {
               value += 1 + static_cast<std::int64_t>(pick(3));
               m.attributes.push_back({"value", std::to_string(value)});
            }
            v.members.push_back(std::move(m));
         }
         const std::string name = "U" + std::to_string(n);
         s.types.push_back(Member{name, Box<AnyType>{AnyType{std::move(v)}}, {}});
         unions.push_back(name);
      }

      std::string scalar_default(const AnyType& type)
      {
         if (const auto* i = std::get_if<Int>(&type.value))
         {
            if (i->isSigned)
            {
               const std::int64_t hi = i->bits == 64 ? std::numeric_limits<std::int64_t>::max()
                                                      : (std::int64_t{1} << (i->bits - 1)) - 1;
               switch (pick(4))
               {
                  case 0: return std::to_string(hi);
                  case 1: return std::to_string(-hi - 1);
                  case 2: return "-1";
                  default: return std::to_string(1 + pick(100));
               }
            }
            const std::uint64_t hi = i->bits == 64 ? ~std::uint64_t{0}
                                                    : (std::uint64_t{1} << i->bits) - 1;
            return std::to_string(chance(30) ? hi : 1 + pick(100));
         }
         if (std::holds_alternative<Float>(type.value))
         {
            static const char* values[] = {"1.5", "-0.25", "nan", "inf", "-inf", "1e+30", "3"};
            return values[pick(7)];
         }
         return "true";
      }

      void make_table(const std::string& name)
      {
         Object      o;
         bool        has_key = false;
         std::size_t count   = pick(9);
         for (std::size_t i = 0; i < count; ++i)
         {
            Member m;
            m.name = "f" + std::to_string(i);
            bool keyable = false, by_offset = true;
            switch (pick(12))
            {
               case 0: case 1: case 2:
               {
                  AnyType t = scalar();
                  by_offset = false;
                  if (chance(20))
                     t = AnyType{Option{Box<AnyType>{std::move(t)}}};
                  else
                  {
                     keyable = !std::holds_alternative<Custom>(t.value);
                     if (chance(40))
                        m.attributes.push_back({"default", scalar_default(t)});
                  }
                  *m.type = std::move(t);
                  break;
               }
               case 3:
                  if (!enums.empty())
                  {
                     const auto& e = enums[pick(enums.size())];
                     by_offset     = false;
                     if (chance(15))
                        *m.type = AnyType{Option{Box<AnyType>{AnyType{Type{e.name}}}}};
                     else
                     {
                        *m.type = AnyType{Type{e.name}};
                        // 0 must be an enumerator, or the field says which
                        // one it defaults to.
                        const std::uint64_t value = e.values[pick(e.values.size())];
                        if ((!e.has_zero && !e.flags) || (chance(40) && value != 0))
                           m.attributes.push_back(
                              {"default", e.is_signed
                                             ? std::to_string(static_cast<std::int64_t>(value))
                                             : std::to_string(value)});
                     }
                     break;
                  }
                  [[fallthrough]];
               case 4:
                  if (!structs.empty())
                  {
                     *m.type   = AnyType{Type{structs[pick(structs.size())]}};
                     by_offset = false;
                     break;
                  }
                  [[fallthrough]];
               case 5:
                  *m.type = string();
                  keyable = true;
                  break;
               case 6:
                  *m.type = AnyType{Type{tables[pick(tables.size())]}};
                  break;
               case 7:
                  if (!unions.empty())
                  {
                     *m.type = AnyType{Type{unions[pick(unions.size())]}};
                     break;
                  }
                  [[fallthrough]];
               default:
               {
                  AnyType elem;
                  switch (pick(6))
                  {
                     case 0: elem = scalar(); break;
                     case 1: elem = string(); break;
                     case 2: elem = AnyType{Type{tables[pick(tables.size())]}}; break;
                     case 3:
                        elem = structs.empty() ? scalar()
                                               : AnyType{Type{structs[pick(structs.size())]}};
                        break;
                     case 4:
                        elem = enums.empty() ? scalar() : AnyType{Type{enums[pick(enums.size())].name}};
                        break;
                     default:
                        elem = unions.empty() ? string()
                                              : AnyType{Type{unions[pick(unions.size())]}};
                  }
                  *m.type = AnyType{List{Box<AnyType>{std::move(elem)}}};
               }
            }
            const bool is_union =
               std::holds_alternative<Type>(m.type->value) &&
               std::find(unions.begin(), unions.end(), std::get<Type>(m.type->value).type) !=
                  unions.end();
            if (keyable && !has_key && chance(15))
            {
               m.attributes.push_back({"key", std::nullopt});
               has_key = true;
            }
            else if (by_offset && !is_union && chance(10))
               m.attributes.push_back({"required", std::nullopt});
            else if (chance(8))
               m.attributes.push_back({"deprecated", std::nullopt});
            o.members.push_back(std::move(m));
         }
         // Ids out of declaration order: every member says its number.
         if (count > 1 && chance(35))
         {
            std::vector<std::size_t> order(count);
            for (std::size_t i = 0; i < count; ++i)
               order[i] = i;
            std::shuffle(order.begin(), order.end(), rng);
            bool in_order = true;
            for (std::size_t i = 0; i < count; ++i)
               in_order = in_order && order[i] == i;
            if (!in_order)
            {
               fbs_layout_detail::model types{s};
               std::uint64_t            slot = 0;
               for (const std::size_t i : order)
               {
                  Member&    m        = o.members[i];
                  const bool two      = fbs_layout_detail::union_field(types, types.shape_of(*m.type));
                  slot               += two ? 2 : 1;
                  m.attributes.insert(m.attributes.begin(), Attribute{"field", std::to_string(slot)});
               }
            }
         }
         for (auto& entry : s.types)
            if (entry.name == name)
               *entry.type = AnyType{std::move(o)};
      }

      Schema run()
      {
         if (chance(70))
            s.package.name = chance(50) ? "gen" : "gen:deep";
         for (std::size_t i = 0, n = pick(4); i < n; ++i)
            make_enum(i);
         for (std::size_t i = 0, n = pick(4); i < n; ++i)
            make_struct(i);
         // Tables may refer to each other and to unions of each other: the
         // names first, then the unions, then the members.
         const std::size_t table_count = 1 + pick(5);
         for (std::size_t i = 0; i < table_count; ++i)
            tables.push_back("T" + std::to_string(i));
         std::vector<std::size_t> union_at;
         for (std::size_t i = 0, n = pick(3); i < n; ++i)
            make_union(i);
         for (const auto& name : tables)
            s.types.push_back(Member{name, Box<AnyType>{AnyType{Object{}}}, {}});
         for (const auto& name : tables)
            make_table(name);
         // (in the order the parser keeps them: the root type last)
         if (chance(30))
            s.package.attributes.push_back({"fbs_file_identifier", "GEN" + std::to_string(pick(10))});
         if (chance(50))
            s.package.attributes.push_back({"fbs_root_type", tables[pick(tables.size())]});
         return std::move(s);
      }
   };
}  // namespace

TEST_CASE("fbs schema: generated schemas against flatc", "[fbs][schema][flatc]")
{
   scratch tmp;
   for (std::uint64_t seed = 1; seed <= 250; ++seed)
   {
      generator g;
      g.rng.seed(seed);
      const Schema      schema = g.run();
      const std::string text   = psio::emit_fbs(schema);
      INFO("seed " << seed << "\n" << text);

      // Written from the normal form, read back as it.
      const Schema parsed = psio::parse_fbs(text);
      REQUIRE(parsed == schema);
      REQUIRE(psio::emit_fbs(parsed) == text);

      // The layout psio computes is flatc's.
      const std::string official = flatc_schema(tmp, text, "g" + std::to_string(seed));
      same_text(layout_text(schema), layout_text(schema_of(official)), "the layout of a generated schema");
   }
}
