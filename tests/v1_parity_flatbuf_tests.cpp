// v1 ↔ psio native flatbuf byte-parity tests.
//
// psio::flatbuf (zero-dep native) must produce byte-identical output
// to psio::flatbuf_lib (Google flatbuffers runtime) on shared fixtures.

#include <psio/flatbuf.hpp>
#include <psio/flatbuf_lib.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbn {
   struct Token { std::uint16_t kind; std::string text; };
   PSIO_REFLECT(Token, kind, text)

   struct Msg {
      std::uint64_t              id;
      std::vector<std::uint64_t> refs;
      std::string                note;
   };
   PSIO_REFLECT(Msg, id, refs, note)
}

TEST_CASE("flatbuf native parity: simple table",
          "[flatbuf][native][parity]")
{
   fbn::Token t{42, "hello"};
   auto native = psio::encode(psio::flatbuf{}, t);
   auto lib    = psio::encode(psio::flatbuf_lib{}, t);
   REQUIRE(native == lib);
}

TEST_CASE("flatbuf native parity: variable table",
          "[flatbuf][native][parity]")
{
   fbn::Msg m{0xDEAD, {1, 2, 3}, "note"};
   auto native = psio::encode(psio::flatbuf{}, m);
   auto lib    = psio::encode(psio::flatbuf_lib{}, m);
   REQUIRE(native == lib);
}

TEST_CASE("flatbuf native round-trip",
          "[flatbuf][native][round-trip]")
{
   fbn::Msg in{99, {7, 8, 9}, "yo"};
   auto     bv   = psio::encode(psio::flatbuf{}, in);
   auto     back = psio::decode<fbn::Msg>(psio::flatbuf{},
                                            std::span<const char>{bv});
   REQUIRE(back.id == 99);
   REQUIRE(back.refs == std::vector<std::uint64_t>{7, 8, 9});
   REQUIRE(back.note == "yo");
}

TEST_CASE("flatbuf native → flatbuf_lib decode",
          "[flatbuf][native][round-trip]")
{
   fbn::Msg in{123, {10, 20}, "abc"};
   auto     bv = psio::encode(psio::flatbuf{}, in);
   auto     back =
      psio::decode<fbn::Msg>(psio::flatbuf_lib{},
                               std::span<const char>{bv});
   REQUIRE(back.id == 123);
   REQUIRE(back.refs == std::vector<std::uint64_t>{10, 20});
   REQUIRE(back.note == "abc");
}

TEST_CASE("flatbuf_lib → flatbuf native decode",
          "[flatbuf][native][round-trip]")
{
   fbn::Msg in{456, {30, 40, 50}, "xyz"};
   auto     bv = psio::encode(psio::flatbuf_lib{}, in);
   auto     back =
      psio::decode<fbn::Msg>(psio::flatbuf{},
                               std::span<const char>{bv});
   REQUIRE(back.id == 456);
   REQUIRE(back.refs == std::vector<std::uint64_t>{30, 40, 50});
   REQUIRE(back.note == "xyz");
}
