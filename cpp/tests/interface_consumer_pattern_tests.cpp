// Regression: declaration patterns real consumers use must keep compiling
// unchanged and keep producing byte-identical output.
//
// fixtures/interface_consumer_pattern.golden.txt was captured from
// interface_consumer_pattern.hpp BEFORE PSIO_INTERFACE / PSIO_PACKAGE were
// reworked to register through any namespace (commit 1adb3f2).

#include "interface_consumer_pattern.hpp"

#include <catch.hpp>

#include <fstream>
#include <sstream>

TEST_CASE("consumer pattern: WASI-style declarations produce the pinned text",
          "[structural][regression]")
{
   std::ifstream in{PSIO_CONSUMER_PATTERN_GOLDEN, std::ios::binary};
   REQUIRE(in.good());
   std::ostringstream golden;
   golden << in.rdbuf();
   CHECK(consumer_pattern::dump() == golden.str());
}

TEST_CASE("consumer pattern: registry contents", "[structural][regression]")
{
   using clock = psio::detail::interface_info<wasi_clocks_monotonic_clock>;
   STATIC_REQUIRE(clock::name == std::string_view{"wasi_clocks_monotonic_clock"});
   STATIC_REQUIRE(std::is_same_v<clock::package, PSIO_PACKAGE_TYPE_(wasi_clocks)>);
   STATIC_REQUIRE(clock::package::version == std::string_view{"0.2.3"});
   STATIC_REQUIRE(std::is_same_v<clock::types, std::tuple<>>);
   STATIC_REQUIRE(std::tuple_size_v<clock::func_types> == 3);
   STATIC_REQUIRE(clock::method<2>() == &wasi_clocks_monotonic_clock::subscribe_instant);
   CHECK(std::string_view{*clock::param_names[2].begin()} == "when");

   using poll = psio::detail::interface_info<wasi_io_poll>;
   STATIC_REQUIRE(std::is_same_v<poll::package, PSIO_PACKAGE_TYPE_(wasi_io)>);
   STATIC_REQUIRE(std::is_same_v<poll::types, std::tuple<pollable>>);
   STATIC_REQUIRE(std::is_same_v<typename psio::interface_of<pollable>::type, wasi_io_poll>);
   STATIC_REQUIRE(std::is_same_v<typename psio::interface_of<input_stream>::type, wasi_io_streams>);
   STATIC_REQUIRE(psio::package_of<pollable>::name == std::string_view{"wasi_io"});

   using impl = psio::detail::iface_impl<wasi_clock_host, wasi_clocks_monotonic_clock>;
   STATIC_REQUIRE(std::is_same_v<impl::tag, wasi_clocks_monotonic_clock>);
   STATIC_REQUIRE(std::get<0>(impl::methods) == &wasi_clock_host::now);
   STATIC_REQUIRE(impl::names.size() == 2);
   CHECK(impl::names[1] == "resolution");
   STATIC_REQUIRE(std::is_same_v<typename psio::impl_of<wasi_clock_host>::type,
                                 std::tuple<wasi_clocks_monotonic_clock>>);
}

namespace
{
   // The dispatcher consumers write: index and member pointer are template
   // VALUE arguments, the proxy is built from the dispatcher's own arguments.
   struct consumer_dispatch
   {
      int         account;
      std::string service;
      consumer_dispatch(int a, std::string s) : account(a), service(std::move(s)) {}

      template <std::size_t I, auto MemberPtr, typename... Args>
      auto call(Args&&... args)
      {
         using result = typename psio::method_traits<decltype(MemberPtr)>::result;
         last = service + "#" + std::to_string(I) + "/" + std::to_string(sizeof...(Args)) + "@" +
                std::to_string(account);
         same = false;
         if constexpr (std::is_same_v<decltype(MemberPtr),
                                      decltype(&wasi_clocks_monotonic_clock::subscribe_instant)>)
            same = MemberPtr == &wasi_clocks_monotonic_clock::subscribe_instant;
         ((void)args, ...);
         if constexpr (std::is_same_v<result, std::uint64_t>)
            return std::uint64_t{42};
         else
            return;
      }
      std::string last;
      bool        same = false;
   };
}  // namespace

TEST_CASE("consumer pattern: value-form dispatcher built as proxy_t{a, b}",
          "[structural][regression]")
{
   using proxy_t =
      psio::detail::interface_info<wasi_clocks_monotonic_clock>::proxy<consumer_dispatch>;
   proxy_t clock{7, "clocks"};
   CHECK(clock.now() == 42);
   CHECK(clock.last == "clocks#0/0@7");
   clock.subscribe_instant(std::uint64_t{5});
   CHECK(clock.last == "clocks#2/1@7");
   CHECK(clock.same);
}
