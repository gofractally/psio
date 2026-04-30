#pragma once
//
// psio/format.hpp — format tags + primary view templates.
//
// A "format" is a wire/in-memory representation tag. Each format
// provides specializations of the view templates to expose a uniform
// accessor interface, regardless of the underlying byte layout:
//
//   psio::dynamic_view<Fmt>  — schemaless. Caller doesn't know the
//                              type up front; navigates by name/index.
//                              Only meaningful for self-describing
//                              formats (pjson, JSON, CBOR, MessagePack,
//                              BSON).
//
//   psio::view<T, Fmt>       — schema-aware. Caller knows T at compile
//                              time; field access is by reflected
//                              member. Available for every format.
//                              Schema-required formats (SSZ, fracpack,
//                              bin) ONLY have this form.
//
// The format tag is the second template argument. Code that needs to
// be format-agnostic (writers like view_to_json, materializers like
// to_struct) takes the view by template parameter and walks the same
// concept: type(), is_*(), as_*(), find()/at(), for_each_*.
//
// Adding a new format means: define a tag, specialize dynamic_view<Fmt>
// (if self-describing) and view<T, Fmt> for it. The framework code
// (writers, transcoders, walkers) doesn't change.

#include <psio/storage.hpp>

namespace psio {

   // ── format tags ───────────────────────────────────────────────────────

   struct pjson_format {};         // self-describing binary; pjson.hpp
   struct json_format  {};         // JSON text (self-describing)
   struct ssz_format   {};         // SSZ — schema-required
   struct fracpack_format {};      // fracpack — schema-required
   struct bin_format   {};         // raw little-endian — schema-required
   struct cbor_format  {};         // future
   struct msgpack_format {};       // future
   struct bson_format {};          // future

   // ── primary view templates (specialized per-format) ──────────────────

   template <typename Fmt>
   class dynamic_view;             // schemaless; only self-describing
                                   // formats specialize this.

   template <typename T, typename Fmt, storage Store = storage::const_borrow>
   class view;                     // schema-aware; every format
                                   // specializes this.  Store is defaulted
                                   // for headers (e.g. pjson_typed.hpp) that
                                   // only forward-name the 2-arg form.

}  // namespace psio
