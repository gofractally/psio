// psio::parse_protobuf and psio::emit_protobuf against protoc, at run time:
// a `.proto` file keeps its descriptor (what protoc compiles it to) through
// parse → emit, for hand-written files, for the schema of the reflected
// shapes, and for generated schemas.

#include <psio/emit_protobuf.hpp>
#include <psio/protobuf_parser.hpp>

#include <catch.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protobuf_random.hpp"
#include "protobuf_shapes.hpp"

namespace
{
   namespace fs = std::filesystem;

   std::string read_file(const fs::path& p)
   {
      std::ifstream     in(p, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
   }
   void write_file(const fs::path& p, const std::string& text)
   {
      fs::create_directories(p.parent_path());
      std::ofstream out(p, std::ios::binary);
      out << text;
   }

   // protoc's descriptor set of a file named `name` holding `text` (imports
   // from `imports`); empty when protoc refuses it.
   std::string descriptor(const std::string& side, const std::string& name,
                          const std::string& text, const std::string& imports = {})
   {
      const fs::path dir = fs::path{PSIO_PROTOBUF_WORK_DIR} / side;
      write_file(dir / name, text);
      const fs::path out = dir / (name + ".pb");
      fs::remove(out);
      std::string cmd = std::string{"\""} + PSIO_PROTOC + "\" -I \"" + dir.string() + "\"";
      if (!imports.empty())
         cmd += " -I \"" + imports + "\"";
      cmd += " --descriptor_set_out=\"" + out.string() + "\" \"" + (dir / name).string() +
             "\" 2> \"" + (dir / (name + ".err")).string() + "\"";
      if (std::system(cmd.c_str()) != 0)
         return {};
      return read_file(out);
   }

   // The file keeps its descriptor through parse → emit, and the emitted
   // text is a fixed point.
   void keeps_descriptor(const std::string& name, const std::string& text,
                         const std::string& imports = {})
   {
      INFO(name << "\n" << text);
      const std::string before = descriptor("original", name, text, imports);
      REQUIRE(!before.empty());
      std::string emitted;
      try
      {
         emitted = psio::emit_protobuf(psio::parse_protobuf(text));
      }
      catch (const std::exception& e)
      {
         FAIL(e.what());
      }
      INFO("emitted:\n" << emitted);
      const std::string after = descriptor("emitted", name, emitted, imports);
      REQUIRE(!after.empty());
      REQUIRE(before == after);
      REQUIRE(psio::emit_protobuf(psio::parse_protobuf(emitted)) == emitted);
   }

   // ── Generated schemas ───────────────────────────────────────────────
   struct generator
   {
      pb_rng                   r;
      bool                     proto3;
      std::string              package;
      std::vector<std::string> messages, enums;  // names from the package
      std::string              out;
      unsigned                 counter = 0;

      std::string fresh(const char* prefix) { return prefix + std::to_string(counter++); }

      struct node
      {
         std::string       name, full;
         bool              is_enum = false;
         std::vector<node> children;
      };

      node tree(const std::string& scope, unsigned depth)
      {
         node n;
         n.name = fresh("M");
         n.full = scope.empty() ? n.name : scope + "." + n.name;
         messages.push_back(n.full);
         const auto kids = depth >= 3 ? 0 : r.below(3);
         for (std::uint64_t i = 0; i < kids; ++i)
         {
            if (r.chance(35))
            {
               node e;
               e.name    = fresh("E");
               e.full    = n.full + "." + e.name;
               e.is_enum = true;
               enums.push_back(e.full);
               n.children.push_back(std::move(e));
            }
            else
               n.children.push_back(tree(n.full, depth + 1));
         }
         return n;
      }

      std::string reference(const std::string& full)
      {
         return r.chance(25) ? "." + (package.empty() ? "" : package + ".") + full : full;
      }

      void enumeration(const node& n, const std::string& in)
      {
         out += in + "enum " + n.name + " {\n";
         const auto        count = 1 + r.below(5);
         const bool        alias = r.chance(20) && count > 1;
         std::vector<int>  used;
         if (alias)
            out += in + "  option allow_alias = true;\n";
         for (std::uint64_t i = 0; i < count; ++i)
         {
            int value = 0;
            if (i == 0)  // (proto2 too: an enum that is a map value starts at zero)
               value = 0;
            else if (alias && i == count - 1)
               value = used.front();
            else
               for (;;)
               {
                  value = static_cast<int>(r.below(60)) - 8;
                  bool taken = false;
                  for (int u : used)
                     taken |= u == value;
                  if (!taken)
                     break;
               }
            used.push_back(value);
            out += in + "  " + fresh("V") + " = " + std::to_string(value) +
                   (r.chance(10) ? " [deprecated = true]" : "") + ";\n";
         }
         if (r.chance(15))
            out += in + "  reserved 1000 to 1005, 2000;\n";
         out += in + "}\n";
      }

      // text, packable, may be a map key, has a default text
      struct picked
      {
         std::string text;
         bool        numeric = false, key = false;
         const char* some = nullptr;
      };
      picked scalar()
      {
         static const picked all[] = {
            {"int32", true, true, "-3"},    {"int64", true, true, "7"},    {"uint32", true, true, "9"},
            {"uint64", true, true, "1"},    {"sint32", true, true, "-1"},  {"sint64", true, true, "2"},
            {"fixed32", true, true, "5"},   {"fixed64", true, true, "6"},  {"sfixed32", true, true, "-7"},
            {"sfixed64", true, true, "8"},  {"bool", true, true, "true"},  {"string", false, true, "\"d;x\""},
            {"bytes", false, false, "\"\\001\""}, {"float", true, false, "1.5"}, {"double", true, false, "-2e3"}};
         return all[r.below(15)];
      }
      picked any()
      {
         const auto k = r.below(10);
         if (k < 6 || (messages.empty() && enums.empty()))
            return scalar();
         if (k < 8 && !enums.empty())
            return {reference(enums[r.below(enums.size())]), true, false, nullptr};
         return {reference(messages[r.below(messages.size())]), false, false, nullptr};
      }

      void message(const node& n, const std::string& in)
      {
         out += in + "message " + n.name + " {\n";
         if (r.chance(10))
            out += in + "  option deprecated = true;\n";
         for (const node& c : n.children)
            c.is_enum ? enumeration(c, in + "  ") : message(c, in + "  ");
         std::vector<unsigned> numbers;
         const auto            number = [&] {
            for (;;)
            {
               static const unsigned big[] = {1000, 18999, 20000, 536870911, 65536};
               const unsigned n2 = r.chance(8) ? big[r.below(5)] : 1 + static_cast<unsigned>(r.below(60));
               bool           taken = false;
               for (unsigned u : numbers)
                  taken |= u == n2;
               if (!taken)
               {
                  numbers.push_back(n2);
                  return std::to_string(n2);
               }
            }
         };
         const auto fields = r.below(9);
         for (std::uint64_t i = 0; i < fields; ++i)
         {
            const auto kind = r.below(12);
            if (kind == 0)
            {
               picked key;
               do
                  key = scalar();
               while (!key.key);
               out += in + "  map<" + key.text + ", " + any().text + "> " + fresh("f") + " = " +
                      number() + ";\n";
            }
            else if (kind == 1)
            {
               out += in + "  oneof " + fresh("o") + " {\n";
               const auto alts = 1 + r.below(3);
               for (std::uint64_t a = 0; a < alts; ++a)
                  out += in + "    " + any().text + " " + fresh("f") + " = " + number() + ";\n";
               out += in + "  }\n";
            }
            else
            {
               const picked             t     = any();
               const auto               label = r.below(3);
               std::vector<std::string> options;
               std::string              text  = in + "  ";
               if (label == 0)
                  text += "repeated ";
               else if (label == 1)
                  text += "optional ";
               else if (!proto3)
                  text += "required ";
               if (label == 0 && t.numeric && r.chance(30))
                  options.push_back(r.chance(50) ? "packed = false" : "packed = true");
               if (label == 1 && !proto3 && t.some && r.chance(40))
                  options.push_back(std::string{"default = "} + t.some);
               if (r.chance(10))
                  options.push_back("deprecated = true");
               if (r.chance(10))
                  options.push_back("json_name = \"j" + std::to_string(counter++) + "\"");
               text += t.text + " " + fresh("f") + " = " + number();
               for (std::size_t o = 0; o < options.size(); ++o)
                  text += (o ? ", " : " [") + options[o];
               out += text + (options.empty() ? "" : "]") + ";\n";
            }
         }
         if (r.chance(15))
            out += in + "  reserved 600 to 700, 900;\n";
         if (r.chance(10))
            out += in + "  reserved \"gone\", \"lost\";\n";
         out += in + "}\n";
      }

      std::string run()
      {
         out = proto3 ? "syntax = \"proto3\";\n" : r.chance(50) ? "syntax = \"proto2\";\n" : "";
         if (r.chance(80))
         {
            package = r.chance(50) ? "gen" : "gen.sub.v2";
            out += "package " + package + ";\n";
         }
         if (r.chance(30))
            out += "option java_multiple_files = true;\n";
         std::vector<node> top;
         const auto        count = 1 + r.below(4);
         for (std::uint64_t i = 0; i < count; ++i)
         {
            if (r.chance(25))
            {
               node e;
               e.name = e.full = fresh("E");
               e.is_enum       = true;
               enums.push_back(e.full);
               top.push_back(std::move(e));
            }
            else
               top.push_back(tree({}, 0));
         }
         for (const node& n : top)
            n.is_enum ? enumeration(n, "") : message(n, "");
         if (!messages.empty() && r.chance(30))
         {
            out += "service " + fresh("S") + " {\n";
            const auto rpcs = 1 + r.below(3);
            for (std::uint64_t i = 0; i < rpcs; ++i)
               out += "  rpc " + fresh("R") + " (" + (r.chance(20) ? "stream " : "") +
                      reference(messages[r.below(messages.size())]) + ") returns (" +
                      (r.chance(20) ? "stream " : "") +
                      reference(messages[r.below(messages.size())]) + ");\n";
            out += "}\n";
         }
         return out;
      }
   };
}  // namespace

TEST_CASE("protobuf schema: hand-written files keep their descriptor", "[protobuf][schema]")
{
   const std::string dir = PSIO_PROTOBUF_SCHEMA_DIR;
   keeps_descriptor("common.proto", read_file(dir + "/common.proto"));
   keeps_descriptor("sample3.proto", read_file(dir + "/sample3.proto"), dir);
   keeps_descriptor("sample2.proto", read_file(dir + "/sample2.proto"));
   keeps_descriptor("nosyntax.proto", read_file(dir + "/nosyntax.proto"));
}

TEST_CASE("protobuf schema: the reflected shapes, emitted, compile and read back",
          "[protobuf][schema]")
{
   const std::string text = psio::emit_protobuf(protobuf_shapes_schema());
   keeps_descriptor("shapes.proto", text);
}

TEST_CASE("protobuf schema: generated schemas keep their descriptor", "[protobuf][schema]")
{
   unsigned refused = 0;
   for (unsigned i = 0; i < 300; ++i)
   {
      generator g{pb_rng{1000 + i}, i % 3 != 0};
      const std::string text = g.run();
      keeps_descriptor("generated.proto", text);
   }
   CHECK(refused == 0);
}
