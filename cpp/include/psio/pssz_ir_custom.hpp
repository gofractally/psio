#pragma once
//
// psio/pssz_ir_custom.hpp — handlers of custom types for the schema-driven
// codecs (pssz_ir.hpp: binary; pssz_ir_json.hpp: JSON / I-JSON).
//
// A Custom node whose id is not part of the binary form (pssz_ir.hpp lists
// those: bool, char, string, utf8, hex, bitvector, bitlist, box) and is not
// a time id names a TYPE with an identity of its own over its wire type:
// `Custom{type: Int{64}, id: "AccountNumber"}` is a u64 on the wire. What
// the id adds — a text presentation, an invariant, or both — is code, which
// a schema cannot carry, so it is supplied as a handler registered for the
// id:
//
//   to_text / from_text   the presentation: between the PSSZ bytes of the
//                         wire type and the text, which is ONE JSON string
//                         in both text formats (the rule of a typed text
//                         adapter). Optional: a type with an invariant and
//                         no text form registers neither, and its text
//                         stays the wire type's.
//   check                 the invariant: whether the PSSZ bytes of the wire
//                         type are a value of the type. Optional.
//
// The bytes a handler sees and produces are the automatic profile (the
// typed psio::pssz bytes) of the node's wire type.
//
//   psio::pssz_ir::CustomHandlers handlers;
//   handlers.add<AccountNumber>();        // all three, from the C++ type
//   handlers.add("Version", {to_text, from_text});          // or by hand
//   handlers.add("Key", {.check = non_empty_der});
//   pssz_to_json(schema, root, psio::ijson{}, bytes, text, nullptr, &handlers);
//   validate_pssz(schema, root, bytes, nullptr, &handlers);
//
// Every schema-driven entry point takes the registry. With it, `check` runs
// on every value of a custom node that has one: on reads after the value's
// structure is accepted, on writes before any byte or text is returned —
// nested values first, and a node under several ids innermost first. The
// status `check` returns is passed through unchanged.
//
// An id with no handler is its wire type: the wire type's text, structure
// checked only (the invariant is NOT checked). `require_all = true` (strict)
// makes a custom node with no handler an error instead, on the binary
// entry points as well as the text ones — for a caller that must not accept
// or produce a value it cannot check and present exactly.
//
// Built-in presentations. The time ids ("time-point-<unit>", "duration-
// <unit>", "monotonic-<unit>", time.hpp) are presentations the codecs supply
// themselves; "box" is its inner type. They, and the other ids of PSIO's
// own types (adapter.hpp reserved_custom_id), take no handler — registering
// one is an error (try_add returns it, add fails through codec_abort) — and
// are not subject to `require_all`.
//
// A handler cannot express a presentation that is not a single JSON string
// (an object, a bare number), or one that depends on the text format or on
// the surrounding value.

#include <psio/adapter.hpp>
#include <psio/error.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psio::pssz_ir
{
class CustomHandlers;

namespace detail
{
// The handler of a C++ type: defined in pssz_ir_json.hpp, which has the
// typed codecs it is made of.
template <typename T> struct typed_custom_handler;
} // namespace detail

class CustomHandlers
{
 public:
   struct Handler
   {
      // PSSZ bytes of the wire type -> the text (appended, unquoted).
      std::function<codec_status(std::span<const char> pssz, std::string &text)> to_text = {};
      // The text -> PSSZ bytes of the wire type (appended). An error for
      // text that is not this presentation.
      std::function<codec_status(std::string_view text, std::vector<char> &pssz)> from_text = {};
      // Whether structurally valid PSSZ bytes of the wire type are a value
      // of the type: its invariant.
      std::function<codec_status(std::span<const char> pssz)> check = {};
   };

   // A custom node with no handler is an error rather than its wire type.
   bool require_all = false;

   // Registers `handler` under `id`, or says why it cannot: an id reserved
   // for PSIO's own types (adapter.hpp reserved_custom_id) has a built-in
   // meaning that no handler replaces.
   [[nodiscard]] codec_status try_add(std::string id, Handler handler)
   {
      if (reserved_custom_id(id))
         return codec_fail("pssz IR: a custom handler claims an id reserved for PSIO's own types",
                           0, "pssz");
      add_unchecked(std::move(id), std::move(handler));
      return codec_ok();
   }
   // The same, failing through codec_abort (codec_exception, or abort
   // without exceptions) on a reserved id.
   CustomHandlers &add(std::string id, Handler handler)
   {
      if (auto status = try_add(std::move(id), std::move(handler)); !status.ok())
         ::psio::codec_abort(status.error());
      return *this;
   }
   // The handler of the C++ type T under `id` (default: the id the schema
   // builder gives T, adapter.hpp schema_custom_id): to_text / from_text
   // from T's text_category adapter when it has one, check from T's
   // invariant when it has one (the typed `validate<T>(psio::pssz{}, bytes)`,
   // which runs the invariants of T's members too). T's typed psio::pssz
   // encoding is the wire type's bytes by construction. Needs
   // pssz_ir_json.hpp.
   template <typename T> CustomHandlers &add(std::string id = std::string{schema_custom_id<T>()})
   {
      return add(std::move(id), detail::typed_custom_handler<T>::make());
   }
   const Handler *find(std::string_view id) const
   {
      auto at = handlers_.find(std::string(id));
      return at == handlers_.end() ? nullptr : &at->second;
   }
   // Whether a binary walk has anything to do with this registry.
   bool checks_values() const noexcept { return require_all || checks_ != 0; }

 private:
   void add_unchecked(std::string id, Handler handler)
   {
      handlers_.insert_or_assign(std::move(id), std::move(handler));
      checks_ = 0;
      for (const auto &entry : handlers_)
         checks_ += entry.second.check ? 1 : 0;
   }
   std::unordered_map<std::string, Handler> handlers_;
   std::size_t checks_ = 0;
};
} // namespace psio::pssz_ir
