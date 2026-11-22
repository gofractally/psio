#pragma once
// Declarations in the shape real consumers write them (WASI host bindings).
// interface_consumer_pattern_tests.cpp pins what they produce; this header is
// written against the public macros only and must keep compiling unchanged.

#include <psio/emit_wit.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wit_constexpr.hpp>
#include <psio/wit_encode.hpp>
#include <psio/wit_gen.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct pollable : psio::wit_resource
{
};
PSIO_REFLECT(pollable)

struct input_stream : psio::wit_resource
{
};
PSIO_REFLECT(input_stream)

struct wasi_datetime
{
   std::uint64_t seconds     = 0;
   std::uint32_t nanoseconds = 0;
};
PSIO_REFLECT(wasi_datetime, seconds, nanoseconds)

struct wasi_clocks_wall_clock
{
   static wasi_datetime now();
   static wasi_datetime resolution();
};

struct wasi_clocks_monotonic_clock
{
   static std::uint64_t       now();
   static std::uint64_t       resolution();
   static psio::own<pollable> subscribe_instant(std::uint64_t when);
};

struct wasi_io_poll
{
   static bool                       pollable_ready(psio::borrow<pollable> self);
   static void                       pollable_block(psio::borrow<pollable> self);
   static std::vector<std::uint32_t> poll(std::vector<psio::borrow<pollable>> in);
};

struct wasi_io_streams
{
   static std::vector<std::uint8_t> read(psio::borrow<input_stream> self, std::uint64_t len);
   static psio::own<pollable>       subscribe(psio::borrow<input_stream> self);
   static void                      drop(psio::own<input_stream> self);
};

PSIO_PACKAGE(wasi_clocks, "0.2.3");
#undef PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_clocks)

PSIO_INTERFACE(wasi_clocks_wall_clock, types(wasi_datetime), funcs(func(now), func(resolution)))

PSIO_INTERFACE(wasi_clocks_monotonic_clock,
               types(),
               funcs(func(now), func(resolution), func(subscribe_instant, when)))

PSIO_PACKAGE(wasi_io, "0.2.3");
#undef PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_io)

PSIO_INTERFACE(wasi_io_poll,
               types(pollable),
               funcs(func(pollable_ready, self), func(pollable_block, self), func(poll, in)))

PSIO_INTERFACE(wasi_io_streams,
               types(input_stream),
               funcs(func(read, self, len), func(subscribe, self), func(drop, self)))

struct wasi_clock_host
{
   std::uint64_t now() { return 7; }
   std::uint64_t resolution() { return 1; }
};
PSIO_HOST_MODULE(wasi_clock_host, interface(wasi_clocks_monotonic_clock, now, resolution))

namespace consumer_pattern
{
   template <typename Tag>
   void dump_one(std::string& out, const char* ns, const char* name)
   {
      out += "---- generate_wit_text\n";
      out += psio::generate_wit_text<Tag>(ns, name, "0.2.3");
      out += "---- constexpr interface_text\n";
      constexpr auto text = psio::constexpr_wit::interface_text<Tag>();
      out.append(text.first.data(), text.second);
      out += "---- emit_wit(SchemaBuilder)\n";
      out += psio::emit_wit(psio::SchemaBuilder{}.insert_interface<Tag>().build());
      out += "---- binary\n";
      static const char hex[] = "0123456789abcdef";
      for (std::uint8_t b : psio::generate_wit_binary<Tag>(ns, name, "0.2.3", "imports"))
      {
         out += hex[b >> 4];
         out += hex[b & 15];
      }
      out += "\n---- names\n";
      using info = psio::detail::interface_info<Tag>;
      out += std::string{info::name} + " @ " + std::string{info::package::name} + " " +
             std::string{info::package::version} + "\n";
      for (std::size_t i = 0; i < info::func_names.size(); ++i)
      {
         out += std::string{info::func_names[i]} + "(";
         for (const char* p : info::param_names[i])
            out += std::string{p} + ";";
         out += ")\n";
      }
   }

   inline std::string dump()
   {
      std::string out;
      dump_one<wasi_clocks_wall_clock>(out, "wasi", "clocks");
      dump_one<wasi_clocks_monotonic_clock>(out, "wasi", "clocks");
      dump_one<wasi_io_poll>(out, "wasi", "io");
      dump_one<wasi_io_streams>(out, "wasi", "io");
      return out;
   }
}  // namespace consumer_pattern
