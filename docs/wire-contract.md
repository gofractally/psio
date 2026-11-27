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

Zero with its sign set (`-0.0`, JSON `-0`) is binary16 `61 00 80` in every
implementation, from a native float and from JSON text; `0` stays `20`.

JSON text and pjson (C++ `psio::pjson_json::from_json` / `psio::pjson_to_json`,
Rust `pjson_json::from_json` / `to_json`): a JSON number keeps its value or is
an error. An integer of up to 128 bits of magnitude is an integer; any other
number keeps the form it had (binary64, or the compact integer / decimal that
spells it) when that form is exactly the number written, and is a decimal
otherwise (`1e400` is `70 02 60 0c`; a 36-digit fraction keeps its 36 digits).
A number whose digits do not fit a 127-bit mantissa is refused. Any JSON value
may be the document, a scalar included. To text, a decimal is written exactly,
bytes are standard padded base64, strings keep their text (only the escapes
JSON requires), member order and duplicate names are kept, and a NaN or an
infinity is an error rather than `null` or `inf`. Both directions validate the
pjson bytes first. The `pjson_to_json` rows of `conformance/cpp-golden.json`
pin bytes and text for C++ and Rust.

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
