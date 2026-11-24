// psio::protobuf on bytes that are not what a writer produced: every
// truncation and every byte of valid messages replaced, random corruptions,
// random bytes. Nothing crashes or reads outside the input (the build also
// runs this under AddressSanitizer + UndefinedBehaviorSanitizer), validate
// and decode agree on every input, what is accepted re-encodes to a fixed
// point, and work is bounded by the input.

#include <psio/protobuf.hpp>

#include <catch.hpp>

#include <chrono>
#include <span>
#include <vector>

#include "protobuf_random.hpp"
#include "protobuf_shapes.hpp"

namespace
{
   struct tally
   {
      std::uint64_t inputs = 0, accepted = 0;
   };

   template <typename T>
   void probe(const std::vector<char>& c, tally& t)
   {
      ++t.inputs;
      const bool valid = psio::validate_protobuf<T>(std::span<const char>{c}).ok();
      T          v{};
      const bool decoded = psio::protobuf::decode_checked(std::span<const char>{c}, v).ok();
      REQUIRE(valid == decoded);
      if (!valid)
         return;
      ++t.accepted;
      // What was read is a value: it encodes, and that encoding reads back
      // to the same bytes.
      const std::vector<char> once = psio::to_protobuf(v);
      T                       w{};
      REQUIRE(psio::protobuf::decode_checked(std::span<const char>{once}, w).ok());
      REQUIRE(psio::to_protobuf(w) == once);
   }

   template <typename T>
   void fuzz(std::uint64_t seed, unsigned values, tally& t)
   {
      pb_rng r{seed};
      for (unsigned i = 0; i < values; ++i)
      {
         const std::vector<char> b = psio::to_protobuf(pb_random<T>(r));
         // Every truncation.
         for (std::size_t n = 0; n <= b.size(); ++n)
            probe<T>(std::vector<char>(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n)), t);
         // Every byte replaced.
         std::vector<char> c = b;
         for (std::size_t at = 0; at < b.size(); ++at)
         {
            for (int k = 0; k < 3; ++k)
            {
               c[at] = static_cast<char>(r.below(256));
               probe<T>(c, t);
            }
            c[at] = static_cast<char>(b[at] ^ 0x80);  // the continuation bit
            probe<T>(c, t);
            c[at] = b[at];
         }
         // Random corruptions of several bytes.
         for (int k = 0; k < 60; ++k)
         {
            c = b;
            for (std::uint64_t e = 0, edits = 1 + r.below(4); e < edits; ++e)
               switch (r.below(4))
               {
                  case 0:
                     if (!c.empty())
                        c[r.below(c.size())] = static_cast<char>(r.below(256));
                     break;
                  case 1:
                     c.insert(c.begin() + static_cast<std::ptrdiff_t>(r.below(c.size() + 1)),
                              static_cast<char>(r.below(256)));
                     break;
                  case 2:
                     if (!c.empty())
                        c.erase(c.begin() + static_cast<std::ptrdiff_t>(r.below(c.size())));
                     break;
                  default:
                     c.insert(c.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(r.below(b.size() + 1)));
               }
            probe<T>(c, t);
         }
      }
      // Random bytes (biased to small tags and lengths).
      for (unsigned i = 0; i < values * 40; ++i)
      {
         std::vector<char> c(r.below(48));
         for (char& x : c)
            x = static_cast<char>(r.chance(60) ? r.below(32) : r.below(256));
         probe<T>(c, t);
      }
   }

   template <typename T>
   double seconds(const std::vector<char>& bytes, bool* accepted = nullptr)
   {
      const auto begin = std::chrono::steady_clock::now();
      T          v{};
      const bool ok = psio::protobuf::decode_checked(std::span<const char>{bytes}, v).ok();
      if (accepted)
         *accepted = ok;
      return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
   }
}  // namespace

TEST_CASE("protobuf fuzz: truncations, replaced bytes, corruptions, random bytes", "[protobuf][fuzz]")
{
   tally t;
   fuzz<PbPoint>(1, 40, t);
   fuzz<PbScalars>(2, 60, t);
   fuzz<PbEncodings>(3, 60, t);
   fuzz<PbText>(4, 60, t);
   fuzz<PbOptionals>(5, 60, t);
   fuzz<PbRepeated>(6, 60, t);
   fuzz<PbMaps>(7, 60, t);
   fuzz<PbChoice>(8, 60, t);
   fuzz<PbRenumbered>(9, 40, t);
   fuzz<PbTree>(10, 60, t);
   fuzz<PbOrder>(11, 60, t);
   WARN("protobuf: " << t.inputs << " inputs, " << t.accepted << " accepted");
   CHECK(t.inputs > 300000);
   CHECK(t.accepted > 30000);
}

TEST_CASE("protobuf fuzz: work and memory are bounded by the input", "[protobuf][fuzz]")
{
   bool ok = true;
   // A length that claims 2 GiB, for every length-delimited kind: refused
   // at the length, nothing reserved.
   for (unsigned char tag : {0x0a, 0x12, 0x1a, 0x22, 0x2a, 0x32, 0x3a, 0x42, 0x4a})
   {
      const std::vector<char> claim{static_cast<char>(tag), '\xff', '\xff', '\xff', '\xff', '\x07', 1};
      CHECK(seconds<PbRepeated>(claim, &ok) < 0.01);
      CHECK(!ok);
      CHECK(seconds<PbOrder>(claim, &ok) < 0.01);
      CHECK(!ok);
      CHECK(seconds<PbMaps>(claim, &ok) < 0.01);
      CHECK(!ok);
   }
   // 8 MB of each: unknown varints, empty groups, nested groups, empty
   // records of a list, map entries, one-byte packed lists. Linear.
   const auto repeated = [](std::initializer_list<unsigned char> piece, std::size_t count) {
      std::vector<char> out;
      out.reserve(piece.size() * count);
      for (std::size_t i = 0; i < count; ++i)
         for (unsigned char b : piece)
            out.push_back(static_cast<char>(b));
      return out;
   };
   CHECK(seconds<PbRepeated>(repeated({0xf8, 0x07, 0x01}, 2'000'000), &ok) < 5.0);
   CHECK(ok);
   CHECK(seconds<PbRepeated>(repeated({0xfb, 0x07, 0xfc, 0x07}, 2'000'000), &ok) < 5.0);
   CHECK(ok);
   CHECK(seconds<PbRepeated>(repeated({0xfb, 0x07}, 4'000'000), &ok) < 5.0);
   CHECK(!ok);  // nested too deeply, at the 101st
   CHECK(seconds<PbRepeated>(repeated({0x32, 0x00}, 4'000'000), &ok) < 10.0);
   CHECK(ok);   // four million empty points: 8 bytes each from 2
   CHECK(seconds<PbMaps>(repeated({0x0a, 0x00}, 4'000'000), &ok) < 10.0);
   CHECK(ok);   // the same key four million times: one entry
   CHECK(seconds<PbRepeated>(repeated({0x0a, 0x01, 0x01}, 2'000'000), &ok) < 10.0);
   CHECK(ok);
   CHECK(seconds<PbTree>(repeated({0x12, 0x00}, 4'000'000), &ok) < 20.0);
   CHECK(ok);
}
