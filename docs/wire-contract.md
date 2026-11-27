# Public-library wire contract

The release baseline is the implementation under `cpp/include/psio/`, exercised
through its public encode/decode/validate APIs. C++ fixes for crashes or invalid
output must include regressions; valid wire changes must be reviewed explicitly.
Pjson uses **wire revision 2**, adopting the audited May high-nibble numbering.
C++, Rust and JS/TS implement this revision together. The public constants are
`psio::pjson_wire_revision`, `psio::pjson::WIRE_REVISION` (Rust) and
`PJSON_WIRE_REVISION` (JS/TS).

`cpp/conformance/cpp_golden.cpp` generates `conformance/cpp-golden.json`.
`tools/check-cpp-golden.py` compares those bytes with the copies shipped inside
Rust and JS tests. Its `--update` option intentionally changes all copies and
must be used only when the C++ wire change has been reviewed.

The C++ pjson type codes are:

| High nibble | Meaning |
|---|---|
| 0 | null; low nibble must be zero |
| 1 | bool |
| 2 | inline unsigned integer, 0..15 |
| 3 | inline negative integer, -1..-15; low nibble zero is invalid |
| 4 | unsigned magnitude |
| 5 | negative integer magnitude |
| 6 | IEEE floating point; widths 16/32/64; bit 3 is a notation hint |
| 7 | decimal: zigzag i128 mantissa and zigzag scale |
| 8 | numeric-string assignment; unsupported and rejected |
| 9 | string: raw bytes or escaped bytes |
| A | bytes; low nibble must be zero |
| B | generic or typed array |
| C | object or shared-schema row array |
| D | extension assignment; unsupported and rejected |
| E–F | reserved |

Integer 5 is `25`; integer -5 is `35`. Values -1..-15 now occupy one byte.
This adopts the improved numbering and negative-inline encoding. It does not
adopt every draft semantic: binary128, numeric-string wrappers, extension
payloads and bytes presentation flags remain unsupported. IEEE bit 3 retains
its existing notation-hint meaning pending the separate semantic review.
The public C++ dynamic double encoder chooses its shortest decimal spelling,
then uses the compact integer/decimal representation only when shorter than
binary64. The 274 double fixtures include both selected edge values and a
reproducible sample of bit patterns. This is coverage, not an exhaustive proof.

**One encoding per value.** Every pjson encoder — typed, dynamic, the
canonical JSON transcode — writes the canonical form of
[pjson-spec §15](pjson-spec.md#15-canonical-encoding), and
`psio::pjson::validate` / `decode` (Rust `pjson::validate`, JS
`pjsonValidate`) accept nothing else. The canonical form is a function of
the value, not of the type that held it: `5` is `25` from a `u16`, a `u64`,
an `f64` and JSON text; `[1,2,3]` is the typed array `b5 01 02 03 03 00`
from a list of any numeric type; an empty list is `b0 00 00 00`; an array of
objects with the same names is a row array; a string is raw UTF-8 text.

A binary float stands for the shortest decimal that reads back to it at its
own width (**default chosen, awaiting the owner**, gofractally/psio#17): a
`float` 0.1, a `double` 0.1 and JSON `0.1` are all `70 02 01`. A binary32
frame read as a double goes through that decimal, not a widening cast. Zero
with its sign set is binary16 `61 00 80`, NaN is `61 00 7e`.

`psio::pjson_lazy` (Rust `pjson::lazy`, JS `pjsonLazy`) reads any
well-formed frame. Its one writer is the quick JSON transcode
(`pjson_json::from_json_lazy`), which copies what it parses and is **not**
canonical; `canonicalize` turns such a frame into the canonical one. The
untyped view and `pjson_to_json` read both.

JSON text and pjson (C++ `psio::pjson_json::from_json` / `psio::pjson_to_json`,
Rust `pjson_json::from_json` / `to_json`): a JSON number keeps its value or is
an error (a number whose digits do not fit a 127-bit mantissa is refused);
any JSON value may be the document. To text, a number is written from the
value it is (so one value has one text whichever form carries it), bytes are
standard padded base64, strings and member names keep their text (only the
escapes JSON requires), member order and duplicate names are kept, and a NaN
or an infinity is an error. The `pjson_to_json` rows of
`conformance/cpp-golden.json` pin, for C++, Rust and JS, well-formed frames,
their JSON text and the canonical frame of each; `pjson_f32` and the
`pjson_list_*` rows pin native floats and lists.

Pjson containers use little-endian adaptive offsets and a trailing u16 count.
Generic objects preserve encounter order and duplicate names. Key hashes are
the low byte of XXH3-64 of the key before its last dot. Row schemas reject long
keys (255 bytes or more) in the current implementations. Validation and public
materializing decode now cap nesting at 64 before entering a child.

The historical draft and standalone reference drivers are preserved for
provenance. They are opt-in and are not release acceptance evidence. Strict
canonical validation, text projection options, the full u128 semantic range,
and all typed/view APIs still need an explicit cross-language review.

## Migration from the earlier C++ map

Revision selection is **out of band**: the containing file, database schema,
RPC protocol, or API contract must identify revision 2. The raw value has no
version byte. There is no legacy fallback or auto-detection: `35` is a valid
+5 under the previous C++ map and a valid -5 under revision 2. All newly produced
fixtures identify the current revision by this contract and package version.
Old buffers must be decoded with a known old codec and re-encoded through the
revision 2 codec before mixing datasets. No stored user data was converted by
this source migration. A universal converter is not provided for unversioned
data of unknown provenance.

The former Rust draft already used these type numbers, but its extra features
and canonicalization rules are still outside this supported profile. Shared
numbers alone do not make every draft buffer supported.
