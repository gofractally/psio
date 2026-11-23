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
`max_size.hpp` infers (`docs/pssz-spec.md` §3.2: a record's or tuple's header
and variable slots and a list's or array's offset table counted at the type's
own width through `fit`, one byte per option, a child record's cap reserved in
full, bounded lists and strings, a member `lengthBound` attribute on an
unbounded string or list); otherwise 4. A `BoundedList`, a map or a set of
bounded elements, and a plain `List` whose count the holding member's
`lengthBound` gives, therefore have a 1- or 2-byte offset table when their
bound allows it. Beneath a record with a `maxDynamicData`, every list, array,
map, set and tuple down to the next record is also bounded by that cap for its
width (`docs/pssz-spec.md` §3.6); the codec keeps the cap in effect as it
walks. A custom handler always receives the root encoding of its value. The
cap limits the record's encoded
size, and length bounds are enforced, on encode and decode. The bytes are those
of the typed `psio::pssz` codec; `conformance/pssz-vectors.json` records the IR
of all its types and `cpp/tests/schema_conformance_tests.cpp` checks every
vector through the IR alone. The JSON / I-JSON text path over the same IR is
`<psio/pssz_ir_json.hpp>` (`docs/json.md`).

**Custom types.** Every entry point takes an optional trailing
`const CustomHandlers*` (`<psio/pssz_ir_custom.hpp>`). A `Custom` node whose
id is not part of the binary form names a type with a text form and / or an
invariant; the schema cannot carry either, so they are a handler registered
for the id. Without a registry the codec is structural: a custom node is its
wire type. With one, the `check` of each custom node's handler runs on every
value of the node — on decode / validate after its structure is accepted, on
encode before any byte is returned, nested values first, the status passed
through unchanged — and a strict registry (`require_all`) makes a custom node
with no handler an error. `check` sees the automatic-profile bytes of the
node's wire type under either width profile. `docs/json.md` ("Custom types
through a schema") and `docs/pssz-spec.md` §18 give the rules.

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

**Maps and sets.** `Custom{List{Tuple{[K, V]}}, "map"}` and
`Custom{List{K}, "set"}` (`docs/pssz-spec.md` §5.9) are lists whose entries
are in strictly ascending order of the `key` encoding of K. A dynamic map
value is a `Sequence` of entries, each a `Sequence{key, value}`; a set value
is the `Sequence` of its elements. `encode` sorts the entries (a value built
in any order encodes to the one canonical frame) and refuses two entries with
equal key bytes; `decode_*` / `validate_auto` reject a frame whose entries
are not strictly ascending and return them in wire order. `metadata` refuses
a `map` / `set` node of the wrong shape or with a key type that has no key
encoding. `to_key(schema, root, value, out)` is the key encoder over dynamic
values that this uses, exposed for callers (`docs/key-spec.md` §6).

A `Nested` node has the key of the byte list holding its inner value's
canonical encoding, so it may be (part of) the key type of a map or the
element type of a set, whatever its inner type. `to_key` and `encode` take the
inner dynamic value and frame its encoding (with `nested_bytes::opaque`: the
`Bytes` as they are). The readers check the order on the **bytes read**, not
on the decoded inner values: a compatible read drops unknown fields of an
inner record, so two keys that differ only there are distinct, ordered
entries of the frame whose decoded values are equal — `has_unknown` is set,
and re-encoding that dynamic value fails with the duplicate-key error rather
than silently merging them.

The node budget matches Rust: root 0, node 64 accepted, node 65 rejected;
present record/tuple fields, sequence items, Some payloads and variant payloads
add one. Pruned/synthesized trailing None adds no child. String is atomic;
nonempty byte vectors and arrays traverse byte elements. Empty containers add
no child. Schema alias/minimum analysis is separately bounded and never uses
the unsafe legacy raw-pointer resolution cache.

Supported IR shapes are integers (1/8/16/32/64/128, and unsigned 256 carried as
its 32 little-endian bytes), IEEE32/64, records, DWNC,
tuple, option, variant, list, bounded list, fixed array, bool/char/string/hex
adapters and Nested. String/hex adapters require unsigned-byte or char-byte
containers.
Resource, unknown adapters, unsupported primitive shapes and duplicate
schema member names return explicit errors.

`Nested{type}` is a byte list holding the PSSZ encoding of a `type` value,
always in the automatic profile, whatever the profile of the frame around it.
Its dynamic value is the inner value: decoding reads the bytes as `type`
under the same unknown-field rule as the rest of the frame (and reports
unknown fields found inside), with the inner root one node level below the
Nested node; encoding writes the inner value's canonical encoding. So a
decode / encode round trip of a frame whose nested bytes carry unknown
fields yields the canonical frame, not the input. A member `lengthBound` on
a Nested member (through Option) counts those bytes. Every entry point takes
a trailing `nested_bytes` argument (after the custom-handler registry): `nested_bytes::opaque` makes the dynamic
value of a Nested node its `Bytes`, read and written without being examined
— the behaviour of a typed `nested<T, nested_validation::deferred>` member,
and of a typed reader that keeps the member's bytes. Reachable schema metadata is checked
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

## Comparing schemas at run time

`<psio/pssz_ir_compare.hpp>` answers, for two types that are only known as
schemas, the two questions a registry of published schemas asks:

```cpp
namespace ir = psio::pssz_ir;
// May the proposed row type replace the installed one?
if (auto r = ir::evolves_from(installed, "Row", proposed, "Row"); !r)
   abort(r.path + ": " + r.reason);   // ".tags[]: the length bound of the string was tightened ([0, 16] -> [0, 8])"
// Is the key still the same type?
if (!ir::equivalent(installed, oldKeyType, proposed, newKeyType, ir::compare::wire)) ...
// A function, an interface, a whole schema (every interface, by name).
ir::evolves_from(installed, installed.interfaces[0], proposed, proposed.interfaces[0]);
ir::evolves_from(installed, proposed);
```

`evolves_from(old, Old, new, New)` is the run-time twin of the compile-time
`psio::pssz_evolves_from<Old, New>` and applies the same rules
(`docs/pssz-spec.md` §7.5 / §7.6 / §16): members are appended and optional,
no offset width moves at any depth, a length bound only widens, `utf8` is
kept, `maxDynamicData` admits everything the old writer produces, `maxFields`
is neither exceeded, raised nor dropped, a variant gains alternatives at the
end, a nested value's inner type evolves as a root of its own. `equivalent`
is the symmetric relation: the same values, the same bytes, the same
permitted revisions. A type argument is an `AnyType` of its schema or a name
(`"Row"` converts to the reference `Type{"Row"}`).

Both return `psio::pssz_ir::compatibility{ok, code, reason, path}`
(contextually a `bool`; `status()` gives a `codec_status`): `code` is a
stable identifier of the first rule broken, `reason` a sentence, `path` where
(`.field`, `[]`, `?`, `<Alt>`, `(nested)`, `name(args)`; the tables are in
`conformance/evolution-vectors.md`). Nothing throws, and a malformed schema
or an unknown type name is the result `malformed-schema`: the comparison runs
in a build without exceptions (a wasm guest).

**What is compared.** The two types as resolved types, in the automatic
profile. Named aliases are followed and their names are not compared: the
order of `Schema::types`, the names of the types and how a schema shares
subtrees are not part of a type. Recursive types are compared co-inductively.

- `compare::wire`: the bytes and which bytes are values — the kind of every
  node, integer width and signedness, array lengths, the presence form of an
  optional, raw string / UTF-8 string / bytes (three types), length bounds as
  the range a position admits (wherever written: `BoundedList<T, 8>` equals a
  `List<T>` member with `lengthBound = "max=8"`), every offset width as the
  codec derives it, `definitionWillNotChange`, `maxDynamicData`, `maxFields`,
  boxes, and the key type of a map / element type of a set (which must be
  equivalent in both relations). A lone `char` is the integer `u8`, and the
  element of a byte list is not looked at, so the
  `Custom{List<Custom{u8, "char"}>, "hex"}` of older schemas and
  `Custom{List<u8>, "hex"}` are one type (`docs/spec-evolution-notes.md`
  E-011). A `Tuple` and an extensible `Object` are the same record.
- `compare::contract` (the default) adds what a consumer of the text forms
  and of the type's identity sees: the names of record members and variant
  alternatives (§7.6: a rename breaks consumers that use the names), an
  alternative's `ijsonName`, custom ids, enums (`evolves_from` lets the new
  type add enumerators; every old name keeps its value), the text shape of a
  record (object, named argument list, positional), `jsonInt` / `ijsonInt`.

Not compared, because the Schema IR does not record them: `sorted` / `unique`
/ `unique_keys`, `as<Tag>` overrides, binary adapters, and the name of a
transparent type (a named alias in the IR). For types that differ only in
those the compile-time check is the stricter one. Also not compared: `doc` /
`description` / `visible`, package, worlds and uses.

**Functions, interfaces, schemas.** A function is its argument list — the
record `psio::pssz_ir::argument_list(func)` of its parameters, with the
function's `maxFields` / `maxDynamicData` — and its result (on both sides or
on neither). Under `evolves_from` both evolve old to new: a new callee accepts
every call an old caller makes, and a stored result of the old function is a
value of the new result type. The functions of two interfaces are matched by
name in any order — every old function exists and evolves; `equivalent`: the
same set — and so are the names in `type_names`. The interfaces of two schemas
are matched by name the same way.

`argument_list(func)` is also what a consumer needs to pack or print a call
from `Schema::interfaces` alone:
`json_to_pssz(schema, argument_list(f), psio::ijson{}, R"({"amount":5})", bytes)`.

**Tests and shared rows.** `conformance/evolution-vectors.json`
(`conformance/evolution-vectors.md`) has pairs of schemas with the four
verdicts of each, in both directions, for the run-time checkers of the other
languages; its emitter refuses a row that disagrees with the compile-time
check. `cpp/tests/pssz_evolution_matrix_tests.cpp` and
`consistency_tests.cpp` replay every pair of their compile-time assertions
through the run-time check (`evolves_parity.hpp`).

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
