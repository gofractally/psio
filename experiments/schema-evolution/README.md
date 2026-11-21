# Schema-evolution experiments — no production format decision

These programs compare the actual old psibase Rust fracpack implementation with
the maintained C++ PSSZ32 encoder/validator and the development Rust exact-schema
PSSZ reader. They do not establish conformance to the published PSSZ specification
or choose the migration's wire format. Exact logs and outputs are under the root
migration artifacts' `psio-stage5/schema-evolution/` directory.

## A provable ambiguity

Start with reader V1 `{ name: String }`. Two legal writer extensions, if arbitrary
appended types are allowed, produce **the identical 14 bytes**:

```text
Writer schema A: { name: String, extra: u32 }
Value A:         { name: "ab", extra: 9 }

Writer schema B: { name: String, extra: String }
Value B:         { name: "a", extra: "b" }

Both PSSZ32:     08000000 08000000 09000000 6162
```

The first word says the writer's fixed region occupies eight bytes. The second
word points to `name`. The third word is either integer 9 or the second string's
offset 9. The final bytes are `ab`. Reader V1 must return `"ab"` for A and `"a"`
for B. Its schema plus these bytes cannot determine which answer is correct.
This is a format-information constraint, rather than a decoder bug that can be
fixed by inspecting the header more carefully.

The current unchecked C++ V1 decoder returns `"ab"` for both. The maintained V1
validator rejects both because its expected fixed region is four bytes. The
development Rust exact-schema API rejects both. Those rejections prevent silent
misinterpretation but do not implement forward-compatible reading.

## What the implementations actually do

| Input | Old fracpack | Current PSSZ32 |
| --- | --- | --- |
| V1 `name="a"` | 11 bytes | 9 bytes |
| V2 appended String `"b"` | 20 bytes; V1 reads `"a"` | 14 bytes; unchecked V1 reads `"ab"`, V1 validator rejects |
| V2 appended fixed u32 `9`, name `"ab"` | 16 bytes; V1 rejects (not an optional extension) | 14 bytes; unchecked V1 reads `"ab"`, V1 validator rejects |
| V1 read by V2 with appended `Option<String>` | Succeeds with None | Current validator/exact reader rejects |
| V2 appended `Option<String>=None` | Pruned to V1's same 11 bytes | 14 bytes; unchecked V1 name becomes `"a\0"`, V1 validator rejects |
| V2 appended empty String | 15 bytes | 13 bytes; no maximal trailing-default pruning |
| V2 appended zero u32 | 15 bytes | 13 bytes; no maximal trailing-default pruning |

Fracpack's actual extension contract is **appending optional fields**, not
arbitrary fixed fields. Its string payload includes a u32 byte length. Its vector
payload starts with a u32 byte count for that vector's fixed region. Thus known
string/vector payloads are self-delimiting even when the reader cannot interpret
later unknown payloads. Unknown record suffix entries are treated as offset slots,
which explains the restriction on appended fixed values.

Old-writer/new-reader tests for newly required String/fixed fields are not
supported fracpack upgrades: String returns BadOffset; the old Rust fixed-field
decoder panics in its debug `remaining()` subtraction. The repro catches/logs
these panics so all cases are reported. Optional suffix evolution succeeds.
This finding does not broaden fracpack's documented extension contract.

Current C++ PSSZ writes raw string bytes without a string length. The last known
variable field's end comes from the next known offset, or the entire enclosing
span. Its record decoder skips the fixed-region header without using the
writer's size to decode unknown fields. The current validator also does not
provide the published specification's broad evolution/default-pruning behavior.

## Prototypes using the exact same values

`prototype.py` first reconstructs each current PSSZ packet and requires byte
equality with actual C++ output. It then runs these independent experiments.
Their hex and sizes are in `prototype-results.jsonl`.

### Offset-bearing suffix contract: no additional wire bytes

Restrict every appended field to an offset-bearing wire shape. `Option<T>` gives
fixed T such a shape; strings/lists already have one. Then the old reader can:

1. Read writer fixed-region size H and its own known fixed-region size K.
2. Parse its known prefix normally.
3. Treat bytes K..H as W-byte offset slots, because this is guaranteed by the
   writer's schema contract; verify alignment, bounds, and monotone offsets.
4. Use the first unknown offset as the end of its last known variable field.
5. Default an absent new optional field to None when reading an older writer.

This parser correctly returns `name="a"` from the actual appended optional
Some/None packets and reads V1 as V2Optional with None. The prototype can prune
the trailing None from 14 to 9 bytes and still decode it with the new reader.

The contract must be enforced by schemas/producers. Its parser returns `"a"`
for ambiguous writer A, which violates that contract; bytes alone cannot detect
the violation. It also cannot semantically validate unknown field contents
without their schema. Existing readers/validators would require changes.

This option preserves implicit payload sizing and existing wire bytes, at the
cost of restricting future field additions. It cannot meet an unrestricted
append-any-type promise.

### Self-delimiting variable payloads

The prototype adds a u32 length to each string payload, retaining the mixed fixed
region. The known string's own length resolves both ambiguous writers correctly:

* Two strings: 14 → 22 bytes.
* String `"ab"` plus fixed u32: 14 → 18 bytes.

These tiny example sizes include the proposed PSSZ32 four-byte header. Existing
fracpack uses a two-byte record header and needs 20 and 16 bytes respectively.
Length widths/varints and optional adapters could change the overhead. This
experiment makes no throughput, compression, or real-application size claim.

### An offset for every field, plus one terminal offset

The prototype uses `[u32 field count][N+1 u32 offsets][field payloads]`. All
fields, including fixed scalars, have explicit spans. Offsets are relative to
the table start, and the last offset marks the payload end. The old reader can
find its field boundary without understanding later field types.

* Two strings: 14 → 18 bytes.
* String `"ab"` plus fixed u32: 14 → 22 bytes.
* One string `"a"`: 9 → 13 bytes.

A single record-end offset added to the current mixed layout is insufficient:
the older reader still cannot identify which unknown fixed-region words are
offsets versus inline values. The N+1 table works because **every** field is
represented uniformly. Count/header/offset widths and presence/default metadata
would need a specified versioned format.

### Writer schema identity

Keeping the current wire can also work if the reader has the exact writer schema.
The writer's schema tells it whether word 9 is a scalar or an offset. The identity
can be stored per record, per table/version, or derived from reliable revision
metadata; the size cost is not necessarily a schema hash on every row. That
metadata must remain available for historical rows, snapshots, replay, and
service upgrades. A reader schema alone is insufficient.

Read-modify behavior must be explicit for every design. An envelope can retain
unknown field spans/values and merge known changes through the writer schema.
Returning only the old typed value and reserializing it silently loses unknown
fields; if that is intentionally allowed, it must be a documented API choice.

## Reproduction

```sh
cargo run --offline --locked --manifest-path experiments/schema-evolution/Cargo.toml

/opt/homebrew/opt/llvm/bin/clang++ -std=c++23 -DPSIO_EXCEPTIONS_ENABLED=1 \
  -Icpp/include -I/opt/homebrew/include experiments/schema-evolution/pssz.cpp \
  -o /absolute/evidence/cpp-evolution
/absolute/evidence/cpp-evolution

python3 experiments/schema-evolution/prototype.py /absolute/evidence/cpp-results.jsonl
```

The standalone Cargo manifest pins the local old psibase fracpack path explicitly
so it cannot accidentally use the maintained psio crate-root comparison codec.
The experiment lockfile records resolved dependencies. Source hashes, revisions,
commands, captured stderr, and every attempted run are retained in the evidence.
