// Validation: feed production-shaped WASI 2.3 inputs through the v3
// WIT toolchain (wit_gen + wit_constexpr + wit_encode) and assert the
// shapes they produce.  This exercises real-world coverage that the
// synthetic fixtures don't:
//
//   - resource markers (pollable, input_stream — empty structs
//     deriving wit_resource, registered with PSIO_REFLECT(T) plus a
//     PSIO_INTERFACE on the resource itself for its methods),
//   - own<T> / borrow<T> as function param + return types,
//   - lists of borrows (list<borrow<pollable>>), the WASI poll signature,
//   - multi-interface packages with shared types crossing interface
//     boundaries.

#include <psio/wit_gen.hpp>
#include <psio/wit_constexpr.hpp>
#include <psio/wit_encode.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ─── wasi:clocks shapes ──────────────────────────────────────────────

namespace wasi23
{
   struct datetime
   {
      std::uint64_t seconds     = 0;
      std::uint32_t nanoseconds = 0;
   };
   PSIO_REFLECT(datetime, seconds, nanoseconds)

   // Resource marker — opaque handle exchanged across the canonical
   // ABI as a u32.  PSIO_REFLECT(pollable) with no fields is the v3
   // way to give the type a stable name without modelling internals.
   struct pollable : psio::wit_resource
   {
   };
   PSIO_REFLECT(pollable)
}  // namespace wasi23

using wasi23::datetime;
using wasi23::pollable;

// Interface anchors — global scope so PSIO_INTERFACE's `::NAME` works.

struct wasi_clocks_wall_clock
{
   static datetime now();
   static datetime resolution();
};

struct wasi_clocks_monotonic_clock
{
   static std::uint64_t       now();
   static std::uint64_t       resolution();
   static psio::own<pollable> subscribe_instant(std::uint64_t when);
   static psio::own<pollable> subscribe_duration(std::uint64_t when);
};

struct wasi_io_poll
{
   static bool                       pollable_ready(psio::borrow<pollable> self);
   static void                       pollable_block(psio::borrow<pollable> self);
   static std::vector<std::uint32_t> poll(
      std::vector<psio::borrow<pollable>> in);
};

PSIO_PACKAGE(wasi_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_clocks)

PSIO_INTERFACE(wasi_clocks_wall_clock,
               types(datetime),
               funcs(func(now), func(resolution)))

PSIO_INTERFACE(wasi_clocks_monotonic_clock,
               types(),
               funcs(func(now),
                     func(resolution),
                     func(subscribe_instant, when),
                     func(subscribe_duration, when)))

PSIO_PACKAGE(wasi_io, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_io)

PSIO_INTERFACE(wasi_io_poll,
               types(pollable),
               funcs(func(pollable_ready, self),
                     func(pollable_block, self),
                     func(poll, in)))

// ─── Tests ───────────────────────────────────────────────────────────

TEST_CASE("wasi shapes: wall-clock generates the expected WIT text",
          "[wit_wasi]")
{
   auto text = psio::generate_wit_text<wasi_clocks_wall_clock>(
      "wasi", "clocks", "0.2.3");

   const std::string expected =
      "package wasi:clocks@0.2.3;\n"
      "\n"
      "interface wasi-clocks-wall-clock {\n"
      "  record datetime {\n"
      "    seconds: u64,\n"
      "    nanoseconds: u32,\n"
      "  }\n"
      "\n"
      "  now: func() -> datetime;\n"
      "  resolution: func() -> datetime;\n"
      "}\n"
      "\n"
      "world wasi-clocks-wall-clock {\n"
      "  export wasi-clocks-wall-clock;\n"
      "}\n";

   CHECK(text == expected);
}

TEST_CASE("wasi shapes: monotonic-clock returns own<pollable>", "[wit_wasi]")
{
   auto text = psio::generate_wit_text<wasi_clocks_monotonic_clock>(
      "wasi", "clocks", "0.2.3");

   // pollable is a resource declared in wasi:io/poll.  When referenced
   // from a foreign interface the runtime generator currently emits
   // a record entry for it — that's a known limitation flagged by the
   // canonical-ABI follow-up; what we validate here is that own<>
   // wraps it correctly in function signatures.
   CHECK(text.find("subscribe-instant: func(when: u64) -> own<pollable>;") !=
         std::string::npos);
   CHECK(text.find("subscribe-duration: func(when: u64) -> own<pollable>;") !=
         std::string::npos);
}

TEST_CASE("wasi shapes: io/poll exercises borrow<T> + list<borrow<T>>",
          "[wit_wasi]")
{
   auto text = psio::generate_wit_text<wasi_io_poll>("wasi", "io", "0.2.3");

   CHECK(text.find("package wasi:io@0.2.3;") != std::string::npos);
   CHECK(text.find("resource pollable") != std::string::npos);
   CHECK(text.find("pollable-ready: func(self: borrow<pollable>) -> bool;") !=
         std::string::npos);
   CHECK(text.find("pollable-block: func(self: borrow<pollable>);") !=
         std::string::npos);
   CHECK(text.find("poll: func(in: list<borrow<pollable>>) -> list<u32>;") !=
         std::string::npos);
}

TEST_CASE("wasi shapes: consteval and runtime emit identical interface blocks",
          "[wit_wasi]")
{
   auto runtime = psio::generate_wit_text<wasi_io_poll>("wasi", "io", "0.2.3");

   auto begin = runtime.find("interface ");
   auto end   = runtime.find("\n\nworld ");
   REQUIRE(begin != std::string::npos);
   REQUIRE(end != std::string::npos);
   auto runtime_iface = runtime.substr(begin, end - begin) + "\n";

   constexpr auto pair =
      psio::constexpr_wit::interface_text<wasi_io_poll>();
   std::string_view consteval_iface{pair.first.data(), pair.second};

   CHECK(std::string{consteval_iface} == runtime_iface);
}

TEST_CASE("wasi shapes: Component Model binary carries expected strings",
          "[wit_wasi]")
{
   auto bytes = psio::generate_wit_binary<wasi_io_poll>(
      "wasi", "io", "0.2.3", "imports");

   // Header.
   REQUIRE(bytes.size() >= 8);
   CHECK(bytes[0] == 0x00);
   CHECK(bytes[1] == 0x61);
   CHECK(bytes[2] == 0x73);
   CHECK(bytes[3] == 0x6d);

   auto contains = [&](std::string_view needle) {
      if (needle.empty() || bytes.size() < needle.size())
         return false;
      for (std::size_t i = 0; i + needle.size() <= bytes.size(); ++i)
      {
         bool ok = true;
         for (std::size_t j = 0; j < needle.size(); ++j)
            if (bytes[i + j] != static_cast<std::uint8_t>(needle[j]))
            {
               ok = false;
               break;
            }
         if (ok)
            return true;
      }
      return false;
   };

   CHECK(contains("wit-component-encoding"));
   CHECK(contains("pollable"));
   CHECK(contains("pollable-ready"));
   CHECK(contains("poll"));
   CHECK(contains("wasi:io/wasi-io-poll@0.2.3"));
}
