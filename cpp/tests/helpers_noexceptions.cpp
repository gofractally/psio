// The per-format helpers with exceptions and RTTI disabled (how a
// WebAssembly guest builds them). Bad input is found with validate_F, which
// reports a status; the checked decode from_F does not return on it — it
// calls psio::codec_abort, which here is std::abort. Run with the argument
// "abort" the program decodes a truncated frame and must die.
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

struct Rec
{
   std::uint32_t              id = 0;
   std::string                name;
   std::vector<std::uint32_t> xs;

   friend bool operator==(const Rec&, const Rec&) = default;
};
PSIO_REFLECT(Rec, id, name, xs)

int main(int argc, char** argv)
{
   const Rec  sample{7, "alice", {1, 2, 3}};
   const auto bytes = psio::to_pssz(sample);
   const std::span<const char> in{bytes};
   const auto                  cut = in.first(in.size() / 2);

   if (argc > 1 && std::strcmp(argv[1], "abort") == 0)
   {
      (void)psio::from_pssz<Rec>(cut);
      return 0;  // not reached: the test expects the abort
   }

   if (psio::pssz_size(sample) != bytes.size()) return 1;
   if (!psio::validate_pssz<Rec>(in).ok()) return 2;
   if (!(psio::from_pssz<Rec>(in) == sample)) return 3;
   if (!(psio::from_pssz<Rec>(psio::trusted{in}) == sample)) return 4;

   // The no-exceptions pattern: ask, then decode what was accepted.
   auto st = psio::validate_pssz<Rec>(cut);
   if (st.ok()) return 5;
   if (st.error().what.empty()) return 6;

   auto copy = bytes;
   psio::pssz_view<Rec> mv{copy};
   if (!mv.get<0>().set(9u)) return 7;
   if (psio::pssz_view<const Rec>{copy}.get<0>() != 9u) return 8;

   psio::shared_pssz<Rec> shared{std::move(copy)};
   psio::shared_pssz<Rec> other = shared;
   if (psio::bytes(shared).data() != psio::bytes(other).data()) return 9;
   if (psio::from_pssz<Rec>(psio::bytes(other)).id != 9u) return 10;

   const std::string text = psio::to_json(sample);
   if (!psio::validate_json<Rec>(text).ok()) return 11;
   if (!(psio::from_json<Rec>(text) == sample)) return 12;
   if (psio::validate_json<Rec>(R"({"id":7)").ok()) return 13;

   const auto key = psio::to_key(std::uint32_t{5});
   if (psio::from_key<std::uint32_t>(key) != 5u) return 14;
   if (psio::validate_key<std::uint32_t>(std::span<const char>{key}.first(3)).ok()) return 15;
   return 0;
}
