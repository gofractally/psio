# GraphQL

`psio::graphql` (C++, `#include <psio/graphql.hpp>`) serves GraphQL queries
from reflected C++ types. The schema is derived from `PSIO_REFLECT`
(fields and methods); a query is parsed, validated against that schema and
turned into a reusable plan; the plan is executed against a C++ value or
directly against encoded bytes through a psio view. Scalars are written and
read by the I-JSON codec (`docs/json.md`), so a value has one text form in
JSON and in GraphQL.

Conformance target: the GraphQL October 2021 specification. What is not
implemented is listed under [Not supported](#not-supported).

```cpp
struct Account { std::uint64_t id; std::string name; std::optional<std::string> memo; };
PSIO_REFLECT(Account, id, name, memo)

struct Query {
   const Db* db;
   std::optional<Account> account(std::uint64_t id) const;            // account(id: UInt64!): Account
   auto accounts(psio::graphql::page_args page) const                  // accounts(first: Int, last: Int, before: String,
   {                                                                   //          after: String): AccountConnection
      return psio::graphql::paginate(db->accounts_by_id(), page);
   }
};
PSIO_REFLECT(Query, attr(db, psio::graphql::hidden), method(account, id), method(accounts, page))

using api = psio::graphql::schema<Query>;
std::string out;
api::run(Query{&db}, R"({ accounts(first: 10) { edges { node { id name } } pageInfo { endCursor } } })", out);
```

## Three phases

| Phase | Header | Input → output |
|---|---|---|
| parse | `graphql/parser.hpp` | query text → syntax tree (flat vectors, 32-bit links, names as views of the text, a byte offset per node) |
| validate + plan | `graphql/plan.hpp` | syntax tree + schema → `document` (the plan), or errors with line and column |
| execute | `graphql/execute.hpp` | plan + root value + variables → response text |

`schema<Query, Mutation = void, Introspection = true>` (`graphql.hpp`) ties
them together:

```cpp
static bool        compile(std::string_view text, document& plan, std::vector<error>& errors, const limits& = {});
static std::size_t execute(const document& plan, const Query& root, std::string& out, const request& = {}, const limits& = {});
static std::size_t execute(const document& plan, const Query& root, Mutation& mutation, std::string& out, ...);
static std::size_t run(const Query& root, std::string_view text, std::string& out, const request& = {}, const limits& = {});
static std::string sdl();
static constexpr schema_desc desc;   // for plan_cache and the free functions
```

`execute` and `run` append the response to `out` and return the number of
errors in it. `request` carries `operation_name`, `variables` (the text of a
JSON object) and an optional per-request object (`request::user(obj)`).
`write_error_response(errors, out)` writes `{"errors":[...]}` for a document
that did not compile.

**The plan.** Validation produces the document with everything resolved, so
that execution compares no names and never sees the query text:

- fragments are expanded and fields with one response key merged
  (CollectFields), per concrete object type — a field of union type has one
  selection set per member, selected by the member's index;
- a planned field is: its response key as ready JSON text (`,"key":`), the
  index of its resolver in the object type's resolver table, its argument
  values in the order of the resolver's parameters, its child selection set;
- input-object member names and enum values in arguments are resolved to
  indices; variables are slots;
- `@skip` / `@include` with literal arguments are decided; those with
  variables become guards evaluated per request.

A plan holds every operation of its document, refers to neither the query
text nor a root value, and is immutable: reuse it for any number of requests,
roots and variable sets, from several threads. `plan_cache` is an optional
fixed-capacity LRU keyed by query text
(`cache.get(api::desc, text, errors) -> const document*`); it is not
synchronised.

**Response.** Written straight into the caller's `std::string` — there is no
intermediate tree. Shape: `{"data":…}` followed by `,"errors":[…]` when there
are errors; `{"errors":[…]}` when the request failed before execution. An
error has `message`, `locations` (line, column) and, for a field error,
`path`. A failed field becomes `null`; if its type is non-null the nearest
nullable parent becomes `null` (the output written for it is discarded),
up to `"data":null`.

## Type mapping

No per-type declarations: the GraphQL type of a C++ type follows from its
shape.

| C++ | GraphQL |
|---|---|
| reflected record | Object. Fields: the reflected data members, then the reflected `const` (or static) methods. A method with parameters is a field with arguments named by the reflected parameter names (`method(find, id, limit)`; every parameter must be named). A `const` method returning `void` is not a field. A member annotated `psio::graphql::flatten` is replaced by its own fields ([Flattened members](#flattened-members)) |
| reflected record without members or methods (`struct TLSInfo { PSIO_REFLECT(TLSInfo) };`) | Object with the one field `_: Boolean`, always `true` ([Records without members](#records-without-members)) |
| instantiation of a reflected class template, `Box<A>` | Object named `Box_A` ([Class templates](#class-templates)) |
| the same record as a parameter | Input object named `<Name>Input` (unchanged if the name already ends in `Input`); must be default-constructible. A record marked `psio::graphql::arguments` is not an input object but one argument per member ([Argument groups](#argument-groups)) |
| `std::optional<T>`, `T*`, `unique_ptr`, `shared_ptr` | nullable `T`; every other type is non-null |
| `psio::graphql::result<T>` (or any type with `has_value()`, `*`, `error()`) | nullable `T`; an error becomes a field error |
| `std::vector`, `std::array`, `std::span`, any type with `begin()`/`end()` | List. The range is iterated once, lazily: a method can return a generator-like range and nothing is materialised |
| `std::variant<A, B, …>` of reflected records | Union named `A_or_B_…` |
| enum with `PSIO_REFLECT_ENUM` | Enum of the listed names (a value without a name is a field error). An enum without names is its integer |
| `std::tuple` of one element type | List |
| any other `std::tuple` | Object `Tuple_<A>_<B>…` with fields `_0`, `_1`, … |
| `std::pair<A, B>` | the tuple `std::tuple<A, B>`: a List when `A` and `B` are one type, else the Object `Tuple_<A>_<B>` |
| `std::set`, `std::unordered_set` | List of the elements, in the container's iteration order |
| `std::map`, `std::unordered_map` | List of its entries, each the pair `(key, value)`: `[Tuple_<K>_<V>!]!` with fields `_0` (key) and `_1` (value). Through a `psio::view` the entries come in wire (ascending key) order |
| `bool` | `Boolean` |
| integers of at most 32 bits | `Int` |
| `float`, `double` | `Float` |
| `std::string`, `utf8_string<N>`, `std::string_view` (output only) | `String` |
| member annotated `psio::graphql::id` (string or integer) | `ID` (always written as a string) |
| 64-bit integers | `Int64`, `UInt64` — a number up to 2^53 − 1, a string beyond |
| `psio::uint128`, `int128`, `uint256` | `UInt128`, `Int128`, `UInt256` — decimal strings |
| byte sequences (`vector`/`array` of `char`/`unsigned char`/`std::byte`, `byte_array<N>`) | `Bytes` — hex string |
| `system_clock` time point | `DateTimeSeconds`, `DateTimeMilliseconds`, `DateTimeMicroseconds`, `DateTimeNanoseconds`, or `DateTime_<num>_<den>`, by the unit of its ticks — RFC 3339 UTC |
| `std::chrono::duration` | `Seconds`, `Milliseconds`, `Microseconds`, `Nanoseconds`, or `Duration_<num>_<den>` — the tick count |
| time point of another clock (monotonic) | `MonotonicSeconds`, `MonotonicMilliseconds`, `MonotonicMicroseconds`, `MonotonicNanoseconds`, or `Monotonic_<num>_<den>` — the tick count |
| any of the three with a tick integer that is not a signed 64-bit one | the name above followed by `_i32`, `_u32`, `_u64`, …: `Seconds_u32`, `DateTimeMilliseconds_i32` |
| type with a text adapter (`PSIO_ADAPTER(T, psio::text_category, …)`), of any shape — record, enum, transparent wrapper, `std::variant`, `std::tuple` | scalar named after the type (the adapter's `schema_id`, else the reflected name) — a string holding the adapter's text |
| transparent type (`PSIO_REFLECT(T, transparent(), member)`) over a scalar | scalar named after the type (its reflected name), with the member's text: `Id` over a 64-bit integer reads and writes as `UInt64` does, a wrapped byte list as `Bytes` does. A transparent type over a record, a list or a variant has no mapping without a text adapter (a compile error): expose its member |
| `psio::view<T, Fmt, Store>`, `psio::buffer<T, Fmt, Store>`, `std::reference_wrapper<T>` | `T` (see [Views](#views)). The same record reached through different sources is one GraphQL type |
| `psio::nested<T>` (`psio/nested.hpp`), as a value or through a view | `T`: the inner value, read in place through the view over the nested bytes. Output only |
| `psio::graphql::connection<Node>` | `<Node>Connection`, `<Node>Edge`, `PageInfo` (see [Connections](#connections)) |
| a method result `R` with a `psio::graphql::resolver<R>` | what `resolver<R>::resolve` returns; the field gains `resolver<R>::arguments` ([Results with arguments of their own](#results-with-arguments-of-their-own)) |

A type invariant (`docs/pssz-spec.md` §18) holds here as in `psio::ijson`: an
argument or variable that violates it is a validation error, a result that
violates it a field error ("Cannot represent value: ...").

Scalars are written by `psio::ijson`'s writer and arguments and variables
are read by its typed reader, including the custom scalars: `UInt64` accepts
`7` and `"18446744073709551615"`, a `DateTime…` scalar any RFC 3339
date-time its unit can hold,
`Bytes` hex of either case. `Int` arguments are checked against the C++
parameter (an `std::int8_t` parameter rejects `300` at validation).

A time scalar is named per unit (and per tick integer) because the value a
name admits depends on both: `DateTimeSeconds` refuses a fractional second
that `DateTimeMilliseconds` accepts, and `Seconds_u32` refuses a negative
count. These are the names Rust's `async-graphql` scalars have
(`psio::graphql_scalars`), so one schema describes a service written in
either language.

Decisions worth knowing:

- **`Int` covers `std::uint32_t`**, as the mapping rule says, although the
  spec's `Int` is signed 32-bit: values above 2^31 − 1 are written and
  accepted. Use a 64-bit member where a strict client matters.
- **A homogeneous tuple is a List, a mixed one an Object with positional
  fields** (`_0`, `_1`): a List cannot hold mixed types, and I-JSON's array
  form has no counterpart in GraphQL's type system.
- **Fields keep their C++ names** (no case conversion). Rename with
  `psio::graphql::name_as`.
- **Arguments have no default values** (C++ parameters have none in
  reflection): an optional parameter is a nullable argument that reads as
  empty when absent. Introspection therefore shows
  `fields(includeDeprecated: Boolean)` without `= false`.

### Customisation

Annotations (`annotate.hpp`), on a type or a member (a data member or a
method):

```cpp
PSIO_TYPE_ATTRS(Account, psio::graphql::name_as{"User"} | psio::graphql::doc{"A user account."})
PSIO_FIELD_ATTRS(Account, password_hash, psio::graphql::hidden)
PSIO_FIELD_ATTRS(Account, id,            psio::graphql::id)
PSIO_FIELD_ATTRS(Account, legacy_name,   psio::graphql::name_as{"name"} | psio::graphql::deprecated{"use handle"})
PSIO_FIELD_ATTRS(Query,   search,        psio::graphql::cost{50} | psio::graphql::doc{"Full-text search."})
```

`attr(member, spec)` inside `PSIO_REFLECT` does the same for a data member.
`psio::deprecated_spec` on a member also marks it deprecated. A type that
cannot carry an annotation (a `std::variant`, a class template) is named by
specialising `psio::graphql::type_name<T>` (`input_type_name<T>` for its
input-object name) with a `static constexpr std::string_view value`.

Descriptions (`doc`) and deprecation appear in introspection and SDL.
Enum values and the parameters of a method have no place for an annotation
and so no description; a member of an argument group has (its `doc` is the
argument's description).

### Records without members

GraphQL has no object type without fields, and a selection set cannot be
empty. A reflected record with no members and no methods — a marker such as
`struct TLSInfo { PSIO_REFLECT(TLSInfo) };`, an alternative of a union that
carries no data — is the object type

```graphql
type TLSInfo { _: Boolean }
```

whose field `_` is always `true`. This is the placeholder SDL-first servers
write for a type that has no field yet (`_: Boolean` in graphql-tools and
Apollo schemas); the type keeps its name, so `__typename` and `... on
TLSInfo` work, an `std::optional<TLSInfo>` is still `null` or an object, and
a client that only wants presence selects `tls { _ }` or
`tls { __typename }`. The same holds for a query root without members. A
record whose members are all *hidden* is still a compile error (that is a
mistake, not a marker), and so is an empty record as an input object.

### Class templates

`PSIO_REFLECT` gives every instantiation of a class template the template's
own name, so two instantiations in one schema would be two types with one
name. The GraphQL name of an instantiation whose template arguments are all
types is therefore

```
<Template>_<Arg>_<Arg>...        Box<Account> -> Box_Account     Pair<A, B> -> Pair_A_B
```

where each argument is written as in a tuple's or a union's name: its
GraphQL name (`Int`, `String`, `Account`, `Box_Account`), `Optional<T>` for
an optional, `<T>List` for a list (`Box<std::vector<Box<int>>>` is
`Box_Box_IntList`). Its input object is `<that>Input`. The name depends only
on the type, not on the schema or on which instantiations appear, so it is
the same in every build. An argument that has no GraphQL name (a tag type)
is a compile error asking for a `type_name` specialisation.

To choose the name, specialise `type_name` for the template — the existing
mechanism; a partial specialisation covers all instantiations:

```cpp
// by a string that is a template argument
template <typename Node, psio::fixed_string Name>
struct psio::graphql::type_name<Page<Node, Name>> { static constexpr std::string_view value = Name.view(); };

// composed from the argument's GraphQL name: Account -> AccountWithBlockContext
template <typename T>
struct psio::graphql::type_name<WithBlockContext<T>> {
   static constexpr std::string_view value = psio::graphql::affixed_name<"", T, "WithBlockContext">;
};
```

`affixed_name<Prefix, T, Suffix>` is `Prefix` + the GraphQL name of `T` +
`Suffix`. A template with non-type arguments (`Page<Node, "AccountPage">`)
cannot be taken apart by the library: without a `type_name` every
instantiation has the template's name, which is a compile error as soon as
two that differ meet in a schema. (Two C++ types with one name and the same
fields are one GraphQL type; that is how a record reached through a value
and through a view is one type.)

`psio::graphql::connection<Node>` and `edge<Node>` are named
`<Node>Connection` and `<Node>Edge` by such specialisations; specialise
`type_name<connection<X>>` to rename one.

### Flattened members

`psio::graphql::flatten` on a data member puts the member's fields in the
enclosing object type, in the member's place:

```cpp
template <typename T>
struct WithBlockContext {
   EventBlockInfo block;
   T              data;
   PSIO_REFLECT(WithBlockContext, block, attr(data, psio::graphql::flatten))
};
// type InviteEventWithBlockContext { block: EventBlockInfo!  inviteId: Int!  actor: AccountNumber!  event: String! }
```

- The member must be a reflected record held by value (not an optional, a
  list or a variant: those have no fields to lift). Its data members and its
  methods (with their arguments) become fields; a member it hides stays
  hidden; flattening nests (a flattened member of a flattened member).
- **Two fields with one name are a compile error** ("two members of an
  object type have the same GraphQL name"): rename one with `name_as` or
  hide it.
- The member itself is not a field (`data { ... }` is unknown), and its type
  appears in the schema only if something else refers to it.
- Resolution costs one extra indirect call per flattened field; through a
  view the member is the sub-view, so only selected fields are read.
- **GraphQL only.** The record's encodings — PSSZ, JSON, the key — and its
  schema (`psio::schema`) are unchanged: `data` remains a nested member
  there. As an *input* object the type also keeps `data` as a nested input
  object. In Rust the same shape is `#[graphql(flatten)]` on the field of an
  `async-graphql` `SimpleObject`, which is likewise GraphQL-only; serde's
  `#[serde(flatten)]` (a JSON-level flatten) has no psio counterpart and the
  psio derives reject it, so the JSON of the two languages stays the same
  nested form.

### Compile-time errors

Rejected with a `static_assert` that names the reason
(`cpp/tests/graphql_schema_fail.cpp`): two C++ types of one schema with the
same GraphQL name (including `Foo`'s input object against a type named
`FooInput`), two members of a type with the same name, a member whose type
has no mapping, a `std::variant` alternative that is not a record, a method
with unnamed parameters, an unsupported argument type, an optional of an
optional, `id` on a non-scalar, an object type whose members are all hidden,
`flatten` on a member that is not a reflected record, two arguments of a
field with one name (a parameter and a member of an argument group), a
template argument without a GraphQL name, `page_budget` with a source that
has no `row_bytes()`.
A `psio::nested<T, nested_validation::deferred>` member is a type with no
mapping (an enclosing reader has not checked that its bytes are a `T`: hide
it, or expose the `T` from a method after validating), and a nested value
is not an argument type (`cpp/tests/nested_fail.cpp`).

The list of a schema's types is computed at compile time by walking the
descriptors. A very large schema can exceed the compiler's constant
evaluation budget; raise it with `-fconstexpr-steps=`.

## Views

The executor is written against a *source*: a C++ value, or a psio view over
encoded bytes. The two differ in four accessors:

| | value | view |
|---|---|---|
| record field `I` | `obj.*member` | `view.get<I>()` — a scalar by value, or a sub-view |
| optional | `has_value()`, `*o` | `has_value()`, `*v` |
| list | range-for | range-for (elements are views) |
| variant | `index()`, `get_if<I>` | `index()`, `visit` |

So when a field or method yields `psio::view<T, psio::pssz, …>` or
`psio::buffer<T, psio::pssz, …>` — a database row, say — the engine reads
the selected fields straight from the buffer:

- unselected members are never looked at;
- strings are escaped, and byte sequences hex-encoded, from the buffer into
  the response, with no intermediate copy;
- nested records and lists stay sub-views;
- record evolution comes from the view: a trailing optional the writer of an
  older row did not know reads as `null`.

A query gives byte-identical responses over values and over views of the
same data (`cpp/tests/graphql_tests.cpp`, "values and PSSZ views").

Limits of the view path:

- **Views are over validated bytes**, as everywhere in psio
  (`validated_pssz_view`, or bytes the program wrote itself). The engine
  does not re-validate. The same holds for a `psio::nested<T>` value held in
  memory: one built with `nested<T>::unchecked` must hold a valid encoding.
- **A method needs the object.** Selecting a reflected *method* of a
  view-backed record decodes that one record (`psio::decode`) for the call,
  once per selected method. Data members never do.
- **A leaf with a wire form of its own and a text adapter** (an
  account-name-like record) is decoded as that one value to produce its
  text.
- The view must exist for the format: PSSZ (`pssz_view.hpp`) is what is
  tested. PSSZ does not encode `psio::bounded`, `byte_array` or
  `std::vector<bool>`; those types work as values only (`std::vector<bool>`
  not at all).

## Methods

A reflected method becomes a field. Its parameters are decoded from the
plan and the request's variables, in declaration order:

- scalars, enums, `std::optional`, `std::vector` and reflected records
  (input objects) of those;
- an *argument group*: one argument per member (below);
- a parameter of type `psio::graphql::context&` (or `const context&`)
  receives the request context and is not an argument. `context::user<T>()`
  returns the object attached with `request::user(obj)` if it is a `T`, else
  `nullptr`.

It may return a value, a reference, a view or buffer of encoded data, an
optional or pointer, a range of any of those, or `result<T>` /
`psio::graphql::fail("message")` for a field error.

### Argument groups

A reflected record marked as a group of arguments is, as a parameter, not
an input object: each of its members is an argument of the field, named by
the member, in the member's place among the arguments.

```cpp
struct Paging { std::optional<std::int32_t> first; std::optional<std::string> after; };
PSIO_REFLECT(Paging, first, attr(after, psio::graphql::doc{"Where to start."}))
PSIO_TYPE_ATTRS(Paging, psio::graphql::arguments)

std::vector<Row> rows(std::string prefix, Paging page) const;     // method(rows, prefix, page)
// rows(prefix: String!, first: Int, after: String): [Row!]!
```

A class template cannot be annotated; specialise the trait instead:
`template <typename K> inline constexpr bool
psio::graphql::argument_group<Range<K>> = true;`.

- Each member is typed as a parameter of that type would be (an optional
  member is a nullable argument, any other a required one); `name_as`,
  `hidden` (not an argument; keeps its default) and `doc` apply.
- A member that is itself a group is taken apart the same way, so groups
  compose (`struct IndexArgs { key_range<K> range; page_args page; }`).
- The parameter's own name is not used. Two arguments of one field with the
  same name are a compile error.
- A group must be default-constructible. Only a parameter (or a group's
  member) of exactly the group type is taken apart: `std::optional<Paging>`
  and `std::vector<Paging>` are an input object `PagingInput`.
- The plan is the same as for separate parameters — one value per argument
  — so variables, validation, introspection and plan reuse see ordinary
  arguments.

`psio::graphql::page_args` (`first`, `last`, `before`, `after`) and
`psio::graphql::key_range<K>` (`gt`, `ge`, `lt`, `le`) are groups
([Connections](#connections)).

### Results with arguments of their own

`psio::graphql::resolver<R>` lets a *result type* bring the arguments that
complete it. A method that returns an `R` is then a field with the method's
arguments followed by those of `resolver<R>::arguments` (an argument
group), and its value — and so its GraphQL type — is what `resolve`
returns:

```cpp
template <typename Key>
struct IndexArgs { psio::graphql::key_range<Key> range; psio::graphql::page_args page;
                   PSIO_REFLECT(IndexArgs, range, page) };
template <typename Key>
inline constexpr bool psio::graphql::argument_group<IndexArgs<Key>> = true;

template <typename Row, typename Key>
struct psio::graphql::resolver<TableIndex<Row, Key>> {
   using arguments = IndexArgs<Key>;
   static auto resolve(TableIndex<Row, Key>&& index, const arguments& a)   // optionally a third: context&
   { return psio::graphql::paginate(IndexView{std::move(index)}, a.range, a.page); }
};

auto users() const { return table.getIndex<0>(); }   // method(users)
// users(gt: K, ge: K, lt: K, le: K, first: Int, last: Int, before: String, after: String): UserConnection
```

Written once by the embedder for its index type, this makes every method
that returns an index a connection field, with nothing in the service code.
It applies to method results only (a data member of such a type is mapped
as its type is).

**Mutations.** `schema<Query, Mutation>`: every reflected method of
`Mutation` (const or not) is a field of the mutation type; a `void` method
is `Boolean!` and yields `true`. The top-level fields of a mutation run one
after another in document order, on the `Mutation&` passed to `execute`.
Subscriptions are rejected at validation.

## Connections

`graphql/connection.hpp` implements Relay cursor connections (the GraphQL
Cursor Connections Specification) over rows the embedder supplies, ordered
by opaque key bytes. There is no database code in psio.

```cpp
struct Query {
   // accounts(gt: AccountNumber, ge: AccountNumber, lt: AccountNumber, le: AccountNumber,
   //          first: Int, last: Int, before: String, after: String): AccountConnection
   auto accounts(psio::graphql::key_range<AccountNumber> range, psio::graphql::page_args page) const
   { return psio::graphql::paginate(accounts_index(), range, page); }
};
PSIO_REFLECT(Query, method(accounts, range, page))
```

```graphql
type AccountConnection { edges: [AccountEdge!]!  pageInfo: PageInfo! }
type AccountEdge       { node: Account!  cursor: String! }
type PageInfo          { hasNextPage: Boolean!  hasPreviousPage: Boolean!  startCursor: String  endCursor: String }
```

### The source

The embedder supplies one of two things; `paginate` takes either.

**An ordered index** — what a database index has: positions from `begin()`
to `end()`, found by key bytes.

```cpp
template <typename X>
concept ordered_index = requires(const X& x, std::span<const char> key, /* position */ it) {
   x.begin();  x.end();           // bidirectional positions: ++it, --it, it == it
   x.lower_bound(key);            // first position with key >= `key`, else end()
   x.upper_bound(key);            // first position with key >  `key`, else end()
   x.key(it);                     // the key bytes there: a span, or an owning container
   x.row(it);                     // the row there: a value, or a view / buffer of an encoded row
   // optional: x.row_bytes(it)   // its size, for a byte budget
};
```

An index over part of a table (the rows under a key prefix) answers within
its part: its bounds never leave `[begin(), end())`, and its keys are the
bytes after the prefix. The index is held by value when it is passed as an
rvalue and by reference when it is an lvalue, so make it a cheap handle.

**A row source** — a cursor with seek and step, for a store that is not
iterator-shaped. `index_source<X>` is the row source over an ordered index;
`paginate` works on this form.

```cpp
template <typename S>
concept row_source = requires(S& s, std::span<const char> key) {
   { s.seek_first() } -> bool;        { s.seek_last() } -> bool;
   { s.seek_after(key) } -> bool;     // first row with key > `key`
   { s.seek_before(key) } -> bool;    // last row with key < `key`
   { s.next() } -> bool;              { s.prev() } -> bool;
   { s.key() } -> std::span<const char>;
   s.row();
   // optional: s.seek_at_or_after(key) -> bool   (else: seek_before + next)
   // optional: s.row_bytes() -> std::size_t      (for a byte budget)
};
```

`psio::graphql::transform(source, fn)` is the source whose rows are
`fn(row)` — a stored row shown as another record. Keys and cursors are the
underlying source's. When the rows are views and are not transformed, only
the selected fields of each node are read from the encoded row.

The key bytes are whatever orders the rows. For a database index they are
the `psio::key` encoding of the index key, which sorts bytewise as the
values do (`cpp/tests/graphql_service_fixture.hpp` has such an index over a
`std::map`: `Index<Row, Key>`).

### Pages, ranges, budgets

```cpp
paginate(source, page)                 // all rows
paginate(source, range, page)          // the rows in a key_range<K>
paginate(source, bounds, page)         // the rows in key_bounds (bytes)
   // each with an optional last argument:
   //   page_limits  — a number: the largest page (default 100)
   //   page_budget{.max_bytes = N, .max_page_size = 100}
-> result<connection<Node>>            // Node: what source.row() yields
```

**`page_args`** — `first`, `last` (`Int`), `before`, `after` (`String`).
`first` pages forward from `after`, `last` backward from `before`; with
neither, the first `max_page_size` rows. `before` and `after` together bound
a window. `first` with `last`, a negative size and a malformed cursor are
field errors. A page never has more than `max_page_size` edges, whatever
`first` says.

**`pageInfo`** follows the Relay algorithm. Paging forward, `hasNextPage`
is true when the range has a row beyond the page — one that `first`, the
page-size cap or the byte budget left out, or one at or after `before` —
and `hasPreviousPage` when `after` is given and the range has a row before
the page. Paging backward, the mirror image. `startCursor` / `endCursor`
are `null` for a page without edges.

**Cursors** are the key bytes of the row in **hex** (`String`): a cursor is
the row's position in the index, so it stays valid when rows are inserted or
removed around it, and one index needs no cursor state. Hex rather than
base64 because the engine writes it from the key with no intermediate
buffer, it reads in either case, it is safe in a URL and in JSON without
escaping, and it is the form psibase's cursors already have; base64 would
save a third of the cursor's length and nothing else. Clients must treat it
as opaque.

**`key_range<K>`** — `gt`, `ge`, `lt`, `le`, each an optional `K`, the
arguments typed by the key (`gt: AccountNumber`). The bounds are encoded
with `psio::key` (`to_bounds(range) -> result<key_bounds>`) and combined
with the cursors: a page shows rows inside the range *and* between the
cursors, and a cursor outside the range does not widen it. `K` may be a
leading part of the index key — the first element of a composite key
`(owner, id)`: `ge` / `lt` then bound by that element, `gt` skips and `le`
includes every row that starts with it (`key_bounds::beyond` / `through`
use `psio::key_prefix_upper_bound`). For a whole key the two readings
coincide: no key encoding is a prefix of another's. A bound that has no key
(a string that is not UTF-8) is a field error. A service that computes its
range builds the `key_range` (`{.ge = start}`) or the `key_bounds` itself.

**`page_budget`** — a page's rows together have at most `max_bytes` bytes,
as the source counts them (`row_bytes()`: the stored size of the row; a
compile error if the source has none). A page that has rows to show always
has at least one edge, however large the row, and a page cut short says so
in `pageInfo`, so a client that follows `endCursor` sees every row. The
budget is on what the page *reads*, not on the response text: `paginate`
returns before a byte of the response is written, and what the response
holds of a row depends on the selection. `limits::max_response_bytes`
remains the bound on the response.

`paginate` reads one page — its rows, plus one step on each side for
`pageInfo` — whatever the size of the index. A paged field's arguments
named `first` / `last` (of type `Int`, whether parameters or members of a
group) are its page size for the complexity limit.

**A connection built by hand** (rows of a query in another system):
`connection_builder<Node>` — `add(node, cursor_bytes)`, then
`std::move(b).finish(has_previous_page, has_next_page)`. The cursor bytes
are what the client hands back, in hex.

## Introspection and SDL

`__schema`, `__type(name:)` and `__typename` are implemented in full; the
introspection types are ordinary reflected records over the schema's
descriptors (`graphql/introspection.hpp`), resolved by the same executor.
The query GraphiQL and code generators send (graphql-js
`getIntrospectionQuery`) runs within the default limits. `schema::sdl()`
prints the schema; the tests rebuild the SDL from an introspection response
and require it to equal `sdl()`.

`schema<Query, Mutation, false>` builds a schema without introspection:
`__schema` / `__type` are then unknown fields and none of their code is
instantiated.

## Limits

`psio::graphql::limits`, passed to `compile` and `execute` (use the same
values for both). All are enforced before execution starts, except the two
response limits.

| Limit | Default | Enforced |
|---|---:|---|
| `max_document_bytes` | 64 KiB | parse |
| `max_tokens` | 16 384 | parse |
| `max_depth` — selection-set nesting, after fragment expansion; also the nesting of fragment spreads | 32 | parse and validation |
| `max_fields` — selections after fragment expansion (a fragment spread N times counts N times), whole document | 8 192 | validation |
| `max_input_depth` — lists / input objects in argument and variable values | 16 | parse, variable coercion |
| `max_variables_bytes` | 64 KiB | variable coercion |
| `max_errors` — errors reported | 16 | all phases |
| `max_complexity` | 100 000 | before execution, per request |
| `default_list_size` — assumed length of a list without a page-size argument | 10 | (complexity) |
| `max_response_fields` — fields and list elements written | 1 000 000 | execution |
| `max_response_bytes` | 16 MiB | execution |

*Complexity* is the sum over selected fields of the field's cost
(`psio::graphql::cost`, default 1), where the cost of the selection below a
list field is multiplied by its page size: the `first` / `last` argument when
the field has one and it is given (a literal or a variable), else
`default_list_size`; an element of a list of scalars costs 1.

Alias and fragment bombs are bounded by `max_fields` (every selection
visited during expansion counts, whether or not it yields a field); fragment
cycles are a validation error; list types nest at most 7 deep. The parser,
the planner and the executor recurse only under these depth limits. Nothing
allocates more than a small multiple of the input size before a limit stops
it. Every failure is an error in the response — the engine has no `throw`
and no abort on any query, variables or limit, and builds and runs with
`-fno-exceptions -fno-rtti` (`cpp/tests/graphql_noexceptions.cpp`).

## Measurements

`cpp/benchmarks/bench_graphql.cpp` (`psio_bench_graphql`), arm64 Apple
M-series, clang 22 `-O3`. ns per request, minimum over two runs of 7 trials
each. **The machine was heavily loaded by an unrelated build while these
were taken (load average 5 at the start, above 30 during the runs): treat
differences under about 15% as noise**, in particular value against view.
"old" is the previous engine (`psio1::gql_query`), built in its own
translation unit on the same data. "plan" executes a compiled plan; "text"
compiles the query for every request. Allocations are heap allocations per
request; the new engine appends to a response string the caller reuses, the
old one returns a new string.

| case | old | new value, plan | new view, plan | new value, text | new view, text | allocs old / plan / text |
|---|---:|---:|---:|---:|---:|---|
| object, 3 of 7 fields | 316 | 78 | 72 | 738 | 734 | 2 / 0 / 26 |
| list of 100 rows, 3 of 20 fields | 18 080 | 2 848 | 2 870 | 3 583 | 3 731 | 8 / 0 / 26 |
| list of 10 000 rows, 3 of 20 fields | 2 006 137 | 283 932 | 299 869 | 289 020 | 285 481 | 15 / 0 / 26 |
| list of 100 wide rows (8 KiB of strings, 512 integers), 2 small fields | 9 042 | 1 674 | 1 714 | 2 104 | 2 299 | 7 / 0 / 24 |
| nested lists, 100 x 10 | 116 422 | 19 680 | 20 964 | 20 911 | 21 767 | 11 / 0 / 34 |
| connection page of 100 | 38 204 | 5 069 | 4 867 | 6 234 | 6 384 | 210 / 10 / 67 |
| connection page of 10 000 | 4 385 172 | 516 591 | 523 809 | 515 856 | 538 352 | 20 017 / 29 / 86 |
| fragments + variables, 100 rows | not supported | 2 883 | 2 960 | 5 039 | 5 103 | – / 2 / 73 |

Reading it: with a plan the new engine is 4 to 8.5 times faster than the old
one on these cases and allocates nothing for a plain query; compiling a
query costs about 0.6 µs and 26 allocations for a small one, which is why
"text" loses to the old engine on the single small object (738 against
316 ns) and why a plan cache pays for small queries. Values and views are
within noise of each other here — the value side reads fields that are
already in memory, so this benchmark shows that reading through a view costs
no more; it does not include what a value-based server pays first (decoding
each row). The old engine's page is a `Connection` of copied row values,
which is what its consumer built from unpacked rows.

**Code size.** A small guest-style program (one record, a root with two
methods, one `run` call) built for arm64 with
`-Os -fno-exceptions -fno-rtti` and dead-stripping: 7.1 KiB of code with
only `psio::ijson` encoding, 84.7 KiB with the engine and no introspection
(+77.5 KiB), 101.1 KiB with introspection (+94 KiB). Not measured for
WebAssembly (no WASI sysroot on the machine).

Connections, measured the same way (`__text` bytes, no introspection): the
program above is 82.1 KiB; with one connection field over an ordered index
of a `std::map`, taking the four page arguments as parameters and a
hand-written row source, 91.1 KiB (+9 KiB for `paginate`, the connection
types and the map); with `key_range<std::uint64_t>` and `page_args` as
parameters over the same index through `index_source`, 93.6 KiB (+2.5 KiB:
the key encoding of the bounds, the range checks, the adapter); with a
`page_budget`, 93.8 KiB. Argument groups, flattened members, template names
and the `_` field cost a program that does not use them 0.2 KiB.

## Not supported

- **Subscriptions.**
- **Interfaces.** Reflection has no notion that maps to them; unions
  (`std::variant`) are the only abstract types.
- **Argument and input-field default values**; `ID` as an argument type.
- **A byte budget on the response text of a page** (`page_budget` counts
  the rows read; see [Connections](#connections)).
- **Custom directives**; only `@skip` and `@include` (and `@deprecated` in
  the schema). `@specifiedBy` is not listed and `specifiedByURL` is null.
- **Descriptions of enum values and of method parameters** (a member of an
  argument group has one).
- **Unicode escapes of the form `\u{…}`** (they postdate October 2021).
- Validation reports the errors it finds while planning: a fragment that is
  never used is reported as unused but its body is not validated further, and
  at most `max_errors` errors are returned.
- Custom scalars are checked by name for variables only where every C++ type
  with that name accepts the same values (`UInt64`, `Int64`, …); a variable
  of type `Int`, `Float`, `String`, `Bytes`, a time point or a duration that
  the specific C++ parameter rejects (an `std::int8_t` given `300`) is a
  field error at the field that uses it, not a request error.

## Files

| | |
|---|---|
| `cpp/include/psio/graphql.hpp` | `schema<Query, Mutation, Introspection>` |
| `cpp/include/psio/graphql/schema.hpp` | descriptors, the mapping, annotations, `context`, `result` |
| `cpp/include/psio/graphql/parser.hpp` | `limits`, `error`, lexer and parser |
| `cpp/include/psio/graphql/plan.hpp` | validation, `document`, `plan_cache` |
| `cpp/include/psio/graphql/execute.hpp` | `request`, the executor, response writing |
| `cpp/include/psio/graphql/introspection.hpp` | `__schema` / `__type`, `sdl()` |
| `cpp/include/psio/graphql/connection.hpp` | `ordered_index`, `row_source`, `transform`, `page_args`, `key_range`, `paginate`, `connection` |
| `cpp/tests/graphql_tests.cpp`, `graphql_fixture.hpp` | the conformance suite and its schema |
| `cpp/tests/graphql_service_tests.cpp`, `graphql_service_fixture.hpp`, `graphql_service_checks.hpp` | a query root shaped like a psibase service's (connections over indexes, key ranges, a byte budget, transformed rows, flattened members, class templates, markers) and the rules behind it |
| `cpp/tests/graphql_noexceptions.cpp`, `graphql_service_noexceptions.cpp` | exception-disabled build-and-run checks |
| `cpp/tests/graphql_schema_fail.cpp` | schemas that must not compile |
| `cpp/benchmarks/bench_graphql.cpp` | benchmark against the previous engine |
