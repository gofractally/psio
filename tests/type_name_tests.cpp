// libraries/psio/cpp/tests/type_name_tests.cpp
//
// Round-trip + v1↔v3 byte-parity for compress_name / get_type_name.

#include <psio/compress_name.hpp>
#include <psio/get_type_name.hpp>

#include <psio1/compress_name.hpp>
#include <psio1/get_type_name.hpp>

#include <catch.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace {
   constexpr std::string_view kMethods[] = {
      "transfer", "issue", "bid", "settle", "publish",
      "begin", "commit", "abort",
      "open", "close", "read", "write",
      "ack",
   };

}  // namespace

struct ReflectMe { int x; };
PSIO_REFLECT(ReflectMe, x)

TEST_CASE("compress_name v1↔v3 byte-parity", "[compress_name][parity]")
{
   for (auto m : kMethods) {
      INFO("method=" << m);
      const auto v1 = psio1::detail::method_to_number(m);
      const auto v3 = psio::detail::method_to_number(m);
      REQUIRE(v1 == v3);
      REQUIRE(psio1::detail::number_to_method(v1)
              == psio::detail::number_to_method(v3));
      REQUIRE(psio1::detail::is_hash_name(v1)
              == psio::detail::is_hash_name(v3));
   }
}

TEST_CASE("compress_name round-trips short method names",
          "[compress_name][roundtrip]")
{
   for (auto m : kMethods) {
      INFO("method=" << m);
      const auto u = psio::detail::method_to_number(m);
      // Some long inputs may not compress and use the hash fallback;
      // compressed inputs round-trip exactly.
      if (!psio::detail::is_hash_name(u))
         REQUIRE(psio::detail::number_to_method(u) == m);
   }
}

TEST_CASE("get_type_name primitives", "[type_name]")
{
   REQUIRE(std::string_view(psio::get_type_name<bool>())     == "bool");
   REQUIRE(std::string_view(psio::get_type_name<std::int8_t>())  == "int8");
   REQUIRE(std::string_view(psio::get_type_name<std::uint32_t>())== "uint32");
   REQUIRE(std::string_view(psio::get_type_name<std::int64_t>()) == "int64");
   REQUIRE(std::string_view(psio::get_type_name<float>())    == "float32");
   REQUIRE(std::string_view(psio::get_type_name<double>())   == "double");
   REQUIRE(std::string_view(psio::get_type_name<std::string>()) == "string");
}

TEST_CASE("get_type_name composites", "[type_name]")
{
   REQUIRE(std::string_view(psio::get_type_name<std::vector<std::uint32_t>>())
           == "uint32[]");
   REQUIRE(std::string_view(psio::get_type_name<std::optional<std::int64_t>>())
           == "int64?");
   REQUIRE(std::string_view(psio::get_type_name<std::array<std::uint8_t, 32>>())
           == "uint8[32]");

   // tuple_<T1>_<T2>...
   constexpr auto t = psio::get_type_name<std::tuple<std::int32_t, std::string>>();
   REQUIRE(std::string_view(t) == "tuple_int32_string");

   constexpr auto v = psio::get_type_name<std::variant<std::int32_t, double>>();
   REQUIRE(std::string_view(v) == "variant_int32_double");
}

TEST_CASE("get_type_name reflected types use psio::reflect<T>::name",
          "[type_name][reflected]")
{
   STATIC_REQUIRE(psio::is_reflected_v<ReflectMe>);
   REQUIRE(std::string_view(psio::get_type_name<ReflectMe>())
           == "ReflectMe");
}

TEST_CASE("get_type_name chrono", "[type_name][chrono]")
{
   using dur_ms = std::chrono::milliseconds;
   REQUIRE(std::string_view(psio::get_type_name<dur_ms>()) == "duration");

   using sec_tp = std::chrono::time_point<
      std::chrono::system_clock,
      std::chrono::duration<std::int64_t>>;
   REQUIRE(std::string_view(psio::get_type_name<sec_tp>()) == "TimePointSec");

   using usec_tp = std::chrono::time_point<
      std::chrono::system_clock,
      std::chrono::duration<std::int64_t, std::micro>>;
   REQUIRE(std::string_view(psio::get_type_name<usec_tp>()) == "TimePointUSec");
}

TEST_CASE("hash_name / is_compressed_name", "[type_name][hash_name]")
{
   constexpr auto u = psio::hash_name("transfer");
   REQUIRE(u == psio::detail::method_to_number("transfer"));
   REQUIRE(psio::is_compressed_name(u)
           == !psio::detail::is_hash_name(u));
}
