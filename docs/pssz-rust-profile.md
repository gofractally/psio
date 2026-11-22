# Explicit PSSZ protocol profiles in Rust

The crate-root `Pack`, `Unpack`, and `ToSchema` APIs remain legacy fracpack.
They do not switch formats when PSSZ support is enabled. PSSZ entry points live
under `psio::pssz`; callers explicitly supply `Pssz8`, `Pssz16`, or `Pssz32`.

The initial psibase protocol profile is **PSSZ32**: C++ `psio::pssz32` and Rust
`Pssz32`. This gives both languages the same deterministic header and offset
width for every nested type. C++'s `psio::pssz` automatic width selector is a
different profile and must not silently replace it. A future compact profile
requires an explicit protocol version and its own cross-language fixtures.

Use `try_to_pssz::<F, _>(&value)` for fallible encoding and
`from_pssz::<F, T>(&bytes)` for exact-span decoding. The infallible `to_pssz` and
low-level `pssz_pack` remain compatibility conveniences and panic on an encode
error. Checked container/record implementations validate bounds and offsets
before appending to their caller's output. Custom implementations must override
`pssz_pack_checked` when encoding can fail.

Primitive spans must have exactly the expected length; booleans must be 0 or 1.
Generic arrays, vectors, boxed arrays, and records decode each field through its
codec rather than treating arbitrary Rust memory layouts as wire data. A matching
memory size does not establish a safe representation, particularly for bool and
user-defined records. Boxed arrays are still constructed on the heap.

Variable sequence tables must begin exactly at the end of their offset table,
and record payloads must begin exactly after their fixed region. Offsets must be
monotone and in bounds. Checked record decoding requires the declared schema's
exact fixed-region header; unknown extension layouts are explicit errors pending
an extension-aware schema policy. Fixed records reject trailing bytes. Variable
payload lengths belong to their field codecs; appended text bytes, for example,
are part of that text value.

Optionals follow `docs/pssz-spec.md` 5.5, as the C++ codec does: when the inner
type's minimum encoded size is positive (`MIN_ENCODED_SIZE > 0`: scalars,
extensible records, which always have their header, and an option that itself
uses a selector) None is zero bytes and Some is the bare inner encoding; when it
is zero (strings, vectors, zero-size records, an option of a positive-minimum
type) selector 0 is None and selector 1 precedes the payload.

Not yet in Rust: optional-suffix evolution and mandatory pruning (the record
decoder requires the reader's own fixed-region size and the encoder writes every
slot), type-derived automatic width, and raw-byte strings (`String` decoding and
validation reject non-UTF-8, where C++ passes a plain string through).

This checked surface is a prerequisite for explicit derives and a mirror of the
new C++ schema IR. The old fracpack `AnyType`/`CompiledSchema` do not describe the
PSSZ layout and are not reused as PSSZ schema descriptors.
