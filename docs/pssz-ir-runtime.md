# Checked PSSZ codec for schema IR

`<psio/pssz_ir.hpp>` consumes the actual `schema_ir.hpp` graph. It never falls
back to the older lowercase `psio::schema` runtime or to fracpack.

All entry points select width explicitly with template parameter 1, 2 or 4.
This library checkpoint does not choose the final migration profile. A deployed
compatible lineage must keep its width; the codec never automatically widens it.

The typed `psio::pssz_ir::Value` distinguishes bool, signed/unsigned 128-bit
integers, Float32/Float64, UTF-8 string, raw bytes, ordered record, sequence,
explicit Optional and variant selector/payload. `Value::none()` and
`Value::some(value)` preserve None/Some(unit)/Some(None). The `char` adapter
represents one raw byte as Unsigned(0..255), independently of host char signedness.

`encode<W>(schema, root, value, destination)` returns `codec_status`. On success
it appends bytes; on failure the destination is unchanged. `decode_exact<W>`
rejects unknown fields and publishes its output only on success. Here exact
means structural exact-schema reading: a well-formed older unpruned None is
accepted. `validate_canonical_for_width<W>` separately checks re-encoding equality
including trailing-None pruning for the caller's width.

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

Supported IR shapes are integers (1/8/16/32/64/128), IEEE32/64, records, DWNC,
tuple, option, variant, list, bounded list, fixed array and bool/char/string/hex
adapters. String/hex adapters require unsigned-byte or char-byte containers.
Resource, FracPack, unknown adapters, unsupported primitive shapes and duplicate
schema member names return explicit errors. Reachable schema metadata is checked
before encoding/decoding; constraints are never bypassed by an omitted Option.
DWNC and collection count bounds are implemented. `maxFields`, `maxDynamicData`
and unrecognized annotation attributes are explicitly rejected until enforced;
doc/description/visible are whitelisted descriptive metadata. This is an adoption
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
