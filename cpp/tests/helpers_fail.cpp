// Misuse of the per-format helpers (psio/helpers.hpp) must not compile:
// each case names a helper its format does not have. With no case selected
// the file compiles.
#include <psio/json.hpp>
#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct Rec
{
   std::uint32_t id = 0;
   std::string   name;
};
PSIO_REFLECT(Rec, id, name)

int main()
{
   Rec               rec{1, "a"};
   std::vector<char> bytes = psio::to_pssz(rec);
   psio::pssz_view<const Rec> read{bytes};
   psio::pssz_view<Rec>       write{bytes};
   (void)read;
   (void)write;

#if defined(HELPERS_FAIL_JSON_VIEW)
   // JSON has no view.
   psio::json_view<const Rec> v{bytes};
#elif defined(HELPERS_FAIL_SSZ_MUTABLE_VIEW)
   // SSZ has a read view only.
   psio::ssz_view<Rec> v{bytes};
#elif defined(HELPERS_FAIL_WIT_SINK)
   // wit encodes into a fresh buffer only.
   psio::to_wit(rec, bytes);
#elif defined(HELPERS_FAIL_JSON_VECTOR_SINK)
   // JSON text is appended to a std::string.
   psio::to_json(rec, bytes);
#elif defined(HELPERS_FAIL_UNCHECKED_BYTES)
   // Skipping validation is spelled psio::trusted{bytes}, never implied.
   std::span<const char> in{bytes};
   psio::trusted         t = in;
#elif defined(HELPERS_FAIL_CONST_VIEW_SET)
   // A const view has no write.
   read.get<0>().set(2u);
#elif defined(HELPERS_FAIL_SHARED_MUTABLE)
   // Shared bytes are immutable: no mutable view of them.
   psio::shared_pssz<Rec> shared{std::move(bytes)};
   auto                   mv = psio::as_mutable_view(shared);
#endif
   return 0;
}
