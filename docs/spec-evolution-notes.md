# pjson spec evolution notes

Documented future work for pjson-spec.md beyond v1. None of these are
in v1; they're tracked here so they don't get lost between conversations.

Each entry includes the design intent, the proposed mechanism, and any
constraints that would need resolving before promoting to the spec.

---

## E-001 — `is_sorted` hint bit on arrays AND objects

**Intent.** When the encoder knows the children of an `array` (§5.1)
are sorted by value, OR the entries of an `object` (§5.2) are sorted
by key bytes, record that fact so a view can use it.

**Use cases enabled in views:**

For arrays:
- O(log N) binary search by child value, instead of the current O(N) scan.
- Merge-join two sorted arrays without materializing intermediate state.
- Membership queries against a sorted typed homogeneous array become a
  single `lower_bound`-style probe.
- Decoder fast paths that need not preserve insertion order can build
  set-like data structures without re-sorting.

For objects:
- O(log N) `lower_bound(key)` and range queries via binary search on
  the slot table (instead of the hash-prefilter + linear-verify scan).
- Merge-join two sorted-key objects in O(N + M) without re-hashing.
- Trivial sorted-traversal — the slot table already is the iteration
  order.

**Sources that get the bit "for free":**

- C++ `std::map<string, T>` and `std::set<T>` — sorted by spec.
- Rust `BTreeMap<String, T>` and `BTreeSet<T>` — same.
- Go's `encoding/json` `MarshalSorted` mode; Python's `json` with
  `sort_keys=True`; serde_json when fed a `BTreeMap`.
- Canonical-JSON encoders (RFC 8785, JCS) — always lex-sorted by spec.

**Proposed mechanism — objects (§5.2).**

The object width byte already has reserved bits:

```
[width byte: low 2 bits = slot_w_code (u8/u16/u24/u32); bits 2..7 reserved]
```

Assign **bit 2 = is_sorted_keys**:

```
bit 0..1 = slot_w_code
bit 2    = is_sorted_keys (1 = slot table in lex-sorted key-byte order)
bit 3..7 = reserved
```

Free bit, no wire-size cost. The same width byte already exists in
generic arrays (§5.1) and we'd add an equivalent **bit 2 = is_sorted_values**
there.

**Sort key for objects** — open question: as-stored or
suffix-stripped (§7.4)?

- **As-stored** — `"amount.b64"` and `"amount.hex"` sort distinctly.
  Deterministic total order over key bytes. Matches what the bytes
  literally are.
- **Suffix-stripped** — collides with the hash-byte stripping
  convention but leaves ties unresolved.

Recommendation: **as-stored**. Sorting is a layout property; suffix-
stripping is a hash-prefilter concern. Don't conflate.

**Proposed mechanism — typed arrays (§5.1.1).**

Typed arrays don't have a width byte (no slot table to encode width
for). Two options:

- (a) **Burn a low-nibble bit on the tag.** Currently low_nibble 1..10
  encodes element_code 0..9. Reserved range is 11..15 (5 codes).
  Reframe the low nibble as `[is_sorted_bit (bit 3)] [element_code 0..7
  (bits 2..0)]`. That fits 8 element codes; `f32`/`f64` (codes 8/9)
  would need a separate handling.
- (b) **Add a 1-byte width-byte before the body.** Mirrors the generic
  array / object structure. Costs 1 byte per typed array; uniform
  with the rest.

Recommendation: **(b)**. The 1-byte cost amortizes across N elements,
and the uniform layout simplifies decoder code paths. Generic array
and typed array would share the same width byte structure — only the
element-format differs.

**Validator policy** (linked to E-002 below):

- **Lenient** — trust the bit; views can do binary search without
  verifying sortedness. If the encoder lied, lookups may return wrong
  answers (the cost of trusting an untrusted source).
- **Strict** — validator walks the slot table and confirms order
  before accepting the bit. Becomes E-002's `verify_sorted_hints` flag.
- **Encoder rule**: emitting the bit means "I guarantee sorted
  order." Lying is a wire-format bug.

**Wire-byte cost summary:**

| container | bit cost | extra wire bytes |
|---|---|---|
| generic array (§5.1) | bit 2 of width byte | 0 (bit was reserved) |
| object (§5.2)        | bit 2 of width byte | 0 (bit was reserved) |
| typed array (§5.1.1) | new width byte      | 1 (per typed array) |

**Status.** Deferred from v1. Revisit when a real consumer needs it.
Object-side hint is the highest-leverage of the three because
JSON-from-sorted-maps is extremely common in API workloads.

---

## E-002 — Validation extent: template-parameterized verifier

**Intent.** A pjson buffer carries claims (offset table monotonicity,
hash byte correctness, canonical encoding, sorted-array hint, etc.)
that range from "must-check-or-the-parser-will-crash" to "claim that
costs work to verify but failure-to-check just produces wrong
results."

A single `validate()` function that always checks everything is
either too slow for hot paths or too lax for adversarial inputs.
Different callers want different points on the spectrum.

**Proposed mechanism.** A `validate<Policy>(...)` template (or
runtime-flag equivalent) where `Policy` is a struct of bool flags:

```cpp
struct validation_policy {
    // Always on. Failure means parser would crash or produce
    // out-of-bounds reads. No legitimate caller can disable.
    static constexpr bool bounds_safety = true;

    // Reject reserved tags / reserved low-nibble bits.
    bool reject_reserved          = true;

    // Verify slot-table monotonicity (offsets increase) and
    // count consistency.
    bool verify_slot_invariants   = true;

    // Verify hash[i] == key_hash8(stored_key_i). Without this,
    // a malformed buffer can cause every key lookup to miss.
    bool verify_hash_bytes        = true;

    // Verify canonical encoding (smallest bc, RNE rounding,
    // canonical NaN, no negative zero on negint). Strict mode
    // for content-addressable / signed payloads.
    bool verify_canonical         = false;

    // Verify "is_sorted" array hints actually hold (E-001).
    // Cheap for typed arrays, expensive for generic.
    bool verify_sorted_hints      = false;

    // Verify numeric_string inner is one of {2..7} and that the
    // canonical-decimal of the inner equals the JSON form an
    // emitter would render.
    bool verify_numeric_string    = true;
};
```

Pre-defined policy presets:

- `policy::just_dont_crash`     — only `bounds_safety` (always on).
- `policy::default_safe`        — bounds + reject_reserved + slot_invariants
                                  + hash_bytes + numeric_string.
- `policy::strict_canonical`    — everything, including canonical-encoding
                                  and sorted-hint verification. Use for
                                  signed/content-addressed buffers.

**Spec-level question.** Should the spec normatively define these
presets, or stay implementation-agnostic and just enumerate the
validatable invariants? Suggest the latter — the spec lists every
checkable claim in a single section; implementations choose the
preset taxonomy.

**Status.** Deferred from v1. The current spec already requires
parsers to bounds-check (§9 errors); the template-parameterized form
is a code-organization concern more than a spec concern.

---

## E-003 — Sub-type id assignments for `extension` (§4.11)

**Intent.** v1 ships the `extension` framework with no sub-type ids
populated. Future v1.x revisions should bless ids 0..7 for
common-enough types; ids 8..15 stay private/experimental.

**Candidate sub-types** (all forward-compatible — adding a sub-type
id doesn't break v1.0 readers):

| id | name | body |
|----|------|------|
| 0 | `uuid` | 16 raw bytes (RFC 4122 byte order) |
| 1 | `ipv4` | 4 raw bytes (network order) |
| 2 | `ipv6` | 16 raw bytes (network order) |
| 3 | `duration_ns` | varint of i128 nanoseconds (sub-spec for the varint) |
| 4 | `timestamp_unix_ns` | varint of i128 nanoseconds since Unix epoch |
| 5 | `cid` | content-addressed reference (multihash format) |
| 6 | `ed25519_pubkey` | 32 raw bytes |
| 7 | `ed25519_signature` | 64 raw bytes |

**Status.** Deferred from v1. Each blessed id needs its own conformance
fixture and JSON-projection rules. Promote one at a time as concrete
applications require them.

---

## E-004 — `bytes` encoding hint extensions

§4.10 currently defines hints 0..3 (base64, hex, base58, base64url).
Future candidates if a real use case shows up:

- `4 = base32` (RFC 4648)
- `5 = z85` (ZeroMQ ascii85 variant)
- `6 = ascii85` (Adobe ascii85)

**Status.** Deferred. The four current hints cover the dominant cases
(JSON APIs, hex-coded crypto material, Bitcoin-class base58, URL-safe
base64). Adding more is cheap (low-nibble has space) but not motivated
yet.

---

## E-006 — psio annotations drive pjson encoding (C++ binding)

**Intent.** The C++ side of the project already has `psio::reflect<T>`
and an annotation channel (`psio::annotate<X>`) that lets a type
declare per-type and per-field text-representation hints. These
annotations are part of psio's existing infrastructure (predates
pjson). When pjson encodes a typed C++ value via psio's reflection
layer, **the encoder must respect and emit per the annotations.**

**Hard rule (binding constraint, parallel to E-005's rule for serde).**

> When pjson encodes a typed C++ value through `psio::reflect<T>`,
> per-type and per-field annotations drive the encoding choice.
> The annotations are part of the type's contract — they specify
> the JSON text representation the consumer expects, and pjson's
> wire form must match.

**Examples of annotations that should map to pjson encodings:**

| psio annotation                          | pjson encoding choice                              |
|------------------------------------------|----------------------------------------------------|
| `bytes_encoding = "base64"`              | `bytes` with hint = 0                              |
| `bytes_encoding = "hex"`                 | `bytes` with hint = 1                              |
| `bytes_encoding = "base58"`              | `bytes` with hint = 2                              |
| `bytes_encoding = "base64url"`           | `bytes` with hint = 3                              |
| `as_numeric_string` on a `u64` field     | `numeric_string` wrapping `uint`                   |
| `as_numeric_string` on a typed integer   | `numeric_string` wrapping the corresponding int    |
| `as_decimal` on a `Decimal`-valued field | `decimal` (not `ieee_float` — even if shorter)     |
| `key_suffix = ".b64"` on a field         | object key written as `<name>.b64` (suffix-stripped at hash byte per §5.3) |
| `sorted_keys` on a struct                | (future) is_sorted hint bit per E-001 set on the object |
| `is_sorted` on a `std::vector`           | (future) is_sorted hint bit per E-001 set on the array |

**Why this matters.** psio's value proposition is that the type's
declaration owns the wire shape — the user writes the type once, with
annotations, and every format (pjson, fracpack, capnp, flatbuf, etc.)
emits the same value with the same external representation. pjson is
no exception: a `User` struct with a `binary_blob` field annotated
`bytes_encoding = "hex"` MUST produce pjson `bytes` with hint=1, so a
JSON consumer sees `"deadbeef"` whether the source was pjson, JSON,
or any other psio-supported format.

**Constraint enforcement.**

- **Native typed input** (the dominant C++ path): `psio::reflect<T>`
  walks the type. For each field, the encoder dispatch consults the
  annotation channel and selects the pjson form accordingly. Zero
  byte-inspection of values — the annotation IS the instruction.
- **JSON-source input:** the JSON parser produces a generic value
  tree. Annotations from a target schema (when one is provided) can
  still drive the ingress: `from_json<User>(text)` knows the schema,
  so a JSON string in a `bytes_encoding = "hex"` field is decoded
  via hex, not stored as `string`. Without a schema, the §4.8 / §7.1
  default rules apply (numeric_string lift detection, etc.).

**Status.** Not deferred — this is a **binding requirement** for the
production C++ encoder. Tracked here as a design contract that must
hold across the existing-library-migration phase (when
`cpp/include/psio/pjson*.hpp` migrates to the post-audit wire format
and the conformance driver's logic gets promoted to library code).

The conformance driver itself does NOT use `psio::reflect<T>` — it
has its own DSL — so this requirement is invisible at the driver
level. The library implementation that follows the driver must
honor it.

**Not in scope here:**
- Defining a complete annotation taxonomy. That's psio's
  responsibility; pjson consumes whatever annotations psio provides
  and maps each to its wire form.
- Cross-format consistency (e.g., the same annotation must produce
  consistent JSON output across pjson, fracpack, etc.). That's
  enforced at the format-tag-base layer, not pjson-internally.

---

## E-005 — serde adapter (Rust compatibility mode)

**Intent.** Provide a Rust serde adapter so any type that derives
`Serialize` / `Deserialize` round-trips through pjson with no extra
code. This is a **compatibility layer** that leverages existing serde
infrastructure (the rich `#[derive]` ecosystem, format-agnostic
container types) — it is **not** a redefinition of the wire format.

**Hard rule (binding constraint).**

> When encoding a native typed value, **the host type controls.**
> The encoder respects what the type system tells it (a `Vec<u32>`
> is a typed homogeneous array; a `String` is a string; a `BTreeMap`
> is a sorted-key object). The encoder **only peeks at string
> contents** when parsing JSON source — to detect the JSON-number
> grammar and lift to `numeric_string` (§4.8) on the JSON-side
> ingress path.
>
> pjson does **not** change its wire format to fit serde's data
> model. Serde adapts to pjson's types via attribute newtypes and
> wrapper types where necessary.

**Why this matters.** Serde's data model is a lowest-common-denominator
intersection across many formats — it doesn't have first-class
representations for several pjson types:

| pjson type           | serde model has...                            | adapter strategy                  |
|----------------------|-----------------------------------------------|-----------------------------------|
| `numeric_string`     | (no equivalent — `String` looks like text)    | `pjson::NumericString<T>` newtype |
| `bytes` + hint       | `Vec<u8>` or `serde_bytes::ByteBuf`           | `pjson::Bytes<HINT>` newtype OR a `#[pjson(bytes_encoding = "hex")]` field attr |
| `decimal` (§4.7)     | (no equivalent — `f64` loses precision)       | `pjson::Decimal` type, similar to `rust_decimal::Decimal` shape |
| `string` `raw_text` vs `escape_form` flag | `String` is just bytes      | adapter defaults to `escape_form` when invoked from JSON path; `raw_text` for typed `String` from native code |
| typed-array element-code selection | sequence visitor, no element-type peek | rely on Rust's static typing — `Vec<u32>` → element_code 6, etc. |
| `row_array`          | sequence-of-struct, but no homogeneity bit    | adapter detects when `Vec<S>` for reflected `S`; emit row_array unconditionally for that shape |

**Constraint enforcement.**

- **Native typed input:** the adapter sees `T` at compile time. It
  emits the natural pjson form (typed array for `Vec<u32>`, row_array
  for `Vec<Struct>`, decimal for `Decimal`, etc.) with **zero
  inspection of string bytes**.
- **JSON-source input:** when the entry point is a JSON parser
  (e.g. `pjson::from_json_str`), the encoder runs the §4.8
  numeric-string-lift detection on string fields. This is the
  **only** byte-inspection path the spec allows.

**Status.** Deferred to **Phase 6** (after Phases 1–5 complete).
Order:

| Phase | scope |
|-------|-------|
| 6.1 | `Serializer` for primitives (null, bool, ints, floats, strings, bytes via newtypes) |
| 6.2 | `Serializer` for sequences and maps (generic array, object) |
| 6.3 | `Serializer` for typed-array detection (specialization or attribute-driven) |
| 6.4 | `Serializer` for row_array detection (struct-shape homogeneity) |
| 6.5 | `Deserializer` symmetric to all of the above |
| 6.6 | Integration tests against the conformance corpus — every fixture's `input_value` must round-trip through serde with the same wire bytes |

**Out of scope.**

- C++ analog: pjson's C++ side already has `psio::reflect<T>` doing
  the type-driven dispatch. No "compatibility-mode for some other
  C++ serialization framework" is planned. If a parallel C++ effort
  is wanted later (e.g. Boost.PFR adapter), it would be its own
  evolution-note entry.
- Changing the wire format to make serde's life easier. The
  binding constraint above is non-negotiable.

---

## E-008 — Rust-side reflection + annotations + generic views

**Intent.** Build a Rust analogue of psio's C++ infrastructure
(`psio::reflect<T>`, `psio::annotate<X>`, format-tag dispatch, generic
zero-copy views over arbitrary formats). The serde adapter (E-005) is
a compatibility layer for *existing* infrastructure; **E-008 is the
native Rust pjson library architecture** that pjson encoders and
decoders should ultimately ride on.

**Why both E-005 AND E-008?** They serve different audiences:

- **E-005 (serde adapter):** users already invested in serde's
  derive ecosystem get pjson support for free. They keep their
  existing `#[derive(Serialize, Deserialize)]` types and add a new
  format. Coverage is broad but constrained by serde's data model.
- **E-008 (native Rust library):** users who want pjson's full
  expressive power — multi-format dispatch, zero-copy views,
  annotation-driven encoding, runtime reflection — write types
  against the native Rust API. Mirrors the C++ side's architecture
  exactly so a `User` struct in Rust and the same `User` in C++
  produce byte-identical pjson on every wire format.

**Components needed.**

| Rust | C++ analogue | role |
|------|--------------|------|
| `psio::reflect<T>` derive | `psio::reflect<T>` macro/concept | walks a type's fields at compile time |
| `psio::annotate` attribute | `psio::annotate<X>` template | per-type / per-field metadata channel |
| `psio::format_tag<F>` trait | `psio::format_tag_base<F>` template | format-dispatch base |
| `psio::View<T, F>` | C++ view/typed-view classes | zero-copy view over a buffer in format F, typed as T |
| `psio::encode<F>(v)` | C++ encode CPO | format-dispatched encoder |
| `psio::decode<F, T>(buf)` | C++ decode CPO | format-dispatched decoder |
| `psio::validate<F, T>(buf)` | C++ validate CPO | format-dispatched validator |

**Annotation-driven encoding.** The same binding constraint as E-006
applies on the Rust side: when `psio::encode::<Pjson>(&user)` walks
`User`'s fields, the annotation channel drives the wire-form
selection.

```rust
#[derive(psio::reflect)]
struct User {
    id: u64,
    #[psio(numeric_string)]
    snowflake_id: u64,                    // → numeric_string wrapping uint
    #[psio(bytes_encoding = "hex")]
    pubkey: Vec<u8>,                      // → bytes hint=1
    #[psio(bytes_encoding = "base58")]
    address: Vec<u8>,                     // → bytes hint=2
    #[psio(key_suffix = ".unix_ms")]
    created_at: i64,                      // → object key written as "created_at.unix_ms"
}

let bytes = psio::encode::<Pjson>(&user)?;
let user2: User = psio::decode::<Pjson, User>(&bytes)?;

// Same type, different format:
let json_text = psio::encode::<Json>(&user)?;
let frac_bytes = psio::encode::<Fracpack>(&user)?;
```

**Generic views.** A pjson buffer should support typed-view access
that's symmetric to C++'s `view::field<I>()` pattern:

```rust
let view = psio::View::<User, Pjson>::from_buffer(&bytes)?;
let id: u64 = view.id();                              // typed accessor
let pubkey: &[u8] = view.pubkey_bytes();              // zero-copy slice
let snowflake: psio::NumericString<u64> = view.snowflake_id();
```

The view type is generated at compile time from `psio::reflect<T>`'s
output — same approach as the C++ side.

**Cross-format consistency requirement.** The same Rust type must
produce byte-identical pjson on every host language (C++ and Rust).
This is enforced at the conformance corpus level:

```
fixture user_with_snowflake_id.json:
  - input_value: a Rust/C++ User struct with annotated fields
  - wire_hex:   the canonical pjson byte sequence
  - Both `psio::encode::<Pjson>(&user)` (Rust) and
    `psio::encode<pjson>(user)` (C++) must produce wire_hex.
```

**Status.** Phase 7+ work — after the conformance driver and
production library are stable across pjson, the Rust library
architecture migrates from "ad-hoc Value enum + serde adapter" to
the full reflection/annotation/views model. The serde adapter (E-005)
remains as a compatibility layer for users who don't migrate.

**Sequencing.** Likely order, after Phases 1–6 complete:

| Phase | scope |
|-------|-------|
| 7.1 | `psio::reflect` derive — walk struct fields at compile time |
| 7.2 | `psio::annotate` attribute — per-field metadata in the derive output |
| 7.3 | `psio::format_tag<F>` trait + `Pjson` tag impl using the conformance driver's logic as the kernel |
| 7.4 | `psio::encode::<Pjson>(&T)` / `psio::decode::<Pjson, T>` driven by reflect+annotate |
| 7.5 | `psio::View<T, Pjson>` with typed-accessor methods |
| 7.6 | Cross-format consistency tests — same types emit consistent bytes across `Pjson`, `Fracpack`, etc. |

**Out of scope for this entry:**
- Defining the format-dispatch trait surface in detail. That's a
  large architectural exercise that depends on which other formats
  the Rust library targets (just Pjson? Pjson + Fracpack?
  full multi-format parity with C++?). Tracked as a follow-on once
  the requirement is concrete.
- Naming bikeshed. `reflect`, `annotate`, `format_tag` are placeholders.

---

## E-009 — Renamed fields in the PSSZ compatibility check

**Status.** Open: owner decision. No behaviour was changed.

**What the spec says.** `pssz-spec.md` §7.6 lists "Rename a field" under
what cannot change, and says in the same line why it is different from
every other entry there: "the wire format doesn't carry names …, so this
is a no-op for the wire but consumers using reflected names will break".
§7.5 says the C++ check `pssz_evolves_from` "checks every rule of this
section and the next", which for this one rule it does not.
`annotations.md` §8.4 has the row "rename a field: binary and key yes;
breaks text and schema clients".

**What each implementation does.**

| | record field renamed | variant alternative renamed | type renamed |
|---|---|---|---|
| C++ `pssz_evolves_from` | accepted (members are compared by position and type) | accepted | accepted, except a `transparent()` type, which must keep its name |
| Rust `pssz_evolves_from` | accepted (`Shape` carries no member names) | accepted | accepted |
| Python `psio.evolves_from` | accepted, and documented: "Member *names* are not compared: the wire does not carry them" | accepted | accepted |
| JS `psszEvolvesFrom` | **rejected**: `field N was renamed or reordered ("a" -> "b")` | **rejected** | accepted ("the names of the types themselves are not compared") |

A tuple element has no name in any of them.

**What a rename breaks.** Not PSSZ bytes and not keys (both positional).
It breaks every surface that names the member: JSON / I-JSON objects (and
the object form of a method's named arguments), the published Schema IR
(`Member.name`, `Func.params[i].name`), GraphQL field names, WIT and the
other schema emitters, and any stored JSON.

**Options.**

- **(a) Reject a rename always.** One rule everywhere; the check then
  covers the text contract as well as the bytes. Cost: C++ / Rust / Python
  start refusing revisions they accept today (C++ has the member names at
  compile time; Rust's `Shape` needs them added by the derive; Python has
  them in the compiled node). A binary-only user loses the ability to say
  "these bytes are compatible" for a renamed field unless the layout-only
  check stays available under its own name.
- **(b) Reject by default, with an explicit "renamed from" annotation.**
  `attr(owner_id, renamed_from("owner"))` makes the rename deliberate and
  visible to the check, and could later let text readers accept the old
  key. Cost: a new annotation and a Schema IR attribute (`renamedFrom`) in
  four languages, plus the question of whether readers honour it (an
  alias) or only the check does. It is the only option that can make a
  rename *compatible* for text; it is also the largest.
- **(c) Ignore names, as C++ / Rust / Python do now**, and document that
  JSON and schema names are part of the contract and are not covered by
  the check. Cost: JS has to drop its name comparison (or keep it behind
  an option) to agree; the most common accidental break of a JSON API
  stays invisible to the tool whose job is to catch breaks.

**Recommendation.** Two checks with honest names rather than one check
with two meanings, then (a) as the default:

1. Keep the layout relation exactly as C++ / Rust / Python compute it
   today, under a name that says "bytes": it answers "does the new reader
   read the old bytes".
2. Make the headline check (`pssz_evolves_from`, `psszEvolvesFrom`,
   `evolves_from`) that relation **plus** equal member names — record
   fields, variant alternatives, and the parameter names of an argument
   list whose text form is the object. Type names are not compared (the
   old revision is normally kept as a copy under another name). That is
   JS's behaviour today, so JS does not change.
3. Defer (b) until a real rename needs to be carried through a schema; it
   layers on top of (a) without changing it.

The reason for (a) over (c): services publish their Schema IR and clients
pack arguments and read rows by name, so a rename is the change most
likely to be made by accident and least likely to be noticed by a test of
bytes. If the owner prefers the check to stay bytes-only, (c) is coherent
too, but then §7.5's "checks every rule of this section and the next" and
the JS check must both be corrected to say so.

---

## E-010 — `sorted` / `unique` / `unique_keys` in the Schema IR

**Status.** Design note for a later round. Not implemented.

**Today.** The three order constraints (`pssz-spec.md` §3.5) are typed
only. C++ (`psio::sorted`, `unique`, `unique_keys`), Rust
(`#[psio(sorted)]`, …), JS (`sorted` / `unique` / `uniqueKeys` member
options) and Python (`psio.sorted`, …) enforce them in their typed codecs.
The Schema IR does not carry them, so a schema-driven reader of the same
schema accepts an unordered list, `pssz_evolves_from` cannot compare them
between two fetched schemas, and no shared fixture row covers them (each
language tests its own typed enforcement).

**How to put them in the IR.** Two candidates.

*A. Wire custom ids over the list node* (recommended). The same device
that already carries the order of a map and a set
(`Custom{List<Tuple<K, V>>, "map"}`, `Custom{List<K>, "set"}`):

| constraint | node |
|---|---|
| `sorted` | `Custom{<list or array>, "sorted"}` |
| `sorted`, descending | `Custom{…, "sorted-desc"}` |
| `unique` (`sorted_unique`) | `Custom{…, "unique"}` |
| `unique`, descending | `Custom{…, "unique-desc"}` |
| `unique_keys` | `Custom{…, "unique-keys"}` |

`<list or array>` is `List`, `BoundedList`, `Array`, or the `"hex"` custom
over one. `unique_keys` together with `sorted` nests, `"unique-keys"`
outermost. It works at every depth with no extra grammar
(`element(sorted)` is just the node of the element type), for a method
parameter and result, and for a list that is a root type. A reader that
does not know the ids treats the node as its wire type, which is exactly
today's behaviour; a strict registry refuses it. The ids join the closed
list of wire ids that take no handler (`bool`, `char`, `string`, `utf8`,
`hex`, `bitvector`, `bitlist`, `map`, `set`, `box`, `unknown`).

*B. Attributes.* On the member, in the camelCase of the existing ones
(`lengthBound`, `maxDynamicData`, `maxFields`): `sorted` = `"asc"` |
`"desc"`, `unique` = `"asc"` | `"desc"`, `uniqueKeys` = `""`; on a `Func`,
`result.sorted` etc. (the existing `result.` prefix). Type nodes and type
interning are untouched. But a constraint below the member needs a path in
the attribute name (`element.sorted`, `values.element.unique`,
`at1.sorted`), and a list that is not a member (a root type, a named
alias) has nowhere to carry it. `lengthBound` avoids this only because a
bound below the member always has a type form (`BoundedList`); an order
constraint has none.

**Which codecs must order dynamic values.** Every schema-driven entry
point that enforces map / set order today, for the same reason and with
the same comparison (bytewise order of the elements' `key` encodings):

- PSSZ: checked validate, decode to a dynamic value, encode from one;
- JSON / I-JSON: text → PSSZ and PSSZ → text (which validates its input);
- key: value → key and PSSZ → key.

Not the trusted decode and not views, as for the typed codecs. C++ has the
pieces in `detail/pssz_ir_assoc.hpp` (`key_at`, `assoc_order`,
`key_bytes_less`, `has_key`); what is new relative to a set:

- `sorted` allows equal neighbours (`<=`), `unique` does not (`<`);
- descending order;
- `unique_keys` requires no order: compare a sorted copy of the key
  encodings, O(n log n) time and O(n) memory, with the reader's existing
  size limits;
- the key of an element is the element, or element 0 of a pair / tuple
  element for `unique_keys`;
- an element type with no key encoding (whatever `has_key` refuses for
  a map key: "the key type of a map / set has no key encoding") makes the
  schema invalid, at schema validation, not per value.

**Evolution.** The ids are part of the type: `pssz_evolves_from` and its
mirrors compare them as they compare `map` / `set` (no change allowed in
either direction, §7.6), now also between two fetched schemas.

**Fixture rows needed.**

- `pssz-vectors.json`: a class `list-order`. For each of the five
  constraints × element kinds (`u32`, raw string, UTF-8 string,
  `tuple<u32, string>`, a record) × positions (record member; below a
  member via `element(...)`; inside an optional; an array; a list of
  lists): one `canonical` row in order, one `canonical` row with equal
  neighbours for plain `sorted`, and `reject` rows (out of order; equal
  neighbours under `unique`; a repeated key under `unique_keys`). As in
  `map-set-order`, each rejected frame appears a second time read as the
  plain list type (`canonical`), to show that only the constraint rejects
  it. All of these types are `ir: true`, so the schema-driven tests of
  every language run the same rows. Order of magnitude: the existing
  `map-set-order` class is 352 rows.
- `json-vectors.json`: accept / reject rows for the same types (an
  unordered JSON array is rejected, in `json` and `ijson`).
- `time-key-vectors.json`: the key of an ordered list is the key of the
  list; an unordered value has no key.
- `interface-vectors.json`: one method with an ordered parameter and one
  with an ordered result.
- `schema-json-vectors.json`: a schema using the ids (accept), and
  rejects: an order id over a node that is not a list or array, over a
  map / set, over elements with no key.
- The `schema_ir` and rows of the existing fixtures do not change: no
  fixture type carries an order constraint today.

**Mirror cost per language** (relative; C++ first because its emitters
write the fixtures).

| | schema builder | schema-driven codec | check / other |
|---|---|---|---|
| C++ | emit the node from the specs at the member and below it (`annotated_type`) | accept the ids in `wire_custom_id`; one ordering hook reused by PSSZ, JSON and key (the map / set hook with a mode) | emitters pass an unknown custom through already (`emit_wit`, capnp, fbs, protobuf); the fixture emitters gain the new classes |
| Rust | the derive already records the order (`Shape::ordered`); the schema builder emits the node | `pssz::dynamic` and the dynamic JSON / key paths: the same hook | `pssz_evolves_from` already compares `order` for typed shapes |
| JS | `types.ts` builds the node instead of keeping the option beside the record; this also lifts today's restriction that the options belong to a record member | one compiled-node flag in `pssz-core.ts`, reusing the map / set comparison; `pssz-json.ts`, `pssz-key.ts` call it | `psszEvolvesFrom` compares the ids for fetched schemas too |
| Python | `schema.py` emits the node | `_core.py` compiled node flag, `_key.py` | `evolution.py`: drop the "typed declarations only" caveat |

The largest single piece is the C++ fixture emitter work; the codec change
in each language is small because map / set ordering already exists.

---

## E-012 — The WIT text of an interface's schema

**Status.** Report. `cpp/tests/interface_wit_tests.cpp` pins what C++
`psio::emit_wit` writes today for the schema of
`conformance/interface-vectors.json`. JS `emitWit` prints the same two
defects because it mirrors C++; there is no correct reference yet.

**The text** (C++, today):

```
package iface_conf;

interface token {
  @maxFields(8)
  @maxDynamicData(256)
  transfer: func(to: string, amount: u64, memo: option<string>) -> receipt;
  names: func(limit: u32) -> list<string>/* @psio:max=4 */;
  @maxDynamicData(64)
  note: func(text: string);
  add: func(: u32, : u32) -> u64;
  ping: func();
  batch: func(names: list<string>/* @psio:max=4 */, tip: u8) -> bool;
}

record receipt {
  id: u64,
  note: string
}
type token.transfer = /* TODO: inline record not supported */;
type token.names = /* TODO: inline record not supported */;
type token.note = /* TODO: inline record not supported */;
type token.add = /* TODO: inline record not supported */;
type token.ping = /* TODO: inline record not supported */;
type token.batch = /* TODO: inline record not supported */;
```

**Defects, and what a fix has to decide.**

1. *Positional parameters:* `add: func(: u32, : u32)`. A method declared
   without names has `""` parameter names in the `Func`; WIT requires an
   identifier. Options: synthesize `arg0`, `arg1`, … (WIT stays valid; a
   parser must not read those back as declared names — the argument list's
   `tuple` attribute, `"positional"`, already says which it is), or refuse
   to emit an interface that has an unnamed parameter.
2. *Argument-list types:* the fixture registers each argument list as a
   named type `token.<method>` whose node is a reference to an interned
   `Object`. The emitter resolves the reference, meets an `Object` in type
   position and prints the TODO; and `token.transfer` is not a WIT
   identifier (kebab-case, no dots). Options: do not emit a named type
   whose `Object` carries the `tuple` attribute (it restates the `func`
   line above it); or emit it as `record token-transfer-args { … }` when
   every member is named and skip it when positional. The same resolution
   step would make any alias of a record print the TODO, so the general
   fix is: a named type whose resolved node is an `Object` / `Struct` /
   `Variant` is declared as `record` / `variant` under its own name.
3. *Bounds* (by design, valid WIT): a bounded list is
   `list<T>/* @psio:max=N */`; the bound of a string (`to` ≤ 16, `memo` ≤
   80, `text` ≤ 62, the elements of `names`) and `utf8` are dropped,
   because both raw and UTF-8 strings print as `string`.

**For the JS fix.** Match the pinned C++ text for everything outside
defects 1 and 2; change those two in both implementations together, and
update `interface_wit_tests.cpp` in the same change.

---

## Conventions for this file

- Each entry has a stable id (E-NNN). IDs are append-only — never reused
  even after promotion or rejection.
- Promoting an entry to the spec moves the body into `pjson-spec.md`
  and replaces the entry here with a pointer to the spec section that
  absorbed it.
- Rejecting an entry leaves it here with status "rejected" and a brief
  note explaining the reason — useful for future contributors who reach
  the same idea.
