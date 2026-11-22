# Checked PSSZ codec for schema IR

`<psio/pssz_ir.hpp>` consumes the actual `schema_ir.hpp` graph. It never falls
back to the older lowercase `psio::schema` runtime or to fracpack.

There are two width profiles.

**Explicit** (`encode<W>`, `decode_exact<W>`, `decode_compatible<W>`, W = 1, 2
or 4; the `psio::pssz_<W>` tags): one width inherited by every type.

**Automatic** (`encode_auto`, `decode_auto`, `validate_auto`, `auto_width`; the
`psio::pssz` tag): every record, array and list uses its own type-derived
width, computed from the IR exactly as `auto_pssz_width<T>()` computes it from
the type. A record's `maxDynamicData` attribute rules; otherwise the bound
`max_size.hpp` infers (four bytes per header and per variable slot, one byte
per option, a child record's cap reserved in full, bounded lists and strings,
a member `lengthBound` attribute on an unbounded string or list); otherwise 4.
Lists, tuples and variants always use 4. The cap limits the record's encoded
size, and length bounds are enforced, on encode and decode. The bytes are those
of the typed `psio::pssz` codec; `conformance/pssz-vectors.json` records the IR
of all its types and `cpp/tests/schema_conformance_tests.cpp` checks every
vector through the IR alone. The JSON / I-JSON text path over the same IR is
`<psio/pssz_ir_json.hpp>` (`docs/json.md`).

The explicit profile still rejects `maxFields` / `maxDynamicData`, which it does
not enforce. A deployed compatible lineage must keep its width; the codec never
automatically widens it.

The typed `psio::pssz_ir::Value` distinguishes bool, signed/unsigned 128-bit
integers, Float32/Float64, UTF-8 string, raw bytes, ordered record, sequence,
explicit Optional and variant selector/payload. `Value::none()` and
`Value::some(value)` preserve None/Some(unit)/Some(None). The `char` adapter
represents one raw byte as Unsigned(0..255), independently of host char signedness.

`encode<W>(schema, root, value, destination)` returns `codec_status`. On success
it appends bytes; on failure the destination is unchanged. `decode_exact<W>`
rejects unknown fields and publishes its output only on success. Here exact
means structural exact-schema reading. Pruning is mandatory since the trunk
merge: `decode_exact` and `decode_compatible` both reject a record or tuple whose
last written slot is a stored None (when unknown appended slots follow, the
reader cannot judge the writer's last slot and accepts).
`validate_canonical_for_width<W>` separately checks full re-encoding equality
for the caller's width.

`decode_compatible<W>` reports unknown optional suffix fields through a separate
boolean, including unknown fields nested inside known records/lists. It uses
the shared checked record-span parser: unknown suffix offsets bound the final
known variable field. Missing known trailing Options synthesize None. A compatible
read does not retain unknown payloads in the typed value. A caller that intends
to rewrite a record must reject `has_unknown` or preserve/rewrite it through a
suitable newer schema; blindly re-encoding the older typed value loses data.
Output and unknown flag remain unchanged on any failure.

The node budget matches Rust: root 0, node 64 accepted, node 65 rejected;
present record/tuple fields, sequence items, Some payloads and variant payloads
add one. Pruned/synthesized trailing None adds no child. String is atomic;
nonempty byte vectors and arrays traverse byte elements. Empty containers add
no child. Schema alias/minimum analysis is separately bounded and never uses
the unsafe legacy raw-pointer resolution cache.

Supported IR shapes are integers (1/8/16/32/64/128, and unsigned 256 carried as
its 32 little-endian bytes), IEEE32/64, records, DWNC,
tuple, option, variant, list, bounded list, fixed array and bool/char/string/hex
adapters. String/hex adapters require unsigned-byte or char-byte containers.
Resource, FracPack, unknown adapters, unsupported primitive shapes and duplicate
schema member names return explicit errors. Reachable schema metadata is checked
before encoding/decoding; constraints are never bypassed by an omitted Option.
DWNC and collection count bounds are implemented, as are the member attributes
`lengthBound` (enforced), `jsonInt` / `ijsonInt` and the variant-member
attribute `ijsonName` (text forms). `maxDynamicData` is implemented by the
automatic profile and rejected by the explicit one; `maxFields` likewise is
accepted (as descriptive) only by the automatic profile. Unrecognized
attributes are rejected; doc/description/visible are whitelisted descriptive
metadata. This is an adoption
limit, not a claim that those annotations are implemented.

Allocation failure follows the existing `codec_status` library allocation model:
arithmetic/capacity errors produce status, while process memory exhaustion is
unrecoverable. The checked codec has no explicit throws and compiles/runs with
exceptions disabled. Its signed/unsigned 128-bit storage requires the GCC/Clang
128-bit integer extension used by the supported native and WASM toolchains.

The shared test corpus contains literal schemas, typed values and expected bytes
from the same 63 Rust fixtures at every width. The generated C++ fixture header
embeds the JSONL SHA256 and is reproducible with
`cpp/tools/generate_pssz_ir_fixtures.py`. Tests also cover all 256 byte values
through actual C++ SchemaBuilder vector/array/optional char shapes, native typed
byte parity, signed/unsigned char builds, unknown suffixes, malformed inputs,
zero-sized fixed values, bounds, recursion, output atomicity and canonicality.

## JavaScript / TypeScript

`js/src/pssz-core.ts` is a port of this codec (both profiles) for browser
clients, with the JSON / I-JSON walk in `js/src/pssz-json.ts`; the public API
is `PsszSchema` / `PsszType` (`js/README.md`). It is the package's only
codec: the typed definitions of `js/src/types.ts` (`record`, `list`, ...)
generate the Schema IR `SchemaBuilder` produces for the equivalent C++ types
and run on it, and `type.view(bytes)` (`js/src/pssz-view.ts`) reads records,
lists, tuples and variants in place with the same frame checks. It is checked
against every vector of `conformance/pssz-vectors.json` (all four formats),
`conformance/json-vectors.json` and `conformance/time-key-vectors.json`,
through the recorded IR alone and again through typed definitions of every
fixture type, whose generated IR must equal the fixture's byte for byte. Differences from
the C++ value model: 64/128/256-bit integers are `bigint`, a NaN with a payload
is a `PsszNaN` (bit-exact re-encode), `Some` of a nested optional is
`{ some }`, and a fixed-count array of zero-size elements is limited to 2^20
elements.
