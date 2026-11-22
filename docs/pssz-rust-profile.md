# PSSZ in Rust

The crate-root `Pack`, `Unpack`, and `ToSchema` APIs remain legacy fracpack.
They do not switch formats when PSSZ support is enabled. PSSZ lives under
`psio::pssz` and never generates fracpack implementations.

Rust and C++ must produce and accept byte-identical PSSZ encodings. C++
(`cpp/include/psio/pssz.hpp`) is the reference; the shared fixture
`conformance/pssz-vectors.json` is generated from it and consumed by both
`ctest` and `cargo test` (see `conformance/pssz-vectors.md`).

## Format tags

| Rust tag | C++ tag | Offset width |
|---|---|---|
| `Pssz` | `psio::pssz` | type-derived: each logical type selects its own |
| `Pssz8` / `Pssz16` / `Pssz32` | `psio::pssz_<1/2/4>` | one width inherited through the whole recursion |

`Pssz` is the production profile. The explicit tags are retained as
regression / compatibility tags exactly as in C++.

**Width is a property of the type, not of the caller.** An explicit cap on the
type always rules: `max_dynamic_data = N` selects the width class of N
(`<= 255` → 1, `<= 65535` → 2, otherwise 4). Without a cap the class of the
inferred bound applies (`PsszPack::AUTO_BOUND`, the mirror of C++
`max_encoded_size<T>()`); the bound is finite only if every variable-length
subfield is bounded, otherwise the width is 4. The only spare capacity is the
rounding from the bound to its width class. Each record, vector and array owns
its header and offset slots at its own width (`PsszPack::OFFSET_WIDTH`);
children use theirs.

The inferred bound is ABI and reproduces the C++ bookkeeping exactly: four
bytes per header and per offset slot regardless of the width finally chosen,
one byte for every `Option`, a declared child cap reserved in full, and no
bound for vectors, strings, tuples, variants, bit types and `Box`.

The cap also limits the complete encoded size of the value. The automatic
profile enforces it at every node on encode, decode and validate; explicit
tags enforce it at the top level only, as C++ does.

## Declaring records

Proc-macro derives and the declarative macros generate the same codec through
the same helpers (`psio::pssz_checked`); `rust/psio/tests/pssz_parity.rs` runs
every rule against both.

```rust
#[derive(PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
#[pssz(max_dynamic_data = 200)]            // optional explicit cap
struct Account { id: u32, name: String, memo: Option<RawString> }

#[derive(PsszPack, PsszUnpack)]
#[pssz(definition_will_not_change)]        // fixed layout: no header
struct Point { x: u32, y: u32 }

pssz_struct!(#[pssz(max_dynamic_data = 200)] Account2 { id: u32, name: String });
pssz_struct_dwnc!(Point2 { x: u32, y: u32 });
```

Other derive attributes: `transparent` (single-field newtype with the inner
type's wire shape), `variant`, `name = "..."`, `crate = "..."`. Field-level
annotations are not supported; bounds are expressed with bounded types.
`pssz_struct_dwnc!` copies fields by value so that it works on
`#[repr(packed)]` structs; a fixed-layout record with a non-`Copy` field uses
the derive.

## Rules

1. **Record evolution.** An extensible record starts with a header holding the
   size of the fixed region its writer emitted. Readers honor that header, not
   their own schema: unknown trailing slots written by a newer schema are
   skipped (and still delimit the last known variable field); trailing
   optionals missing from older data read as `None`. A field appended after a
   type is published must be `Option<T>`.
2. **Mandatory pruning.** A value has exactly one encoding. The encoder always
   drops trailing `None` fields and writes the reduced header. Every reader —
   `from_pssz`, `from_pssz_exact`, `from_pssz_compatible`, `pssz_validate*`,
   validated views, the schema-driven codec — rejects a record whose last
   written slot is a `None` it should have pruned. When the writer appended
   slots the reader does not know, the reader cannot judge the last slot; it
   accepts the frame and reports unknown fields. Fixed-layout records have no
   header, are never pruned and cannot evolve.
3. **Optional presence** (`docs/pssz-spec.md` 5.5). A selector byte exactly
   when the payload can encode to zero bytes (`MIN_ENCODED_SIZE == 0`):
   strings, vectors, zero-size fixed-layout records, zero-length arrays, and an
   option of a positive-minimum payload. Otherwise `None` is zero bytes and
   `Some` is the bare payload.
4. **No trailing data.** Every value occupies exactly its span.
5. **Width** as above.
6. **Strings.** The wire shape is the same for all four; the type carries the
   restriction.

   | Rust | C++ | Check |
   |---|---|---|
   | `String` | unbounded `psio::utf8_string` | UTF-8 |
   | `BoundedString<N>` | `psio::utf8_string<N>` | UTF-8, at most N bytes |
   | `RawString` (`RawStr<'a>` borrowed) | `std::string` | none |
   | `BoundedRawString<N>` | `std::string` with `length_bound{.max = N}` | at most N bytes |

   UTF-8 types reject invalid UTF-8 in decode and validate. Raw strings pass
   bytes through, except under the exact readers (`from_pssz_exact`,
   `pssz_validate_exact`, `validated_view_exact`), which check UTF-8 as C++
   `validate_strict` does. A `String` view returns `Result<&str>` and never
   panics; a `RawString` view hands out bytes.
7. **Enums.** A field-less enum encodes as its underlying integer and needs an
   explicit `#[repr(u8 | u16 | u32 | u64 | i8 | i16 | i32 | i64)]`; it is a
   fixed-size value stored inline. An enum with payloads is a variant, as C++
   `std::variant`: one index byte in declaration order, then the alternative.
   A unit alternative is a zero-field fixed-layout record (no bytes); an
   alternative with several fields is a positional tuple record.
   `#[pssz(variant)]` forces the variant encoding for a field-less enum.

Tuples are extensible records addressed by position, as C++ `std::tuple`;
`()` is a bare header. `Box<T>` pairs with C++ `psio::schema_types::Box<T>`:
wire-transparent, no inferred bound, and a `Box<Option<T>>` field is not an
omittable optional.

## Entry points

| Rust | C++ |
|---|---|
| `try_to_pssz::<F, _>(&v)` (`to_pssz` panics on error) | `psio::encode(F{}, v)` |
| `from_pssz::<F, T>(bytes)` | `validate` + `decode` (unknown fields accepted) |
| `from_pssz_compatible` → `Decoded { value, has_unknown }` | `decode_pssz_checked` |
| `from_pssz_exact` | `validate_strict` + `decode` |
| `pssz_validate` / `pssz_validate_compatible` / `pssz_validate_exact` | `validate` / `validate_pssz_compatible` / `validate_strict` |
| `PsszView::try_new`, `validated_view_compatible`, `validated_view_exact` | `validated_pssz_view` |
| `auto_pssz_width::<T>()` | `auto_pssz_width_v<T>` |
| `const { assert!(pssz_evolves_from::<Old, New>()) }` | `static_assert(psio::pssz_evolves_from<Old, New>)` |

`pssz_evolves_from` is a `const fn` and can fail the build. It compares the
`PsszPack::SHAPE` descriptors of the two types under the automatic profile:
New is Old plus trailing `Option` fields, at an unchanged width and
fixed-layout status, recursively through fields, vectors, arrays, optionals,
variants and tuples. Hand-written `PsszPack` impls are opaque and never
evolve. A directly recursive type has no finite const descriptor; use the
schema-based `pssz::upgrade::validate_optional_upgrade` for those.

Decoding untrusted input returns errors and never panics; see
`docs/rust-pssz-safety.md`.

## Differences from C++ that remain

See `docs/compatibility.md` ("PSSZ: Rust and C++") for the exhaustive list.
