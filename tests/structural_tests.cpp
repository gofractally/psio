// Tests for psio/structural.hpp — PSIO_PACKAGE + PSIO_INTERFACE.
//
// Mirrors the pattern WASI 2.3 host bindings use: a package declaration
// followed by one or more interfaces whose Tag is the user-authored
// struct of static stubs.

#include <psio/structural.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

// ─── Sample shapes used across tests ─────────────────────────────────

namespace test_structural
{

   struct datetime
   {
      std::uint64_t seconds     = 0;
      std::uint32_t nanoseconds = 0;
   };

   struct empty_value
   {
   };

}  // namespace test_structural

using test_structural::datetime;
using test_structural::empty_value;

// ─── Interface anchors at global scope ───────────────────────────────
//
// PSIO_INTERFACE uses `::NAME` qualification, so the anchors must live
// at global scope (or have a global `using` alias).

struct test_clocks_wall
{
   static datetime now();
   static datetime resolution();
};

struct test_clocks_monotonic
{
   static std::uint64_t now();
   static std::uint64_t resolution();
   static empty_value   subscribe_instant(std::uint64_t when);
   static empty_value   subscribe_duration(std::uint64_t when);
};

struct test_random
{
   static std::uint64_t get_random_u64();
};

// ─── Package + interface registrations ───────────────────────────────

PSIO_PACKAGE(test_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(test_clocks)

PSIO_INTERFACE(test_clocks_wall,
               types(datetime),
               funcs(func(now), func(resolution)))

PSIO_INTERFACE(test_clocks_monotonic,
               types(),
               funcs(func(now),
                     func(resolution),
                     func(subscribe_instant, when),
                     func(subscribe_duration, when)))

PSIO_PACKAGE(test_random_pkg, "1.0.0");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(test_random_pkg)

// Interface with a single bare-identifier entry (no func() wrapper) —
// the WASI bindings use this form for parameterless functions.
PSIO_INTERFACE(test_random,
               types(),
               funcs(get_random_u64))

// ─── Tests ───────────────────────────────────────────────────────────

TEST_CASE("structural: package_info carries name and version", "[structural]")
{
   using clocks_pkg = PSIO_PACKAGE_TYPE_(test_clocks);
   STATIC_REQUIRE(clocks_pkg::name == std::string_view{"test_clocks"});
   STATIC_REQUIRE(clocks_pkg::version == std::string_view{"0.2.3"});

   using random_pkg = PSIO_PACKAGE_TYPE_(test_random_pkg);
   STATIC_REQUIRE(random_pkg::name == std::string_view{"test_random_pkg"});
   STATIC_REQUIRE(random_pkg::version == std::string_view{"1.0.0"});

   STATIC_REQUIRE(!std::is_same_v<clocks_pkg, random_pkg>);
}

TEST_CASE("structural: interface_info exposes name, package, types, funcs",
          "[structural]")
{
   using info = psio::detail::interface_info<test_clocks_wall>;

   STATIC_REQUIRE(info::name == std::string_view{"test_clocks_wall"});
   STATIC_REQUIRE(std::is_same_v<info::package, PSIO_PACKAGE_TYPE_(test_clocks)>);

   // types(datetime) → tuple<datetime>
   STATIC_REQUIRE(std::is_same_v<info::types, std::tuple<datetime>>);

   // funcs(func(now), func(resolution)) → 2-element tuple of fn-ptr types
   STATIC_REQUIRE(std::tuple_size_v<info::func_types> == 2);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, info::func_types>,
                                 decltype(&test_clocks_wall::now)>);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<1, info::func_types>,
                                 decltype(&test_clocks_wall::resolution)>);

   // func_names is a fixed-size array of string_views.
   STATIC_REQUIRE(info::func_names.size() == 2);
   CHECK(info::func_names[0] == std::string_view{"now"});
   CHECK(info::func_names[1] == std::string_view{"resolution"});
}

TEST_CASE("structural: param_names captures arg names per func", "[structural]")
{
   using info = psio::detail::interface_info<test_clocks_monotonic>;

   STATIC_REQUIRE(info::func_names.size() == 4);
   CHECK(info::func_names[0] == std::string_view{"now"});
   CHECK(info::func_names[1] == std::string_view{"resolution"});
   CHECK(info::func_names[2] == std::string_view{"subscribe_instant"});
   CHECK(info::func_names[3] == std::string_view{"subscribe_duration"});

   // now / resolution: 0 params; subscribe_*: 1 param named "when".
   CHECK(info::param_names[0].size() == 0);
   CHECK(info::param_names[1].size() == 0);
   CHECK(info::param_names[2].size() == 1);
   CHECK(info::param_names[3].size() == 1);

   CHECK(std::string_view{*info::param_names[2].begin()} ==
         std::string_view{"when"});
   CHECK(std::string_view{*info::param_names[3].begin()} ==
         std::string_view{"when"});
}

TEST_CASE("structural: bare-identifier funcs entry works", "[structural]")
{
   using info = psio::detail::interface_info<test_random>;

   STATIC_REQUIRE(info::func_names.size() == 1);
   CHECK(info::func_names[0] == std::string_view{"get_random_u64"});
   CHECK(info::param_names[0].size() == 0);
}

TEST_CASE("structural: interface_of reverse-maps types to their tag",
          "[structural]")
{
   STATIC_REQUIRE(
       std::is_same_v<typename psio::interface_of<datetime>::type,
                      test_clocks_wall>);
}

TEST_CASE("structural: empty types() and zero-arg funcs compile", "[structural]")
{
   // test_random has empty types() and a single zero-arg func — exercised
   // above. This case proves the empty-types branch and zero-arg
   // PSIO_IFACE_QUOTED_ARGS branch don't blow up at preprocessor time.
   using info = psio::detail::interface_info<test_random>;
   STATIC_REQUIRE(std::tuple_size_v<info::types> == 0);
}

// =====================================================================
// PSIO_USE / PSIO_WORLD / PSIO_HOST_MODULE
// =====================================================================

PSIO_USE(test_clocks, wall_clock, "0.2.3")
PSIO_USE(test_clocks, monotonic_clock, "0.2.3")

TEST_CASE("structural: PSIO_USE specializes use_info", "[structural][use]")
{
   using tag = psio::detail::PSIO_USE_TAG_(test_clocks, wall_clock);
   using info = psio::detail::use_info<tag>;
   STATIC_REQUIRE(info::package        == std::string_view{"test_clocks"});
   STATIC_REQUIRE(info::interface_name == std::string_view{"wall_clock"});
   STATIC_REQUIRE(info::version        == std::string_view{"0.2.3"});
}

PSIO_WORLD(test_clocks_world,
           imports(psio::detail::PSIO_USE_TAG_(test_clocks, wall_clock),
                   psio::detail::PSIO_USE_TAG_(test_clocks, monotonic_clock)),
           exports(test_random))

TEST_CASE("structural: PSIO_WORLD specializes world_info", "[structural][world]")
{
   using info = psio::detail::world_info<
      psio::detail::test_clocks_world_world_tag>;

   STATIC_REQUIRE(info::name == std::string_view{"test_clocks_world"});

   STATIC_REQUIRE(std::tuple_size_v<info::imports> == 2);
   STATIC_REQUIRE(std::tuple_size_v<info::exports> == 1);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, info::exports>,
                                 test_random>);
}

// PSIO_HOST_MODULE — bind a host impl to an interface.

struct test_random_impl
{
   std::uint64_t get_random_u64() { return 42; }
};

PSIO_HOST_MODULE(test_random_impl,
                 interface(test_random, get_random_u64))

TEST_CASE("structural: PSIO_HOST_MODULE specializes iface_impl + impl_of",
          "[structural][host_module]")
{
   using ii = psio::detail::iface_impl<test_random_impl, test_random>;

   STATIC_REQUIRE(std::is_same_v<ii::host, test_random_impl>);
   STATIC_REQUIRE(std::is_same_v<ii::tag,  test_random>);
   STATIC_REQUIRE(std::tuple_size_v<decltype(ii::methods)> == 1);
   STATIC_REQUIRE(ii::names.size() == 1);
   CHECK(ii::names[0] == std::string_view{"get_random_u64"});

   // impl_of reverse map.
   using tags = typename psio::impl_of<test_random_impl>::type;
   STATIC_REQUIRE(std::tuple_size_v<tags> == 1);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, tags>, test_random>);

   // impl_info aggregates iface_impls.
   using ifaces = typename psio::detail::impl_info<test_random_impl>::interfaces;
   STATIC_REQUIRE(std::tuple_size_v<ifaces> == 1);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, ifaces>, ii>);
}
