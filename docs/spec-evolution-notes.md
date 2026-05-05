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

## Conventions for this file

- Each entry has a stable id (E-NNN). IDs are append-only — never reused
  even after promotion or rejection.
- Promoting an entry to the spec moves the body into `pjson-spec.md`
  and replaces the entry here with a pointer to the spec section that
  absorbed it.
- Rejecting an entry leaves it here with status "rejected" and a brief
  note explaining the reason — useful for future contributors who reach
  the same idea.
