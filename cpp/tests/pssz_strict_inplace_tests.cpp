// psio::validate_strict<T>(psio::pssz{}, bytes) is an in-place walk; it must
// accept exactly the byte strings the reference implementation
// (pssz_strict_oracle.hpp: decode, encode again, compare) accepts.

#include "pssz_strict_inplace_cases.hpp"

#include <catch.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace
{
   struct Tally
   {
      std::size_t inputs    = 0;
      std::size_t accepted  = 0;  // strict ok
      std::size_t plain     = 0;  // plain validation ok
      std::size_t disagree  = 0;
   };
   std::string hex(const std::vector<char>& bytes)
   {
      std::string out;
      char        buf[3];
      for (char c : bytes)
      {
         std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(c));
         out += buf;
      }
      return out;
   }
}  // namespace

TEST_CASE("pssz validate_strict in place agrees with decode-and-re-encode", "[pssz][strict]")
{
   std::map<std::string, Tally> by_type;
   Tally                        total;
   std::size_t                  shown = 0;
   // PSIO_STRICT_FUZZ_SCALE=<n>: n times the values per shape.
   std::size_t scale = 1;
   if (const char* env = std::getenv("PSIO_STRICT_FUZZ_SCALE"))
      scale = std::max<std::size_t>(1, std::strtoul(env, nullptr, 10));
   inplace_cases::run_all(
      [&](const char* name, bool strict, bool reference, bool plain,
          const std::vector<char>& input) {
         auto& t = by_type[name];
         for (Tally* tally : {&t, &total})
         {
            ++tally->inputs;
            tally->accepted += strict;
            tally->plain += plain;
            tally->disagree += strict != reference;
         }
         if (strict != reference && shown++ < 20)
            std::printf("DISAGREE %s: strict=%d reference=%d plain=%d bytes=%s\n", name, strict,
                        reference, plain, hex(input).c_str());
      },
      scale);
   std::printf("pssz strict differential: %zu inputs over %zu shapes; %zu accepted, "
               "%zu structurally valid but not canonical/exact, %zu rejected outright; "
               "%zu disagreements\n",
               total.inputs, by_type.size(), total.accepted, total.plain - total.accepted,
               total.inputs - total.plain, total.disagree);
   for (const auto& [name, t] : by_type)
   {
      INFO(name);
      CHECK(t.disagree == 0);
      // Every shape is exercised on both sides of the line (every byte
      // string is a std::string, and a transparent type holding one).
      CHECK(t.accepted > 0);
      if (name != "std::string" && name != "Plain")
         CHECK(t.accepted < t.inputs);
   }
   CHECK(total.disagree == 0);
   CHECK(total.inputs > 500000);
}

// What "canonical" adds to structural validity, one hand-made frame each.
TEST_CASE("pssz validate_strict: the canonical rules, in place", "[pssz][strict]")
{
   using namespace inplace_cases;
   const auto strict = [](auto fmt, const auto& value, auto&& edit) {
      auto bytes = psio::encode(fmt, value);
      edit(bytes);
      using T = std::remove_cvref_t<decltype(value)>;
      const bool mine = psio::validate_strict<T>(fmt, std::span<const char>{bytes}).ok();
      CHECK(mine == strict_oracle::validate<T>(fmt, std::span<const char>{bytes}).ok());
      return mine;
   };
   const auto keep = [](std::vector<char>&) {};

   // The encoding itself.
   CHECK(strict(psio::pssz{}, Opt{1, 2, "b", Fixed{1, 2, true}, std::vector<u8>{1}}, keep));
   // Appended bytes, a missing byte.
   CHECK(!strict(psio::pssz{}, Opt{1, 2}, [](auto& b) { b.push_back(0); }));
   CHECK(!strict(psio::pssz{}, Opt{1, 2}, [](auto& b) { b.pop_back(); }));
   // A trailing absent optional written out instead of pruned: the header
   // one slot longer, the slot pointing at the end.
   {
      const Opt value{7, 5};
      auto      pruned = psio::to_pssz(value);
      // header u32 = fixed region; the region holds id and one slot.
      std::uint32_t region = 0;
      std::memcpy(&region, pruned.data(), 4);
      REQUIRE(region == 8);
      std::vector<char> unpruned = pruned;
      region                     = 12;
      std::memcpy(unpruned.data(), &region, 4);
      // slot a now points 4 further; slot b (None: selector 00) follows.
      std::uint32_t off = 12;
      std::memcpy(unpruned.data() + 8, &off, 4);
      std::uint32_t off_b = 16;
      unpruned.insert(unpruned.begin() + 12, reinterpret_cast<const char*>(&off_b),
                      reinterpret_cast<const char*>(&off_b) + 4);
      unpruned.push_back(0);
      CHECK(psio::validate_strict<Opt>(psio::pssz{}, std::span<const char>{pruned}).ok());
      CHECK(!psio::validate_strict<Opt>(psio::pssz{}, std::span<const char>{unpruned}).ok());
      CHECK(!strict_oracle::validate<Opt>(psio::pssz{}, std::span<const char>{unpruned}).ok());
   }
   // A bool that is not 0 / 1; a variant index out of range; an optional
   // selector that is not 0 / 1.
   CHECK(!strict(psio::pssz{}, Fixed{1, 2, true}, [](auto& b) { b.back() = 2; }));
   CHECK(!strict(psio::pssz{}, std::variant<u8, std::string>{u8{1}}, [](auto& b) { b[0] = 2; }));
   CHECK(!strict(psio::pssz{}, std::optional<std::string>{"a"}, [](auto& b) { b[0] = 2; }));
   // Map keys out of order, and equal.
   {
      const auto bytes = psio::to_pssz(std::vector<u32>{2, 1});
      CHECK(!psio::validate_strict<std::set<u32>>(psio::pssz{}, std::span<const char>{bytes}).ok());
      const auto twice = psio::to_pssz(std::vector<u32>{2, 2});
      CHECK(!psio::validate_strict<std::set<u32>>(psio::pssz{}, std::span<const char>{twice}).ok());
      const auto sorted = psio::to_pssz(std::vector<u32>{1, 2});
      CHECK(psio::validate_strict<std::set<u32>>(psio::pssz{}, std::span<const char>{sorted}).ok());
   }
   // Keys the encoding distinguishes and the container merges.
   {
      const auto bytes = psio::to_pssz(std::vector<std::string>{"A", "a"});
      using CaseSet    = std::set<std::string, CaseLess>;
      CHECK(psio::validate_strict<std::set<std::string>>(psio::pssz{},
                                                         std::span<const char>{bytes}).ok());
      CHECK(psio::validate_pssz<CaseSet>(std::span<const char>{bytes}).ok());
      CHECK(!psio::validate_strict<CaseSet>(psio::pssz{}, std::span<const char>{bytes}).ok());
      CHECK(!strict_oracle::validate<CaseSet>(psio::pssz{}, std::span<const char>{bytes}).ok());
   }
   // A member's as<Tag> adapter that reads a spelling it does not write.
   {
      AsMember value{"id", Letters{"abc"}, 3};
      CHECK(strict(psio::pssz{}, value, keep));
      CHECK(!strict(psio::pssz{}, value, [](auto& b) {
         for (char& c : b)
            if (c == 'b') c = 'B';
      }));
   }
   // A third-party spec, on the member's bytes: where the spec walk goes.
   {
      Spec bad;
      bad.word = "x";
      CHECK(psio::validate_pssz<Spec>(psio::to_pssz(bad)).ok());
      CHECK(!strict(psio::pssz{}, bad, keep));
      Spec words;
      words.words = {"a", "bx"};
      CHECK(!strict(psio::pssz{}, words, keep));
      // An optional<std::string> member is not a string member.
      Spec maybe;
      maybe.maybe = "x";
      CHECK(strict(psio::pssz{}, maybe, keep));
      // Reached through a list of a record; not when the root is no record.
      SpecTree tree;
      CHECK(strict(psio::pssz{}, tree, keep));
      tree.list = {bad};
      CHECK(!strict(psio::pssz{}, tree, keep));
      CHECK(strict(psio::pssz{}, std::vector<Spec>{bad}, keep));
      // Not through an array or a tuple.
      SpecTree array;
      array.array[0] = bad;
      CHECK(strict(psio::pssz{}, array, keep));
      SpecTree map;
      map.map[1] = bad;
      CHECK(!strict(psio::pssz{}, map, keep));
   }
}
