// v1 ↔ psio flatbuf_lib byte-parity tests.
//
// Both sides use Google flatbuffers. Assumes flatbuffers::flatbuffers is
// linked — test target is conditional on find_package(flatbuffers).

#include <psio1/reflect.hpp>
#include <psio1/to_flatbuf.hpp>
#include <psio/flatbuf_lib.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace v1_fb {
   struct Token { std::uint16_t kind; std::string text; };
   PSIO1_REFLECT(Token, kind, text)

   struct Msg {
      std::uint64_t            id;
      std::vector<std::uint64_t> refs;
      std::string              note;
   };
   PSIO1_REFLECT(Msg, id, refs, note)
}
namespace v3_fb {
   struct Token { std::uint16_t kind; std::string text; };
   PSIO_REFLECT(Token, kind, text)

   struct Msg {
      std::uint64_t            id;
      std::vector<std::uint64_t> refs;
      std::string              note;
   };
   PSIO_REFLECT(Msg, id, refs, note)
}

TEST_CASE("flatbuf_lib parity: simple table", "[flatbuf][parity]")
{
   v1_fb::Token a{42, "hello"};
   v3_fb::Token b{42, "hello"};

   flatbuffers::FlatBufferBuilder fbb(256);
   psio1::to_flatbuf_finish(fbb, a);
   std::vector<char> av(
      reinterpret_cast<const char*>(fbb.GetBufferPointer()),
      reinterpret_cast<const char*>(fbb.GetBufferPointer() + fbb.GetSize()));

   auto bv = psio::encode(psio::flatbuf_lib{}, b);

   REQUIRE(av == bv);
}

TEST_CASE("flatbuf_lib parity: variable table", "[flatbuf][parity]")
{
   v1_fb::Msg a{0xDEAD, {1, 2, 3}, "note"};
   v3_fb::Msg b{0xDEAD, {1, 2, 3}, "note"};

   flatbuffers::FlatBufferBuilder fbb(256);
   psio1::to_flatbuf_finish(fbb, a);
   std::vector<char> av(
      reinterpret_cast<const char*>(fbb.GetBufferPointer()),
      reinterpret_cast<const char*>(fbb.GetBufferPointer() + fbb.GetSize()));

   auto bv = psio::encode(psio::flatbuf_lib{}, b);

   REQUIRE(av == bv);
}

TEST_CASE("flatbuf_lib round-trip: encode → decode", "[flatbuf][round-trip]")
{
   v3_fb::Msg b{99, {7, 8, 9}, "yo"};
   auto       bv   = psio::encode(psio::flatbuf_lib{}, b);
   auto       back = psio::decode<v3_fb::Msg>(psio::flatbuf_lib{},
                                                std::span<const char>{bv});
   REQUIRE(back.id == 99);
   REQUIRE(back.refs == std::vector<std::uint64_t>{7, 8, 9});
   REQUIRE(back.note == "yo");
}

TEST_CASE("flatbuf_lib: v1 encode → v3 decode", "[flatbuf][round-trip]")
{
   v1_fb::Msg a{123, {10, 20}, "abc"};
   flatbuffers::FlatBufferBuilder fbb(256);
   psio1::to_flatbuf_finish(fbb, a);
   std::vector<char> av(
      reinterpret_cast<const char*>(fbb.GetBufferPointer()),
      reinterpret_cast<const char*>(fbb.GetBufferPointer() + fbb.GetSize()));

   auto back = psio::decode<v3_fb::Msg>(psio::flatbuf_lib{},
                                         std::span<const char>{av});
   REQUIRE(back.id == 123);
   REQUIRE(back.refs == std::vector<std::uint64_t>{10, 20});
   REQUIRE(back.note == "abc");
}
