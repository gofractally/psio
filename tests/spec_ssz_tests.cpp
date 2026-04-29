// Official Ethereum consensus-spec SSZ tests.
//
// Driven by a manifest file produced by spec_tests/preprocess.py.
// Each line names a type + expected value + raw serialized bytes file
// (decompressed from ssz_snappy). For each fixture we run the decode
// path through psio::ssz and compare to the expected value; then
// re-encode and compare bytes for round-trip identity.
//
// Fixtures covered today: valid uint_{8,16,32,64} and boolean.
// Bitvectors, bitlists, basic_vectors, and containers are out of
// scope for the MVP psio SSZ codec and are skipped by the script.
//
// The manifest location is discovered via an environment variable
// PSIO_SPEC_MANIFEST, with a fallback to the in-tree build path.

#include <psio/ssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

   std::filesystem::path manifest_path()
   {
      if (const char* env = std::getenv("PSIO_SPEC_MANIFEST"))
         return env;
      return "build/spec-tests/_decoded/manifest.txt";
   }

   std::filesystem::path decoded_root()
   {
      if (const char* env = std::getenv("PSIO_SPEC_DECODED_ROOT"))
         return env;
      return "build/spec-tests/_decoded";
   }

   std::vector<char> read_raw(const std::filesystem::path& p)
   {
      std::ifstream in(p, std::ios::binary);
      std::ostringstream oss;
      oss << in.rdbuf();
      auto s = oss.str();
      return std::vector<char>(s.begin(), s.end());
   }

   struct manifest_entry
   {
      std::string category;     // "uints", "boolean", ...
      std::string status;       // "valid" or "invalid"
      std::string name;         // test-case name
      std::string type_tag;     // "uint8", "uint16", ..., "bool"
      std::string value_lit;    // decimal string, or "0"/"1" for bool
   };

   std::vector<manifest_entry> load_manifest()
   {
      std::vector<manifest_entry> out;
      std::ifstream               in(manifest_path());
      std::string                 line;
      while (std::getline(in, line))
      {
         if (line.empty()) continue;
         manifest_entry e;
         auto           split = [&](std::string& field)
         {
            auto tab = line.find('\t');
            field    = line.substr(0, tab);
            line.erase(0, (tab == std::string::npos ? line.size() : tab + 1));
         };
         split(e.category);
         split(e.status);
         split(e.name);
         split(e.type_tag);
         e.value_lit = line;
         out.push_back(std::move(e));
      }
      return out;
   }

   template <typename T>
   void run_one(const manifest_entry& e)
   {
      auto raw_path = decoded_root() / e.category / e.status / e.name /
                      "raw.ssz";
      auto bytes = read_raw(raw_path);

      // Expected value (decimal from YAML).
      T expected{};
      if constexpr (std::is_same_v<T, bool>)
         expected = (e.value_lit == "1");
      else
      {
         std::uint64_t u = std::stoull(e.value_lit);
         expected = static_cast<T>(u);
      }

      // Decode psio::ssz and compare.
      auto decoded =
         psio::decode<T>(psio::ssz{}, std::span<const char>{bytes});
      REQUIRE(decoded == expected);

      // Re-encode and byte-match the original fixture.
      auto re_encoded = psio::encode(psio::ssz{}, expected);
      REQUIRE(re_encoded == bytes);
   }

   // Basic-vector fixtures: decode as std::vector<T> and verify each
   // element matches the CSV expected values. Re-encode and compare
   // bytes. SSZ Vector[T, N] and List[T, *] produce identical wire
   // layouts for fixed-size T (both are just concatenated elements).
   template <typename T>
   void run_vec(const manifest_entry& e)
   {
      auto raw_path =
         decoded_root() / e.category / e.status / e.name / "raw.ssz";
      auto bytes = read_raw(raw_path);

      // Parse CSV into expected vector.
      std::vector<T> expected;
      if (!e.value_lit.empty())
      {
         std::string        tok;
         std::istringstream iss(e.value_lit);
         while (std::getline(iss, tok, ','))
         {
            if constexpr (std::is_same_v<T, bool>)
               expected.push_back(tok == "1");
            else
            {
               std::uint64_t u = std::stoull(tok);
               expected.push_back(static_cast<T>(u));
            }
         }
      }

      auto decoded = psio::decode<std::vector<T>>(
         psio::ssz{}, std::span<const char>{bytes});
      REQUIRE(decoded.size() == expected.size());
      for (std::size_t i = 0; i < expected.size(); ++i)
         REQUIRE(decoded[i] == expected[i]);

      auto re_encoded = psio::encode(psio::ssz{}, expected);
      REQUIRE(re_encoded == bytes);
   }

   bool starts_with(const std::string& s, const std::string& p)
   {
      return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
   }

}  // namespace

TEST_CASE("consensus-spec ssz_generic: decode + re-encode identity",
          "[spec][ssz][generic]")
{
   auto entries = load_manifest();
   REQUIRE_FALSE(entries.empty());

   for (const auto& e : entries)
   {
      if (e.status != "valid")
         continue;

      SECTION(e.category + "/" + e.name)
      {
         if (e.type_tag == "uint8")       run_one<std::uint8_t>(e);
         else if (e.type_tag == "uint16") run_one<std::uint16_t>(e);
         else if (e.type_tag == "uint32") run_one<std::uint32_t>(e);
         else if (e.type_tag == "uint64") run_one<std::uint64_t>(e);
         else if (e.type_tag == "bool")   run_one<bool>(e);
         else if (starts_with(e.type_tag, "vec_uint8_"))
            run_vec<std::uint8_t>(e);
         else if (starts_with(e.type_tag, "vec_uint16_"))
            run_vec<std::uint16_t>(e);
         else if (starts_with(e.type_tag, "vec_uint32_"))
            run_vec<std::uint32_t>(e);
         else if (starts_with(e.type_tag, "vec_uint64_"))
            run_vec<std::uint64_t>(e);
         else if (starts_with(e.type_tag, "vec_bool_"))
            // SSZ Vector[bool, N] is N bytes of 0x00/0x01; easier to
            // handle as std::vector<uint8_t> than fight std::vector<bool>'s
            // proxy references.
            run_vec<std::uint8_t>(e);
         else
            FAIL("unknown type tag: " + e.type_tag);
      }
   }
}

TEST_CASE("consensus-spec ssz_generic: manifest loaded",
          "[spec][ssz][generic][smoke]")
{
   auto entries = load_manifest();
   REQUIRE(entries.size() >= 30);
   INFO("Loaded " << entries.size() << " fixture entries");
}
