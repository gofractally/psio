#pragma once
//
// psio/schema_ir.hpp — Schema IR (the multi-format pivot).
//
// The canonical in-memory representation that mediates between every
// schema format psio supports.  WIT text, pSSZ native, GraphQL,
// FlatBuffers, Cap'n Proto, JSON Schema and so on all read and write
// through this IR.  Each format pair composes through the IR rather
// than as a point-to-point converter, keeping conversion costs at
// 2(N-1) per format added instead of N(N-1) lossy pairs.
//
// pSSZ is the native schema-schema.  Every IR type is `PSIO_REFLECT`'d
// so the IR itself is a structurally-reflected schema — the fastest
// path to read or parse a stored schema is "decode the IR with pssz."
// All other formats (WIT text, GQL, fbs, …) emit/parse in terms of
// the IR; pSSZ is the canonical wire form.
//
// The runtime schema *value* (`psio::schema`, `record_descriptor`,
// Phase 14a) lives in `psio/schema.hpp` and is a different artifact:
// schema_ir.hpp is the structural description (what the type *looks
// like*); schema.hpp is the type-erased runtime walker.  Both
// coexist; the IR feeds the schema value when needed.
//
// IR types live in `psio::schema_types`; common consumers see the
// names at namespace `psio` scope through aliases at the bottom.
//
// Companion headers:
//   schema_builder.hpp  — walks reflection / interface_info /
//                         world_info / use_info into the IR.
//   emit_wit.hpp        — Schema → WIT text (one of N emitters).
//   wit_parser.hpp      — WIT text → Schema (already ported).

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>
#include <psio/untagged.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace psio::schema_types
{

   // ── Box<T> ──────────────────────────────────────────────────────────
   //
   // Smart-pointer wrapper used to break the cycle in `AnyType`'s
   // recursive variant alternatives (List, Option, Array, BoundedList,
   // Custom, Nested — all hold an inner AnyType).  unique_ptr-backed
   // for value semantics; copy is deep so Schema instances are
   // value-typed all the way down.
   template <typename T>
   struct Box
   {
      std::unique_ptr<T> value;

      Box() : value(new T()) {}
      Box(T v) : value(new T(std::move(v))) {}
      Box(const Box& o) : value(new T(*o.value)) {}
      Box(Box&&) noexcept            = default;
      Box& operator=(Box&&) noexcept = default;
      Box& operator=(const Box& o)
      {
         *value = *o.value;
         return *this;
      }

      T&       operator*() noexcept { return *value; }
      const T& operator*() const noexcept { return *value; }
      T*       operator->() noexcept { return value.get(); }
      const T* operator->() const noexcept { return value.get(); }
      T*       get() noexcept { return value.get(); }
      const T* get() const noexcept { return value.get(); }

      friend bool operator==(const Box& a, const Box& b) { return *a == *b; }
   };

   // Forward declarations for the cyclic graph.
   //
   // The IR has a real type-system cycle: AnyType holds Object/Struct/
   // Variant/Resource by value, those hold vector<Member>/vector<Func>,
   // and Member/Func hold AnyType.  libc++ enforces complete types in
   // std::vector instantiations, so we break the cycle at Member::type
   // and Func::result by Box-ing the AnyType — those become fixed-size
   // (string + unique_ptr + vector<Attribute>), which lets the
   // containing vectors instantiate before AnyType is complete.
   struct AnyType;
   struct Member;
   struct Func;
   struct Schema;

   // ── Attribute ───────────────────────────────────────────────────────
   //
   // A name / value pair on an IR node. `value` is nullopt for a bare
   // flag (definitionWillNotChange), otherwise the rendered argument
   // ("5", "max=32", "string"). The attributes the schema builder writes
   // and the codecs read are listed in pssz_ir.hpp; the C++ annotations
   // marked RESERVED in annotate.hpp (final, canonical, flags, padding,
   // since, unstable, ...) are not written here or by any emitter.
   //
   // Carried first-class on Member, Func, Variant cases, Object,
   // Resource, Package, Use, Interface, World — every IR site that
   // can survive a cross-format conversion.
   struct Attribute
   {
      std::string                name;
      std::optional<std::string> value = std::nullopt;
      friend bool                operator==(const Attribute&,
                                            const Attribute&) = default;
   };
   PSIO_REFLECT(Attribute, name, value)

   // ── Member + Func — defined first so the records that hold
   // vector<Member> / vector<Func> below see complete types.  Both
   // break their AnyType cycle through Box<AnyType>.

   struct Member
   {
      std::string            name;
      Box<AnyType>           type;
      std::vector<Attribute> attributes;
      friend bool            operator==(const Member&, const Member&) = default;
   };
   PSIO_REFLECT(Member, name, type, attributes)

   struct Func
   {
      std::string                 name;
      std::vector<Member>         params;
      std::optional<Box<AnyType>> result;
      std::vector<Attribute>      attributes;
      friend bool                 operator==(const Func&,
                                             const Func&) = default;
   };
   PSIO_REFLECT(Func, name, params, result, attributes)

   // ── Records ─────────────────────────────────────────────────────────

   struct Object
   {
      std::vector<Member>    members;
      std::vector<Attribute> attributes;
      friend bool            operator==(const Object&, const Object&) = default;
   };
   PSIO_REFLECT(Object, members, attributes)

   struct Struct
   {
      std::vector<Member>    members;
      std::vector<Attribute> attributes;
      friend bool            operator==(const Struct&, const Struct&) = default;
   };
   PSIO_REFLECT(Struct, members, attributes)

   // ── Resource (WIT opaque handle with reflected methods) ─────────────
   //
   // Methods cross the boundary as named operations on a u32 handle;
   // data members of resource types are intentionally not carried —
   // resources don't cross by value.
   struct Resource
   {
      std::string            name;
      std::vector<Func>      methods;
      std::vector<Attribute> attributes;
      friend bool operator==(const Resource&, const Resource&) = default;
   };
   PSIO_REFLECT(Resource, name, methods, attributes)

   // ── Collections ─────────────────────────────────────────────────────

   struct Array
   {
      Box<AnyType>  type;
      std::uint64_t len = 0;
      friend bool   operator==(const Array&, const Array&) = default;
   };
   PSIO_REFLECT(Array, type, len)

   struct List
   {
      Box<AnyType> type;
      friend bool  operator==(const List&, const List&) = default;
   };
   PSIO_REFLECT(List, type)

   // Bounded variable-length list: same wire semantics as List but
   // the count is bounded by maxCount at the schema level.  Emerges
   // from psio::bounded_list<T, N>; kept distinct from List so
   // unbounded consumers retain their existing JSON round-trip.
   struct BoundedList
   {
      Box<AnyType>  type;
      std::uint64_t maxCount = 0;
      friend bool   operator==(const BoundedList&,
                               const BoundedList&) = default;
   };
   PSIO_REFLECT(BoundedList, type, maxCount)

   struct Option
   {
      Box<AnyType> type;
      friend bool  operator==(const Option&, const Option&) = default;
   };
   PSIO_REFLECT(Option, type)

   // ── Variant + Tuple ─────────────────────────────────────────────────

   struct Variant
   {
      std::vector<Member>    members;
      std::vector<Attribute> attributes;
      friend bool            operator==(const Variant&, const Variant&) = default;
   };
   PSIO_REFLECT(Variant, members, attributes)

   struct Tuple
   {
      std::vector<Box<AnyType>> members;
      friend bool               operator==(const Tuple&, const Tuple&) = default;
   };
   PSIO_REFLECT(Tuple, members)

   // ── Format/codec markers ────────────────────────────────────────────

   // Custom: extension hook.  `id` names a registered custom-handler.
   // Survives across formats as an opaque carrier; emitters that
   // don't recognise the id render it as a comment or skip it.
   struct Custom
   {
      Box<AnyType> type;
      std::string  id;
      friend bool  operator==(const Custom&, const Custom&) = default;
   };
   PSIO_REFLECT(Custom, type, id)

   // Nested: a byte list holding the PSSZ encoding (automatic profile) of
   // a value of `type` (C++ psio::nested<T>, nested.hpp). On the wire it is
   // exactly a list of u8; a checking reader also validates the bytes as
   // `type`; its text is the text of the inner value. A member
   // `lengthBound` attribute bounds the byte count. (Variant position 10,
   // formerly the unused `FracPack` marker: same IR bytes.)
   struct Nested
   {
      Box<AnyType> type;
      friend bool  operator==(const Nested&, const Nested&) = default;
   };
   PSIO_REFLECT(Nested, type)

   // ── Primitives ──────────────────────────────────────────────────────

   struct Int
   {
      std::uint32_t bits     = 0;
      bool          isSigned = false;
      friend bool   operator==(const Int&, const Int&) = default;
   };
   PSIO_REFLECT(Int, bits, isSigned)

   // IEEE-754 form: `exp` exponent bits, `mantissa` significand bits.
   // {8, 23} is f32; {11, 52} is f64.
   struct Float
   {
      std::uint32_t exp      = 0;
      std::uint32_t mantissa = 0;
      friend bool   operator==(const Float&, const Float&) = default;
   };
   PSIO_REFLECT(Float, exp, mantissa)

   // ── Enum ────────────────────────────────────────────────────────────
   //
   // An enumeration without payloads. On the wire it is exactly `type`,
   // its underlying integer, and any in-range value is structurally
   // valid; the enumerators give the text formats their names (a listed
   // value is written as its name, any other as its number). `value` is
   // the enumerator's value as 64 two's-complement bits (sign-extended
   // for a signed underlying type).
   struct Enumerator
   {
      std::string   name;
      std::uint64_t value = 0;
      friend bool   operator==(const Enumerator&, const Enumerator&) = default;
   };
   PSIO_REFLECT(Enumerator, name, value)

   struct Enum
   {
      Box<AnyType>            type;
      std::vector<Enumerator> members;
      friend bool             operator==(const Enum&, const Enum&) = default;
   };
   PSIO_REFLECT(Enum, type, members)

   // ── Named-type reference ────────────────────────────────────────────
   //
   // `Type{"my-record"}` resolves against Schema::types[name].
   // Allows the IR to express forward references and shared subtrees
   // without duplicating them.
   struct Type
   {
      std::string type;
      friend bool operator==(const Type&, const Type&) = default;
   };
   PSIO_REFLECT(Type, type)

   // ── AnyType — the open-world variant ────────────────────────────────
   //
   // Every IR type-position field is an AnyType.  The variant carries
   // the discriminator + payload for the 15 alternatives. The legacy
   // `resolved` slot is unwired and unused: cached pointers cannot remain
   // valid across copies, moves, schema reuse or alias mutation. Constructors
   // accept each alternative by value plus a pair of std::string /
   // const char* convenience overloads that produce a Type
   // reference.
   struct AnyType
   {
      std::variant<Struct,
                   Object,
                   Array,
                   List,
                   BoundedList,
                   Option,
                   Variant,
                   Tuple,
                   Int,
                   Float,
                   Nested,
                   Custom,
                   Resource,
                   Type,
                   // Added after the original 14: schemas written before
                   // it existed decode unchanged.
                   Enum>
                             value;
      mutable const AnyType* resolved = nullptr;

      AnyType()                          = default;
      AnyType(const AnyType& other) : value(other.value) {}
      AnyType(AnyType&& other) noexcept : value(std::move(other.value))
      { other.resolved = nullptr; }
      AnyType& operator=(const AnyType& other)
      {
         if (this != &other) value = other.value;
         resolved = nullptr;
         return *this;
      }
      AnyType& operator=(AnyType&& other) noexcept
      {
         if (this != &other) value = std::move(other.value);
         resolved = nullptr;
         other.resolved = nullptr;
         return *this;
      }

      AnyType(Int v) : value(std::move(v)) {}
      AnyType(Float v) : value(std::move(v)) {}
      AnyType(Object v) : value(std::move(v)) {}
      AnyType(Struct v) : value(std::move(v)) {}
      AnyType(Option v) : value(std::move(v)) {}
      AnyType(List v) : value(std::move(v)) {}
      AnyType(BoundedList v) : value(std::move(v)) {}
      AnyType(Array v) : value(std::move(v)) {}
      AnyType(Variant v) : value(std::move(v)) {}
      AnyType(Tuple v) : value(std::move(v)) {}
      AnyType(Nested v) : value(std::move(v)) {}
      AnyType(Custom v) : value(std::move(v)) {}
      AnyType(Resource v) : value(std::move(v)) {}
      AnyType(Type v) : value(std::move(v)) {}
      AnyType(Enum v) : value(std::move(v)) {}
      AnyType(std::string name) : value(Type{std::move(name)}) {}
      AnyType(const char* name) : value(Type{std::string{name}}) {}

      friend bool operator==(const AnyType& a, const AnyType& b)
      {
         return a.value == b.value;
      }

      // Resolve Type-by-name references against a schema's `types`
      // map with bounded alias traversal. Defined below once Schema is
      // complete. The supplied schema always determines the result.
      const AnyType* resolve(const Schema& schema) const;
   };
   PSIO_REFLECT(AnyType, value)

   // ── FunctionType — pre-Member form used by reflection-walkers ──────
   //
   // Once an interface lifts a Func into Interface::funcs, FunctionType
   // is unused; kept here to match v1's wire layout so cross-version
   // IR remains round-trippable.
   struct FunctionType
   {
      Box<AnyType>                params;
      std::optional<Box<AnyType>> result;
      friend bool                 operator==(const FunctionType&,
                                             const FunctionType&) = default;
   };
   PSIO_REFLECT(FunctionType, params, result)

   // ── Structural envelope ─────────────────────────────────────────────
   //
   // Schema grows from a bare types map into a WIT-shaped envelope:
   // a package header, interfaces grouping types + functions, worlds
   // composing interfaces, and cross-package `use` declarations.
   // Mirrors `wit_world` in `wit_types.hpp` so a parsed `.wit` and a
   // reflected C++ world produce the same IR.

   struct Package
   {
      std::string            name;
      std::string            version;
      std::vector<Attribute> attributes;
      friend bool            operator==(const Package&, const Package&) = default;
   };
   PSIO_REFLECT(Package, name, version, attributes)

   struct UseItem
   {
      std::string                name;
      std::optional<std::string> alias;
      friend bool                operator==(const UseItem&,
                                            const UseItem&) = default;
   };
   PSIO_REFLECT(UseItem, name, alias)

   struct Use
   {
      std::string          package;
      std::string          interface_name;
      std::string          version;
      std::vector<UseItem> items;
      friend bool          operator==(const Use&, const Use&) = default;
   };
   PSIO_REFLECT(Use, package, interface_name, version, items)

   struct UseRef
   {
      std::string package;
      std::string interface_name;
      friend bool operator==(const UseRef&, const UseRef&) = default;
   };
   PSIO_REFLECT(UseRef, package, interface_name)

   struct Interface
   {
      std::string              name;
      std::vector<std::string> type_names;
      std::vector<Func>        funcs;
      std::vector<Attribute>   attributes;
      friend bool              operator==(const Interface&,
                                          const Interface&) = default;
   };
   PSIO_REFLECT(Interface, name, type_names, funcs, attributes)

   struct World
   {
      std::string              name;
      std::vector<UseRef>      imports;
      std::vector<std::string> exports;
      std::vector<Attribute>   attributes;
      friend bool              operator==(const World&, const World&) = default;
   };
   PSIO_REFLECT(World, name, imports, exports, attributes)

   // ── Schema (top-level container) ────────────────────────────────────
   //
   // Carries the package header, every interface / world / use
   // declaration, and the named-type list referenced by Type{} entries
   // throughout the AnyType graph.  Format emitters consume this;
   // format parsers produce it.
   //
   // `types` is a vector<Member> rather than a map<string, AnyType>:
   //   1. Order is meaningful (declaration order should round-trip
   //      through every format that preserves it).
   //   2. Member already has the right shape (name + Box<AnyType> +
   //      attributes) and is PSIO_REFLECT'd, so pssz / every other
   //      format that walks reflected records carries it without an
   //      additional std::map case.
   //   3. The map invariants (uniqueness, lookup) are restated as
   //      Schema::insert / Schema::get; insertion deduplicates by
   //      name so the visible behaviour matches v1.
   //
   // ir_version. The version of the IR's MEANING the schema is written in:
   //
   //   1   a boxed value is marked Custom{.., "box"}, a UTF-8 string is
   //       "utf8" and "string" is a raw string.
   //
   // The field was appended to the record, so the IR's own encoding is
   // backward compatible: a schema written before it existed decodes with
   // no version. Such a schema predates the markers above — read as
   // version 1 it would silently derive other offset widths and skip UTF-8
   // validation — so every schema-driven codec (pssz_ir.hpp,
   // pssz_ir_json.hpp, and the Rust / JS ones) refuses it with an error
   // instead of guessing, as it refuses a version newer than its own. A
   // Schema built in code (default-constructed, SchemaBuilder) is in the
   // current version.
   inline constexpr std::uint32_t schema_ir_version = 1;

   struct Schema
   {
      Package                package;
      std::vector<Interface> interfaces;
      std::vector<World>     worlds;
      std::vector<Use>       uses;
      std::vector<Member>    types;
      std::optional<std::uint32_t> ir_version = schema_ir_version;

      friend bool operator==(const Schema&, const Schema&) = default;

      // Lookup by name; returns nullptr for unknown.
      const AnyType* get(std::string_view name) const noexcept
      {
         for (const auto& m : types)
            if (m.name == name)
               return m.type.get();
         return nullptr;
      }

      // Insert-or-overwrite by name (vector preserves declaration
      // order; later inserts of the same name update in place).
      void insert(std::string name, AnyType type)
      {
         for (auto& m : types)
            if (m.name == name)
            {
               *m.type = std::move(type);
               return;
            }
         types.push_back(Member{std::move(name),
                                Box<AnyType>{std::move(type)},
                                {}});
      }
   };
   PSIO_REFLECT(Schema, package, interfaces, worlds, uses, types, ir_version)

   // AnyType::resolve definition lands here, after Schema is complete.
   inline const AnyType* AnyType::resolve(const Schema& schema) const
   {
      const AnyType* result = this;
      // A terminating alias chain cannot visit more aliases than there are
      // named definitions. A repeated definition is an invalid alias cycle.
      for (std::size_t hops = 0; hops <= schema.types.size(); ++hops)
      {
         auto* t = std::get_if<Type>(&result->value);
         if (!t) return result;
         result = schema.get(t->type);
         if (!result) return nullptr;
      }
      return nullptr;
   }

}  // namespace psio::schema_types

// ── Pull commonly-used names up to namespace psio ──────────────────────
//
// Convenience aliases match v1's surface so consumers don't have to
// spell `psio::schema_types::Schema` everywhere.  Fully-qualified
// names remain available where disambiguation matters.
namespace psio
{
   using schema_types::AnyType;
   using schema_types::Attribute;
   using schema_types::Func;
   using schema_types::Interface;
   using schema_types::Member;
   using schema_types::Package;
   using schema_types::Resource;
   using schema_types::Use;
   using schema_types::UseItem;
   using schema_types::UseRef;
   using schema_types::World;
   // `Schema` (capital S) is the IR top-level type.  The lowercase
   // `psio::schema` (Phase-14a runtime walker) lives in psio/schema.hpp
   // — distinct artifact, distinct name.
   using schema_types::Schema;
   using schema_types::schema_ir_version;
}  // namespace psio
