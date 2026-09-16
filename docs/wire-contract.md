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
| 0 | null |
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
