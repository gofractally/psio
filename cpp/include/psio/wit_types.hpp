#pragma once
//
// psio/wit_types.hpp — WIT (WebAssembly Interface Types) IR.  Direct
// port of psio/wit_types.hpp.
//
// Foundation for the rest of the WIT toolchain: parser produces these
// structures, encoder serialises them to Component Model binary,
// generator builds them from PSIO_REFLECT'd C++ types.
//
// Type references use index-based encoding:
//    type_idx >= 0  → index into wit_world::types[]
//    type_idx < 0   → primitive: static_cast<wit_prim>(-(type_idx + 1))
// Helpers wit_prim_idx() and idx_to_prim() convert between the two.

#include <psio/reflect.hpp>

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace psio {

   // ── Primitive types ────────────────────────────────────────────────
   enum class wit_prim : std::uint8_t {
      bool_   = 0,
      u8      = 1,
      s8      = 2,
      u16     = 3,
      s16     = 4,
      u32     = 5,
      s32     = 6,
      u64     = 7,
      s64     = 8,
      f32     = 9,
      f64     = 10,
      char_   = 11,
      string_ = 12,
   };

   /// Encode a primitive as a negative type index.
   constexpr std::int32_t wit_prim_idx(wit_prim p)
   {
      return -(static_cast<std::int32_t>(p) + 1);
   }

   /// Decode a negative type index back to a primitive.
   constexpr wit_prim idx_to_prim(std::int32_t idx)
   {
      return static_cast<wit_prim>(-(idx + 1));
   }

   /// True if the type index refers to a primitive (negative).
   constexpr bool is_prim_idx(std::int32_t idx) { return idx < 0; }

   /// Sentinel: "no type" for variant cases without payloads
   /// and result types without ok/err.
   constexpr std::int32_t WIT_NO_TYPE = INT32_MIN;

   // ── Compound type definitions ──────────────────────────────────────

   /// Discriminates the kind of a compound type definition.
   ///
   /// Values 0-7 are data types (records, variants, enums, etc.).
   /// Values 8-10 are resource-related types from the WIT Component Model.
   ///
   /// Resource types (8-10) model opaque handle-based objects:
   ///   - resource_: An opaque type with methods, crossed as u32 handles.
   ///   - own_:      Ownership transfer of a resource (element_type_idx → resource).
   ///   - borrow_:   Temporary reference to a resource (element_type_idx → resource).
   ///
   /// own_ and borrow_ are type constructors like list_ and option_ — they
   /// wrap a resource type via element_type_idx. On the canonical ABI wire
   /// they are both u32.
   enum class wit_type_kind : std::uint8_t {
      record_   = 0,
      variant_  = 1,
      enum_     = 2,
      flags_    = 3,
      list_     = 4,
      option_   = 5,
      result_   = 6,
      tuple_    = 7,
      resource_ = 8,   ///< Opaque handle type with methods
      own_      = 9,   ///< Owning handle: element_type_idx → resource type
      borrow_   = 10,  ///< Borrowed handle: element_type_idx → resource type
   };

   /// An attribute attached to a WIT item, field, or case.
   ///
   /// Examples:
   ///   @since(version = 0.2.0)  → name="since", arg_key="version", arg_value="0.2.0"
   ///   @unstable(feature = foo) → name="unstable", arg_key="feature", arg_value="foo"
   ///   @final                   → name="final", arg_key="", arg_value=""
   ///   @sorted                  → name="sorted", arg_key="", arg_value=""
   ///
   /// Spec-endorsed: @since, @unstable, @deprecated (feature gating).
   /// PSIO extends with @final (closed record), @sorted, @unique-keys,
   /// @canonical, @utf8 (semantic invariants).
   ///
   /// Unknown attributes are preserved on the AST so they round-trip
   /// through import/export without loss.
   struct wit_attribute {
      std::string name;
      std::string arg_key;
      std::string arg_value;
   };
   PSIO_REFLECT(wit_attribute, name, arg_key, arg_value)

   /// A named field within a record, variant case, function param/result,
   /// or label within an enum/flags.
   struct wit_named_type {
      std::string                name;
      // negative = wit_prim, non-negative = index into types[]
      std::int32_t               type_idx = 0;
      std::vector<wit_attribute> attributes;
   };
   PSIO_REFLECT(wit_named_type, name, type_idx, attributes)

   /// A compound type definition (record, variant, enum, flags, list,
   /// resource, etc.).
   ///
   /// For resource_ types:
   ///    name             — the resource name (e.g. "cursor")
   ///    fields           — unused (resources are opaque)
   ///    method_func_idxs — indices into wit_world::funcs[]
   ///
   /// For own_/borrow_ types:
   ///    element_type_idx — index of the resource_ type being wrapped
   ///    name             — empty (anonymous type constructors)
   struct wit_type_def {
      std::string                 name;
      std::uint8_t                kind = 0;  // wit_type_kind
      // record fields, variant cases, enum/flag labels, tuple elements
      std::vector<wit_named_type> fields;
      // list/option/own/borrow element; result ok type
      std::int32_t                element_type_idx = 0;
      // result err type (0 = none)
      std::int32_t                error_type_idx = 0;
      // resource methods (indices into world.funcs[])
      std::vector<std::uint32_t>  method_func_idxs;
      // @since, @final, @sorted, etc.
      std::vector<wit_attribute>  attributes;
   };
   PSIO_REFLECT(wit_type_def, name, kind, fields, element_type_idx,
                 error_type_idx, method_func_idxs, attributes)

   // ── Functions ──────────────────────────────────────────────────────

   /// A WIT function signature with optional link to a WASM core export.
   struct wit_func {
      std::string                 name;
      std::vector<wit_named_type> params;
      std::vector<wit_named_type> results;
      // WASM export index, or UINT32_MAX if unlinked
      std::uint32_t               core_func_idx = UINT32_MAX;
      std::vector<wit_attribute>  attributes;
   };
   PSIO_REFLECT(wit_func, name, params, results, core_func_idx, attributes)

   // ── Interfaces ─────────────────────────────────────────────────────

   /// A named group of types and functions (WIT interface).
   struct wit_interface {
      std::string                name;
      std::vector<std::uint32_t> type_idxs;
      std::vector<std::uint32_t> func_idxs;
      std::vector<wit_attribute> attributes;
   };
   PSIO_REFLECT(wit_interface, name, type_idxs, func_idxs, attributes)

   // ── World (top-level container) ────────────────────────────────────

   /// Complete WIT world definition.  Contains both the raw .wit source
   /// text and pre-parsed structures for efficient runtime query.
   struct wit_world {
      std::string                package;     // e.g. "test:inventory@1.0.0"
      std::string                name;        // world name
      std::string                wit_source;  // raw .wit text for tooling
      std::vector<wit_type_def>  types;       // all type definitions
      std::vector<wit_func>      funcs;       // all function signatures
      std::vector<wit_interface> exports;
      std::vector<wit_interface> imports;
      std::vector<wit_attribute> attributes;
   };
   PSIO_REFLECT(wit_world, package, name, wit_source, types, funcs,
                 exports, imports, attributes)

   // Backward-compat alias for psizam consumers (predates the rename).
   using pzam_wit_world = wit_world;

}  // namespace psio
