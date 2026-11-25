# Rust: views of one shape for every format

`psio::view` reads a value in place the same way whatever format its bytes
are in. Code written against one format works with another by changing a
type parameter, as with `psio::view<T, Fmt>` in C++. It sits on top of the
formats' own views (`PsszView`, `FbView`), which stay as they are.

Formats: PSSZ (every format tag: `Pssz`, `PsszUnder<C>`, `Pssz8`, `Pssz16`,
`Pssz32`) and FlatBuffers.

```rust
use psio::view::{Reads, View};
use psio::pssz::Pssz;
use psio::flatbuf::Flatbuf;

#[derive(PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Item { product: String, qty: u32, price: f64 }

#[derive(PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Order { id: u64, items: Vec<Item>, note: Option<String> }

// Written once. `F` is the format; `Reads<Order>` is the only bound.
fn total<F: Reads<Order>>(order: View<'_, Order, F>) -> u64 {
    let mut total = 0;
    for item in order.items() {
        if !item.product().is_empty() {
            total += item.qty() as u64;
        }
    }
    total
}

total(View::<Order, Pssz>::new(&pssz_bytes)?);
total(View::<Order, Flatbuf>::new(&flatbuf_bytes)?);
let note: &str = View::<Order, Pssz>::new(&pssz_bytes)?.note().unwrap_or("");
```

## What a value reads as

`View<'a, T, F>` is what a `T` reads as in the format `F`. It is the same
for every format:

| type | reads as |
|---|---|
| integer, float, `bool`, `Uint256` | the value |
| `String`, `BoundedString`, `&str` | `&str` |
| `RawString`, `BoundedRawString`, `&[u8]` | `&[u8]` |
| record | a struct `<Record>View` the derive writes: `new`, `trusted`, `unpack`, and one method per member, as visible as the member |
| `Vec<T>`, `[T; N]`, `BoundedList` | `ListView`: `len`, `is_empty`, `get(i)`, `at_index(i)`, `first`, `last`, `iter`, `for`, `unpack`; an array also `to_array()` and `map(f)` |
| `BTreeMap`, `BoundedMap` | `MapView`: entries by ascending key (`entry(i)`, `iter`, `keys`, `values`, `for`) and `get(key)`, `contains_key(key)`, `find(key)` |
| `BTreeSet`, `BoundedSet` | `SetView`: keys ascending (`get(i)`, `iter`, `for`) and `contains(key)`, `find(key)` |
| `Option<T>` | `Option` of what `T` reads as: the standard `Option`, with all its methods |
| `Box<T>`, `Rc<T>`, `Arc<T>`, `RefCell<T>`, `&T` | what `T` reads as |
| `(A, B, …)` up to 12 | the tuple of what the elements read as |
| enum without payloads | the enum's value (the enum is `Copy`) |
| enum with payloads | an enum `<Name>View` the derive writes, one alternative per variant holding what the payload reads as: `match` on it |

A member with a bound (`#[psio(max = 8)] tags: Vec<String>`) reads as the
type the bound describes (`BoundedList<String, 8>`), which is what PSSZ
stores and what selects the width of its offsets.

A list's elements are values, not references into the buffer, so a list
has no `[]`: `at_index(i)` panics past the end as `[]` does.

What one generic function can read in several formats is what all of them
hold. FlatBuffers has no tuple, no `Box`, no list of lists or of optionals,
no tuple struct, a union's alternatives hold tables, and only a table is the
root of a buffer.

## Validated and trusted

* `View::<T, F>::new(bytes)` and `view::read::<T, F>(bytes)` run the
  format's validator, then borrow. Use them for bytes from outside.
* `View::<T, F>::trusted(bytes)` and `view::read_trusted::<T, F>(bytes)` do
  not validate: the caller vouches for the bytes (validated earlier, or
  written by the format's encoder).

Every function that makes a view of bytes without validating them has
`trusted` in its name, nothing converts a trusted view into a validated one,
and nothing here writes. Both are the same type. Accessors are safe on any
bytes: on bytes that would not validate, an accessor of a trusted PSSZ view
may panic and one of a trusted FlatBuffers view reads a default (a string
that is not UTF-8 panics in PSSZ and reads as `""` in FlatBuffers); neither
reads outside the buffer or makes an invalid value. A string is checked to
be UTF-8 each time it is read.

`view::read` is also how the root of a PSSZ buffer that is not a record is
read: `view::read::<Vec<Token>, Pssz>(bytes)`.

## `#[derive(View)]`

The type also derives the own view of each format it names (`PsszView`;
`FbView`) and, for `unpack()`, the decoder (`PsszUnpack`; `FbUnpack`).

| `#[view(...)]` | |
|---|---|
| `pssz`, `flatbuf` | the formats; `pssz` when none is named |
| `name = "Name"` | the name of the struct or enum the derive writes, instead of `<Type>View` |
| `no_validator` | a record that borrows (`&'a str` members) has no PSSZ validator, because PSSZ derives its validators with its decoders: `new` reports that, and `trusted` makes the view |
| `no_debug` | the view of an enum with payloads is not `Debug` (needed for two enums that hold each other; one that holds itself is found by the derive) |

The derive does not ask what kind of type a member is. A type alias, a
renamed import, a qualified path, a transparent newtype, a tuple struct
(`field_0()`, as in `PsszView`), a record with type or lifetime parameters
and a record that holds itself need nothing said.

A value type of your own says what it reads as and how each format family
reads it:

```rust
impl ViewShape for Cents {
    type Members = ();
    type Shape<'a, F: ViewTag, R: FieldList<F, ()>> = Cents;
}
impl<F: PsszFormat> ViewIn<PsszFamily, F> for Cents {
    type Fields = Nil<F, F>;
    fn validate(bytes: &[u8]) -> PsszResult<()> { PsszView::<Cents, F>::new(bytes).map(|_| ()) }
    fn trusted_root<'a>(bytes: &'a [u8]) -> Cents { Cents { amount: view::read_trusted::<u64, F>(bytes) } }
}
```

## Limits

* A member's format is its own type in generic code (`view::B0<Order, F>`
  for the first): two members are not known to be at the same format until
  `F` is a format. Passing a member's view to a function generic over the
  format works; the function infers its own.
* The type of member `k` is found through `k` steps. A record of more than
  about 100 members needs `#![recursion_limit = "..."]` in the crate that
  defines it (tried: 64 members as is, 200 with the limit raised).
* A member named `new`, `trusted` or `unpack` is refused: it would hide the
  view's own.
* A variant with named fields is not supported (hold a record), nor an enum
  with type parameters.
* A view of a record prints as its value (through `unpack()`, so the record
  is `Debug` and decodable). The view of an enum that holds itself is not
  `Debug`.
* FlatBuffers does not check `#[psio(max = N)]` when it reads (psio#14): a
  buffer PSSZ would refuse for a bound is accepted by `View::<_, Flatbuf>::new`.
* Not there: a view that writes, the `try_*` accessors of `PsszView`,
  `new_strict` / `new_checked`, the formats other than PSSZ and FlatBuffers,
  `HashMap` / `HashSet`, `Nested`, the bit types, and `Vec<u8>` as `&[u8]`
  (it is a list of `u8`; use `RawString`).
