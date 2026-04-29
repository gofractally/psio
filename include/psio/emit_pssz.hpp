#pragma once
//
// psio/emit_pssz.hpp — Schema IR → pSSZ wire bytes.
//
// pSSZ is psio's native schema-schema wire format.  Every IR type is
// PSIO_REFLECT'd, so encoding a Schema is just `psio::encode(pssz{},
// s)` — pssz walks the reflected struct directly, with Box<T>
// recognised as a transparent cycle-breaker (commit 3461d0f).  This
// header is the named entry point so consumers can spell the
// operation symmetrically with the other format emitters
// (emit_wit ✓, emit_gql, emit_fbs, …).
//
//   auto bytes  = psio::emit_pssz(schema);
//   auto schema = psio::parse_pssz(bytes);   // inverse, free function
//
// The fastest path to read or parse a stored schema is precisely
// "pssz-decode the IR directly" — that's what `parse_pssz` exposes.
//
// Companion / inverse:
//   - emit_wit       (Schema → WIT text, ✓)
//   - wit_parser     (WIT text → Schema, ✓)
//   - schema_builder (C++ reflection → Schema, ✓)

#include <psio/pssz.hpp>
#include <psio/schema_ir.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace psio::schema_types
{

   /// Encode a Schema to its canonical pSSZ wire form.
   inline std::vector<char> emit_pssz(const Schema& schema)
   {
      return ::psio::encode(::psio::pssz{}, schema);
   }

   /// Decode a Schema from pSSZ wire bytes.  Inverse of emit_pssz.
   inline Schema parse_pssz(std::span<const char> bytes)
   {
      return ::psio::decode<Schema>(::psio::pssz{}, bytes);
   }

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::emit_pssz;
   using schema_types::parse_pssz;
}
