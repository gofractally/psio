# PSIO type annotations — rules for AI coding agents and reviewers

Terse rules for writing and reviewing PSIO-reflected types. Each rule
links to the section of [annotations.md](annotations.md) that states and
justifies it; the walk-through is [tutorial-annotations.md](tutorial-annotations.md).
Do not invent syntax: every spelling you may use is in the
[master table](annotations.md#4-master-table).

> **Pending wire change.** List offset-table widths **[W1]**, a 255-element
> tuple limit **[W2]**, `utf8` enforcement **[W3]** and bit-type bounds
> **[W4]** are changing in a commit that is not yet merged. Rules that
> depend on one carry its tag; see
> [annotations.md, Pending wire change](annotations.md#pending-wire-change).

The C++ snippets are consecutive lines of
[`cpp/tests/doc_annotations_examples.cpp`](../cpp/tests/doc_annotations_examples.cpp)
(test `psio_doc_annotations_examples`).

## The model in five sentences

1. One `PSIO_REFLECT` with annotations defines the type in every format:
   pssz, key, JSON / I-JSON, Schema IR, WIT, GraphQL.
2. Bounds are written **on the member**, not by changing its type.
3. The pssz offset width (1, 2 or 4 bytes) is **part of the type**: an
   explicit `maxDynamicData` always rules, else the inferred bound rounded
   up to the next width.
4. A published type changes in exactly one way: `std::optional` members
   appended at the end, at an unchanged width.
5. Untrusted bytes are validated; views and `psio::trusted` check nothing
   you can rely on.

## Decision rules

### Declaring a type

| # | Rule | Why / reference |
|---|---|---|
| D1 | Keep the member types the code wants (`std::string`, `std::vector<T>`, `std::map<K, V>`). **Never change a member's type to express a bound; annotate** with `attr(member, specs)`. Use `psio::bounded<T, N>` / `psio::utf8_string<N>` only where the existing code already uses them or the member must be read through a view under a narrowed nested width. | [2.3](annotations.md#23-wrapper-types), [5.3](annotations.md#53-element-keys-values-ati--positions-below-the-member) |
| D2 | **If a member is variable-size, give it a bound** (`max<N>`) unless it is genuinely unbounded content (a document body, a blob). One unbounded member makes the whole type 32-bit. | [3](annotations.md#3-width-what-a-bound-buys) |
| D3 | Bound **every level**: `max<N> \| element(max<M>)` for a list of strings, `keys(...)` / `values(...)` for a map, `at<I>(...)` for a tuple, pair or variant. A count without element bounds bounds nothing. | [5.3](annotations.md#53-element-keys-values-ati--positions-below-the-member) |
| D4 | **Human-readable text is `utf8`; bytes are raw.** `std::string` without `utf8` is raw bytes (Rust `RawString`); with `utf8` it is Rust `String`. Byte data: `std::vector<std::uint8_t>` / `std::array<std::uint8_t, N>`. **[W3]** | [5.2](annotations.md#52-utf8--text-or-raw-bytes), [6.5](annotations.md#65-bytes-and-hex) |
| D5 | **Set `maxDynamicData(N)` on every type that may gain a field** (rows, action arguments, messages). Pick N ≥ the current inferred bound + planned growth. Default choice: a 16-bit budget (N ≤ 65535). | [5.4](annotations.md#54-maxdynamicdatan) |
| D6 | Use `definitionWillNotChange()` only for values closed forever (point, hash, id pair). It removes the header and all evolution. | [5.5](annotations.md#55-definitionwillnotchange) |
| D7 | Wrap ids, keys and quantities in a `transparent()` type; put the bound in the member's **type** (a transparent member takes no `attr`). | [5.7](annotations.md#57-transparent) |
| D8 | A rule bounds cannot express is an `invariant(fn)`: `noexcept`, returns `psio::codec_status`, never throws, depends only on the value. | [5.8](annotations.md#58-invariantfn) |
| D9 | Give an enum an explicit underlying type and `PSIO_REFLECT_ENUM`. | [6.3](annotations.md#63-enums) |
| D10 | Annotate integer text form (`json_string`, `ijson_number`, …) only when a client requires one fixed form. JavaScript clients read I-JSON. | [5.9](annotations.md#59-json_number-json_string-ijson_number-ijson_string) |
| D11 | A method's parameters are annotated like members; give `method(...)` a `maxDynamicData(N)` if it may ever gain a parameter; annotate bounded results with `returns(...)`. | [5.11](annotations.md#511-methodname-) |
| D12 | `static_assert` the width (and, when no cap is declared, the inferred bound) next to every published type. | [3](annotations.md#3-width-what-a-bound-buys) |
| D13 | A range is one spec: `psio::length_bound{.max = B, .min = A}`. `exact<N>` / `min<N>` are for the member itself only. | [5.1](annotations.md#51-maxn-exactn-minn--length-bounds) |

### Changing a published type

| # | Rule | Why / reference |
|---|---|---|
| E1 | **New fields are `std::optional<T>` and go last.** Trailing `None` is pruned, so old bytes stay valid and new writers that leave the field out write old bytes. | [8.1](annotations.md#81-the-compatible-change) |
| E2 | **A new field must not change the width.** Without an explicit cap there is only the rounding slack; if the inferred bound crosses 255 or 65535 the change is incompatible. Reserve with `maxDynamicData` *before* publication. | [8.1](annotations.md#81-the-compatible-change), [8.2](annotations.md#82-what-is-not-compatible-and-how-the-tooling-catches-it) |
| E3 | Never insert, remove, reorder or retype an existing member. | [8.2](annotations.md#82-what-is-not-compatible-and-how-the-tooling-catches-it) |
| E4 | Never move `maxDynamicData` across a width class (255 / 65535), and never add or remove `definitionWillNotChange()` or `transparent()`. | [8.4](annotations.md#84-summary) |
| E5 | **Treat member bounds, `utf8` and invariants as frozen.** `pssz_evolves_from` does not compare them. | [8.3](annotations.md#83-what-pssz_evolves_from-does-not-see) |
| E6 | **Enums: only add enumerators.** Never change the underlying type, renumber or reuse a value. | [6.3](annotations.md#63-enums) |
| E7 | Do not add a variant alternative to a published variant; do not change an array length. | [8.2](annotations.md#82-what-is-not-compatible-and-how-the-tooling-catches-it) |
| E8 | Methods: append `std::optional` parameters only, under the method's cap. | [5.11](annotations.md#511-methodname-) |
| E9 | Prove every revision: `static_assert(psio::pssz_evolves_from<Old, New>)` against a kept copy of the previous declaration. | [8.2](annotations.md#82-what-is-not-compatible-and-how-the-tooling-catches-it) |

### Reading and writing

| # | Rule | Why / reference |
|---|---|---|
| R1 | **Validate untrusted bytes**: action arguments, cross-service data, user input. `psio::from_pssz<T>(bytes)` or `psio::validate_pssz<T>(bytes)` first. | [7.3](annotations.md#73-the-trust-rule) |
| R2 | **`psio::trusted{bytes}` and views only for bytes your own service wrote and stored**, or validated earlier in the same flow. | [7.3](annotations.md#73-the-trust-rule) |
| R3 | A `pssz_view` checks nothing: not structure, not bounds, not invariants. Use `psio::validated_pssz_view` on untrusted bytes. | [7.2](annotations.md#72-what-is-checked-where) |
| R4 | Validate keys from outside with `validate_key` / `from_key`: the C++ key **encoder** does not check member bounds or `utf8`. **[W3]** | [6.19](annotations.md#619-tables-and-keys) |
| R5 | Read views are const: do not try to write through one. In-place writes use `psio::pssz_mutable_view<T>`, fixed-size values only. | [7.4](annotations.md#74-const-safety) |
| R6 | Handle the failure: `to_pssz` / `to_json` throw `psio::codec_exception` (abort without exceptions) for a value outside its bounds; use `encode_pssz_checked` / `encode_json_checked` / `validate_*` where a status is needed. | [5.1](annotations.md#51-maxn-exactn-minn--length-bounds) |
| R7 | Do not use a small-capped type as a JSON root in C++: the JSON validator applies the cap to the text size. | [5.4](annotations.md#54-maxdynamicdatan) |

### Across languages

| # | Rule | Reference |
|---|---|---|
| X1 | Mirror a declaration field by field. C++ `utf8 \| max<N>` on `std::string` = Rust `#[psio(max = N)] String` = TS `field(rawString, { utf8: true, max: N })`. C++ `max<N>` on `std::string` = Rust `#[psio(max = N)] RawString` = TS `field(rawString, { max: N })`. | [4](annotations.md#4-master-table) |
| X2 | Mirror the cap: `maxDynamicData(N)` = `#[pssz(max_dynamic_data = N)]` = `{ maxDynamicData: N }`. Mirror `definitionWillNotChange()` = `#[psio(definition_will_not_change)]` = `{ fixed: true }`. | [4](annotations.md#4-master-table) |
| X3 | Do not use `exact<N>` / `min<N>` / `maxFields` in a type that Rust must mirror (not available there). | [4](annotations.md#4-master-table) |
| X4 | Confirm agreement by comparing the offset width and the bytes of one sample value in each language. | [Rust and TypeScript snippets](annotations.md#rust-and-typescript-snippets) |

## Checklist: adding a type

1. Write the struct with plain member types. List members in wire order
   in `PSIO_REFLECT`.
2. For each variable-size member (string, list, map, set, nested value,
   and optionals / variants / tuples that contain one): add `max<N>` and
   bounds for every level below it, or record why it is unbounded.
3. Mark text `utf8`.
4. Decide growth: `maxDynamicData(N)` (may grow),
   `definitionWillNotChange()` (closed forever), or neither (accepts that
   only the rounding slack is available).
5. Add the assertions (below) for width and bound.
6. Add a round-trip test, one rejection at bound + 1 per bounded member,
   and — if the type has an invariant — one violating value.
7. If the type crosses languages, add the mirrored declaration and compare
   width and sample bytes.
8. If the type is an action argument or arrives from another service, the
   reading code validates.

## Checklist: changing a published type

1. Keep the old declaration (rename it `…V1` in a test).
2. Append the new member as `std::optional<T>`, last, with its bounds.
3. `static_assert(psio::pssz_evolves_from<Old, New>)`.
4. Assert the width did not change.
5. Confirm by hand that no existing bound, `utf8`, invariant, enum value
   or JSON form changed (the check does not see them).
6. Test: new-with-`None` bytes equal old bytes; old bytes decode as new;
   new bytes decode as old.

## Checklist: reviewing a diff

- [ ] No member type was changed just to carry a bound.
- [ ] Every new variable-size member is bounded at every level, or
      justified as unbounded.
- [ ] Width is asserted; it is the same as before for a changed type.
- [ ] New members of a published type are optional and last.
- [ ] No published bound, `utf8`, cap class, DWNC, enum value, variant
      alternative list or member order changed.
- [ ] Untrusted input goes through a validating entry point.
- [ ] No reliance on `sorted`, `maxFields`, `padding`, `skip_spec`,
      `default_value_spec`, `since_spec`, `final_v`, `canonical`, `flags`
      (nothing enforces or reads them —
      [5.13](annotations.md#513-sorted-unique-unique_keys),
      [5.15](annotations.md#515-reserved-vocabulary)).
- [ ] Rust / TypeScript mirrors updated with the same bounds and cap.

## Anti-patterns

| Anti-pattern | Do instead |
|---|---|
| `psio::bounded<std::string, 8> tags;` introduced to "add a limit" | `std::vector<std::string> tags;` + `attr(tags, max<8> \| element(utf8 \| max<16>))` |
| `attr(tags, max<8>)` on a `std::vector<std::string>` and calling it bounded | add `element(max<N>)`; assert `max_encoded_size<T>().has_value()` |
| Publishing an uncapped record that will obviously grow | `maxDynamicData(N)` from the first version |
| Adding a required field, or an optional field in the middle | optional, last |
| "Fixing" a too-small bound in place after publication | new optional field or a new type; bounds are frozen |
| `attr(code, min<2> \| max<8>)` | `attr(code, psio::length_bound{.max = 8, .min = 2})` |
| `attr(count, max<100>)` on an integer to limit its value | an `invariant(fn)`; `max<N>` is a length bound |
| `psio::pssz_view<T>{untrusted_bytes}` | `psio::validated_pssz_view(...)` or `validate_pssz` first |
| `from_pssz<T>(psio::trusted{action_args})` | `from_pssz<T>(action_args)` |
| Relying on `sorted` for an ordered list | `std::set` / `std::map`, or an invariant |
| `std::string` for a hash or signature | `std::array<std::uint8_t, N>` / `std::vector<std::uint8_t>` + `max<N>` |
| An invariant that throws or calls a throwing parser | catch inside; return `psio::codec_fail(...)` |
| Renumbering an enum, changing its underlying type | append enumerators only |
| A different `maxDynamicData` per language | the same N everywhere |

## Snippets to paste

Width and bound of a type without a cap (`tut-2b`):

```cpp
static_assert(psio::max_encoded_size<Account>() == 4 + 8 + (4 + 32) + (4 + 8 * (16 + 4)) + 8);
static_assert(psio::max_encoded_size<Account>() == 220);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);
```

Width of a capped type (`tut-3`):

```cpp
static_assert(psio::max_dynamic_data_v<AccountV1> == 1024);
static_assert(psio::auto_pssz_width_v<AccountV1> == 2);
```

"Still unbounded" is a fact worth asserting too (`tut-2c`):

```cpp
static_assert(!psio::max_encoded_size<AccountCountOnly>().has_value());
static_assert(psio::auto_pssz_width_v<AccountCountOnly> == 4);
```

Compatibility of a revision (`tut-4a`):

```cpp
static_assert(psio::auto_pssz_width_v<AccountV2> == 2);
static_assert(psio::pssz_evolves_from<AccountV1, AccountV2>);
```

Method arguments (`tut-9a`, `tut-9b`):

```cpp
using OpenArgs     = psio::reflect<Bank>::method_arg_tuple<0>;
using TransferArgs = psio::reflect<Bank>::method_arg_tuple<1>;
// open has no cap: header 4 + owner (4 + 32) + tags (4 + 8 x (16 + 4)) = 204, 8-bit.
static_assert(psio::max_encoded_size<OpenArgs>() == 4 + (4 + 32) + (4 + 8 * (16 + 4)));
static_assert(psio::auto_pssz_width_v<OpenArgs> == 1);
// transfer is capped: 16-bit for its lifetime.
static_assert(psio::max_dynamic_data_v<TransferArgs> == 1024);
static_assert(psio::auto_pssz_width_v<TransferArgs> == 2);
```

```cpp
using TransferArgsV2 = psio::reflect<BankV2>::method_arg_tuple<0>;
static_assert(psio::pssz_evolves_from<TransferArgs, TransferArgsV2>);
```

Round trip and rejection at bound + 1 (`tut-2-run`; Catch2):

```cpp
const Account alice{7, "alice", {"vip", "beta"}, 100};
const std::vector<char> bytes = psio::to_pssz(alice);
CHECK(psio::from_pssz<Account>(bytes) == alice);          // validate, then decode

Account long_owner = alice;
long_owner.owner   = std::string(33, 'x');                // one byte past max<32>
CHECK_THROWS_AS(psio::to_pssz(long_owner), psio::codec_exception);

Account nine_tags = alice;
nine_tags.tags    = std::vector<std::string>(9, "t");     // one element past max<8>
CHECK_THROWS_AS(psio::to_pssz(nine_tags), psio::codec_exception);

Account not_text = alice;
not_text.owner   = "\xff";                                // not UTF-8
CHECK_THROWS_AS(psio::to_pssz(not_text), psio::codec_exception);
```

Both directions of an evolution (`tut-4-run`):

```cpp
const AccountV1 v1{7, "alice", {"vip"}, 100};
const AccountV2 v2_none{7, "alice", {"vip"}, 100, std::nullopt};
const AccountV2 v2_some{7, "alice", {"vip"}, 100, "alice@example.com"};

// A new writer that leaves the field out writes the old bytes: trailing
// absent optionals are pruned.
CHECK(psio::to_pssz(v2_none) == psio::to_pssz(v1));

// Old bytes read as the new type: the field is absent.
CHECK(psio::from_pssz<AccountV2>(psio::to_pssz(v1)) == v2_none);

// New bytes read as the old type: the unknown field is skipped ...
CHECK(psio::from_pssz<AccountV1>(psio::to_pssz(v2_some)) == v1);
// ... and a reader that wants to know is told.
AccountV1 out;
bool      has_unknown = false;
CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>(psio::to_pssz(v2_some)), out,
                                has_unknown)
         .ok());
CHECK(has_unknown);
// The exact-schema check refuses unknown fields.
CHECK_FALSE(psio::validate_strict<AccountV1>(psio::pssz{},
                                             std::span<const char>(psio::to_pssz(v2_some)))
               .ok());
```

Validated read, view, trusted read (`tut-11-run`):

```cpp
const AccountV2 alice{7, "alice", {"vip", "beta"}, 100, "alice@example.com"};
const std::vector<char> bytes = psio::to_pssz(alice);

// Untrusted bytes: validate first. validate_pssz never throws.
REQUIRE(psio::validate_pssz<AccountV2>(bytes).ok());

// A view reads in place: nothing is copied or allocated.
const psio::pssz_view<AccountV2> view{bytes};
CHECK(view.get<0>() == 7u);
CHECK(view.get<1>().view_() == "alice");
CHECK(view.get<2>().size() == 2);
CHECK(view.get<2>()[1].view_() == "beta");

// from_pssz<T>(bytes) validates and decodes; trusted skips the validation
// pass for bytes this program already validated or wrote itself.
CHECK(psio::from_pssz<AccountV2>(bytes) == alice);
CHECK(psio::from_pssz<AccountV2>(psio::trusted{bytes}) == alice);
```

How the inferred bound adds up, for writing the expected number in an
assertion ([annotations.md §3](annotations.md#3-width-what-a-bound-buys)):
4 per extensible record / tuple header; a fixed-size member is its size; a
variable-size member is 4 (slot) + its bound; a list of N variable-size
elements is N × (element bound + 4) **[W1]**; an optional adds 1; a
variant is 1 + its largest alternative; a member whose type has its own
cap contributes the cap.

## Limits to state honestly

- The C++ key encoder does not check member bounds or `utf8` **[W3]**.
- Views are not validated on construction.
- `pssz_evolves_from` does not compare bounds, `utf8`, enumerators or
  invariants.
- `maxFields`, `sorted` / `unique` and the reserved vocabulary are not
  enforced.
- `psio::validate_json` applies a root type's `maxDynamicData` to the text
  size; the JSON writer does not.
- A member whose annotations narrow a nested tuple / pair / map entry /
  array of variable elements cannot be read through a `pssz_view`.
- Rust has no `exact` / `min` / `maxFields`; TypeScript has no
  `pssz_evolves_from` and no return-value annotation.
