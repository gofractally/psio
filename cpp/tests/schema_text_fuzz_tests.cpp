// The schema-text parsers on text nobody wrote: psio::parse_fbs and
// psio::parse_capnp read untrusted input.
//
// For every text: the parser returns a schema or throws its own error type
// (never anything else, never a crash); the layout of an accepted schema is
// computed or refused with std::runtime_error; and when the schema can be
// written back, the text written parses again and is a fixed point of
// parse → emit. The texts: every prefix of the hand-written and emitted
// schemas, every byte of them replaced, random edits and splices, random
// bytes, and inputs built to exhaust a parser that recurses or allocates
// without a bound (deep nesting, very many members, very many types).
//
// Built a second time with AddressSanitizer and UndefinedBehaviorSanitizer
// (psio_schema_text_fuzz_tests_sanitized).

#include <psio/capnp_layout.hpp>
#include <psio/capnp_parser.hpp>
#include <psio/emit_capnp.hpp>
#include <psio/emit_fbs.hpp>
#include <psio/fbs_layout.hpp>
#include <psio/fbs_parser.hpp>

#include <catch.hpp>

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "capnp_shapes.hpp"
#include "flatbuf_schema_shapes.hpp"

namespace
{
   std::string read_file(const std::string& path)
   {
      std::ifstream     in(path, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      REQUIRE(in);
      return text.str();
   }

   struct fbs
   {
      using error = psio::fbs_parse_error;
      static psio::Schema parse(std::string_view text) { return psio::parse_fbs(text); }
      static std::string  emit(const psio::Schema& s) { return psio::emit_fbs(s); }
      static void         layout(const psio::Schema& s) { (void)psio::fbs_layout(s); }
   };
   struct capnp
   {
      using error = psio::capnp_parse_error;
      static psio::Schema parse(std::string_view text) { return psio::parse_capnp(text); }
      static std::string  emit(const psio::Schema& s) { return psio::emit_capnp(s); }
      static void         layout(const psio::Schema& s) { (void)psio::capnp_layout(s); }
   };

   struct tally
   {
      std::size_t texts = 0, refused = 0, parsed = 0, written = 0;
   };

   // Fails the test (an exception escapes) on anything but the documented
   // outcomes.
   template <typename F>
   void probe(std::string_view text, tally& t)
   {
      ++t.texts;
      psio::Schema schema;
      try
      {
         schema = F::parse(text);
      }
      catch (const typename F::error&)
      {
         ++t.refused;
         return;
      }
      ++t.parsed;
      try
      {
         F::layout(schema);
      }
      catch (const std::runtime_error&)
      {
      }
      std::string emitted;
      try
      {
         emitted = F::emit(schema);
      }
      catch (const std::runtime_error&)
      {
         return;  // (a type of a file that was not read, ...)
      }
      ++t.written;
      psio::Schema again;
      try
      {
         again = F::parse(emitted);
      }
      catch (const typename F::error& e)
      {
         FAIL("the text written does not parse: " << e.what() << "\n--- from\n"
                                                   << text << "\n--- written\n"
                                                   << emitted);
      }
      const std::string twice = F::emit(again);
      if (twice != emitted)
      {
         // The lines around the first difference.
         std::size_t at = 0;
         while (at < twice.size() && at < emitted.size() && twice[at] == emitted[at])
            ++at;
         const std::size_t from = at > 200 ? at - 200 : 0;
         FAIL("the text written is not a fixed point\n--- written\n"
              << emitted.substr(from, 400) << "\n--- then\n"
              << twice.substr(from, 400) << "\n--- from\n"
              << text);
      }
   }

   const std::string_view alphabet = "{}()[]:;,=@$#.\"'\n 0aZ-/*!_<>\\\t9x";

   template <typename F>
   tally mutate(const std::vector<std::string>& seeds, const std::vector<std::string_view>& words,
                std::uint64_t seed, std::size_t replacements, std::size_t edits, std::size_t random)
   {
      tally           t;
      std::mt19937_64 rng(seed);
      const auto      pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };

      for (const auto& text : seeds)
      {
         // Every prefix.
         for (std::size_t n = 0; n <= text.size(); ++n)
            probe<F>(std::string_view{text}.substr(0, n), t);
         // Every byte replaced.
         std::string copy = text;
         for (std::size_t i = 0; i < text.size(); ++i)
         {
            for (std::size_t k = 0; k < replacements; ++k)
            {
               copy[i] = alphabet[pick(alphabet.size())];
               probe<F>(copy, t);
            }
            copy[i] = text[i];
         }
      }
      // Edits: a span removed, repeated, moved, taken from another schema; a
      // word of the language inserted.
      for (std::size_t n = 0; n < edits; ++n)
      {
         std::string text = seeds[pick(seeds.size())];
         for (std::size_t k = 0, count = 1 + pick(4); k < count && !text.empty(); ++k)
         {
            const std::size_t at  = pick(text.size());
            const std::size_t len = std::min<std::size_t>(pick(40), text.size() - at);
            switch (pick(6))
            {
               case 0: text.erase(at, len); break;
               case 1: text.insert(at, text.substr(at, len)); break;
               case 2:
               {
                  const std::string span = text.substr(at, len);
                  text.erase(at, len);
                  text.insert(pick(text.size() + 1), span);
                  break;
               }
               case 3:
               {
                  const std::string& other = seeds[pick(seeds.size())];
                  const std::size_t  from  = pick(other.size());
                  text.insert(at, other.substr(from, pick(80)));
                  break;
               }
               case 4: text.insert(at, std::string{words[pick(words.size())]} + " "); break;
               default: text[at] = alphabet[pick(alphabet.size())];
            }
         }
         probe<F>(text, t);
      }
      // Bytes.
      for (std::size_t n = 0; n < random; ++n)
      {
         std::string text(pick(200), ' ');
         for (char& c : text)
            c = pick(4) == 0 ? static_cast<char>(pick(256)) : alphabet[pick(alphabet.size())];
         probe<F>(text, t);
      }
      return t;
   }

   std::string repeat(std::string_view piece, std::size_t n)
   {
      std::string out;
      out.reserve(piece.size() * n);
      for (std::size_t i = 0; i < n; ++i)
         out += piece;
      return out;
   }

   // The parser's verdict on `text`, and that it took no more than a
   // moment: `refused`, or accepted.
   template <typename F>
   bool refused(const std::string& text)
   {
      const auto start = std::chrono::steady_clock::now();
      tally      t;
      probe<F>(text, t);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      INFO(text.substr(0, 60) << "... (" << text.size() << " bytes) took " << ms << " ms");
      // (linear in the text: these are megabytes; the bound is loose enough
      // for the sanitizer build on a loaded machine)
      CHECK(ms < 20000);
      return t.refused == 1;
   }
}  // namespace

TEST_CASE("fbs text: mutated schemas", "[fbs][fuzz]")
{
   const std::string dir = PSIO_FLATBUF_SCHEMA_DIR;
   const std::vector<std::string> seeds{
      read_file(dir + "/interop.fbs"), read_file(dir + "/schema_all.fbs"),
      read_file(dir + "/monster.fbs"), psio::emit_fbs(flatbuf_shapes_schema())};
   const std::vector<std::string_view> words{
      "table", "struct", "enum", "union", "namespace", "root_type", "file_identifier", "include",
      "attribute", "rpc_service", "null", "true", "nan", "(id: 0)", "(key)", "(deprecated)",
      "(required)", "(force_align: 8)", "(bit_flags)", "= 1", "[int:3]", "[", "]", "ubyte", "string"};
   const tally t = mutate<fbs>(seeds, words, 0xf1a7b0f5, 4, 60000, 20000);
   INFO(t.texts << " texts: " << t.refused << " refused, " << t.parsed << " parsed, " << t.written
                << " written back");
   CHECK(t.texts > 100000);
   CHECK(t.refused > 1000);
   CHECK(t.written > 1000);
   WARN("fbs: " << t.texts << " texts, " << t.refused << " refused, " << t.parsed << " parsed, "
                << t.written << " written back and read again");
}

TEST_CASE("capnp text: mutated schemas", "[capnp][fuzz]")
{
   const std::string dir = PSIO_CAPNP_SCHEMA_DIR;
   const std::vector<std::string> seeds{
      read_file(dir + "/interop.capnp"), read_file(dir + "/schema_all.capnp"),
      psio::emit_capnp(capnp_shapes_schema())};
   const std::vector<std::string_view> words{
      "struct", "enum", "interface", "union", "group", "using", "const", "annotation", "import",
      "extends", "List(", ")", "@0", "@1", "@2!", ":union {", ":group {", "}", "= 1", "= (a = 1)",
      "$Json.name(\"x\")", "-> (", "stream", ":Void;", ":Text", ":UInt8", "@0xdbb9ad1f14bf0b36"};
   const tally t = mutate<capnp>(seeds, words, 0xca9f0f5, 4, 60000, 20000);
   CHECK(t.texts > 100000);
   CHECK(t.refused > 1000);
   CHECK(t.written > 1000);
   WARN("capnp: " << t.texts << " texts, " << t.refused << " refused, " << t.parsed << " parsed, "
                  << t.written << " written back and read again");
}

TEST_CASE("fbs text: nothing recurses or grows without a bound", "[fbs][fuzz]")
{
   const std::size_t n = 200000;
   // Nesting.
   CHECK(refused<fbs>("table T { a:" + repeat("[", n) + "int" + repeat("]", n) + "; }"));
   CHECK(refused<fbs>(repeat("[", n)));
   CHECK(refused<fbs>("table T (" + repeat("a(", n)));
   CHECK(refused<fbs>(repeat("table T {", n)));
   CHECK(refused<fbs>("/*" + repeat("/*", n)));
   CHECK(refused<fbs>("table T { a:int = " + repeat("-", n) + "1; }"));
   CHECK(refused<fbs>("table T { a:" + repeat("a.", n) + "b; }") == false);  // a long name, unknown
   CHECK(refused<fbs>("enum E : int { a = " + repeat("9", n) + " }"));
   CHECK(refused<fbs>("struct S { a:[int:" + repeat("9", n) + "]; }"));
   // Very many of everything: linear.
   {
      std::string fields;
      for (std::size_t i = 0; i < 32765; ++i)
         fields += "f" + std::to_string(i) + ":int = " + std::to_string(i) + ";\n";
      CHECK_FALSE(refused<fbs>("table T {\n" + fields + "}"));
      // One more field than a vtable has room for.
      CHECK(refused<fbs>("table T {\n" + fields + "last:int;\n}"));
   }
   {
      std::string values;
      for (std::size_t i = 0; i < n; ++i)
         values += "v" + std::to_string(i) + ",";
      CHECK_FALSE(refused<fbs>("enum E : int { " + values + " }"));
      CHECK(refused<fbs>("enum E : ubyte { " + values + " }"));
   }
   {
      // A chain of tables, of structs (each holding the one before), of
      // unions and of the tables that hold them.
      std::string text = "struct S0 { x:int; }\ntable T0 { s:S0; }\nunion U0 { T0 }\n";
      for (std::size_t i = 1; i < 20000; ++i)
      {
         const std::string k = std::to_string(i), p = std::to_string(i - 1);
         text += "table T" + k + " { t:T" + p + "; ts:[T" + p + "]; u:U" + p + "; }\n";
         text += "union U" + k + " { T" + k + ", T" + p + " }\n";
      }
      CHECK_FALSE(refused<fbs>(text));
   }
   {
      // Structs nested as deep as the text is long: a struct's size needs
      // every struct below it.
      std::string text = "struct S0 { x:ubyte; }\n";
      for (std::size_t i = 1; i < 20000; ++i)
         text += "struct S" + std::to_string(i) + " { s:S" + std::to_string(i - 1) + "; y:ubyte; }\n";
      CHECK_FALSE(refused<fbs>(text));
      // … and declared last first, which flatc does not take either.
      std::string reversed;
      for (std::size_t i = 300; i > 0; --i)
         reversed += "struct S" + std::to_string(i) + " { s:S" + std::to_string(i - 1) + "; }\n";
      CHECK(refused<fbs>(reversed + "struct S0 { x:ubyte; }\n"));
   }
   CHECK_FALSE(refused<fbs>("table T (" + [&] {
      std::string attrs;
      for (std::size_t i = 0; i < n; ++i)
         attrs += "a" + std::to_string(i) + ",";
      return attrs;
   }() + "z) { }"));
   CHECK(refused<fbs>("table " + repeat("x", 4 * n) + " { a:int }"));
}

TEST_CASE("capnp text: nothing recurses or grows without a bound", "[capnp][fuzz]")
{
   const std::size_t n = 200000;
   CHECK(refused<capnp>("struct S { a @0 :" + repeat("List(", n) + "Bool" + repeat(")", n) + "; }"));
   CHECK(refused<capnp>(repeat("struct S { ", n)));
   CHECK(refused<capnp>("struct S { " + repeat("g :group { ", n)));
   CHECK(refused<capnp>("struct S { " + repeat("u :union { g :group { ", n)));
   CHECK(refused<capnp>(repeat("interface I { ", n)));
   CHECK(refused<capnp>("struct S { a @0 :Text = " + repeat("(", n) + "; }"));
   CHECK_FALSE(refused<capnp>("struct S { a @0 :Text = " + repeat("(", n) + repeat(")", n) + "; }"));
   CHECK_FALSE(refused<capnp>("struct S { a @0 :Text " + repeat("$x(1) ", n) + "; }"));
   CHECK(refused<capnp>("struct S { a @0 :Text $x(" + repeat("[", n) + "; }"));
   CHECK(refused<capnp>("const c :Int32 = " + repeat("(", n)));
   CHECK(refused<capnp>("struct S { a @" + repeat("9", n) + " :Bool; }"));
   CHECK(refused<capnp>("@0x" + repeat("f", n) + ";"));
   CHECK(refused<capnp>(repeat("@", n)));
   CHECK(refused<capnp>(repeat("$", n)));
   {
      // As many fields as a struct can have; one more.
      std::string fields;
      for (std::size_t i = 0; i < 65536; ++i)
         fields += "f" + std::to_string(i) + " @" + std::to_string(i) + " :UInt8;\n";
      CHECK_FALSE(refused<capnp>("struct S {\n" + fields + "}"));
      CHECK(refused<capnp>("struct S {\n" + fields + "g @65536 :UInt8;\n}"));
   }
   {
      // A union of very many members, groups among them.
      std::string members;
      for (std::size_t i = 0; i < 30000; ++i)
         members += i % 3 == 0 ? "g" + std::to_string(i) + " :group { a" + std::to_string(i) + " @" +
                                    std::to_string(i) + " :UInt64; }\n"
                               : "m" + std::to_string(i) + " @" + std::to_string(i) +
                                    (i % 3 == 1 ? " :Text;\n" : " :UInt16;\n");
      CHECK_FALSE(refused<capnp>("struct S { union {\n" + members + "} }"));
   }
   {
      // Very many types that refer to each other, nested and by alias.
      std::string text = "struct S0 { x @0 :UInt8; }\nusing A0 = S0;\n";
      for (std::size_t i = 1; i < 20000; ++i)
      {
         const std::string k = std::to_string(i), p = std::to_string(i - 1);
         text += "struct S" + k + " { s @0 :S" + p + "; l @1 :List(A" + p + "); e @2 :S" + k +
                 ".E = b; enum E { a @0; b @1; } }\nusing A" + k + " = S" + k + ";\n";
      }
      CHECK_FALSE(refused<capnp>(text));
   }
   {
      // Aliases of aliases, and one of itself.
      std::string text = "struct S { x @0 :UInt8; }\nusing A0 = S;\n";
      for (std::size_t i = 1; i < 20000; ++i)
         text += "using A" + std::to_string(i) + " = A" + std::to_string(i - 1) + ";\n";
      CHECK_FALSE(refused<capnp>(text + "struct T { a @0 :A19999; }"));
      CHECK(refused<capnp>("using A = B; using B = A; struct T { a @0 :A; }"));
   }
}
