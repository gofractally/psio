// Must not compile: types with no key encoding are rejected with a message
// naming the reason (see CMakeLists.txt for the expected diagnostics).
#include <psio/key.hpp>

#include <chrono>
#include <map>
#include <utility>

int main()
{
#if defined(KEY_FAIL_MULTIMAP)
   // A std::pair is a value (the 2-tuple); a container that can hold two
   // entries with one key is not.
   auto k = psio::encode(psio::key{}, std::multimap<int, int>{{1, 2}});
#elif defined(KEY_FAIL_BITLIST)
   auto k = psio::encode(psio::key{}, psio::bitlist<8>{});
#elif defined(KEY_FAIL_STEADY_CLOCK)
   // A monotonic time point is a value (its key is its tick count) but has
   // no date: the RFC 3339 codec does not accept it.
   auto k = psio::to_rfc3339(std::chrono::steady_clock::time_point{});
#elif defined(KEY_FAIL_FLOAT_REP)
   auto k = psio::encode(psio::key{}, std::chrono::duration<double>{1.5});
#elif defined(KEY_FAIL_PREFIX_ARITY)
   auto k = psio::key_prefix<std::tuple<int, int>>(1, 2, 3);
#elif defined(KEY_FAIL_PREFIX_TYPE)
   auto k = psio::key_prefix<std::tuple<int, std::string>>(1, 2);
#elif defined(KEY_FAIL_DECODE_RAW_KEY)
   // A run of raw key bytes does not say where it ends: write-only.
   std::vector<char> k;
   auto v = psio::from_key<std::tuple<psio::raw_key, int>>(k);
#elif defined(KEY_FAIL_DECODE_STRING_VIEW)
   std::vector<char> k;
   auto v = psio::validate_key<std::tuple<int, std::string_view>>(k);
#endif
   return static_cast<int>(k.size());
}
