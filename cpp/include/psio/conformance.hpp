#pragma once
//
// psio/conformance.hpp — format-neutral test harness (§4.4).
//
// Writing one test case and sweeping it across every compatible format
// is a success criterion for v2 (§8 #4). This header ships three
// iteration macros:
//
//   PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(X)
//       — ssz, pssz, frac32, bin, borsh, bincode, avro, key
//   PSIO_FOR_EACH_TEXT_FMT(X)
//       — json
//   PSIO_FOR_EACH_BINARY_FMT(X)
//       — every binary format above
//
// Each X is a macro of shape `X(Name, FmtValue)` — `Name` is a C++
// identifier suitable for concatenation; `FmtValue` is the format tag
// instance. Users expand the iteration with a local X macro that
// generates whatever they need (round-trip assertion, encode parity,
// size probe).
//
//   #include <psio/conformance.hpp>
//
//   #define MY_ROUND_TRIP(FmtName, FmtValue)                              \
//      TEST_CASE("round-trip " #FmtName, "[conformance][" #FmtName "]")   \
//      {                                                                   \
//         Pt in{3, -7};                                                   \
//         auto bv = psio::encode(FmtValue, in);                          \
//         auto out = psio::decode<Pt>(FmtValue, std::span{bv});          \
//         REQUIRE(out.x == 3);                                            \
//         REQUIRE(out.y == -7);                                           \
//      }
//   PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(MY_ROUND_TRIP)
//
// Adding a new format: add one entry to the list below. Every existing
// use site picks it up automatically.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/capnp.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/ssz.hpp>
#include <psio/wit.hpp>

// Two sweeps:
//
//   PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT — every binary format on its
//     supported shapes. Capnp joins here once its MVP lands.
//   PSIO_FOR_EACH_RECORD_BINARY_FMT — subset that skips free-standing
//     std::variant encoding. Capnp encodes unions only as a *field* of
//     a reflected record (upstream capnp spec), so top-level variant
//     round-trips aren't applicable.
//
// Adding a new format: append to the broad list if it supports every
// shape, or only to the subset if a shape is deferred. Tests pick the
// list that matches the shape they exercise.

#define PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(X) \
   X(ssz,     ::psio::ssz{})                   \
   X(pssz,    ::psio::pssz{})                \
   X(frac32,  ::psio::frac32{})                \
   X(bin,     ::psio::bin{})                   \
   X(borsh,   ::psio::borsh{})                 \
   X(bincode, ::psio::bincode{})               \
   X(avro,    ::psio::avro{})                  \
   X(key,     ::psio::key{})                   \
   X(capnp,   ::psio::capnp{})                 \
   X(wit,     ::psio::wit{})

#define PSIO_FOR_EACH_RECORD_BINARY_FMT(X) \
   X(ssz,     ::psio::ssz{})                \
   X(pssz,    ::psio::pssz{})             \
   X(frac32,  ::psio::frac32{})             \
   X(bin,     ::psio::bin{})                \
   X(borsh,   ::psio::borsh{})              \
   X(bincode, ::psio::bincode{})            \
   X(avro,    ::psio::avro{})               \
   X(key,     ::psio::key{})

#define PSIO_FOR_EACH_TEXT_FMT(X) \
   X(json,    ::psio::json{})

#define PSIO_FOR_EACH_BINARY_FMT(X) \
   PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(X)
