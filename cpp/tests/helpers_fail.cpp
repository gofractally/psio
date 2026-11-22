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
   psio::pssz_view<Rec>         read{bytes};
   psio::pssz_view<const Rec>   also_read{bytes};
   psio::pssz_mutable_view<Rec> write{bytes};
   psio::view<const Rec, psio::pssz> generic = read;
   (void)also_read;
   (void)write;
   (void)generic;

#if defined(HELPERS_FAIL_JSON_VIEW)
   // JSON has no view.
   psio::json_view<Rec> v{bytes};
#elif defined(HELPERS_FAIL_SSZ_MUTABLE_VIEW)
   // SSZ has a read view only.
   psio::ssz_mutable_view<Rec> v{bytes};
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
#elif defined(HELPERS_FAIL_VIEW_SET)
   // A view has no write.
   read.get<0>().set(2u);
#elif defined(HELPERS_FAIL_CONST_VIEW_SET)
   // Nor under its const spelling.
   generic.set(rec);
#elif defined(HELPERS_FAIL_SHARED_MUTABLE)
   // Shared bytes are immutable: no mutable view of them.
   psio::shared_pssz<Rec> shared{std::move(bytes)};
   auto                   mv = psio::as_mutable_view(shared);
#elif defined(HELPERS_FAIL_MUTABLE_FROM_CONST_BYTES)
   // A mutable view needs mutable bytes.
   const std::vector<char>      frozen = bytes;
   psio::pssz_mutable_view<Rec> mv{frozen};
#elif defined(HELPERS_FAIL_MUTABLE_FROM_CONST_SPAN)
   std::span<const char>        frozen{bytes};
   psio::pssz_mutable_view<Rec> mv{frozen};
#elif defined(HELPERS_FAIL_MUTABLE_FROM_CONST_BUFFER)
   // A const buffer gives no mutable view.
   const psio::pssz_buffer<Rec> frozen{std::move(bytes)};
   auto                         mv = psio::as_mutable_view(frozen);
#elif defined(HELPERS_FAIL_MUTABLE_FROM_VIEW)
   // A read-only view does not turn into a mutable one.
   psio::pssz_mutable_view<Rec> mv{read};
#elif defined(HELPERS_FAIL_MUTABLE_FROM_VIEW_BYTES)
   // Nor do the bytes it hands out.
   psio::pssz_mutable_view<Rec> mv{psio::bytes(read)};
#elif defined(HELPERS_FAIL_VIEW_ASSIGNED_TO_MUTABLE)
   psio::pssz_mutable_view<Rec> mv = read;
#elif defined(HELPERS_FAIL_MUTABLE_VIEW_OF_CONST)
   // The named mutable view of a const type.
   psio::pssz_mutable_view<const Rec> mv{bytes};
#elif defined(HELPERS_FAIL_GENERIC_MUTABLE_VIEW_OF_CONST)
   // The generic one.
   psio::mutable_view<const Rec, psio::pssz> mv{bytes};
#elif defined(HELPERS_FAIL_VIEW_MUT_BORROW)
   // The mutable-borrow storage kind does not make a view writable.
   psio::view<Rec, psio::pssz, psio::storage::mut_borrow> v{bytes};
#elif defined(HELPERS_FAIL_VIEW_OWNING)
   // A view borrows: the owning storage kind names a buffer, not a view.
   psio::view<Rec, psio::pssz, psio::storage::owning> v{bytes};
#elif defined(HELPERS_FAIL_VIEW_SHARED)
   psio::view<Rec, psio::pssz, psio::storage::shared> v{bytes};
#elif defined(HELPERS_FAIL_VIEW_OWNING_SCALAR)
   // The generic (non-record) views too.
   psio::view<std::uint32_t, psio::ssz, psio::storage::owning> v{bytes};
#elif defined(HELPERS_FAIL_VIEW_SHARED_CONST)
   // ...and through the const spelling.
   psio::view<const Rec, psio::pssz, psio::storage::shared> v{bytes};
#endif
   return 0;
}
