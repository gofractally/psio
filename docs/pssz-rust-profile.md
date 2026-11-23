# PSSZ in Rust

The `psio` crate (`rust/psio`, derives in `rust/psio-macros`). One set of
derives and one attribute, `#[psio(...)]`, describe a type; pSSZ
(`psio::pssz`), Json / IJson text (`psio::json`), keys (`psio::key`) and the
Schema IR read that description. `rust/psio/src/lib.rs` has the attribute
table; this document is the pSSZ side. (`psio::experimental::frac`, behind
the cargo feature `experimental-frac`, is a format kept for benchmarks and
is not a supported wire format.)

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

The inferred bound is ABI and reproduces the C++ bookkeeping exactly. The
normative rule is `docs/pssz-spec.md` §3.2 (`fit`): a type that has offsets —
a record, tuple, array of variable elements, bounded list, map or set — counts
its header / slots / table at its own width, the least of 1, 2, 4 whose total
fits; one byte for every `Option`; a declared child cap reserved in full; no
bound for unbounded vectors and strings and for `Box`. (The Rust
implementation described here predates that rule — it counts four bytes per
header and slot and gives lists a 4-byte table — until it is brought in line;
`conformance/pssz-vectors.json` is the reference.)

The cap also limits the complete encoded size of the value. The automatic
profile enforces it at every node on encode, decode and validate; explicit
tags enforce it at the top level only, as C++ does.

## Declaring records

```rust
use psio::{PsszPack, PsszUnpack, PsszView, ToPsszSchema};
use psio::pssz::RawString;

#[derive(PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
#[pssz(max_dynamic_data = 200)]            // optional explicit cap
struct Account { id: u32, name: String, memo: Option<RawString> }

#[derive(PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]        // fixed layout: no header
struct Point { x: u32, y: u32 }
```

The derives are the only way to declare a record (there is no declarative
`pssz_struct!` macro). What describes the type for every format is a
`#[psio(...)]` key: `definition_will_not_change`, `transparent` (the type is
its one member), `invariant = "path"`, `variant`, `custom` / `custom = "id"`,
`name = "..."`, `rename_all = "..."`, `skip`, `other`, `crate = "..."`, and
the bounds below. `#[pssz(...)]` holds the one key that is pSSZ's own,
`max_dynamic_data = N`. A key under the wrong attribute, or an unknown key,
is a compile error that names the right spelling
(`#[pssz(definition_will_not_change)]` says to write
`#[psio(definition_will_not_change)]`). `#[serde(...)]` attributes with a
PSIO meaning (`transparent`, `rename`, `rename_all`, `skip`) are read too;
`psio::serde_attrs` has the rule for each.

A custom id reserved for PSIO's own types (`bool`, `char`, `string`,
`utf8`, `hex`, `bitvector`, `bitlist`, `map`, `set`, `unknown`, `box`, and
`time-point` / `duration` / `monotonic` alone or followed by `-`) is refused:
`#[psio(custom = "time-point-seconds")]` is a compile error and
`SchemaBuilder::insert` refuses a hand-written `ToPsszSchema` that claims
one (`psio::pssz::schema_ir::reserved_custom_id`).

## Bounds by annotation

A bound does not have to be in the type (`docs/pssz-spec.md` 3.5). A member
keeps its plain type and carries its bounds:

```rust
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
struct App {
    #[psio(max = 30)]
    name: String,
    #[psio(max = 3, element(max = 30))]
    tags: Vec<String>,
    #[psio(max = 4, keys(max = 16), values(max = 2))]
    counts: BTreeMap<String, Vec<u32>>,
    #[psio(at(0, max = 16), at(2, max = 5))]
    triple: (String, u32, Vec<u8>),
    #[psio(element(max = 16))]
    names: [String; 3],
}
```

| Rust | C++ | bounds |
|---|---|---|
| `max = N` | `max<N>` | the member: bytes of a `String` / `RawString`, elements of a `Vec`, `BTreeSet` or `BTreeMap` |
| `element(...)` | `element(...)` | every element of a `Vec`, an array or a `BTreeSet` |
| `keys(...)`, `values(...)` | `keys(...)`, `values(...)` | every key / mapped value of a `BTreeMap` |
| `at(I, ...)` | `at<I>(...)` | element `I` of a tuple |

The keys nest to any depth and an `Option` is looked through. Whether a
string is UTF-8 is its type (`String` is `utf8`, `RawString` is not), so
there is no `utf8` key. The alternatives of a variant are annotated where
the enum declares them (`Text(#[psio(max = 16)] String)`), since a Rust
variant is the user's own enum; a transparent type annotates its member
(`struct Memo(#[psio(max = 80)] String)`).

An annotated member is exactly the type the bounded types spell —
`Vec<String>` under `max = 8, element(max = 16)` is
`BoundedList<BoundedString<16>, 8>`: the same inferred bound and widths at
every depth, the same bytes in all four profiles, the same Schema IR, and
the same checks on encode, validate, decode and views, in Json / IJson text
and in keys (`rust/psio/tests/pssz_annotated_bounds.rs` compares the two
spellings per shape and against the IR C++ wrote into the fixture).
`psio::pssz::bounds` documents the mechanism: the derives take the
constants, the validator, the reader and the schema from the described
type; a value is written from the field as it stands where the bytes are
the same (lists, strings, sets) and converted first where a bound narrows
an offset width below the member (a tuple, a map entry, an array of
variable elements). Not annotatable today: `HashMap` / `HashSet`,
`Nested<T>` (its bound is the type parameter `N`), and a tuple of more than
eight elements.

## Methods and interfaces

`#[psio::interface]` on a trait is the counterpart of C++
`PSIO_REFLECT(T, method(name, params...))` + `PSIO_INTERFACE(T)`
(`psio::interface`):

```rust
#[psio::interface(name = "token")]
pub trait Token {
    #[pssz(max_dynamic_data = 256)]
    fn transfer(&mut self, #[psio(max = 16)] to: String, amount: u64, #[psio(max = 80)] memo: String);
    #[psio(returns(max = 8))]
    fn symbol(&self, id: u32) -> String;
}
```

generates a module `token` with `Tag` (`psio::interface::Interface`: the
name, `METHODS`, `build_interface`), `args::<method>` (the argument record
of each method, `psio::interface::Method`), `dispatch` / `dispatch_name`
(validated arguments in, encoded result out) and `Proxy<C>` over a
`psio::interface::Caller`. `SchemaBuilder::insert_interface::<token::Tag>()`
adds the `Interface` and `Func` nodes C++ `insert_interface<T>()` builds.

**The pSSZ of a function's arguments** is one rule in both languages: the
tuple of the (decayed) parameter types, in declaration order, with the
tuple's width — the positional extensible record of the parameters. A
record with the same members has the same bytes and width, so the Rust
argument type is a record named after the method. A parameter's annotation
is its member's; `#[pssz(max_dynamic_data = N)]` on the function is the
record's cap (C++ `maxDynamicData(N)`), which fixes the argument frame's
width so that an optional argument can be appended later;
`#[psio(returns(...))]` bounds the result (C++ `returns(...)`). A method
that returns nothing has no result bytes.

## Rules

1. **Record evolution.** An extensible record starts with a header holding the
   size of the fixed region its writer emitted. Readers honor that header, not
   their own schema: unknown trailing slots written by a newer schema are
   skipped (and still delimit the last known variable field); trailing
   optionals missing from older data read as `None`. A field appended after a
   type is published must be `Option<T>`.
2. **Mandatory pruning.** A value has exactly one encoding. The encoder always
   drops trailing `None` fields and writes the reduced header. Every checking
   reader — `from_pssz`, `pssz::decode_strict`, `pssz::decode_checked`,
   `pssz::validate*`, validated views, the schema-driven codec — rejects a
   record whose last
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
   bytes through, except under the exact readers (`pssz::decode_strict`,
   `pssz::validate_strict`, `PsszView::new_strict`), which check UTF-8 as
   C++ `validate_strict` does. A `String` view returns `Result<&str>` and never
   panics; a `RawString` view hands out bytes.
7. **Enums.** A field-less enum encodes as its underlying integer and needs an
   explicit `#[repr(u8 | u16 | u32 | u64 | i8 | i16 | i32 | i64)]`; it is a
   fixed-size value stored inline. An enum with payloads is a variant, as C++
   `std::variant`: one index byte in declaration order, then the alternative.
   A unit alternative is a zero-field fixed-layout record (no bytes); an
   alternative with several fields is a positional tuple record.
   `#[psio(variant)]` forces the variant encoding for a field-less enum.

Tuples are extensible records addressed by position, as C++ `std::tuple`;
`()` is a bare header. `Box<T>` pairs with C++ `psio::schema_types::Box<T>`:
wire-transparent, no inferred bound, and a `Box<Option<T>>` field is not an
omittable optional.

## Entry points

The helper names at the crate root are for the automatic profile; the
generic forms in `psio::pssz` take the format tag `F`.

| Rust | C++ |
|---|---|
| `psio::try_to_pssz(&v)` (`to_pssz` panics on a value with no encoding), `pssz::try_encode::<F, _>` | `psio::encode(F{}, v)` |
| `psio::from_pssz::<T>(bytes)`, `pssz::decode::<F, T>` | `validate` + `decode` (unknown fields accepted) |
| `pssz::decode_checked::<F, T>` → `Decoded { value, has_unknown }` | `decode_pssz_checked` |
| `pssz::decode_strict::<F, T>` | `validate_strict` + `decode` |
| `psio::validate_pssz::<T>`, `pssz::validate` / `validate_compatible` / `validate_strict` | `validate` / `validate_pssz_compatible` / `validate_strict` |
| `psio::from_pssz_trusted::<T>(bytes)`, `pssz::decode_trusted::<F, T>` | `psio::from_pssz<T>(psio::trusted{bytes})` |
| `PsszView::new`, `new_checked`, `new_strict`; `PsszView::trusted` | `validated_pssz_view`; `pssz_view` |
| `pssz::auto_pssz_width::<T>()` | `auto_pssz_width_v<T>` |
| `const { assert!(pssz_evolves_from::<Old, New>()) }` | `static_assert(psio::pssz_evolves_from<Old, New>)` |

**Trusted decode.** `from_pssz_trusted` is for bytes the caller vouches for
(a service's own table rows). It skips what only guards against bytes
nobody vouched for — the depth limit, caps, length bounds, canonical-form
rules, invariants, the checked reader's per-value bookkeeping — and keeps
what memory safety needs: slices are bounds-checked and a `String` is
verified UTF-8. It is therefore a safe function: invalid bytes cause a
panic or yield some value, never undefined behaviour
(`docs/rust-pssz-safety.md`).

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
