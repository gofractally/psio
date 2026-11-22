// Must not compile: containers that are not PSIO values, and maps / sets
// whose key type cannot order their entries, are rejected with a message
// naming the reason (see CMakeLists.txt for the expected diagnostics).
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A type whose key bytes are its adapter's: no schema-visible order.
struct Opaque
{
   std::uint32_t v = 0;
   friend bool   operator<(const Opaque& a, const Opaque& b) { return a.v < b.v; }
};
PSIO_REFLECT(Opaque, v)
struct OpaqueKey
{
   static std::size_t packsize(const Opaque&) { return 4; }
   static void        encode(const Opaque& o, std::vector<char>& out)
   {
      for (int i = 3; i >= 0; --i)
         out.push_back(static_cast<char>(o.v >> (8 * i)));
   }
   static psio::codec_status validate(std::span<const char> bytes)
   {
      return bytes.size() >= 4 ? psio::codec_ok() : psio::codec_fail("short", 0, "key");
   }
   static psio::codec_status validate_strict(std::span<const char> bytes)
   {
      return validate(bytes);
   }
   static Opaque decode(std::span<const char>) { return {}; }
};
PSIO_ADAPTER(Opaque, psio::sortable_binary_category, OpaqueKey)

int main()
{
#if defined(ASSOC_FAIL_PSSZ_MULTIMAP)
   auto out = psio::encode(psio::pssz{}, std::multimap<int, int>{{1, 2}});
#elif defined(ASSOC_FAIL_PSSZ_MULTISET)
   auto out = psio::encode(psio::pssz{}, std::multiset<int>{1});
#elif defined(ASSOC_FAIL_PSSZ_UNORDERED_MULTIMAP)
   auto out = psio::decode<std::unordered_multimap<int, int>>(psio::pssz{},
                                                             std::span<const char>{});
#elif defined(ASSOC_FAIL_PSSZ_VALIDATE_MULTISET)
   auto status = psio::validate<std::unordered_multiset<int>>(psio::pssz{},
                                                             std::span<const char>{});
   std::string out(status.ok() ? 1 : 0, 'x');
#elif defined(ASSOC_FAIL_JSON_MULTIMAP)
   auto out = psio::encode(psio::json{}, std::multimap<std::string, int>{});
#elif defined(ASSOC_FAIL_IJSON_MULTISET)
   auto out = psio::decode<std::multiset<int>>(psio::ijson{}, std::span<const char>{});
#elif defined(ASSOC_FAIL_KEY_MULTISET)
   auto out = psio::encode(psio::key{}, std::unordered_multiset<int>{});
#elif defined(ASSOC_FAIL_PSSZ_BITLIST_KEY)
   // bitlist has no key encoding.
   auto out = psio::encode(psio::pssz{}, std::map<psio::bitlist<8>, int>{});
#elif defined(ASSOC_FAIL_PSSZ_VALIDATE_BITLIST_SET)
   auto status = psio::validate<std::set<psio::bitlist<8>>>(psio::pssz{},
                                                            std::span<const char>{});
   std::string out(status.ok() ? 1 : 0, 'x');
#elif defined(ASSOC_FAIL_JSON_BITLIST_KEY)
   auto out = psio::encode(psio::json{}, std::set<std::vector<psio::bitlist<8>>>{});
#elif defined(ASSOC_FAIL_KEY_NESTED_KEY)
   // A map is a key only when its own key type orders its entries.
   auto out = psio::encode(psio::key{}, std::map<std::map<psio::bitlist<8>, int>, int>{});
#elif defined(ASSOC_FAIL_PSSZ_ADAPTER_KEY)
   auto out = psio::encode(psio::pssz{}, std::map<Opaque, int>{});
#elif defined(ASSOC_FAIL_JSON_ADAPTER_KEY)
   auto out = psio::decode<std::set<Opaque>>(psio::json{}, std::span<const char>{});
#else
   // Control: the same types are fine where no map / set orders by them.
   auto out = psio::encode(psio::key{}, Opaque{7});
#endif
   return static_cast<int>(out.size());
}
