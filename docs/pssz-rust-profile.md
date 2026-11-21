# Explicit Rust PSSZ APIs and optional suffix evolution

The crate-root `Pack`, `Unpack`, and `ToSchema` APIs remain legacy fracpack.
Explicit `PsszPack`, `PsszUnpack`, `PsszView`, and `ToPsszSchema` derives are
independent; deriving both families on one type does not change either wire.
Use `#[pssz(crate = "renamed_crate")]` for a renamed dependency. Records are
extensible by default; `definition_will_not_change` suppresses their header, and
`transparent` explicitly unwraps a single-field struct.

## Width is explicit and stable within a lineage

`Pssz8`, `Pssz16`, and `Pssz32` select header/offset widths matching C++
`psio::pssz8`, `psio::pssz16`, and `psio::pssz32`. All widths are tested. The
migration's final width/profile is not selected here. Readers and writers of a
compatible type lineage must keep the same width; C++ automatic width selection
must not silently change a deployed lineage when fields are added.

## Evolution contract

An upgrade preserves the original ordered field prefix and may append only
optional fields. Existing names, types, bounds, and annotations remain compatible;
DWNC layouts cannot grow. The actual new C++ schema IR is mirrored under
`pssz::schema_ir`, including its AnyType record wrapper, ordered alternatives,
attributes and package/interface/world envelope. `ToPsszSchema` emits that IR.
Schema definitions reserve their slots before walking children, matching C++
preorder declaration output and deterministic @N naming; failed insertion rolls
back its new definitions. This changes schema metadata ordering, not user
payload framing or bytes.
`upgrade::validate_optional_upgrade` checks old/new schema graphs at deployment;
no writer schema or schema identity is needed to read stored records.

The reader uses the writer's fixed-region header H and its known fixed region K.
Present known slots must fit wholly inside H. Missing trailing known Options
become None; a missing required field or split slot rejects. When H>K, every
unknown suffix slot is an offset because schema upgrades enforce optional-only
additions. H-K must be a multiple of W, even when K itself is odd. All known and
unknown variable offsets share one checked monotone offset list, so the first
unknown offset correctly terminates the last known variable field.

Only trailing None values are pruned. Required empty strings and zero scalars
remain present. DWNC records keep their entire layout. Option payloads use the
minimum-encoded-size rule: a selector is present exactly when the inner type can
encode zero bytes. Extensible records have a positive header minimum and need no
selector; zero-byte DWNC units need one, preserving None versus Some(unit).

## Checked entry points and unknown fields

`try_to_pssz::<F,_>` returns encode errors. `from_pssz_exact::<F,T>` rejects
unknown fields while accepting missing/pruned trailing Options. For compatible
reads use `from_pssz_compatible`, returning `{value, has_unknown}`; the flag
propagates through nested records and sequences. `into_known()` rejects a typed
rewrite that would discard unknown fields. Reading through an older schema does
not implicitly preserve unknown data when its typed value is re-encoded.

`validate_canonical_for_profile` checks exact-schema bytes against the encoder
for the supplied width, including maximal trailing-None pruning. Structural
readers also accept well-formed older unpruned None encodings. This API does not
choose a global schema width or claim the other published-spec canonical rules
are finalized. Infallible `to_pssz` and `pssz_pack` remain conveniences that panic
on encode error; protocol code should use the checked entry point.

Primitive spans are exact, bool bytes are 0/1, offsets are in bounds and monotone,
and first variable offsets equal the writer's fixed region. Arrays, vectors and
records walk codecs instead of assuming Rust memory layout equals wire layout.
Boxed arrays decode on the heap. A nonempty list of zero-byte fixed elements is
ambiguous and rejected; a fixed-count array can represent those elements.

## Views and runtime schemas

`PsszView` derives constant field locations and named accessors. Checked exact
and compatible view constructors structurally validate without materializing
payload values. A getter reads its offset and next known/first unknown offset
in O(1); omitted optional fields use static canonical None bytes. Legacy record
macros share the same checked layout/span helpers. The low-level `PsszView::new`
constructor is for already-trusted buffers.

`pssz::dynamic` consumes the same new schema IR. Typed dynamic values preserve
Option state and variant selectors that ordinary JSON can collapse. Unsupported
IR types/custom adapters return explicit errors, and the legacy FracPack marker
never silently selects a different codec.

The byte-preserving C++ `char` adapter maps to typed Unsigned(0..255), including
inside hex containers; it is distinct from Rust's Unicode `char`. Runtime schema
metadata is preflighted throughout the reachable graph, including omitted
optional branches. DWNC and collection count bounds are supported. Unsupported
constraints such as maxFields/maxDynamicData and unrecognized annotations reject
explicitly; doc/description/visible are whitelisted descriptive metadata.

The default safety budget numbers the root node 0: record/tuple fields, sequence
elements, Some payloads and variant payloads add one level. Transparent boxes and
wrappers add none; None and empty containers have no child. Node 64 is accepted;
node 65 rejects. This is a wire traversal budget: omitted trailing None fields
are synthesized defaults, so they add no node. Writers prune those fields before
traversing the kept prefix. A present unpruned None does count its Option node,
while Some(unit) also counts the unit payload. Checked typed and dynamic writers,
material readers, structural validation and validated views enforce this rule.
Shared fixtures cover these boundaries and every width. Depth-invalid fixture
bytes are built independently, never emitted by a supported checked writer.
Strings are atomic UTF-8 nodes. Byte vectors, including the runtime `hex`
adapter, traverse their nonempty elements; an empty byte vector has no child.
Schema minimum/layout analysis has a separate bounded recursion counter, so a
recursive optional alias fails explicitly rather than recursing indefinitely.
