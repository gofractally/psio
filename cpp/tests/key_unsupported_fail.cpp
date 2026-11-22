// Must not compile: types with no key encoding are rejected with a message
// naming the reason (see CMakeLists.txt for the expected diagnostics).
#include <psio/key.hpp>

#include <chrono>
#include <utility>

int main()
{
#if defined(KEY_FAIL_PAIR)
   auto k = psio::encode(psio::key{}, std::pair<int, int>{1, 2});
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
#endif
   return static_cast<int>(k.size());
}
