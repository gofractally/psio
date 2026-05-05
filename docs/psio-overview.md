# psio — cross-language overview

**Status:** authoritative architectural reference. Defines what psio
**is** and what every host-language implementation must provide for a
user to recognize "this is the psio library I expect, expressed in
this language's idioms."

This document is **language-agnostic**. Specific format wire layouts
(pjson, fracpack, etc.) live in their own `<format>-spec.md` files.
Implementation details for a given host (C++ template machinery, Rust
derive macros, etc.) live in that language's source.

---

## 1. What psio is

psio is a **multi-format, schema-driven, reflection-powered
serialization library**. A user declares a type once, with
annotations that capture text-representation hints, and gets:

- **Encoding to any of N wire formats** — pjson, fracpack, capnp,
  flatbuf, ssz, JSON text, CBOR, MessagePack — without rewriting the
  type.
- **Decoding from any of those formats** back into the same typed
  structure, byte-equivalent across host languages.
- **Zero-copy views** over a buffer in any of those formats — typed
  field accessors that don't allocate or materialize a value tree.
- **Format-agnostic validators** that reject malformed input without
  decoding.
- **Cross-format consistency**: the same `User` type produces
  byte-identical pjson on C++ and Rust hosts. The same `User`
  produces byte-identical fracpack on C++ and Rust hosts. Etc.

The headline value: **declare the type once, serve it everywhere.**

---

## 2. Core concepts

Every psio host implementation MUST surface these concepts, named per
the host's idioms but conceptually identical:

### 2.1 Type

A user-declared aggregate (struct, record, class) with named fields
of typed values. The type's **declaration is the wire contract** —
adding/removing/renaming fields is a wire change.

**Per-host:**
- C++: any class with `PSIO_REFLECT(...)` macro applied
- Rust: any struct with `#[derive(psio::Reflect)]`
- (Future) Python: any class with `@psio.reflect` decorator
- (Future) Go: any struct with the `psio:"..."` field-tag convention

### 2.2 Format

A wire-format-tag type that selects an encoding. Each format has its
own tag and provides `encode`, `decode`, `validate`, and (where
applicable) `view` operations dispatched by the tag.

**Built-in formats** that every host implementation MUST support:

| Format tag | Wire spec | Notes |
|------------|-----------|-------|
| `pjson` | `docs/pjson-spec.md` | Schemaless binary peer of JSON; primary format |
| `fracpack` | (TBD spec doc) | Tail-indexed offset-table layout, generic and typed |
| `json` | RFC 8259 | Text serialization for human/JS interop |
| `capnp` | Cap'n Proto wire | Cross-language IDL-driven format |
| `flatbuf` | FlatBuffers wire | IDL-driven, alignment-strict |
| `ssz`, `pssz` | Eth2 SSZ + psio variant | Beacon-chain and content-addressed payloads |

A host MAY support additional formats (msgpack, CBOR, protobuf, etc.)
via the same dispatch mechanism. **Adding a format does not require
changes to the type declarations** — only a new format-tag.

### 2.3 View

A zero-copy typed accessor over a buffer in a specific format:

```text
view: View<T, F>      // T = type, F = format
view.field<I>()       // typed access to field I
view.field_by_name(s) // dynamic key lookup
```

Constant-time access; no allocation; no value-tree materialization.
**Bounded work per read** is the load-bearing invariant.

### 2.4 Encode / Decode / Validate (CPOs)

Three customization-point operations dispatched by the format tag:

- **Encode**: `encode<F>(value: T) -> bytes`
- **Decode**: `decode<F, T>(bytes) -> T`
- **Validate**: `validate<F, T>(bytes) -> ok | error`

Each is a per-format implementation that walks the type's fields
(via reflection) and performs the wire-format-specific work.

### 2.5 Annotation channel

Per-type and per-field metadata that drives wire-form selection
without changing the field's runtime type. The annotation channel is
**format-agnostic** — the same annotation can be honored differently
by each format, but its presence is uniform.

**Examples:**

```
@bytes_encoding("hex")        — bytes field rendered as lowercase hex in JSON-text formats
@numeric_string                — integer field rendered as quoted JSON string (snowflake IDs)
@key_suffix(".unix_ms")        — object key written with this suffix
@as_decimal                    — float-valued field encoded as exact decimal (not IEEE binary)
@sorted_keys                   — object's keys are guaranteed sorted (sets the is_sorted hint)
@is_sorted                     — array's elements are guaranteed sorted
```

Every host MUST support a uniform annotation taxonomy. Format
implementations consult the annotation channel during dispatch; the
binding is **annotation determines wire form**, not the value.

---

## 3. Cross-language consistency

The single load-bearing invariant: **the same type, with the same
annotations, produces byte-identical wire output across all host
languages, for every supported format.**

```text
// C++
auto bytes_cpp = psio::encode<pjson>(user);
auto bytes_cpp_frac = psio::encode<fracpack>(user);

// Rust
let bytes_rust = psio::encode::<Pjson>(&user)?;
let bytes_rust_frac = psio::encode::<Fracpack>(&user)?;

assert(bytes_cpp == bytes_rust);
assert(bytes_cpp_frac == bytes_rust_frac);
```

Enforced via **the conformance corpus**: shared `*.json` fixtures
with input value, expected wire bytes, expected JSON projection. Both
host-language driver binaries must produce identical output for every
fixture across every format.

This is the strongest signal that psio is the same library in every
language. If a fixture passes on C++ but fails on Rust, it's a port
defect, not a language preference.

---

## 4. Per-language implementation expectations

Every host language implementation MUST provide:

### 4.1 Reflection mechanism

A way to walk a type's fields at compile time (or runtime if the
host doesn't have compile-time facilities). The mechanism is
host-idiomatic:

| Host  | Mechanism |
|-------|-----------|
| C++   | `PSIO_REFLECT(T, field1, field2, ...)` macro that emits a `psio::reflect<T>` specialization |
| Rust  | `#[derive(psio::Reflect)]` proc-macro generating a `Reflect for T` impl |
| Python| `@psio.reflect` class decorator using PEP 484 type hints |
| Go    | Field-tag convention `` `psio:"name"` `` walked via the `reflect` package at runtime |
| Java  | Annotation processor or `Field` reflection |
| JS/TS | Schema definition function or TypeScript type → reflected metadata |

The output of the reflection step is uniform: an iterable list of
`(name, type, annotations)` tuples per type, plus type-class
information (struct vs union vs enum).

### 4.2 Annotation channel

A way to attach per-type and per-field metadata, accessible during
the reflection walk. Same taxonomy across hosts (§2.5).

| Host  | Mechanism |
|-------|-----------|
| C++   | `psio::annotate<X>` template wrapping the field type, OR `PSIO_REFLECT_TYPE_ANNOTATIONS(...)` for type-level |
| Rust  | `#[psio(annotation = "value")]` attribute on the field/struct |
| Python| Class decorator with field metadata, OR `Annotated[T, psio.AnnotationName]` from `typing` |
| Go    | Extended field-tag syntax: `` `psio:"name,bytes_encoding=hex"` `` |
| Java  | `@PsioAnnotate(...)` field annotation |
| JS/TS | Decorators (TC39) or schema-builder fluent API |

### 4.3 Format-tag dispatch

A way to select the wire format at the call site. Whether via traits,
templates, function-overload sets, or runtime registry.

| Host  | Mechanism |
|-------|-----------|
| C++   | `psio::encode<format_tag>(v)` — function template specialized per format, dispatched via `tag_invoke` |
| Rust  | `psio::encode::<F: Format>(&v)` — generic over a `Format` trait |
| Python| `psio.encode(v, format='pjson')` — string lookup against a registry |
| Go    | `psio.Encode(v, psio.Pjson)` — interface-typed format parameter |

### 4.4 Conformance driver

Every host language implementation MUST provide a binary that:

1. Reads a conformance-corpus fixture on stdin.
2. Encodes the fixture's `input_value` per the format under test.
3. Compares to the fixture's `wire_hex`.
4. Decodes `wire_hex` back to a value.
5. Compares structurally to `input_value`.
6. Renders to JSON and compares to `json_compact` (when applicable).
7. Exits 0 on match, non-zero on any mismatch.

The driver runs as `psio_<format>_conformance_driver --check`. The
existing `pjson_conformance_driver` (in C++ and Rust) is the
canonical example; future formats and hosts mirror its shape.

### 4.5 Self-test mode

The driver MUST support a `--self-test` mode that asserts
implementation-property invariants (e.g., the §3 tag-byte predicates
in pjson). Mirrors what `cpp/conformance/pjson_conformance_driver`
and `rust/psio/src/bin/pjson_conformance_driver.rs` do today.

---

## 5. Format catalog and capability matrix

A new format-tag implementation in psio MUST provide:

| Capability | Required? | Notes |
|------------|-----------|-------|
| `encode<F>(v: T)` | yes | Walks reflection, produces wire bytes |
| `decode<F, T>(bytes)` | yes | Reads wire bytes, materializes T |
| `validate<F, T>(bytes)` | yes | Bounded-work check; no decode |
| `View<T, F>` | strongly preferred | Zero-copy field access |
| `from_json(text)` ↔ `to_json(F-buffer)` | when applicable | Required for text-formats and pjson; optional for binary-only formats |
| Self-test mode | yes | Driver `--self-test` arm |
| Conformance corpus | yes | Shared `*.json` fixtures matching the format's wire bytes |

A format that doesn't implement `validate` or `View` is **not
production-ready**; the dispatch CPO returns "not implemented" rather
than silently doing the wrong thing.

---

## 6. Annotation taxonomy (initial)

The annotation channel carries metadata that's meaningful across
formats. Below is the **starter taxonomy** every host MUST recognize.
Format implementations MAY honor more, but these are the cross-format
baseline:

### 6.1 Bytes-presentation hints

- `bytes_encoding = base64` (default) | `hex` | `base58` | `base64url`
  — controls JSON-text rendering of byte fields. Wire format MAY
  carry this hint inline (pjson §4.10); other formats encode raw
  bytes and rely on the schema annotation alone.

### 6.2 Integer-presentation hints

- `as_numeric_string` — emit as quoted JSON string in JSON-text
  formats; in binary formats, may use a wrapper type (pjson
  `numeric_string`, §4.8) or just behave normally.
- `as_decimal` — for float-valued fields: encode as exact decimal
  (mantissa + scale). pjson uses `decimal` (§4.7), other formats
  may use a string representation.

### 6.3 String-presentation hints

- `escape_form` — text is already JSON-escape-encoded; emitter
  doesn't re-escape. (pjson §4.9 flag 1)
- `raw_text` (default) — text is unescaped Unicode; emitter runs the
  escape pass.

### 6.4 Container-shape hints

- `sorted_keys` (object) — keys are in lex-sorted byte order.
  Enables binary-search lookups (pjson §E-001).
- `is_sorted` (array) — element values are sorted. Enables binary
  search in array views.
- `key_suffix = ".tag"` — object key carries this suffix for the
  text-format projection.

### 6.5 Format-version hints (future)

- `since_version = 2` — field added in spec version 2; older
  decoders must skip-with-default.
- `extension_subtype = N` — value is a pjson `extension` (§4.11)
  with sub-type id N.

---

## 7. Conformance — the cross-language gate

Every commit that touches encoding behavior MUST pass:

```sh
$ tools/run-conformance.sh
cpp:    N/N passed
rust:   N/N passed
cross-validation: M/M matched   # < the load-bearing line
```

`cross-validation` runs every fixture's `input_value` through both
host-language drivers and asserts they produce identical
`wire_hex` and `json_compact`. A mismatch is a port defect; it
blocks merge.

`docs/spec-compliance.md` enumerates every normative claim from the
format specs. Every row must be ✅ on every implemented host before
that format is considered shipped on that host.

---

## 8. Versioning

Format wire formats version at the format-spec level (`pjson-spec.md`
§13, `fracpack-spec.md` §X, etc.). The psio cross-language API
versions independently — adding a new annotation or format tag is a
psio API change, not a wire-format change.

A psio implementation MAY support older format versions for
backward-compat reading; it MUST NOT silently produce older wire
bytes when newer would be canonical.

---

## 9. What this overview does NOT define

- Specific format wire formats (those have their own `<format>-spec.md`).
- Per-host build/dependency conventions (see `docs/dependency-pattern.md`).
- Per-host API ergonomics (return types, error model, async).
- Performance targets (each format has its own §1 headline numbers).

What it DOES define: **the conceptual model and binding requirements
that any host implementation must satisfy to be called "psio."**

---

## 10. Adding a new host language

Concrete checklist for porting psio to a new host (Python, JS, Java,
etc.):

1. Pick the host's reflection mechanism (§4.1) and document it in
   `docs/dependency-pattern.md`.
2. Pick the host's annotation syntax (§4.2) and document the mapping
   from §2.5's taxonomy to the host's syntax.
3. Pick the host's format-tag dispatch (§4.3).
4. Implement at least the `pjson` format end-to-end.
5. Implement the conformance driver (§4.4) and pass the existing
   conformance corpus.
6. Add a `<host>` row to the conformance harness in
   `tools/run-conformance.sh` and the cross-validation step.
7. Add ✅ to the relevant matrix rows in `docs/spec-compliance.md`
   when each row passes for the new host.

If all six steps complete, the new host is "psio-conforming" — users
can write a type once and serve it from this host as expected.
