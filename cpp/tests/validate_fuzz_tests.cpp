// Fuzz smoke test for psio::validate (§9 step-2 follow-up).
//
// The design promises `validate` is a total function on any byte span:
// it never crashes, it never throws, it never reads past the span. This
// harness feeds truncated / byte-flipped / garbage spans to each
// format's validate and asserts the call returns cleanly (either ok or
// a codec_error, never a crash / UB / exception escape).
//
// Not a cryptographic fuzzer — a determinism check that the noexcept
// contract holds. Deterministic PRNG + fixed fixtures so failures are
// reproducible.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace fuzz {

   struct Blob
   {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO_REFLECT(Blob, version, payload, note)

   template <typename Fmt>
   auto make_good_bytes(Fmt fmt)
   {
      Blob in{42, {1, 2, 3}, "hi"};
      return psio::encode(fmt, in);
   }

   // Shuffle: flip every N-th byte via a seeded PRNG.
   std::vector<char> corrupt(std::vector<char> b, std::uint32_t seed,
                             std::size_t bit_flips = 32)
   {
      if (b.empty())
         return b;
      std::mt19937 rng(seed);
      std::uniform_int_distribution<std::size_t> pos_d(0, b.size() - 1);
      std::uniform_int_distribution<int>         bit_d(0, 7);
      for (std::size_t i = 0; i < bit_flips; ++i)
         b[pos_d(rng)] ^= static_cast<char>(1u << bit_d(rng));
      return b;
   }

}  // namespace fuzz

#define FUZZ_FORMAT(FmtName, FmtValue)                                  \
   TEST_CASE("fuzz [" #FmtName "]: validate is total on corrupt input", \
             "[fuzz][" #FmtName "]")                                    \
   {                                                                    \
      auto good = fuzz::make_good_bytes(FmtValue);                      \
      for (std::uint32_t seed = 1; seed <= 64; ++seed)                  \
      {                                                                 \
         auto bad = fuzz::corrupt(good, seed);                          \
         auto st = psio::validate<fuzz::Blob>(                         \
            FmtValue, std::span<const char>{bad});                      \
         (void)st;  /* must not crash, may return ok OR error */        \
      }                                                                 \
      for (std::size_t drop = 1; drop < good.size(); ++drop)            \
      {                                                                 \
         auto trunc = std::vector<char>(good.begin(),                   \
                                         good.end() - drop);            \
         auto st = psio::validate<fuzz::Blob>(                         \
            FmtValue, std::span<const char>{trunc});                    \
         (void)st;                                                      \
      }                                                                 \
      SUCCEED("validate returned without crashing on all fuzz inputs"); \
   }

FUZZ_FORMAT(ssz,     ::psio::ssz{})
FUZZ_FORMAT(pssz32,  ::psio::pssz32{})
FUZZ_FORMAT(frac32,  ::psio::frac32{})
FUZZ_FORMAT(bin,     ::psio::bin{})
FUZZ_FORMAT(borsh,   ::psio::borsh{})
FUZZ_FORMAT(bincode, ::psio::bincode{})
FUZZ_FORMAT(avro,    ::psio::avro{})
FUZZ_FORMAT(key,     ::psio::key{})

#undef FUZZ_FORMAT

// JSON returns std::string; specialize the fuzz path to convert to
// vector<char> so the shared corrupt/truncate helpers apply.
TEST_CASE("fuzz [json]: validate is total on corrupt input",
          "[fuzz][json]")
{
   fuzz::Blob in{42, {1, 2, 3}, "hi"};
   auto       s = psio::encode(psio::json{}, in);
   std::vector<char> good(s.begin(), s.end());

   for (std::uint32_t seed = 1; seed <= 64; ++seed)
   {
      auto bad = fuzz::corrupt(good, seed);
      auto st = psio::validate<fuzz::Blob>(
         psio::json{}, std::span<const char>{bad});
      (void)st;
   }
   for (std::size_t drop = 1; drop < good.size(); ++drop)
   {
      std::vector<char> trunc(good.begin(), good.end() - drop);
      auto st = psio::validate<fuzz::Blob>(
         psio::json{}, std::span<const char>{trunc});
      (void)st;
   }
   SUCCEED("json validate returned without crashing on all fuzz inputs");
}
