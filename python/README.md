# psio for Python

PSSZ, `key` and JSON / I-JSON serialization from one set of annotated type
declarations — the Python form of PSIO. Pure Python, no dependencies,
Python 3.10 or newer.

```python
from dataclasses import dataclass
from typing import Annotated, Optional
import psio
from psio import u32, u64

@dataclass
class Account:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64

alice = Account(7, "alice", ["vip", "beta"], 100)
data = psio.to_pssz(alice)                      # canonical PSSZ bytes
assert psio.from_pssz(Account, data) == alice   # validates, then decodes
assert psio.to_json(alice) == '{"id":7,"owner":"alice","tags":["vip","beta"],"balance":100}'
assert psio.to_key(alice)[:8] == (7).to_bytes(8, "big")
```

The types are the ones the program wants — a `@dataclass`, `str`, `list`,
`dict`, `Optional`, an `enum.Enum` — and everything PSIO needs beyond the
type hint is `typing.Annotated` metadata. The bytes, the keys, the text and
the Schema IR are those of the C++, Rust and TypeScript libraries: the
shared fixtures under `conformance/` are replayed in full by `tests/`
(nothing skipped), and `psio.schema_of` of the Python declarations of every
fixture type is the fixture's Schema IR byte for byte.

- [Tutorial](#tutorial): the annotations, step by step
  (the Python edition of `docs/tutorial-annotations.md`)
- [Type reference](#type-reference): what each Python type is on the wire
- [Entry points](#entry-points)
- [Without Python classes: the schema-driven codec](#without-python-classes-the-schema-driven-codec)
- [Errors](#errors) · [Performance](#performance) · [Limits](#limits) · [Tests](#tests)

Every `python` block of this file is run, in order, by
`tests/test_readme.py`.

## Tutorial

One example: an account record and a small bank service. Each step has
three parts: **write** (the declaration), **get** (what changes), **check**
(the assertion that proves it). The rules are stated precisely in
[`docs/annotations.md`](../docs/annotations.md) and
[`docs/pssz-spec.md`](../docs/pssz-spec.md).

### Step 1 — Declare it plain

**Write** the dataclass you want.

```python
@dataclass
class AccountPlain:
    id: u64
    owner: str
    tags: list[str]
    balance: u64

# owner and tags may be any size: there is no bound, so offsets are 32-bit.
assert psio.codec(AccountPlain).inferred_bound is None
assert psio.codec(AccountPlain).width == 4
```

**Get.** The type already works in every format: `psio.to_pssz(a)`,
`psio.to_json(a)`, `psio.to_key(a)`, a schema (`psio.schema_of`). An `int`
needs a width, so the hints are `psio.u64`, `psio.u32`, … — they are
`Annotated[int, ...]` aliases: the field is an `int` to Python and to a type
checker.

`psio.codec(T)` is the compiled codec of a type; `inferred_bound` is the
bound its fields add up to (here none), which is what `width`, the size of
the record's offsets, is chosen from: 4 bytes, because nothing bounds it.
`max_encoded_size` is the largest encoding the type allows (here none
either); the two differ only under a `max_dynamic_data` cap (step 4).

### Step 2 — Add bounds until the width drops

**Write** the bounds on the members; the member types stay what they are.

```python
@dataclass
class Account2:
    id: u64
    owner: Annotated[str, psio.max(32)]                                    # at most 32 bytes of UTF-8
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]    # at most 8 tags of 16 bytes
    balance: u64

t = psio.codec(Account2)
# The list: 8 one-byte slots and 8 strings of 16 bytes.
tags = psio.codec(Annotated[list[Annotated[str, psio.max(16)]], psio.bounded(8)])
assert tags.inferred_bound == 8 * 1 + 8 * 16 == 136 and tags.width == 1
# The record: a header and two slots of one byte each, then what the members hold.
assert t.inferred_bound == (1 + 2) * 1 + 8 + 32 + 136 + 8 == 187               # <= 255: 8-bit offsets
assert t.width == 1
```

**Get.** Every level is bounded, so the type has a size bound, and the bound
selects the width: 1 byte up to 255, 2 bytes up to 65535, 4 above. Every type
that has offsets does this for itself: the record (its header and one slot
per variable member) and the list inside it (one slot per element) each take
the narrowest width whose worst case fits. The same value is 15 bytes
smaller:

```python
alice = Account2(7, "alice", ["vip", "beta"], 100)
data = psio.to_pssz(alice)
assert data.hex(" ") == ("12 07 00 00 00 00 00 00 00 12 17 64 00 00 00 00 00 00 00 "   # header, id, 2 slots, balance
                         "61 6c 69 63 65 "                                             # "alice"
                         "02 05 76 69 70 62 65 74 61")                                 # 2 one-byte slots, "vip" "beta"
assert len(data) == 33 and len(psio.to_pssz(AccountPlain(7, "alice", ["vip", "beta"], 100))) == 48
```

(These are the bytes of the C++, Rust and TypeScript tutorials.) A bound on
the list alone bounds nothing — the strings in it are still unbounded, and so
the list's own table stays 4 bytes wide:

```python
@dataclass
class AccountCountOnly:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8)]       # 8 strings of any size
    balance: u64

assert psio.codec(AccountCountOnly).inferred_bound is None
assert psio.codec(AccountCountOnly).width == 4
```

**Check.** A value outside a bound has no encoding, and bytes that hold one
are refused:

```python
import dataclasses

def refused(run):
    try:
        run()
    except psio.PsioError as e:
        return e
    raise AssertionError("accepted")

assert psio.from_pssz(Account2, data) == alice                       # validate, then decode
long_owner = dataclasses.replace(alice, owner="x" * 33)              # one byte past max(32)
assert refused(lambda: psio.to_pssz_checked(long_owner)).kind == "value"
nine_tags = dataclasses.replace(alice, tags=["t"] * 9)               # one element past max(8)
assert isinstance(refused(lambda: psio.to_pssz_checked(nine_tags)), psio.EncodeError)
```

The positions below a member:

| annotation | bounds |
|---|---|
| `psio.max(n)` | the member: bytes of a `str` / `bytes`, elements of a `list` / `dict` / `set`, bytes of a nested value (`psio.min(n)`, `psio.exact(n)`, `psio.length(min=, max=)` too, on the member itself) |
| `psio.element(...)` | every element of a list, an array or a set |
| `psio.keys(...)`, `psio.values(...)` | every key / mapped value of a `dict` |
| `psio.at(i, ...)` | element `i` of a `tuple`, alternative `i` (index or name) of a union |

An `Optional` is looked through, and the specs nest to any depth:
`Annotated[dict[str, list[u32]], psio.max(4), psio.keys(psio.max(16)), psio.values(psio.max(2))]`.

A bound may also be part of a type, for an alias used in many places (an
*opt-in bounded type*). The bytes are the same either way:

```python
Tag = Annotated[str, psio.max(16)]

@dataclass
class Account2b:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[Tag], psio.max(8)]
    balance: u64

assert psio.to_pssz(Account2b(7, "alice", ["vip", "beta"], 100)) == data
assert psio.codec(Account2b).inferred_bound == 187
```

### Step 3 — Reserve room for growth

`Account2` is as tight as it can be, which is the problem: it has 68 bytes of
slack. The day a field is added whose bound exceeds that, the width changes
from 8-bit to 16-bit and every stored record becomes unreadable. Width is
part of the type.

**Write** an explicit cap when the type is first published:

```python
@psio.record(max_dynamic_data=1024)
@dataclass
class AccountV1:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64

# The cap rules: 16-bit offsets, although today's fields need at most 187 bytes.
assert psio.codec(AccountV1).max_dynamic_data == 1024
assert psio.codec(AccountV1).width == 2
```

**Get.** `max_dynamic_data` is the width budget for the type's lifetime and
the limit of the encoded size, header included, enforced by every writer and
reader. The price here is 3 bytes per record:

```python
v1 = AccountV1(7, "alice", ["vip", "beta"], 100)
assert len(psio.to_pssz(v1)) == len(data) + 3
assert psio.from_pssz(AccountV1, psio.to_pssz(v1)) == v1

@psio.record(max_dynamic_data=16)
@dataclass
class Capped:
    text: str

assert len(psio.to_pssz(Capped("x" * 14))) == 16                    # 1 + 1 + 14
refused(lambda: psio.to_pssz_checked(Capped("x" * 15)))
assert not psio.validate_pssz(Capped, psio.to_pssz(Capped("x" * 14)) + b"x")
```

The cap widens only the record. The list in it keeps its own 1-byte table
(its bound, 136, is the tighter one). The other way round, a cap narrows what
lies beneath it: nothing inside a record of at most 255 bytes is larger than
255 bytes, so every list, map, set and tuple beneath it has 1-byte offsets
without a bound of its own — down to the next record, which keeps its own
width, and not into a nested value.

```python
@psio.record(max_dynamic_data=255)
@dataclass
class Memo:
    lines: list[str]                              # no bound: the cap is the bound
    grid: list[list[str]]

@dataclass
class MemoFree:
    lines: list[str]
    grid: list[list[str]]

assert psio.codec(Memo).width == 1
assert psio.to_pssz(Memo(["ab", "c"], [["x"]])).hex(" ") == "02 02 07 02 04 61 62 63 01 01 78"
assert len(psio.to_pssz(MemoFree(["ab", "c"], [["x"]]))) == 12 + 11 + 9
assert psio.codec(Memo).inferred_bound is None  # the cap changes widths, not bounds: lines is still list[str]
assert psio.codec(Memo).max_encoded_size == psio.codec(Memo).max_dynamic_data   # ... and no Memo is longer than its cap
```

`@psio.record(definition_will_not_change=True)` is for values closed forever
(a point, a hash): no header, no evolution.

### Step 4 — Add a field compatibly

**Write** the new field as `Optional`, **last**, and keep everything before
it unchanged:

```python
@psio.record(max_dynamic_data=1024)
@dataclass
class AccountV2:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64
    email: Annotated[Optional[str], psio.max(64)] = None      # new: optional, last

assert psio.codec(AccountV2).width == psio.codec(AccountV1).width == 2
```

**Get / check.** Both directions work:

```python
v1 = AccountV1(7, "alice", ["vip"], 100)
v2_none = AccountV2(7, "alice", ["vip"], 100, None)
v2_some = AccountV2(7, "alice", ["vip"], 100, "alice@example.com")

# A new writer that leaves the field out writes the old bytes: trailing absent optionals are pruned.
assert psio.to_pssz(v2_none) == psio.to_pssz(v1)
# Old bytes read as the new type: the field is absent.
assert psio.from_pssz(AccountV2, psio.to_pssz(v1)) == v2_none
# New bytes read as the old type: the unknown field is skipped ...
assert psio.from_pssz(AccountV1, psio.to_pssz(v2_some)) == v1
# ... and a reader that wants to know is told.
status = psio.validate_pssz(AccountV1, psio.to_pssz(v2_some))
assert status.ok and status.has_unknown
# The exact-schema check refuses unknown fields.
assert not psio.validate_pssz(AccountV1, psio.to_pssz(v2_some), strict=True)
refused(lambda: psio.from_pssz(AccountV1, psio.to_pssz_checked(v2_some), reject_unknown=True))
```

Never insert, remove, reorder or retype a member, and treat bounds as frozen.
(`psio.evolves_from(Old, New)` proves a revision: step 9b.)

### Step 5 — Raw bytes or UTF-8 text

A Python `str` is UTF-8 text (C++ `std::string` + `utf8`, Rust `String`).
`bytes` is a byte list (C++ `std::vector<std::uint8_t>`): hex in I-JSON.
`psio.RawString` is a C++ `std::string` / Rust `RawString`: any bytes on the
wire, `bytes` in Python, a JSON string in text.

```python
from psio import RawString

@dataclass
class Note:
    blob: Annotated[RawString, psio.max(64)]      # raw bytes: any byte string
    text: Annotated[str, psio.max(64)]            # UTF-8 text
    digest: Annotated[bytes, psio.array(4)]       # exactly four bytes

binary = Note(b"\xff\xfe", "café", bytes.fromhex("deadbeef"))
assert psio.from_pssz(Note, psio.to_pssz(binary)) == binary
# A str is text: a frame whose text member is not UTF-8 is refused, with the reason.
bad = psio.to_pssz(Note(b"", "x", bytes(4)))[:-1] + b"\xff"
assert psio.validate_pssz(Note, bad).reason == "utf8_spec: invalid leading byte"
# JSON text is UTF-8: a raw string that is not UTF-8 has no JSON.
assert refused(lambda: psio.to_json_checked(binary)).reason == "json: string is not valid UTF-8"
ok = Note(b"bytes", "café", bytes.fromhex("deadbeef"))
assert psio.to_json(ok) == '{"blob":"bytes","text":"café","digest":[222,173,190,239]}'
assert psio.to_ijson(ok) == '{"blob":"bytes","text":"café","digest":"DEADBEEF"}'
```

### Step 6 — JSON presentation

Nothing to write: the type already has its text. `json` is plain JSON with
every integer a number. `ijson` (I-JSON) is the form for JavaScript clients:
a 64-bit integer beyond ±(2^53 − 1) is a decimal string, bytes are hex, a
variant is `{"Name": value}`, an enum is its member's name. A missing
optional member reads as `None`.

**Write** an annotation only where a client needs one fixed form:

```python
@dataclass
class Totals:
    supply: Annotated[u64, psio.json_string, psio.ijson_string]     # always a string
    holders: Annotated[u64, psio.ijson_number]                      # always a number
    decimals: u32

a = AccountV2(7, "alice", ["vip"], 9007199254740993, None)          # 2^53 + 1
# json: every integer is a number.
assert psio.to_json(a) == '{"id":7,"owner":"alice","tags":["vip"],"balance":9007199254740993,"email":null}'
# ijson: a 64-bit integer beyond 2^53 - 1 is a decimal string.
assert psio.to_ijson(a) == '{"id":7,"owner":"alice","tags":["vip"],"balance":"9007199254740993","email":null}'
assert psio.from_ijson(AccountV2, psio.to_ijson(a)) == a
assert psio.from_ijson(AccountV2, '{"id":7,"owner":"alice","tags":["vip"],"balance":"9007199254740993"}') == a

# The same bounds apply to text.
assert not psio.validate_json(AccountV2, '{"id":7,"owner":"' + "x" * 33 + '","tags":[],"balance":0,"email":null}')

# json_string / ijson_number fix one member's form.
t = Totals(9007199254740993, 9007199254740993, 4)
assert psio.to_json(t) == '{"supply":"9007199254740993","holders":9007199254740993,"decimals":4}'
assert psio.to_ijson(t) == '{"supply":"9007199254740993","holders":9007199254740993,"decimals":4}'
assert psio.to_ijson(Totals(5, 5, 4)) == '{"supply":"5","holders":5,"decimals":4}'
# A reader accepts exactly the forced form.
assert not psio.validate_ijson(Totals, '{"supply":5,"holders":5,"decimals":4}')
# A member that is not Optional must be written: the error names it and its record.
assert refused(lambda: psio.from_json(Totals, '{"supply":"5","holders":5}')).reason == \
    'json: missing required member "decimals" of Totals'
```

**Field numbers, defaults, retired members.** protobuf, Cap'n Proto and
FlatBuffers number a record's members, give a scalar member a default and
retire members. State them on the member; the Schema IR records them as the
member attributes `field`, `default` and `deprecated`, and nothing changes in
the member's PSSZ bytes, key or text:

```python
@dataclass
class Ticket:
    id: Annotated[u32, psio.number(3)]             # its field number, when it is not its position (from 1)
    retries: Annotated[u32, psio.default(5)]       # a bool, an integer, a float, a member of its enum
    legacy: Annotated[u32, psio.deprecated]        # or psio.deprecated("2.0"): the version that retired it

members = psio.schema_of(Ticket).ir["types"][0]["type"]["value"].value["members"]
assert [[(a["name"], a["value"]) for a in m["attributes"]] for m in members] == \
    [[("field", "3")], [("default", "5")], [("deprecated", "")]]
assert psio.to_pssz(Ticket(1, 2, 3)).hex() == "0c010000000200000003000000"
```

A default of zero is the absence of one (no attribute is written).

**Untagged unions.** A union is written with its alternative named
(`{"string":"x"}` under `ijson`, `[0,"x"]` under `json`).
`Annotated[A | B, psio.untagged]` (C++ `psio_is_untagged`, the Variant
attribute `untagged` in the Schema IR) is written as the bare value of its
alternative, the same in both formats, and a reader picks the alternative by
the *kind* of JSON value it finds — null, boolean, number, string, array,
object — with no trial and no backtracking. The kinds two alternatives are
written as must not overlap (the table of `docs/json.md`, "Untagged
variants": a 64-bit integer may be a string, bytes may be a string, a map
may be an array): a declaration whose do is a `SchemaError`. The bytes are
those of any union.

```python
from typing import Union

Extra = Annotated[Union[str, Annotated[list[str], psio.alt("list")]], psio.untagged]

assert psio.to_ijson("s", Extra) == '"s"' and psio.to_json(["a", "b"], Extra) == '["a","b"]'
assert psio.from_ijson(Extra, '["a"]') == ["a"]
assert refused(lambda: psio.from_ijson(Extra, "7")).reason == \
    "json: the value is none of the untagged variant's alternatives"
assert psio.to_pssz(["a"], Extra) == psio.to_pssz(["a"], Union[str, Annotated[list[str], psio.alt("list")]])
# u64 may be written as a string in ijson: it cannot stand beside a string.
assert refused(lambda: psio.codec(Annotated[Union[u64, str], psio.untagged])).kind == "schema"
```

**Any JSON value.** `psio.JsonValue` (C++ `psio::json_value`) is a member
whose shape is not known in advance: `None`, a `bool`, a number, a `str`, a
`list`, or a `dict` with `str` keys, in the order written. In text it is that
value, inline; in PSSZ it is a UTF-8 string holding its compact JSON text; in
the Schema IR it is `Custom{Custom{List<u8>, "utf8"}, "json"}` (`ir.JSON`),
and `json` is a reserved id: no custom type and no handler may take it.
Numbers keep their text: a plain integer is an `int` (exact at any size), any
other number is a `psio.JsonNumber` — a `float` that remembers its token, so
`1.50` and `1e3` are written back unchanged. The formats differ in one thing
only: under `ijson` a string holding a Unicode noncharacter is neither read
nor written.

```python
@dataclass
class PrettyAction:
    service: str
    data: psio.JsonValue

action = psio.from_ijson(PrettyAction, '{"service":"tokens","data":{"amount":1.50,"memo":[true,null]}}')
assert action.data == {"amount": 1.5, "memo": [True, None]} and action.data["amount"].text == "1.50"
assert psio.to_ijson(action) == '{"service":"tokens","data":{"amount":1.50,"memo":[true,null]}}'
# PSSZ: the bytes of a record whose member is the string '{"amount":1.50,"memo":[true,null]}'.
@dataclass
class PrettyActionText:
    service: str
    data: str

assert psio.to_pssz(action) == psio.to_pssz(PrettyActionText("tokens", '{"amount":1.50,"memo":[true,null]}'))
# Bytes that are not one JSON value are not a value of the type.
assert not psio.validate_pssz(PrettyAction, psio.to_pssz(PrettyActionText("tokens", "{not json")))
```

### Step 7 — Keys

A database key is the `key` encoding of one or more fields: a byte string
whose bytewise order is the value's order
([`docs/key-spec.md`](../docs/key-spec.md)).

```python
# The primary key of the accounts table, and a secondary index by owner.
AccountKey = u64
ByOwner = tuple[str, u64]                         # (owner, id)

@dataclass
class OwnerKey:
    owner: Annotated[str, psio.max(32)]
    id: u64

# An integer key is big-endian: byte order is numeric order.
assert psio.to_key(258, AccountKey).hex(" ") == "00 00 00 00 00 00 01 02"
# A composite key is the concatenation; a string is terminated by 00 00.
assert psio.to_key(("bob", 7), ByOwner).hex(" ") == "62 6f 62 00 00 00 00 00 00 00 00 00 07"
# All keys of one owner: a prefix scan from the prefix to its upper bound.
prefix = psio.key_prefix(ByOwner, ["bob"])
assert prefix.hex(" ") == "62 6f 62 00 00"
assert psio.key_prefix_upper_bound(prefix).hex(" ") == "62 6f 62 00 01"
assert psio.from_key(ByOwner, psio.to_key(("bob", 7), ByOwner)) == ("bob", 7)

# A record is the key of its fields in order, with the same bytes as the tuple.
assert psio.to_key(OwnerKey("bob", 7)) == psio.to_key(("bob", 7), ByOwner)
# The key writer checks member bounds and UTF-8 too, as the pssz writer does (in every language).
refused(lambda: psio.to_key_checked(OwnerKey("x" * 33, 7)))
too_long = psio.to_key(("x" * 33, 7), ByOwner)
assert not psio.validate_key(OwnerKey, too_long)
```

`psio.to_key(value, T)` takes the type as its second argument when the value
does not say it (an `int`, a `tuple`); a dataclass instance does.

### Step 8 — Transparent wrappers and invariants

**Write** a transparent wrapper for a value that deserves its own type but
should be exactly its member on the wire — a `NewType`, or a class of one
field:

```python
from typing import NewType

AccountId = NewType("AccountId", u64)             # an int at run time, AccountId to a type checker

@psio.transparent
@dataclass(frozen=True)
class Money:                                      # a class of its own at run time
    units: u64

assert psio.codec(AccountId).fixed_size == 8 and psio.codec(Money).fixed_size == 8
```

The member of a transparent class may carry the bounds that make a plain type
a bounded one — `psio.max`, `psio.utf8` on a raw string, and the position
specs below it. The wrapper then **is** that bounded type wherever it is
held: its bytes, its width, its bounds, its schema node and its
checks in every format (`docs/pssz-spec.md` §5.10).

```python
@psio.transparent
@dataclass(frozen=True)
class Memo:                                       # is BoundedString(80)
    text: Annotated[str, psio.max(80)]

memo = psio.codec(Memo)
assert memo.inferred_bound == 80 and memo.to_pssz(Memo("hi")) == b"hi"
assert psio.to_json(Memo("hi")) == '"hi"'
for write in (psio.to_pssz_checked, psio.to_key_checked, psio.to_json_checked):
    refused(lambda: write(Memo("m" * 81)))        # every writer
assert not psio.validate_pssz(Memo, b"m" * 81)    # every reader
assert not psio.validate_pssz(Memo, b"\xff")      # ... and it is UTF-8

@dataclass
class Receipt:
    id: u32
    memo: Memo                                    # 80 bytes at most, here too
assert psio.codec(Receipt).inferred_bound == (1 + 1) * 1 + 4 + 80 and psio.codec(Receipt).width == 1
```

What a type cannot say is not allowed on that member — `psio.min` /
`psio.exact`, `psio.sorted` / `psio.unique`, an integer text form: state such
a rule as the type's invariant.

**Write** an invariant for a rule that bounds cannot express: a method that
returns `None` when the value is a value of the type and a message when it
is not.

```python
@dataclass
class Symbol:
    code: Annotated[str, psio.max(8)]

    @psio.invariant
    def _upper_case(self) -> Optional[str]:
        return None if self.code.isascii() and self.code.isupper() else "symbol is not upper case"
```

**Get / check.**

```python
# A transparent wrapper is its member in pssz, key and JSON.
assert psio.to_pssz(Money(42)) == psio.to_pssz(42, u64) == psio.to_pssz(AccountId(42), AccountId)
assert psio.to_key(Money(42)) == psio.to_key(42, u64)
assert psio.to_json(Money(42)) == "42"
assert psio.from_pssz(Money, psio.to_pssz(42, u64)) == Money(42)

# A value that violates its invariant has no encoding ...
assert refused(lambda: psio.to_pssz_checked(Symbol("usd"))).invariant
refused(lambda: psio.to_json_checked(Symbol("usd")))
# ... and bytes or text that hold one are refused, with the type's message.
frame = psio.to_pssz(Symbol("USD"))[:-1] + b"d"                     # "USd"
status = psio.validate_pssz(Symbol, frame)
assert not status.ok and status.reason == "symbol is not upper case"
assert not psio.validate_json(Symbol, '{"code":"usd"}')
# The trusted decode does not run invariants: only for bytes this program validated or wrote.
assert psio.from_pssz(Symbol, psio.trusted(frame)) == Symbol("USd")
```

A type with an invariant (or a text form) is a *custom type*: its Schema IR
node is `Custom{<wire type>, "Symbol"}`, so a tool that has only the schema
knows there is more to the type than its structure.

A custom **text form** is two more methods: the type is then ONE JSON string
in both text formats, and its bytes do not change.

```python
@psio.transparent
@dataclass(frozen=True)
class Version:
    packed: u32                                   # major * 65536 + minor

    @psio.to_text
    def __str__(self) -> str:
        return f"{self.packed >> 16}.{self.packed & 0xFFFF}"

    @psio.from_text
    @classmethod
    def parse(cls, text: str) -> "Version":
        major, minor = text.split(".")
        return cls(int(major) << 16 | int(minor))

assert psio.to_json(Version(0x00010002)) == '"1.2"'
assert psio.from_json(Version, '"1.2"') == Version(0x00010002)
assert psio.to_pssz(Version(0x00010002)) == psio.to_pssz(0x00010002, u32)
```

### Step 9 — Methods with capped arguments

**Write** the service as a class with typed method signatures. The arguments
of a call are the tuple of the parameters, and they take the annotations a
record's members take:

```python
from typing import Protocol

@psio.interface(name="bank")
class Bank(Protocol):
    def open(self, owner: Annotated[str, psio.max(32)],
             tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]) -> None: ...

    @psio.method(max_dynamic_data=1024)
    def transfer(self, from_: AccountId, to: AccountId, amount: u64,
                 memo: Annotated[str, psio.max(80)]) -> None: ...

    def symbol(self, token: u32) -> Annotated[str, psio.max(8)]: ...

bank = psio.interface_of(Bank)
assert [m.name for m in bank] == ["open", "transfer", "symbol"]
assert bank["transfer"].params == ["from", "to", "amount", "memo"]   # from_ is `from` on the wire
# open has no cap: header 1 + owner (1 + 32) + tags (1 + 8 x (1 + 16)) = 171, 8-bit.
assert bank["open"].args.inferred_bound == 1 + (1 + 32) + (1 + 8 * (1 + 16)) == 171
assert bank["open"].args.width == 1
# transfer is capped: 16-bit for its lifetime.
assert bank["transfer"].args.max_dynamic_data == 1024 and bank["transfer"].args.width == 2
```

**Get.** `method.args` is the codec of the argument tuple;
`method.to_pssz(*args, **kwargs)` encodes a call:

```python
args = bank["transfer"].to_pssz(AccountId(1), AccountId(2), 500, memo="rent")
assert len(args) == 2 + 8 + 8 + 8 + 2 + 4                           # 16-bit header and slot
assert bank["transfer"].from_pssz(args) == (1, 2, 500, "rent")      # validated: use for untrusted calls
# A memo of 81 bytes has no encoding.
refused(lambda: bank["transfer"].to_pssz_checked(1, 2, 500, "m" * 81))
```

In **text** an argument list has two forms (`docs/json.md`, "Method
arguments"). A method whose parameters have names writes an object keyed by
parameter name, and reads that object — members in any order, a missing
`Optional` argument is `None` — or the positional array. Binary is always the
positional tuple.

```python
transfer = bank["transfer"].args
assert transfer.to_ijson((1, 2, 500, "rent")) == '{"from":1,"to":2,"amount":500,"memo":"rent"}'
assert transfer.json_to_pssz('{"memo":"rent","amount":500,"to":2,"from":1}') == args    # any order
assert transfer.json_to_pssz('[1,2,500,"rent"]') == args                                # or the array
refused(lambda: transfer.from_json('[1,2,500]'))                                        # one element per parameter
```

A method with positional-only parameters (before `/`) has no parameter
names, as a C++ `method(add)` that lists none: its arguments are the array,
written and read. `max_fields` is the most parameters the method will ever
have (at most 255) — a promise that later revisions keep.

```python
@psio.interface(name="calc")
class Calc(Protocol):
    def add(self, a: u32, b: u32, /) -> u64: ...

    @psio.method(max_fields=4, max_dynamic_data=64)
    def put(self, id: u32, text: Annotated[str, psio.max(8)]) -> None: ...

    def names(self, limit: u32) -> Annotated[list[str], psio.max(4), psio.element(psio.max(16))]: ...

calc = psio.interface_of(Calc)
add = calc["add"]
assert (add.named, add.params) == (False, [])
assert add.args.to_json((1, 2)) == "[1,2]" and add.args.from_json("[1,2]") == (1, 2)
refused(lambda: add.args.from_json('{"a":1,"b":2}'))
assert add.to_pssz(1, 2) == psio.to_pssz((1, 2), tuple[u32, u32])
assert calc["put"].max_fields == 4
```

The **result** is annotated like a member (C++ `returns(...)`): it is then
that bounded type, and its bytes are that type's. `method.result` is its
codec; `result_to_pssz` / `result_from_pssz` / `validate_result` are the
short forms, `result_from_pssz(psio.trusted(data))` (or
`result_from_pssz_trusted`) reads bytes already validated, and
`result_view(data)` validates and then reads the result in place — as the
bounded type, whose offset tables a `psio.pssz_view` of the plain return type
would misread. Never write a result with `psio.to_pssz(value, ReturnType)`.

An empty optional result is **zero bytes** when its payload cannot be empty
(a number, a record: `docs/pssz-spec.md` §5.5.1): "no bytes" and `None` are
then the same frame. An optional whose payload can itself be empty (a string,
a list) has a selector byte: `None` is `00`. A method that returns nothing has
no result bytes: `validate_result` accepts only `b""`.

```python
assert bank["symbol"].result_to_pssz("USD") == b"USD"
refused(lambda: bank["symbol"].result_to_pssz_checked("X" * 9))
# A bounded list of bounded strings has a 1-byte offset table, not the 4-byte one of list[str].
names = calc["names"]
assert names.result_to_pssz(["alice", "bob"]).hex(" ") == "02 07 61 6c 69 63 65 62 6f 62"
assert names.result_from_pssz(bytes.fromhex("0207616c696365626f62")) == ["alice", "bob"]
assert len(psio.to_pssz(["alice", "bob"], list[str])) == 16
assert not names.validate_result(psio.to_pssz(["a"] * 5, Annotated[list[Annotated[str, psio.bounded(16)]], psio.bounded(8)]))
view = names.result_view(bytes.fromhex("0207616c696365626f62"))     # validated, read in place
assert (len(view), view[1]) == (2, "bob")
assert names.result_from_pssz(psio.trusted(bytes.fromhex("0207616c696365626f62"))) == ["alice", "bob"]
```

A schema that lists functions its own way — a map of name to
`{params, result}`, without the `Interface` envelope — takes each method's
`FunctionType` from `method.function_type(builder)` (C++
`SchemaBuilder::insert_method<T, I>()`): the argument list, and the result
**as the return annotation types it**. `interface.function_types()` is every
method on a fresh builder.

```python
builder = psio.SchemaBuilder()
fn = names.function_type(builder)                    # {"params": ..., "result": ...}
assert fn["result"]["value"].type == "BoundedList"   # not the plain list of strings
assert builder.schema().type(fn["result"]).validate_pssz(bytes.fromhex("0207616c696365626f62"))
schema, functions = calc.function_types()
assert list(functions) == [m.name for m in calc]
```

Dispatch and a call proxy:

```python
class Ledger:
    def __init__(self) -> None:
        self.moved = 0
    def open(self, owner, tags): ...
    def transfer(self, from_, to, amount, memo):
        self.moved += amount
    def symbol(self, token):
        return "USD"

ledger = Ledger()
assert bank.dispatch(ledger, "transfer", args) == b""               # validate, call, encode the result
# (dispatch writes the result fast; dispatch_checked holds it against the return annotation)
assert bank.dispatch(ledger, 2, bank["symbol"].to_pssz(1)) == b"USD"

remote = bank.proxy(lambda method, data: bank.dispatch(ledger, method.index, data))
remote.transfer(AccountId(1), AccountId(2), 250, "more")
assert ledger.moved == 750 and remote.symbol(1) == "USD"
```

A method evolves like a record: append `Optional` parameters, under a cap
declared from the first version.

```python
@psio.interface(name="bank")
class BankV2(Protocol):
    @psio.method(max_dynamic_data=1024)
    def transfer(self, from_: AccountId, to: AccountId, amount: u64,
                 memo: Annotated[str, psio.max(80)], fee: Optional[u64] = None) -> None: ...

v2 = psio.interface_of(BankV2)["transfer"]
assert v2.to_pssz(1, 2, 500, "rent") == args                         # old callers still work
assert v2.from_pssz(args) == (1, 2, 500, "rent", None)
```

`bank.schema()` (or `psio.schema_of(Bank)`) is the Schema IR with the
`Interface` and `Func` nodes, as C++ `SchemaBuilder::insert_interface` builds
them: per function its parameters (bounded types; `""` for a parameter
without a name), its result, and the attributes `maxFields` /
`maxDynamicData` (a result bound with no type form is `result.lengthBound`).
`schema(arguments=True)` also gives every argument list a name,
`<interface>.<method>`: an `Object` with the attribute `tuple` = `"named"` or
`"positional"`. A tool that has only the schema looks a method up by name:

```python
fetched = psio.Schema.from_pssz(calc.schema(arguments=True).to_pssz()).validate()   # as it would arrive
put = fetched.method("calc", "put")
assert (put.params, put.named, put.max_fields, put.max_dynamic_data) == (["id", "text"], True, 4, 64)
assert put.args.to_pssz({"id": 1, "text": "hi"}) == calc["put"].to_pssz(1, "hi")
assert put.args.json_to_pssz('[1,"hi"]') == calc["put"].to_pssz(1, "hi")
assert fetched.type("calc.put").from_pssz(calc["put"].to_pssz(1, "hi")) == {"id": 1, "text": "hi"}
# A list without names is a tuple in the dynamic model; the result is the bounded type.
assert fetched.method("calc", "add").args.from_pssz(add.to_pssz(1, 2)) == (1, 2)
assert fetched.method("calc", "names").result.to_pssz(["alice", "bob"]) == names.result_to_pssz(["alice", "bob"])
```

`conformance/interface-vectors.json` pins all of this for every language;
`tests/test_interface_conformance.py` declares its interface and replays
every row, typed and schema-driven.

### Step 9b — Order constraints, field caps, the upgrade check

**Write** `psio.sorted`, `psio.unique` or `psio.unique_keys` on a list (or
array) member, a method parameter, a result, or a position below one. Order
and equality are those of maps and sets: bytewise comparison of the elements'
`key` encodings, the same in every language.

| annotation | the elements are |
|---|---|
| `psio.sorted` | in non-descending order |
| `psio.unique` | in strictly ascending order |
| `psio.sorted(descending=True)` | the same, descending |
| `psio.unique_keys` | pairwise different by key: the element, or element 0 of a tuple element (no order) |

```python
@dataclass
class Ranked:
    scores: Annotated[list[u32], psio.sorted, psio.max(8)]
    names: Annotated[list[str], psio.unique]
    entries: Annotated[list[tuple[str, u32]], psio.unique_keys]
    rows: Annotated[list[list[psio.u8]], psio.element(psio.sorted)]

ok = Ranked([1, 1, 2], ["a", "ab"], [("x", 1), ("a", 1)], [[1, 2], []])
assert psio.from_pssz(Ranked, psio.to_pssz(ok)) == ok
bad = dataclasses.replace(ok, scores=[2, 1])
for write in (psio.to_pssz_checked, psio.to_json_checked, psio.to_key_checked):                    # every writer refuses
    assert refused(lambda: write(bad)).reason == "sorted: elements are not in order"
# ... and every checking reader: the frame of the same members without the constraint.
frame = psio.to_pssz(([2, 1], [], [], []), tuple[Annotated[list[u32], psio.bounded(8)], list[str],
                                                 list[tuple[str, u32]], list[list[psio.u8]]])
assert psio.validate_pssz(Ranked, frame).reason == "sorted: elements are not in order"
assert refused(lambda: psio.from_json(Ranked, '{"scores":[],"names":["a","a"],"entries":[],"rows":[]}')).reason \
    == "sorted: elements are not in strictly ascending order"
```

**Get.** No byte, width or bound changes, and the Schema IR does not record
the constraint: a typed reader enforces it, a schema-driven reader of the
same schema does not know it. `psio.trusted` and views do not check it. A map
or a set is ordered and unique by its type and takes no such annotation.

**Write** `max_fields` for the most fields a record (parameters a method)
will ever have. It is enforced where the count is known — the declaration,
and any schema that is read: an `Object` with more members than its
`maxFields`, with more than 255 members, or with `maxFields` above 255 is a
malformed schema.

```python
@psio.record(max_fields=4, max_dynamic_data=64)
@dataclass
class Capped:
    a: u32
    b: Optional[u32] = None

def too_many():
    @psio.record(max_fields=1)
    @dataclass
    class TooMany:
        a: u32
        b: u32
    return psio.codec(TooMany)

assert refused(too_many).kind == "schema"
from psio import ir
wide = {"Object": {"members": [ir.member(f"m{i}", ir.U8) for i in range(256)], "attributes": []}}
assert refused(lambda: psio.Schema({"Wide": wide}).type("Wide")).reason == "pssz IR: a record has more than 255 fields"
```

**Check** a revision before it ships: `psio.evolves_from(Old, New)` is the
schema-upgrade check of `docs/pssz-spec.md` §7.5 / §7.6 (C++
`pssz_evolves_from`). It holds when New's reader reads every value Old's
writer can produce, with the same bytes: the old fields in order followed
only by `Optional` ones, every offset width unchanged, a length bound only
widened, `utf8` / raw and `sorted` / `unique` unchanged, a `max_dynamic_data`
that admits everything the old type encodes to, and no more fields than the
original's `max_fields` (which is neither raised nor dropped).

```python
@psio.record(max_fields=4, max_dynamic_data=64)
@dataclass
class CappedV2:
    a: u32
    b: Optional[u32] = None
    c: Optional[Annotated[str, psio.max(8)]] = None

assert psio.evolves_from(Capped, CappedV2)
status = psio.evolves_from(CappedV2, Capped)
assert not status and (status.code, status.path) == ("fields-removed", "(root)") and "removed" in status.reason
assert psio.evolves_from(AccountV1, AccountV2)                         # step 4's revision
assert not psio.evolves_from(Account2, AccountCountOnly)               # a bound dropped: the widths change
# Two schemas, no classes: the same check over the types of two Schema IRs.
old_ir = psio.Schema.from_pssz(psio.schema_of(Capped).to_pssz())
new_ir = psio.Schema.from_pssz(psio.schema_of(CappedV2).to_pssz())
assert psio.evolves_from(old_ir.type("Capped"), new_ir.type("CappedV2"))

# mode="contract" (the default) also compares what a reader of the text sees; mode="wire" the bytes only.
@psio.record(max_fields=4, max_dynamic_data=64)
@dataclass
class CappedRenamed:
    a: u32
    count: Optional[u32] = None

status = psio.evolves_from(Capped, CappedRenamed)
assert (status.code, status.path) == ("field-renamed", "(root)")
assert psio.evolves_from(Capped, CappedRenamed, mode="wire")
# equivalent: the same type (the same values, bytes and permitted revisions), in both directions.
assert psio.equivalent(Capped, CappedRenamed, mode="wire")
assert psio.equivalent(Capped, CappedV2, mode="wire").code == "field-count-changed"
```

The result is a `psio.Compatibility` (a `Status`): truthy, or the first rule
broken as a stable `code` (the table of
[`conformance/evolution-vectors.md`](../conformance/evolution-vectors.md),
shared with C++ `psio::pssz_ir::evolves_from` / `equivalent`, Rust and
TypeScript), a `reason` for a person and the `path` from the root (`.field`,
`[]`, `[].0` / `[].1` for a map's key and value, `?`, `<Alt>`, `(nested)`;
`(root)` for the root itself). `mode="contract"`, the default, compares what
C++ `compare::contract` does: the bytes, and the names of members and
alternatives, custom ids, enums (a revision may add enumerators), the text
shape of a record, `json_number` / `json_string`, an alternative's I-JSON
name and whether a variant is `untagged`. `mode="wire"` compares the bytes
and which bytes are values, nothing else. Two Schema IRs do not record
`sorted` / `unique` / `unique_keys`; between two Python declarations a
difference in them is reported with the Python-only code `order-changed`.

The same two functions compare two methods (`psio.interface_of(C)[name]`,
`schema.method(interface, name)`: the argument list, path `(args)…`, and the
result, `(result)…`, present on both sides or on neither), two interfaces
(`psio.interface_of(C)`, `schema.interface(name)`: every function and listed
type of the old one must exist in the new one, by name —
`transfer(args).amount`, `type:Row`) and two whole `psio.Schema` values
(every interface, by name: `token/transfer(args)`).

### Step 9c — Names as numbers

`psio.method_to_number` / `number_to_method` and `psio.name_to_number` /
`number_to_name` are the two arithmetic name coders of C++
(`psio/compress_name.hpp`, `psio/name.hpp`): a short method name or an
identifier name (`a`-`z`, `0`-`9`, `-`; at most 18 characters) as a 64-bit
number. A number is a name's only if it decodes back to the name: where the
compressed code would not, `name_to_number` gives 0 and `method_to_number`
takes the hash form (bit 56 set, text `#` and sixteen letters).

```python
n = psio.method_to_number("transfer")
assert psio.number_to_method(n) == "transfer" and not psio.is_hash_name(n)
assert psio.method_to_number("Transfer") == n                          # case is ignored
h = psio.method_to_number("getSymbolType")                             # its code would not decode back
assert h == 0xD55DF3070B8758EB and psio.is_hash_name(h)
assert psio.number_to_method(h) == "#hmlnslheseoypnnp" and psio.method_to_number("#hmlnslheseoypnnp") == h
alice = psio.name_to_number("alice")
assert alice != 0 and psio.number_to_name(alice) == "alice"
assert psio.name_to_number("x--b") == 0 and psio.name_to_number("Alice") == 0
```

Every row of `conformance/name-vectors.json` is replayed by
`tests/test_consistency.py`.

### Step 10 — The same declarations in C++, Rust and TypeScript

| | C++ | Rust | TypeScript | Python |
|---|---|---|---|---|
| bounded text | `attr(owner, utf8 \| max<32>)` on `std::string` | `#[psio(max = 32)] owner: String` | `field(rawString, { utf8: true, max: 32 })` | `owner: Annotated[str, psio.max(32)]` |
| bounded list of bounded text | `attr(tags, max<8> \| element(utf8 \| max<16>))` | `#[psio(max = 8, element(max = 16))] tags: Vec<String>` | `field(list(rawString), { max: 8, element: { utf8: true, max: 16 } })` | `tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]` |
| cap | `maxDynamicData(1024)` | `#[pssz(max_dynamic_data = 1024)]` | `{ maxDynamicData: 1024 }` | `@psio.record(max_dynamic_data=1024)` |
| field-count cap | `maxFields(8)` | | | `@psio.record(max_fields=8)`, `@psio.method(max_fields=8)` |
| order | `attr(scores, sorted)`, `unique`, `unique_keys` | | | `scores: Annotated[list[u32], psio.sorted]`, `psio.unique`, `psio.unique_keys` |
| bounded wrapper | `PSIO_REFLECT(Memo, transparent(), attr(text, utf8 \| max<80>))` | `#[psio(transparent)] struct Memo(#[psio(max = 80)] String)` | `transparent('Memo', boundedUtf8(80))` | `@psio.transparent class Memo: text: Annotated[str, psio.max(80)]` |
| result bound | `method(names, limit, returns(max<4> \| element(utf8 \| max<16>)))` | | | `def names(self, limit: u32) -> Annotated[list[str], psio.max(4), psio.element(psio.max(16))]` |
| no parameter names | `method(add)` | | | `def add(self, a: u32, b: u32, /)` |
| upgrade check | `static_assert(psio::pssz_evolves_from<Old, New>)` | | | `assert psio.evolves_from(Old, New)` |
| fixed layout | `definitionWillNotChange()` | `#[psio(definition_will_not_change)]` | `{ fixed: true }` | `@psio.record(definition_will_not_change=True)` |

Agreement is checked the way the other languages check it: the offset width
and the bytes of a sample value.

```python
schema = psio.schema_of(AccountV1)
assert schema.type("AccountV1").width == 2
assert psio.Schema.from_json(schema.to_json()).to_pssz() == schema.to_pssz()   # the IR travels as text or bytes
```

### Step 11 — Reading: views, validated and trusted

```python
alice2 = AccountV2(7, "alice", ["vip", "beta"], 100, "alice@example.com")
data2 = psio.to_pssz(alice2)

# Untrusted bytes: validate first. validate_pssz never raises.
assert psio.validate_pssz(AccountV2, data2).ok

# A view reads in place: one field or element at a time, nothing else decoded.
view = psio.pssz_view(AccountV2, data2)
assert view.id == 7 and view.owner == "alice"
assert len(view.tags) == 2 and view.tags[1] == "beta"
assert view.to_value() == alice2

# from_pssz validates and decodes; trusted skips what trust makes unnecessary.
assert psio.from_pssz(AccountV2, data2) == alice2
assert psio.from_pssz(AccountV2, psio.trusted(data2)) == alice2
```

A view is read-only: there is no mutable view in this package (decode,
change, encode). The short name is the safe one — `from_pssz` validates.

## Type reference

| Python | wire type | notes |
|---|---|---|
| `psio.u8 … u64`, `i8 … i64`, `u128`, `i128`, `u256` | integers | `Annotated[int, ...]`: an `int` to Python. A bare `int` is an error (it has no width) |
| `bool` | bool | |
| `float`, `psio.f32` | f64, f32 | NaN payloads survive (`psio.NaN`) |
| `str` | UTF-8 string | validated on every read and write |
| `bytes` | byte list | hex in I-JSON, a number array in JSON |
| `psio.RawString` | raw string | `bytes` in Python, a JSON string in text |
| `Annotated[bytes, psio.array(N)]`, `Annotated[list[T], psio.array(N)]` | fixed array | |
| `list[T]`, `tuple[T, ...]` | list | |
| `tuple[A, B]` | tuple (an extensible positional record) | at most 255 elements; so has a method's parameter list |
| `Optional[T]`, `T \| None` | optional | an optional of an optional: `Optional[psio.Some[Optional[T]]]` |
| `dict[K, V]`, `set[T]`, `frozenset[T]` | map, set | written in key order whatever order the container has; `K` / `T` must be hashable (`@dataclass(frozen=True)`) |
| `A \| B` | variant | alternatives are told apart by their class; alternatives that share a class are `psio.Variant(label, value)`. An alternative has one **label** — the key of its I-JSON form and its name in the schema: the name of its type (`Circle`; `uint32`, `string`, `uint8[]`, `T[]`, `T?`, `tuple_A_B`, … for a type without a declared name), its position (`"0"`, `"1"`, …) for a type with no name or a type name two alternatives share, or what `Annotated[A, psio.alt("a")]` states (`docs/annotations.md` "Variant alternative labels") |
| `Annotated[A \| B, psio.untagged]` | variant (attribute `untagged`) | in text the bare value of the alternative, read by its JSON kind; the alternatives' kinds must not overlap |
| `psio.JsonValue` | UTF-8 string holding JSON text (custom id `json`) | any JSON value, inline in text; numbers keep their text (`int`, `psio.JsonNumber`) |
| `@dataclass`, `NamedTuple`, `TypedDict`, an annotated class under `@psio.record` | record | `@psio.record(...)` for the cap, a fixed layout, another name |
| `@psio.enum(u8) class Color(Enum)` | the integer | a member's name in text; `other=True` keeps undeclared values as `int` |
| `NewType("Id", u64)`, `@psio.transparent` class | its member | |
| `Annotated[datetime, psio.time_point("micros")]` (`psio.TimePointUs`, …) | time point | RFC 3339 in text; exact or refused, never rounded; on `int` it is the tick count |
| `Annotated[timedelta, psio.duration("millis")]` | duration | |
| `Annotated[int, psio.monotonic("nanos")]` | monotonic time point | |
| `Annotated[T, psio.nested()]` | nested value | the PSSZ of a `T` carried as a byte list; `Annotated[bytes, psio.nested(T, deferred=True)]` keeps the bytes |
| `Annotated[psio.Bits, psio.bitvector(N)]` / `bitlist(N)` | bit types | or `list[bool]` |
| `Annotated[T, psio.custom("Id", to_text=…, from_text=…, check=…)]` | custom type over any type | a class says the same with `@psio.to_text` / `@psio.from_text` / `@psio.invariant` |

A member's name on the wire is its attribute name; a trailing underscore
after a Python keyword is dropped (`from_` is `from`), and
`psio.name("...")` states any other. Annotations may also be given as
`dataclasses.field(metadata={"psio": [psio.max(8)]})`.

```python
import enum
from datetime import datetime, timezone

@psio.enum(psio.u8)
class Color(enum.Enum):
    RED = 0
    GREEN = 1

@dataclass(frozen=True)
class Circle:
    radius: u32

@dataclass(frozen=True)
class Square:
    side: u32

@dataclass
class Drawing:
    shape: Circle | Square
    color: Color
    at: psio.TimePointUs
    layers: dict[str, list[u32]]

d = Drawing(Square(3), Color.GREEN, datetime(2026, 11, 22, 10, 15, 30, 123456, tzinfo=timezone.utc), {"b": [1], "a": []})
assert psio.from_pssz(Drawing, psio.to_pssz(d)) == d
assert psio.to_ijson(d) == ('{"shape":{"Square":{"side":3}},"color":"GREEN",'
                            '"at":"2026-11-22T10:15:30.123456Z","layers":{"a":[],"b":[1]}}')
assert psio.to_json(d).startswith('{"shape":[1,{"side":3}],')
```

## Entry points

For a format `F` in `pssz`, `key`, `json`, `ijson` (the names of
[`docs/helpers.md`](../docs/helpers.md)):

| | |
|---|---|
| `psio.to_F(value[, T])` | encode: the **fast write**. `T` defaults to `type(value)`. The value is not held against what its type declares (bounds, order, invariants); an `EncodeError` is a value the format cannot represent (an offset that does not fit its width, a `max_dynamic_data` cap, a wrong Python type) |
| `psio.to_F_checked(value[, T])` | the **validated write** (`to_pssz_checked`, `to_key_checked`, `to_json_checked`, `to_ijson_checked`; on a codec and a method likewise, and `result_to_pssz_checked`): refuses what `validate_F` would refuse. Use it where the bytes leave the program; `dispatch_checked` writes results with it (`dispatch` writes them fast) |
| `psio.from_F(T, data)` | validate, then decode |
| `psio.from_F(T, psio.trusted(data))` | decode without what trust makes unnecessary (`from_pssz_trusted`) |
| `psio.validate_F(T, data)` | a `Status` (`ok`, `reason`, `has_unknown`); never raises |
| `psio.pssz_size(value)`, `psio.key_size(value)` | size of the encoding |
| `psio.pssz_view(T, data)` | read-only view |
| `psio.key_prefix(T, elements)`, `psio.key_prefix_upper_bound(prefix)` | prefix scans |
| `psio.json_to_pssz(T, text)`, `psio.pssz_to_json(T, data)` | text ↔ bytes |
| `psio.schema_of(T, ...)` | the Schema IR (`.ir`, `.to_json()`, `.to_pssz()`, `.validate()`); of an `@psio.interface` class, its `Interface` |
| `psio.evolves_from(Old, New, mode="contract")`, `psio.equivalent(A, B, mode="contract")` | the schema-upgrade check and the same-type check over two types, methods, interfaces or schemas: a `Compatibility` (`ok`, `code`, `reason`, `path`); `mode="wire"` compares the bytes only |
| `psio.interface_of(C)`, `schema.method(interface, name)` | methods: `args`, `result`, `named`, `max_fields`, `max_dynamic_data` |
| `psio.method_to_number(s)`, `number_to_method(n)`, `is_hash_name(n)`, `name_to_number(s)`, `number_to_name(n)` | the name coders |
| `psio.codec(T, width=…)` | the compiled codec: every operation as a method, plus `width`, `inferred_bound` (what the width is chosen from), `max_encoded_size` (the largest encoding), `max_dynamic_data`, `fixed_size` |

## Without Python classes: the schema-driven codec

A tool that holds a Schema IR (fetched as bytes or text) and no Python
classes uses the same codec on plain values: records are `dict`, variants
`psio.Variant(name, value)`, enums and time values `int`, a `json` node the
JSON value it holds. A Variant with the attribute `untagged` is written and
read as the bare alternative (a schema whose alternatives' JSON kinds overlap
is refused where a value of it is read), and the reader names a missing
member (`json: missing required member "owner"`).

```python
schema = psio.Schema.from_pssz(psio.schema_of(AccountV2).to_pssz())   # as it would arrive
T = schema.type("AccountV2")
value = T.from_pssz(data2)
assert value == {"id": 7, "owner": "alice", "tags": ["vip", "beta"], "balance": 100, "email": "alice@example.com"}
assert T.to_pssz(value) == data2 and T.json_to_pssz(T.to_json(value), "json") == data2

# A schema may also be written by hand (psio.ir has the spellings).
from psio import ir
point = psio.Schema({"Point": {"Struct": {"x": ir.I32, "y": ir.I32}}}).type("Point")
assert point.to_pssz({"x": 1, "y": -1}).hex() == "01000000ffffffff"
```

Custom types are served by a handler registry, as in JavaScript and Rust:
`psio.Schema.from_pssz(data, handlers={"AccountName": psio.Handler(to_text=…, from_text=…, check=…)},
require_handlers=False)`; `nested_bytes="opaque"` reads every nested value
as its bytes.

## Errors

Every entry point raises a `psio.PsioError` subclass and nothing else for bad
input: `SchemaError` (a declaration or a schema), `EncodeError` (a value that
is not a value of its type), `DecodeError` (bytes), `JsonError` (text). The
message (`error.reason`) is the reason the shared fixtures record where they
pin one — the words of the C++ validator, or the invariant's own message
(`error.invariant` is then true).

## Performance

Each type is compiled once into closures; `psio.to_pssz(v)` of a dataclass
is one dictionary lookup and a call. `python/bench/bench.py` reports encode
/ decode / view numbers. The codec's seam for a native accelerator is
`Codec.encoder(node)` / `Codec.decoder(node)` in `psio/_core.py` (plain
callables per node and position); none is built.

`psio.from_pssz(T, psio.trusted(data))` leaves out what the statement "these
bytes are a valid encoding" makes unnecessary — length bounds and caps,
pruning and map / set order, unused bits, invariants — and parses the frames
as always, so it never reads outside `data`. The gain is what there was to
leave out (`bench.py`, CPython 3.14): about 3% on a record with no bounds,
1.36x on a list of 1,000 bounded, capped records, 1.38x on a map of 1,000
entries (whose order check encodes every key).

## Limits

- No mutable view, no pjson, no WIT / GraphQL emitters, no formats outside
  pssz / key / json / ijson.
- `psio.sorted` / `psio.unique` / `psio.unique_keys` are the typed
  declaration's: the Schema IR does not record them, so a schema-driven
  reader (and `psio.evolves_from` over two Schema IRs) does not know them.
- `psio.evolves_from` compares wire compatibility. It does not compare
  member names, and a transparent type is compared as its member (the Schema
  IR resolves the alias): C++ also requires the same transparent name. It
  follows `docs/pssz-spec.md` §7.5 in accepting an alternative appended to a
  variant, which C++ `pssz_evolves_from` (equal alternative counts) refuses.
- Python compares unions without regard to order (`A | B == B | A`) and
  `typing.List[...]` / `Optional[...]` cache their results under that
  equality, while the order of a variant's alternatives is its wire format.
  Declare a union once, under a name, and use the name.
- `psio.trusted` is for bytes this program wrote or validated. For any other
  input the result is a value outside the type's bounds or a `DecodeError`
  (never a read outside the input): text is still decoded as UTF-8, and a
  frame that cannot be parsed is still refused.
- The Schema IR records a bound, not how it was declared, so a reader's
  message for a value outside a bound is one of
  `pssz: member length_bound violated`, `pssz: bounded list exceeds its bound`,
  `pssz: utf8_string exceeds its bound`.
- attrs / pydantic / msgspec classes are not reflected (dataclasses,
  `NamedTuple`, `TypedDict` and annotated classes are).

## Tests

```sh
cd python
PYTHONPATH=src python3 -m unittest discover -s tests -v
```

The package is typed (`py.typed`). With mypy installed (for example in
`python/.venv`, which git ignores):

```sh
python -m mypy                                             # the package, strict (pyproject.toml)
MYPYPATH=src python -m mypy --strict tests/typing_sample.py  # user code against the public API
```

`tests/` replays every vector of `conformance/pssz-vectors.json` (360 types,
7035 vectors), `json-vectors.json`, `time-key-vectors.json` (56 types, 1164
rows), `schema-json-vectors.json` and `interface-vectors.json` (6 methods, 55
rows), and the pssz / key / json rows of `cpp-golden.json`, on the
schema-driven path and on the typed path, every verdict of
`evolution-vectors.json` (100 type rows and 12 interface rows: `evolves_from`
and `equivalent`, `wire` and `contract`, with code and path), and every row of
`name-vectors.json` (1538 names, 617 numbers), with exact counts asserted.
`psio.schema_of` of the Python declarations of every fixture type — and
`psio.interface_of(Token).schema(arguments=True)` for the interface fixture —
is the fixture's `schema_ir` byte for byte.
