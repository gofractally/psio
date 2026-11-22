# Transparent wrappers, custom types and invariants in the fixtures

The three shared fixtures (`pssz-vectors.json`, `json-vectors.json`,
`time-key-vectors.json`) hold one family of types for transparent wrappers
(`docs/pssz-spec.md` §5.10), custom text forms on a type of any shape and
type invariants (`docs/pssz-spec.md` §18, `docs/json.md` "Custom types
through a schema"). They are defined once, in
`cpp/tests/custom_conformance_types.hpp` (namespace `custom_conf`).

A text form and an invariant are code: a schema cannot carry them, so each
implementation re-implements the rules below by hand. This file states them
exactly. Everything else — the bytes, the wire-type texts, the Schema IR —
is in the fixtures.

## Types

`bytes` is a list of u8; `raw_string` is a raw byte string (C++
`std::string`, Rust `RawString`). "Schema node" is the definition the fixture
`schema_ir` holds for the type (a custom type's node is
`Custom{<wire type>, id}`).

| type | wire type | schema node | text form | invariant |
|---|---|---|---|---|
| `Id` | `u64` (transparent, member `value`) | named alias of `u64` | the integer's | none |
| `Blob` | `bytes` (transparent, member `data`) | `Custom{bytes, "Blob"}` | base64 | none |
| `Endpoint` | `variant<V4, V6>` | `Custom{variant, "Endpoint"}` | address and port | none |
| `Ratio` | `tuple<i16, u16>` | `Custom{tuple, "Ratio"}` | `n/d` | none |
| `Ref` | `variant<u64, raw_string>` (transparent, member `target`) | `Custom{variant, "Ref"}` | `#n` / `@name` | a name is not empty |
| `Name` | fixed-layout record `{value: u64}` | `Custom{record, "Name"}` | the record's | `value < 2^60` |
| `DerKey` | `bytes` (transparent, member `data`) | `Custom{bytes, "DerKey"}` | the byte list's | not empty and first byte `0x30` |
| `Range` | record `{lo: u32, hi: u32}` | `Custom{record, "Range"}` | the record's | `lo <= hi` |
| `Level` | enum over `u8`: `low = 0`, `mid = 1`, `high = 2`, `top = 3` | `Custom{enum, "Level"}` | the enum's | the value is one of the four listed |
| `V4` | fixed-layout record `{address: array<u8,4>, port: u16}` | record | the record's | none |
| `V6` | fixed-layout record `{address: array<u8,16>, port: u16}` | record | the record's | none |
| `Guarded` | record, below | record | the record's | none of its own |
| `ShardKey` (key fixture) | `tuple<u32, raw_string>` (transparent, member `key`) | named alias of the tuple | the tuple's | none |

```
Guarded {
   owner:    Name
   delegate: optional<Name>
   members:  list<Name>
   key:      DerKey
   window:   Range
   levels:   list<Level>
   pair:     array<Level, 2>
   either:   variant<Name, Range>
   ref:      Ref
   id:       Id
   blob:     optional<Blob>
   endpoint: Endpoint
   ratios:   list<Ratio>
}
```

In the pssz fixture `Endpoint` and `Ratio` have the structural ids
`variant<V4,V6>` and `tuple<i16,u16>` (their C++ types are the
`std::variant` and the `std::tuple` themselves: the text form is registered
for that exact type, wherever it is used); the json and key fixtures name
them `Endpoint` and `Ratio`. A transparent type's shape in the pssz
manifest is `transparent{member:T}`.

The handler ids, under which an implementation registers the text form and
/ or the invariant of a type with a schema-driven codec, are the `Custom`
ids: `Blob`, `Endpoint`, `Ratio`, `Ref`, `Name`, `DerKey`, `Range`, `Level`.

## Text forms

Each is carried as ONE JSON string, the same in `json` and `ijson`. Every
value has exactly one text; a reader accepts exactly that text.

**`Blob` — base64.** RFC 4648 §4: alphabet `A-Z a-z 0-9 + /`, `=` padding.
The text length is a multiple of 4; padding (`=` or `==`) appears only at the
end of the last group; the bits a padded group does not use are zero
(`"TQ=="` is the byte `4D`; `"TR=="` is rejected). No whitespace. The empty
list is `""`.

**`Endpoint`.**

- `V4`: `a.b.c.d:port` — the four address bytes and the port as canonical
  decimals (`0`, or a non-zero digit followed by digits: no leading zero, no
  sign), each octet at most 255, the port at most 65535.
- `V6`: `[g:g:g:g:g:g:g:g]:port` — eight groups of **exactly four lowercase
  hex digits**, group `i` being address bytes `2i` (high) and `2i+1`; no
  `::` shorthand, no upper case; the port as for `V4`.

A text that starts with `[` is read as `V6`, any other as `V4`.

**`Ratio`.** `n/d`: `n` is the i16 as `0 | -?[1-9][0-9]*` (so `-0` is
rejected), within −32768..32767; `d` is the u16 as a canonical decimal.

**`Ref`.** `#` followed by the canonical decimal of the u64, or `@` followed
by the name: the rest of the string, verbatim (any characters, including
`#` and `@`). `"@"` is the text of the empty name — well-formed text of a
value that the invariant then refuses. An empty text, or one that starts
with neither marker, is not this presentation.

## Invariants

Each is checked on a decoded value; the message is what the C++ invariant
returns (with the type's name as the status's format name), and is recorded
in the fixtures as `reason`.

| type | holds iff | message |
|---|---|---|
| `Name` | `value >> 60 == 0` | `Name: value is not a canonical name` |
| `DerKey` | `data` is not empty and `data[0] == 0x30` | `DerKey: not a DER sequence` |
| `Range` | `lo <= hi` | `Range: lo is greater than hi` |
| `Level` | the underlying u8 is at most 3 | `Level: not a listed level` |
| `Ref` | the alternative is the u64, or the name has at least one byte | `Ref: a name is not empty` |

Invariants are checked innermost first, and the first failure in wire order
is the one reported (the pssz vector `invariant/guarded/two_violations`).

## What the rows assert

A value that satisfies its invariants behaves as its wire type in bytes and
keys, and as its text form (or its wire type's text) in JSON; the `value` /
`corpus` rows are ordinary rows.

A value that **violates** an invariant is recorded with what three kinds of
reader do with it:

| reader | outcome |
|---|---|
| typed (the language's own type for the fixture type) | rejects, with the invariant's message (`reason`); no typed writer writes the value |
| schema-driven with **no** handler for the type's id | accepts: the value is its wire type. Its bytes validate, decode and re-encode; its text is the wire type's |
| schema-driven **with** the handlers of the ids above | rejects, with the same message, on the binary and the text entry points |
| schema-driven with a **strict** registry that lacks the handler | rejects: "no handler registered for a custom type" |

### pssz-vectors.json

- class `transparent`: every value of a transparent type twice, from the
  same frame — as the wrapper and as its member's type (`type` is the
  member's type, `writer` the wrapper). Two rejects: a frame that encodes
  `Id` as a one-field record, and a short frame.
- class `custom`: values of the custom types and their containers.
- class `invariant`: `hex` is a structurally valid frame of a value that
  violates an invariant, in each width profile; `expect` is `reject` — the
  **typed** outcome — and `reason` the invariant's message. Schema-driven:
  see the table above.
- type manifest: `custom_id` is the id of a custom type's schema node;
  `invariant: true` marks a type that an invariant constrains (its own or
  one of a type it holds).

### json-vectors.json

- `value` rows of a type that holds a custom **text form** carry
  `plain_json` / `plain_ijson` (the type manifest says `custom: true`).
- kind `invariant` (types with `invariant: true` in the manifest): `text` is
  well-formed text of the type under `format` — the typed text, so a custom
  text form where the type has one — whose value violates an invariant;
  `reason` is the message; `pssz` the bytes of that value; `plain_json` /
  `plain_ijson` the wire-type texts of those bytes. Typed: `text` and `pssz`
  are rejected with `reason`. Schema-driven with no handler: `pssz`
  validates, writes `plain_json` / `plain_ijson`, and each reads back to
  `pssz`. With the handlers: `text` and `pssz` are rejected with `reason`,
  and a value decoded without them is not written back (binary or text).
- `reject` rows of these types are texts that are not the type's text at
  all (malformed presentation, or the wire type's form where a text form
  exists).

### time-key-vectors.json

- `value`, `key-order`, `key-reject` and `key-prefix` rows for `Id`, `Blob`,
  `Endpoint`, `Ratio`, `Ref`, `Name`, `DerKey`, `Range`, `Level`, `ShardKey`
  and `tuple<Name,optional<Range>,list<Level>,DerKey>`. A key is the key of
  the wire type: `ShardKey`'s prefixes are those of the tuple it wraps.
- kind `key-invariant`: `key` is the key, and `pssz` the encoding, of a
  value of the wire type that violates an invariant of `type`; `reason` is
  the message. Typed key readers (`validate`, `decode`) and the typed pssz
  validator reject them with `reason`; no typed key writer writes the
  value. A reader that knows only the wire type decodes `key` to the value
  `pssz`.
