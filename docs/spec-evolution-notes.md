# pjson spec evolution notes

Documented future work for pjson-spec.md beyond v1. None of these are
in v1; they're tracked here so they don't get lost between conversations.

Each entry includes the design intent, the proposed mechanism, and any
constraints that would need resolving before promoting to the spec.

---

## E-001 — `array` "is_sorted" hint bit

**Intent.** When the encoder knows the children of an array are sorted
(e.g. serializing a `std::set`, `BTreeMap` keyset, or a sorted index),
record that fact so a view can use it.

**Use cases enabled in views:**

- O(log N) binary search by child value, instead of the current O(N) scan.
- Merge-join two sorted arrays without materializing intermediate state.
- Membership queries against a sorted typed homogeneous array become a
  single `lower_bound`-style probe.
- Decoder fast paths that need not preserve insertion order can build
  set-like data structures without re-sorting.

**Proposed mechanism.** Use a previously-reserved low-nibble bit on
the `array` tag. The current low-nibble assignment (§5.1) is:

```
array tag low nibble:
  0       = generic array
  1..10   = typed homogeneous (element type code = low_nibble − 1)
  11..15  = reserved
```

Plenty of headroom in the reserved range. One reasonable assignment:
add a parallel range `0x?B..0x?F` that mirrors `0..A` with an
"is_sorted" bit set. Or repurpose the high bit of the low nibble:

```
bit 3 of low nibble = is_sorted hint  (1 = sorted)
bits 2..0           = layout selector (0 = generic, 1..7 = typed
                      element code with reduced range, ...)
```

The reduced typed-array range is the cost — fitting 10 element codes
into 3 bits requires either dropping some (ssz/ssz-bool style: i8/i16/
i32/i64/u8/u16/u32/u64 → 8 codes; drop f32/f64 to fit into 7) or using
a 2-byte tag. Both have downsides; the design call has not been made.

**Open questions before promoting:**

1. Which low-nibble layout wins the bit? Reserved-range expansion or
   high-bit repurposing?
2. Should `is_sorted` apply to both generic and typed arrays, or only
   typed (where the comparison is well-defined byte-for-byte)?
3. For generic arrays: what's the comparison rule? Element-tag-then-
   bytes lex? Defined per-element-type? Application-defined?
4. Validator policy: when `is_sorted` is set but the bytes aren't
   actually sorted, is that a wire error or a "hint was wrong, ignore"?
   Suggest: **must** reject in strict-canonical, **may** accept in
   lenient. See E-002 below.

**Status.** Deferred from v1. Revisit when a real consumer needs it
and the trade-off space has been measured against typed-array bench
shapes.

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

## Conventions for this file

- Each entry has a stable id (E-NNN). IDs are append-only — never reused
  even after promotion or rejection.
- Promoting an entry to the spec moves the body into `pjson-spec.md`
  and replaces the entry here with a pointer to the spec section that
  absorbed it.
- Rejecting an entry leaves it here with status "rejected" and a brief
  note explaining the reason — useful for future contributors who reach
  the same idea.
