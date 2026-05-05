# pjson conformance corpus

Shared cross-language test fixtures. Both the C++ and Rust
implementations consume the same files here and must produce
byte-identical results.

## Fixture format

Each fixture is a JSON file at `conformance/fixtures/<id>.json` with
the following shape:

```json
{
  "id": "uint_inline_5",
  "spec_ref": "§4.3",
  "rule_ids": ["UI-001", "UI-002"],
  "description": "uint_inline encodes value 5 as the single byte 0x25",

  "input_value": {"kind": "uint", "value": 5},
  "wire_hex": "25",
  "json_compact": "5",

  "must_round_trip": true,
  "must_validate": true
}
```

Field reference:

- `id` — stable identifier, lowercase snake_case, unique across all
  fixtures.
- `spec_ref` — the spec section that pins this rule (e.g. `§4.3`).
- `rule_ids` — list of compliance-matrix row IDs (e.g. `UI-001`) this
  fixture exercises. Many fixtures touch multiple rows.
- `description` — one-line plain-English summary.
- `input_value` — structured representation of the value, in the small
  DSL described below.
- `wire_hex` — the canonical pjson byte sequence as a lowercase hex
  string, no separators. (`"25"` = a single byte 0x25.)
- `json_compact` — the JSON text that decoding `wire_hex` and rendering
  with default emitter options should produce.
- `must_round_trip` — if `true`, the harness checks that
  `encode(input_value) == wire_hex` AND `decode(wire_hex) == input_value`.
  If `false`, only the decode direction is checked (used for fixtures
  that exercise non-canonical encodings the encoder would not produce).
- `must_validate` — if `true`, the strict validator on `wire_hex` must
  pass. If `false`, the buffer is structurally valid but fails some
  canonicality check.

Optional fields for specialized fixtures:

- `json_pretty` — JSON text with the default pretty-print options (indent=2).
- `json_int_string_large_only` — JSON text with `int_string_mode=LargeOnly`.
- `json_int_string_all` — JSON text with `int_string_mode=All`.
- `must_reject` — if `true`, decoder MUST reject `wire_hex` (see
  `fixtures-reject/` for these cases). `wire_hex` is still required.
- `reject_reason` — short string identifier for the expected error
  class (`"truncated"`, `"reserved_tag"`, `"hash_mismatch"`, etc).

## Input-value DSL

`input_value` is structured so both C++ and Rust harnesses can
construct the same `Value` tree without depending on each other's
internal types.

### Atoms

```json
{"kind": "null"}
{"kind": "bool", "value": true}
{"kind": "uint", "value": 5}                 // any non-negative integer
{"kind": "int", "value": -42}                 // signed; encoder picks form
{"kind": "uint128", "lo": "0x...", "hi": "0x..."}
{"kind": "negint128", "lo": "0x...", "hi": "0x..."}
{"kind": "float", "width": 64, "bits_hex": "0x4008000000000000"}
{"kind": "decimal", "mantissa": "12345", "scale": -2}
{"kind": "string", "encoding": "raw_text", "text": "hello"}
{"kind": "string", "encoding": "escape_form", "text": "hello"}
{"kind": "bytes", "encoding": "base64", "bytes_hex": "deadbeef"}
{"kind": "extension", "subtype": 0, "bytes_hex": "..."}
```

### Wrappers

```json
{"kind": "numeric_string", "inner": {"kind": "uint", "value": 123}}
```

### Containers

```json
{"kind": "array", "children": [{...}, {...}]}
{"kind": "typed_array", "element_code": 6, "elements": [1, 2, 3, 4]}
{"kind": "object", "fields": [{"key": "a", "value": {...}},
                              {"key": "b", "value": {...}}]}
{"kind": "row_array", "shape": ["x", "y"], "rows": [[{...}, {...}], ...]}
```

The `value` field in object entries and the `inner` field in
`numeric_string` recursively use the same DSL.

## Directory layout

```
conformance/
├── README.md                    (this file)
├── fixtures/                    (positive cases — must round-trip)
│   ├── 4.1_null/
│   │   └── null.json
│   ├── 4.2_bool/
│   │   ├── false.json
│   │   └── true.json
│   ├── 4.3_uint_inline/
│   │   └── ...
│   ...
└── fixtures-reject/             (negative cases — must reject)
    ├── 4.4_nint_inline_zero.json
    ├── 4.6_ieee_reserved_bit3.json
    └── ...
```

Sub-directories under `fixtures/` mirror the spec section numbers so
a contributor can find every fixture for a section quickly.

## Running the corpus

```sh
tools/run-conformance.sh --lang cpp     # run all fixtures through C++
tools/run-conformance.sh --lang rust    # run all fixtures through Rust
tools/run-conformance.sh                # run both, plus cross-validation
```

The harness exits non-zero on any mismatch (wire-bytes, JSON,
decoded value, or rejection-status mismatch).
