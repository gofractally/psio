# PSIO type annotations — tutorial

A guided path through one example: an account record and a small bank
service. You declare the type once, annotate it, and watch what each
annotation does to the bytes, the text, the keys and the schema.

- Every fact here is stated precisely in the reference,
  [annotations.md](annotations.md).
- The short rule list is [annotations-for-agents.md](annotations-for-agents.md).

**How to follow along.** Every C++ snippet is a run of consecutive lines of
[`cpp/tests/doc_annotations_examples.cpp`](../cpp/tests/doc_annotations_examples.cpp);
each step names its section (`tut-1`, `tut-2-run`, …). Build and run:

```sh
cmake --build <build-dir> --target psio_doc_annotations_examples
<build-dir>/cpp/tests/psio_doc_annotations_examples
```

Each step has three parts: **write** (the declaration), **get** (what
changes), **check** (the assertion that proves it). The headers are
`<psio/pssz.hpp>`, `<psio/json.hpp>`, `<psio/key.hpp>`,
`<psio/max_size.hpp>`, `<psio/reflect.hpp>` (or `<psio/psio.hpp>`).

## Step 1 — Declare it plain

**Write** the struct you want and list its members in `PSIO_REFLECT`
(`tut-1`):

```cpp
struct AccountPlain
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountPlain, id, owner, tags, balance)
};
// owner and tags may be any size: there is no bound, so offsets are 32-bit.
static_assert(!psio::max_encoded_size<AccountPlain>().has_value());
static_assert(psio::auto_pssz_width_v<AccountPlain> == 4);
```

**Get.** The type already works in every format: `psio::to_pssz(a)`,
`psio::to_json(a)`, `psio::to_key(a)`, a schema. But look at the two
assertions. `psio::max_encoded_size<T>()` is the largest encoding the type
allows; here there is none, because `owner` and `tags` can be any size.
A PSSZ record locates its variable-size members with offsets, and a type
with no size bound needs 32-bit offsets: `auto_pssz_width_v == 4`. Every
record pays a 4-byte header and 4 bytes per variable-size member, and the
list `tags` pays 4 more bytes per element for its own offset table.

**Check.** The two `static_assert`s. Put them next to every type you
publish: the width is part of the type, and you want the compiler to tell
you when it moves.

## Step 2 — Add bounds until the width drops

**Write** the limits the application already has. Do not change the member
types; annotate them with `attr(member, specs)` (`tut-2a`):

```cpp
struct AccountWide
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountWide,
                id,
                attr(owner, utf8 | max<32>),                    // UTF-8, <= 32 bytes
                attr(tags, max<64> | element(utf8 | max<16>)),  // <= 64 strings of <= 16 bytes
                balance)
};
// tags:   a table of 64 slots, then 64 strings of <= 16 bytes. With 1-byte slots that
//         would be 64 + 1024 = 1088 > 255, so the slots are 2 bytes: 2 x 64 + 1024 = 1152.
// record: a header and 2 slots (owner, tags), then id 8 + owner 32 + tags 1152 + balance 8
//         = 1200. 3 + 1200 > 255, so header and slots are 2 bytes: 2 x 3 + 1200 = 1206.
static_assert(psio::max_encoded_size<AccountWide>() == 2 * 3 + 8 + 32 + (2 * 64 + 64 * 16) + 8);
static_assert(psio::max_encoded_size<AccountWide>() == 1206);   // <= 65535: 16-bit offsets
static_assert(psio::auto_pssz_width_v<AccountWide> == 2);
```

- `max<32>` on a string: at most 32 bytes.
- `utf8`: the bytes are UTF-8 text (Step 5).
- `max<64>` on a vector: at most 64 elements.
- `element(...)`: what holds for every element.
- `spec | spec` combines.

**Get.** Every variable-size part is now bounded, so the type has an
inferred bound, and the bound selects the width: at most 255 bytes → 8-bit
offsets, at most 65535 → 16-bit, otherwise 32-bit.

Two things in this type have offsets, and **each gets its own width**: the
record (a header and a slot for `owner` and for `tags`) and the list (a
table of one slot per tag). Each is measured at the width it ends up with —
the comment above the assertion does the sum. The list is 64 slots and
1024 bytes of strings: too much for 1-byte slots, so 2 × 64 + 1024 = 1152.
The record is 3 words and 1200 other bytes: 2 × 3 + 1200 = 1206, 16-bit.

A tighter bound gets the next width (`tut-2b`):

```cpp
struct Account
{
   friend bool operator==(const Account&, const Account&) = default;
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(Account,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance)
};
// tags:   8 slots + 8 x 16 = 136 <= 255: the list's own table is 1 byte per slot.
// record: (header + 2 slots) x 1 byte + id 8 + owner 32 + tags 136 + balance 8.
static_assert(psio::max_encoded_size<Account>() == 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::max_encoded_size<Account>() == 187);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);
```

Now the list is 8 + 128 = 136 bytes with a 1-byte table, and the record
3 + 184 = 187 with a 1-byte header and slots.

**The rule to remember:** the bound is rounded up to the next width, and
*the only spare capacity you get is the rounding*. `Account` can grow by
68 bytes of bound before it stops being 8-bit.

Bounding only part of a member bounds nothing (`tut-2c`):

```cpp
struct AccountCountOnly
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountCountOnly, id, attr(owner, utf8 | max<32>), attr(tags, max<8>), balance)
};
// <= 8 tags, but each tag is still unbounded: no bound, 32-bit offsets.
static_assert(!psio::max_encoded_size<AccountCountOnly>().has_value());
static_assert(psio::auto_pssz_width_v<AccountCountOnly> == 4);
```

**Check** that bounds are enforced, not advisory (`tut-2-run`):

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

A value outside its bounds has no encoding, and readers reject bytes that
hold one (`pssz: member length_bound violated`,
`utf8_spec: invalid leading byte`).

What you saved: `Account{7, "alice", {"vip", "beta"}, 100}` is 33 bytes;
the same value as `AccountPlain` is 48. The frame:

```text
12                         header: the fixed region is 0x12 = 18 bytes
07 00 00 00 00 00 00 00    id
12                         owner: offset
17                         tags: offset
64 00 00 00 00 00 00 00    balance
61 6c 69 63 65             "alice"
02 05                      the list's own offset table (1 byte per element)
76 69 70 62 65 74 61       "vip" "beta"
```

If you prefer the bound in the type, `psio::utf8_string<32>` and
`psio::bounded<psio::utf8_string<16>, 8>` are the same type on the wire —
identical bytes, widths, JSON and schema
([annotations.md §2.3](annotations.md#23-wrapper-types)). Most code keeps
`std::string` and `std::vector` and annotates.

## Step 3 — Reserve room for growth

`Account` is as tight as it can be, which is the problem: it has 68 bytes
of slack. The day a field is added whose bound exceeds that, the width
changes from 8-bit to 16-bit and **every stored record becomes
unreadable**. Width is part of the type.

**Write** an explicit cap when the type is first published (`tut-3`):

```cpp
struct AccountV1
{
   friend bool operator==(const AccountV1&, const AccountV1&) = default;
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountV1,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
// The cap rules: 16-bit offsets, although today's fields need at most 187 bytes.
static_assert(psio::max_dynamic_data_v<AccountV1> == 1024);
static_assert(psio::auto_pssz_width_v<AccountV1> == 2);
// max_encoded_size stays the bound inferred from the fields; the cap is what rules.
static_assert(psio::max_encoded_size<AccountV1>() == 187);
static_assert(psio::effective_max_dynamic_v<AccountV1> == 1024);   // the cap, when one is declared
static_assert(psio::effective_max_dynamic_v<Account> == 187);      // else the inferred bound
```

**Get.** `maxDynamicData(N)` is two things:

1. The **width budget** for the type's lifetime. An explicit cap always
   rules: 1024 → 16-bit, whatever the fields need today.
2. The **limit of the encoded size**, header included, enforced by the
   encoder and every reader.

```cpp
const AccountV1 alice{7, "alice", {"vip", "beta"}, 100};
// 2-byte header and slots now: 3 bytes more than the 8-bit Account. The
// list keeps its 1-byte table: its own bound (136) is tighter than the cap.
CHECK(psio::to_pssz(alice).size() == psio::to_pssz(Account{7, "alice", {"vip", "beta"}, 100}).size() + 3);
CHECK(psio::from_pssz<AccountV1>(psio::to_pssz(alice)) == alice);
```

The price of the reservation is 3 bytes per record here: the header and
two offset slots are 2 bytes instead of 1. The list's table stays 1 byte
wide, because the list's own bound (136) is tighter than the cap.

**Check** the limit (`tut-3-cap`, on a small type with a 16-byte cap):

```cpp
// A cap is the limit of the whole encoded value, header included.
CHECK(psio::to_pssz(ref::Capped{std::string(14, 'x')}).size() == 16);     // 1 + 1 + 14
CHECK_THROWS_AS(psio::to_pssz(ref::Capped{std::string(15, 'x')}), psio::codec_exception);
auto frame = psio::to_pssz(ref::Capped{std::string(14, 'x')});
frame.push_back('x');                                                     // 17 bytes
CHECK_FALSE(psio::validate_pssz<ref::Capped>(frame).ok());
```

**A cap also narrows what is beneath it.** A record with a cap never
encodes to more than N bytes, so no list, map or tuple inside it does
either, and they take their offset width from `min(their own bound, N)`.
Under a cap you get narrow tables without bounding every element. An
action's arguments under 255, the largest cap with 1-byte offsets
(`ref-cap-transfer`):

```cpp
struct Transfer
{
   friend bool operator==(const Transfer&, const Transfer&) = default;
   std::uint32_t                            token  = 0;
   std::uint64_t                            from   = 0;
   std::uint64_t                            amount = 0;
   std::string                              memo;
   std::optional<std::vector<std::uint8_t>> data;    // unbounded: the cap is its only limit
   PSIO_REFLECT(Transfer, token, from, amount, attr(memo, utf8 | max<80>), data,
                maxDynamicData(255))
};
static_assert(psio::auto_pssz_width_v<Transfer> == 1);   // 255: the largest cap with 1-byte offsets
```

```cpp
// Framing: header 1 + token 4 + from 8 + amount 8 + 2 slots = 23 bytes.
// The other 232 bytes are shared by memo and data; a present data spends
// one of them on its selector byte.
const std::string memo(80, 'm');
const Transfer    full{1, 2, 3, memo, std::vector<std::uint8_t>(151, 0)};
CHECK(psio::to_pssz(full).size() == 23 + 80 + 1 + 151);               // 255: exactly the cap
CHECK(psio::from_pssz<Transfer>(psio::to_pssz(full)) == full);
// One byte more has no encoding ...
CHECK_THROWS_AS(psio::to_pssz(Transfer{1, 2, 3, memo, std::vector<std::uint8_t>(152, 0)}),
                psio::codec_exception);
// ... and a short memo leaves its bytes to data.
CHECK(psio::to_pssz(Transfer{1, 2, 3, "", std::vector<std::uint8_t>(231, 0)}).size() == 255);
// An absent trailing data is pruned with its slot: 22 bytes of framing.
CHECK(psio::to_pssz(Transfer{1, 2, 3, "rent", std::nullopt}).size() == 22 + 4);
```

`data` has no bound of its own; the cap is its limit. A *record* beneath a
cap is the exception: it keeps its own width wherever it appears, so give
child records their own cap or bounds
([annotations.md §3.3](annotations.md#33-a-cap-narrows-what-lies-beneath-it)).

**Choosing N** ([annotations.md §3.4](annotations.md#34-how-to-pick-a-cap)).

- State the limits the application has as member bounds first; they are
  enforced one by one.
- Then pick the width class: for database rows and action arguments aim
  for 16-bit, a cap of at most 65535. Use 8-bit (≤ 255) for small records
  you are sure about, and
  [`definitionWillNotChange()`](annotations.md#55-definitionwillnotchange)
  for values that are closed forever (a point, a hash).
- Cover the current inferred bound plus everything the type may gain:
  adding an optional field must not change the width.
- When unsure, go one size up. A width that is too small cannot be changed
  after publication; one that is too large costs a byte or two per offset.
- The cap counts **bytes**. A cap on the **number of fields** is a separate
  keyword, `maxFields(N)` (N ≤ 255): it changes no byte, and once published
  it can be neither exceeded, raised nor dropped
  ([annotations.md §5.6](annotations.md#56-maxfieldsn)).
- Member bounds need the same foresight: after publication a bound may only
  widen (Step 4).

## Step 4 — Add a field compatibly

**Write** the new field as `std::optional`, **last**, and keep everything
before it unchanged (`tut-4a`):

```cpp
struct AccountV2
{
   friend bool operator==(const AccountV2&, const AccountV2&) = default;
   std::uint64_t              id = 0;
   std::string                owner;
   std::vector<std::string>   tags;
   std::uint64_t              balance = 0;
   std::optional<std::string> email;   // new: optional, last
   PSIO_REFLECT(AccountV2,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                attr(email, utf8 | max<64>),
                maxDynamicData(1024))
};
static_assert(psio::auto_pssz_width_v<AccountV2> == 2);
static_assert(psio::pssz_evolves_from<AccountV1, AccountV2>);
```

**Get** compatibility in both directions (`tut-4-run`):

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

- A trailing absent optional is **pruned**: the new writer writes the old
  bytes when the field is not set.
- Old bytes read as the new type: the field is `nullopt`.
- New bytes read as the old type: the unknown field is skipped.

**Check** with `psio::pssz_evolves_from<Old, New>`, at compile time. Keep
the previous revision of every published type (in a test file is fine) and
assert against it.

**What is not compatible** — each rejected by the check (`tut-4b`):

```cpp
// (1) No cap: Account has 255 - 187 = 68 bytes of slack. A new field that needs
//     more (slot 1 + selector 1 + 100) moves the width from 1 to 2, and every
//     existing record becomes unreadable.
struct AccountGrown
{
   std::uint64_t              id = 0;
   std::string                owner;
   std::vector<std::string>   tags;
   std::uint64_t              balance = 0;
   std::optional<std::string> bio;
   PSIO_REFLECT(AccountGrown,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                attr(bio, utf8 | max<100>))
};
// (header + 3 slots) x 2 bytes + id 8 + owner 32 + tags 136 + balance 8 + bio (selector 1 + 100)
static_assert(psio::max_encoded_size<AccountGrown>() == 2 * 4 + 8 + 32 + 136 + 8 + (1 + 100));
static_assert(psio::auto_pssz_width_v<AccountGrown> == 2);
static_assert(!psio::pssz_evolves_from<Account, AccountGrown>);

// (2) The new field is not optional.
struct AccountRequired
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   std::uint32_t            flags = 0;
   PSIO_REFLECT(AccountRequired,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                flags,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountRequired>);

// (3) The new field is optional but not last.
struct AccountReordered
{
   std::uint64_t              id = 0;
   std::optional<std::string> email;
   std::string                owner;
   std::vector<std::string>   tags;
   std::uint64_t              balance = 0;
   PSIO_REFLECT(AccountReordered,
                id,
                attr(email, utf8 | max<64>),
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountReordered>);

// (4) An existing field changes type.
struct AccountRetyped
{
   std::uint32_t            id = 0;   // was std::uint64_t
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountRetyped,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountRetyped>);

// (5) The cap changes width class (1024 -> 100000: 16-bit -> 32-bit).
struct AccountRecapped
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountRecapped,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(100000))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountRecapped>);
```

Case (1) is the one the cap exists for: *you cannot add an optional field
if it would change the implicit width.* Without `maxDynamicData`, `Account`
has 68 bytes of slack. The `email` field of `AccountV2` (slot 1 + selector
1 + 64 = 66) would have squeezed in; `bio` (102) does not, and moves
`Account` from 8-bit to 16-bit. Whether a future field fits should not
depend on such arithmetic: declare the cap.

**A published bound may only widen** (`tut-4c`). The check compares the
annotations too:

```cpp
// The check compares the annotations too. A wider bound is compatible in one
// direction: the new reader reads every record the old writer produced.
struct AccountWider
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountWider,
                id,
                attr(owner, utf8 | max<64>),   // was max<32>
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(psio::pssz_evolves_from<AccountV1, AccountWider>);
static_assert(!psio::pssz_evolves_from<AccountWider, AccountV1>);    // never back

// A tighter bound is refused: the new reader would reject stored records.
struct AccountTighter
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountTighter,
                id,
                attr(owner, utf8 | max<16>),   // was max<32>
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountTighter>);

// So is a wider bound that moves an offset width: 8 tags of <= 40 bytes are
// 8 + 320 > 255, and the list's table would go from 1 byte to 2.
struct AccountWideTags
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountWideTags,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<40>)),   // was max<16>
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountWideTags>);

// utf8 or raw is what the member is: it cannot be added or dropped.
struct AccountRawOwner
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountRawOwner,
                id,
                attr(owner, max<32>),          // was utf8 | max<32>
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountRawOwner>);
static_assert(!psio::pssz_evolves_from<AccountRawOwner, AccountV1>);

// The cap may grow inside its width class, and may not shrink.
struct AccountCapRaised
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountCapRaised,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(4096))          // was 1024
};
static_assert(psio::pssz_evolves_from<AccountV1, AccountCapRaised>);
static_assert(!psio::pssz_evolves_from<AccountCapRaised, AccountV1>);
```

- A **wider** bound is compatible, in one direction: the new reader reads
  everything the old writer produced. Old readers reject the new, longer
  values — upgrade readers before writers.
- A **tighter** bound, an added or dropped `utf8`, an added or dropped
  `sorted` / `unique`, a lowered cap are refused: one side would reject
  what the other wrote.
- A wider bound that moves an offset width (here the table of `tags`) is
  refused too: it changes the bytes.

On bytes (`tut-4c-run`):

```cpp
// What the check promises, on bytes: an old record at its bound is a record
// of the widened type, and is not one of the tightened type.
const AccountV1 wide_owner{7, std::string(32, 'o'), {}, 0};
CHECK(psio::validate_pssz<AccountWider>(psio::to_pssz(wide_owner)).ok());
CHECK_FALSE(psio::validate_pssz<AccountTighter>(psio::to_pssz(wide_owner)).ok());
// One direction only: a new writer can produce what an old reader refuses.
const AccountWider wider{7, std::string(33, 'o'), {}, 0};
CHECK_FALSE(psio::validate_pssz<AccountV1>(psio::to_pssz(wider)).ok());
```

The exact rule is in
[annotations.md §8.3](annotations.md#83-annotations-bounds-may-only-widen)
and [pssz-optional-evolution.md](pssz-optional-evolution.md). What the
check still cannot see — a changed invariant, the meaning of an enumerator
— stays frozen by convention.

## Step 5 — Raw bytes or UTF-8 text

A C++ `std::string` is **raw bytes**. `utf8` makes it text. Across
languages: C++ `std::string` = Rust `RawString`; C++ `std::string` + `utf8`
(or `psio::utf8_string<>`) = Rust `String`.

**Write** (`tut-5`):

```cpp
struct Note
{
   friend bool operator==(const Note&, const Note&) = default;
   std::string blob;   // raw bytes: any byte string
   std::string text;   // UTF-8 text
   PSIO_REFLECT(Note, attr(blob, max<64>), attr(text, utf8 | max<64>))
};
```

**Get / check** (`tut-5-run`):

```cpp
// A raw string holds any bytes in the binary formats.
const Note binary{"\xff\xfe", "caf\xc3\xa9"};
CHECK(psio::from_pssz<Note>(psio::to_pssz(binary)) == binary);
// A utf8 string does not.
CHECK_THROWS_AS(psio::to_pssz(Note{"", "\xff"}), psio::codec_exception);
// JSON text is UTF-8: a raw string that is not UTF-8 has no JSON.
CHECK_THROWS_AS(psio::to_json(binary), psio::codec_exception);
CHECK(psio::to_json(Note{"bytes", "caf\xc3\xa9"}) == "{\"blob\":\"bytes\",\"text\":\"caf\xc3\xa9\"}");
```

`utf8` is always enforced — by every pssz, JSON and key writer and reader.
There is no weaker form, and no stricter reading of a raw string either:
`validate_strict` accepts any bytes in one.

Use `utf8` for anything a person reads (names, memos, URLs) and plain
bytes for hashes, signatures and packed data. For byte data proper,
`std::vector<std::uint8_t>` is the better type: I-JSON shows it as hex
([annotations.md §6.5](annotations.md#65-bytes-and-hex)).

## Step 6 — JSON presentation

Nothing to write: the reflected type already has its text. There are two
text formats ([json.md](json.md)). `psio::json` is plain JSON with every
integer a number. `psio::ijson` (I-JSON) is the form for JavaScript
clients: a 64-bit integer above 2^53 − 1 is a decimal string, bytes are
hex, a variant is `{"name": value}`.

**Write** an annotation only where a client needs one fixed form
(`tut-6`):

```cpp
struct Totals
{
   friend bool operator==(const Totals&, const Totals&) = default;
   std::uint64_t supply   = 0;
   std::uint64_t holders  = 0;
   std::uint32_t decimals = 0;
   PSIO_REFLECT(Totals,
                attr(supply, json_string | ijson_string),   // always a string
                attr(holders, ijson_number),                // always a number
                decimals)
};
```

**Get / check** (`tut-6-run`):

```cpp
const AccountV2 a{7, "alice", {"vip"}, 9007199254740993ull, std::nullopt};   // 2^53 + 1
// json: every integer is a number.
CHECK(psio::to_json(a) ==
      R"({"id":7,"owner":"alice","tags":["vip"],"balance":9007199254740993,"email":null})");
// ijson: a 64-bit integer beyond 2^53 - 1 is a decimal string.
CHECK(psio::to_ijson(a) ==
      R"({"id":7,"owner":"alice","tags":["vip"],"balance":"9007199254740993","email":null})");
CHECK(psio::from_ijson<AccountV2>(psio::to_ijson(a)) == a);

// The same bounds apply to text.
CHECK_FALSE(psio::validate_json<AccountV2>(
               R"({"id":7,"owner":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","tags":[],"balance":0,"email":null})")
               .ok());

// json_string / ijson_number fix one member's form.
const Totals t{9007199254740993ull, 9007199254740993ull, 4};
CHECK(psio::to_json(t) == R"({"supply":"9007199254740993","holders":9007199254740993,"decimals":4})");
CHECK(psio::to_ijson(t) == R"({"supply":"9007199254740993","holders":9007199254740993,"decimals":4})");
CHECK(psio::to_ijson(Totals{5, 5, 4}) == R"({"supply":"5","holders":5,"decimals":4})");
// A reader accepts exactly the forced form.
CHECK_FALSE(psio::validate_ijson<Totals>(R"({"supply":5,"holders":5,"decimals":4})").ok());
```

- Bounds, `utf8` and invariants are enforced in text exactly as in
  binary.
- Without an annotation the I-JSON form of a 64-bit integer depends on the
  *value*; a client must accept both. `ijson_string` / `ijson_number` make
  it depend on the member only, and then the reader accepts exactly that
  form.
- These annotations do not touch any binary format.

## Step 7 — Keys

A database key is the `key` encoding of one or more fields: a byte string
whose bytewise order is the value's order ([key-spec.md](key-spec.md)).

**Write** the key types (`tut-7`):

```cpp
// The primary key of the accounts table, and a secondary index by owner.
using AccountKey = std::uint64_t;
using ByOwner    = std::tuple<std::string, std::uint64_t>;   // (owner, id)

struct OwnerKey
{
   friend bool operator==(const OwnerKey&, const OwnerKey&) = default;
   std::string   owner;
   std::uint64_t id = 0;
   PSIO_REFLECT(OwnerKey, attr(owner, utf8 | max<32>), id)
};
```

**Get / check** (`tut-7-run`):

```cpp
// An integer key is big-endian: byte order is numeric order.
CHECK(hex(psio::to_key(AccountKey{258})) == "00 00 00 00 00 00 01 02");
// A composite key is the concatenation; a string is terminated by 00 00.
CHECK(hex(psio::to_key(ByOwner{"bob", 7})) == "62 6f 62 00 00 00 00 00 00 00 00 00 07");
// All keys of one owner: a prefix scan from the prefix to its upper bound.
const auto prefix = psio::key_prefix<ByOwner>("bob");
CHECK(hex(prefix) == "62 6f 62 00 00");
CHECK(hex(*psio::key_prefix_upper_bound(prefix)) == "62 6f 62 00 01");
CHECK(psio::from_key<ByOwner>(psio::to_key(ByOwner{"bob", 7})) == ByOwner{"bob", 7});

// A reflected record is the key of its fields in order, with the same
// bytes as the tuple: annotations do not change a key.
CHECK(psio::to_key(OwnerKey{"bob", 7}) == psio::to_key(ByOwner{"bob", 7}));

// The key writer checks what the pssz writer checks: a value outside a
// bound, or a utf8 member that is not UTF-8, has no key.
const OwnerKey too_long{std::string(33, 'x'), 7};                 // one byte past max<32>
CHECK_THROWS_AS(psio::to_key(too_long), psio::codec_exception);
std::vector<char> out;
CHECK_FALSE(psio::encode_key_checked(too_long, out).ok());        // the status form
CHECK(out.empty());
CHECK_THROWS_AS(psio::to_key(OwnerKey{"\xff", 7}), psio::codec_exception);
// The readers refuse the key bytes a writer without the bound produces.
const auto unbounded = psio::to_key(ByOwner{std::string(33, 'x'), 7});
CHECK_FALSE(psio::validate_key<OwnerKey>(unbounded).ok());
CHECK_THROWS_AS(psio::from_key<OwnerKey>(unbounded), psio::codec_exception);
```

- Annotations do not change key bytes, and a key has no offset width.
- The key writer checks what the pssz writer checks: `to_key` throws for a
  value outside a member bound or with a `utf8` member that is not UTF-8,
  and `psio::encode_key_checked(value, out)` returns a status instead.
  Validate keys that arrive from outside (`validate_key` / `from_key`).

## Step 8 — Transparent wrappers and invariants

**Write** a transparent wrapper for a value that deserves its own C++ type
but should be exactly its member on the wire (`tut-8a`):

```cpp
struct AccountId
{
   friend auto operator<=>(const AccountId&, const AccountId&) = default;
   std::uint64_t value = 0;
   PSIO_REFLECT(AccountId, transparent(), value)
};
// It is its member: fixed-size, 8 bytes, no header.
static_assert(psio::is_transparent_v<AccountId>);
static_assert(psio::max_encoded_size<AccountId>() == 8);
```

**Write** an invariant for a rule that bounds cannot express (`tut-8b`):

```cpp
struct Symbol
{
   friend bool operator==(const Symbol&, const Symbol&) = default;
   std::string code;
   static psio::codec_status check(const Symbol& s) noexcept
   {
      for (char c : s.code)
         if (c < 'A' || c > 'Z')
            return psio::codec_fail("symbol is not upper case", 0, "Symbol");
      return psio::codec_ok();
   }
   PSIO_REFLECT(Symbol, attr(code, max<8>), invariant(check))
};
static_assert(psio::has_invariant_v<Symbol>);
```

**Get / check** (`tut-8-run`):

```cpp
// A transparent wrapper is its member in pssz, key and JSON.
CHECK(psio::to_pssz(AccountId{42}) == psio::to_pssz(std::uint64_t{42}));
CHECK(psio::to_key(AccountId{42}) == psio::to_key(std::uint64_t{42}));
CHECK(psio::to_json(AccountId{42}) == "42");

// A value that violates its invariant has no encoding ...
CHECK_THROWS_AS(psio::to_pssz(Symbol{"usd"}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_json(Symbol{"usd"}), psio::codec_exception);
// ... and bytes or text that hold one are refused, with the type's message.
std::vector<char> bytes = psio::to_pssz(Symbol{"USD"});
bytes.back() = 'd';                                             // "USd"
const auto status = psio::validate_pssz<Symbol>(bytes);
CHECK_FALSE(status.ok());
CHECK(status.error().what == std::string_view{"symbol is not upper case"});
CHECK_FALSE(psio::validate_json<Symbol>(R"({"code":"usd"})").ok());
```

An invariant is `noexcept`, returns a `psio::codec_status`, never throws,
and depends only on the value. It runs in every writer and every checking
reader of pssz, key and JSON — not in the trusted decode and not in views
(Step 11).

**Write** a first-class bounded type when a bounded string has a name of
its own. The member of a transparent type may carry `utf8`, `max<N>` and
the position specs; the wrapper then *is* that bounded type (`tut-8c`):

```cpp
struct Memo
{
   friend auto operator<=>(const Memo&, const Memo&) = default;
   std::string contents;
   PSIO_REFLECT(Memo, transparent(), attr(contents, utf8 | max<80>))
};
// Memo is the bounded type its member's annotations describe: on the wire it
// is psio::utf8_string<80>.
static_assert(psio::is_transparent_v<Memo>);
static_assert(psio::max_encoded_size<Memo>() == 80);
static_assert(psio::max_encoded_size<Memo>() == psio::max_encoded_size<psio::utf8_string<80>>());

struct MemoInType   // the same type with the bound in the member's type
{
   psio::utf8_string<80> contents;
   PSIO_REFLECT(MemoInType, transparent(), contents)
};

struct Receipt   // a record that holds one: header 1 + id 8 + slot 1 + memo 80
{
   friend bool operator==(const Receipt&, const Receipt&) = default;
   std::uint64_t id = 0;
   Memo          memo;
   PSIO_REFLECT(Receipt, id, memo)
};
static_assert(psio::max_encoded_size<Receipt>() == 1 + 8 + 1 + 80);
static_assert(psio::auto_pssz_width_v<Receipt> == 1);
```

**Get / check** (`tut-8c-run`):

```cpp
// Memo has the bytes, the key and the text of psio::utf8_string<80>.
const Memo memo{"rent"};
CHECK(psio::to_pssz(memo) == psio::to_pssz(psio::utf8_string<80>{"rent"}));
CHECK(psio::to_key(memo) == psio::to_key(psio::utf8_string<80>{"rent"}));
CHECK(psio::to_json(memo) == R"("rent")");
CHECK(psio::from_pssz<Memo>(psio::to_pssz(memo)) == memo);

// Its bound and its UTF-8 rule hold wherever a Memo appears.
CHECK_THROWS_AS(psio::to_pssz(Memo{std::string(81, 'm')}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_pssz(Memo{"\xff"}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_key(Memo{std::string(81, 'm')}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_pssz(Receipt{1, Memo{std::string(81, 'm')}}), psio::codec_exception);
CHECK_FALSE(psio::validate_pssz<Memo>(std::vector<char>(81, 'm')).ok());
CHECK_FALSE(psio::validate_json<Memo>('"' + std::string(81, 'm') + '"').ok());

// Inside a record it costs one 1-byte slot: 1 + 8 + 1 + 4.
const Receipt receipt{1, memo};
CHECK(psio::to_pssz(receipt).size() == 14);
CHECK(psio::to_json(receipt) == R"({"id":1,"memo":"rent"})");
CHECK(psio::from_pssz<Receipt>(psio::to_pssz(receipt)) == receipt);

// The Schema IR is the wrapper-type spelling's: a named alias of the
// bounded UTF-8 string.
CHECK(shape_text<Memo>() == shape_text<MemoInType>());
CHECK(contains(schema_text<Memo>("Memo"), R"("maxCount":80}}},"id":"utf8")"));
```

`Memo` keeps `std::string` as its member and is, in bytes, width, key, text
and schema, `psio::utf8_string<80>`. Every record and every argument list
that holds a `Memo` gets the bound without repeating it.

## Step 9 — Methods with capped arguments

**Write** the service's methods in the same `PSIO_REFLECT`. The arguments
of a call are the tuple of the parameters, and they take the annotations a
record's members take (`tut-9a`):

```cpp
struct Bank
{
   void        open(std::string owner, std::vector<std::string> tags);
   void        transfer(AccountId from, AccountId to, std::uint64_t amount, std::string memo);
   std::string symbol(std::uint32_t token) const;
   PSIO_REFLECT(Bank,
                method(open, attr(owner, utf8 | max<32>),
                       attr(tags, max<8> | element(utf8 | max<16>))),
                method(transfer, from, to, amount, attr(memo, utf8 | max<80>),
                       maxDynamicData(1024)),
                method(symbol, token, returns(utf8 | max<8>)))
};
PSIO_INTERFACE(Bank, types())

using OpenArgs     = psio::reflect<Bank>::method_arg_tuple<0>;
using TransferArgs = psio::reflect<Bank>::method_arg_tuple<1>;
// open has no cap: (header + 2 slots) x 1 byte + owner 32 + tags 136 = 171, 8-bit.
static_assert(psio::max_encoded_size<OpenArgs>() == 3 + 32 + (8 + 8 * 16));
static_assert(psio::auto_pssz_width_v<OpenArgs> == 1);
// transfer is capped: 16-bit for its lifetime.
static_assert(psio::max_dynamic_data_v<TransferArgs> == 1024);
static_assert(psio::auto_pssz_width_v<TransferArgs> == 2);
// max_encoded_size is the bound inferred from the parameters, not the cap:
// header 1 + slot 1 + from 8 + to 8 + amount 8 + memo 80.
static_assert(psio::max_encoded_size<TransferArgs>() == 2 + 8 + 8 + 8 + 80);
```

- `attr(p, specs)` names and annotates a parameter; a bare name is a
  parameter without annotations.
- `maxDynamicData(N)` caps the argument tuple and fixes its width; like a
  record's cap it also narrows the lists and maps among the arguments.
- `returns(specs)` annotates the return value.
- A method has at most 255 parameters: an argument list is a tuple, and a
  tuple has at most 255 elements. `maxFields(N)` in `method(...)` promises
  a lower number for the method's lifetime.

**Get.** `psio::reflect<Bank>::method_arg_tuple<I>` is the type to encode,
decode and validate arguments as:

```cpp
// The arguments of a call are the tuple of the parameters, with the
// annotations and the cap of method(...).
const TransferArgs args{AccountId{1}, AccountId{2}, 500, "rent"};
const auto         bytes = psio::to_pssz(args);
CHECK(bytes.size() == 2 + 8 + 8 + 8 + 2 + 4);                 // 16-bit header and slot
const auto back = psio::from_pssz<TransferArgs>(bytes);       // validated: use for untrusted calls
CHECK(std::get<3>(back) == "rent");

// A memo of 81 bytes has no encoding, and a frame holding one is refused.
CHECK_THROWS_AS(psio::to_pssz(TransferArgs{AccountId{1}, AccountId{2}, 500, std::string(81, 'm')}),
                psio::codec_exception);

// The return value is checked against returns(...).
CHECK(psio::check_method_result<Bank, 2>(std::string("USD")).ok());
CHECK_FALSE(psio::check_method_result<Bank, 2>(std::string(9, 'X')).ok());

// An optional argument appended under the cap: old callers still work.
CHECK(psio::to_pssz(TransferArgsV2{AccountId{1}, AccountId{2}, 500, "rent", std::nullopt}) == bytes);
CHECK(std::get<4>(psio::from_pssz<TransferArgsV2>(bytes)) == std::nullopt);
```

In text, an argument list whose parameters are all named is written as a
JSON object and read as that object or as the positional array
(`tut-9-json`); binary is always the positional tuple:

```cpp
// In text a named argument list is written as an object keyed by parameter
// name; binary stays the positional tuple.
CHECK(psio::to_json(args) == R"({"from":1,"to":2,"amount":500,"memo":"rent"})");
// It is read as that object (members in any order) or as the positional array.
CHECK(std::get<3>(psio::from_json<TransferArgs>(R"({"memo":"rent","amount":500,"to":2,"from":1})")) == "rent");
CHECK(std::get<3>(psio::from_json<TransferArgs>(R"([1,2,500,"rent"])")) == "rent");
// The bounds hold in both forms; a missing required argument is an error.
CHECK_FALSE(psio::validate_json<TransferArgs>(R"({"from":1,"to":2,"amount":500})").ok());
CHECK_FALSE(psio::validate_json<TransferArgs>(R"([1,2,500])").ok());
// A missing optional argument is None.
CHECK(std::get<4>(psio::from_json<TransferArgsV2>(R"({"from":1,"to":2,"amount":500,"memo":"rent"})")) ==
      std::nullopt);
```

A method evolves like a record: append `std::optional` parameters, under a
cap declared from the first version (`tut-9b`):

```cpp
struct BankV2
{
   void transfer(AccountId from, AccountId to, std::uint64_t amount, std::string memo,
                 std::optional<std::uint64_t> fee);
   PSIO_REFLECT(BankV2,
                method(transfer, from, to, amount, attr(memo, utf8 | max<80>), fee,
                       maxDynamicData(1024)))
};
using TransferArgsV2 = psio::reflect<BankV2>::method_arg_tuple<0>;
static_assert(psio::pssz_evolves_from<TransferArgs, TransferArgsV2>);
```

A parameter of a first-class type brings its bound with it. With the `Memo`
of Step 8 the capped transfer needs no `attr` and is the same argument
list, byte for byte (`tut-9c`):

```cpp
struct Payments
{
   void pay(AccountId from, AccountId to, std::uint64_t amount, Memo memo);
   PSIO_REFLECT(Payments, method(pay, from, to, amount, memo, maxDynamicData(1024)))
};
using PayArgs = psio::reflect<Payments>::method_arg_tuple<0>;
// The bound travels with the type: no attr on the parameter, and the argument
// list is the one of Bank::transfer.
static_assert(psio::max_encoded_size<PayArgs>() == psio::max_encoded_size<TransferArgs>());
static_assert(psio::max_dynamic_data_v<PayArgs> == 1024);
static_assert(psio::auto_pssz_width_v<PayArgs> == 2);
```

```cpp
// A Memo parameter: the same bytes and the same text as Bank::transfer.
const PayArgs pay{AccountId{1}, AccountId{2}, 500, Memo{"rent"}};
CHECK(psio::to_pssz(pay) == bytes);
CHECK(psio::to_json(pay) == psio::to_json(args));
CHECK_THROWS_AS(psio::to_pssz(PayArgs{AccountId{1}, AccountId{2}, 500, Memo{std::string(81, 'm')}}),
                psio::codec_exception);
```

A result is encoded as the type its `returns(...)` describes, with
`psio::encode_method_result<T, I>(value, out)` /
`psio::decode_method_result<T, I>(bytes, out)`
([annotations.md §5.11](annotations.md#511-methodname-)).

**Check.** The assertions above; and the schema of the interface carries
the same types (`psio::SchemaBuilder{}.insert_interface<Bank>()`,
`psio::emit_wit` writes `@maxDynamicData(1024)` above `transfer`).

Action arguments arrive from outside: always read them with the validating
`from_pssz<Args>(bytes)`.

## Step 10 — The same declarations in Rust, TypeScript and Python

These snippets are **not compile-checked from this document**
([annotations.md](annotations.md#rust-typescript-and-python-snippets)).
The declarations produce the same 33-byte frame as Step 2 in every
language; each language's own test pins it.

Rust (`rust/psio`): `#[psio(...)]` on the field; `String` is UTF-8 and
`RawString` is raw bytes, so there is no `utf8` key.

```rust
use psio::pssz::{auto_pssz_width, pssz_evolves_from};
use psio::{JsonText, PsszPack, PsszUnpack, ToPsszSchema};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
#[pssz(max_dynamic_data = 1024)]
struct AccountV1 {
    id: u64,
    #[psio(max = 32)]
    owner: String,
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
#[pssz(max_dynamic_data = 1024)]
struct AccountV2 {
    id: u64,
    #[psio(max = 32)]
    owner: String,
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
    #[psio(max = 64)]
    email: Option<String>, // new: optional, last
}

const _: () = {
    assert!(auto_pssz_width::<AccountV1>() == 2);
    assert!(pssz_evolves_from::<AccountV1, AccountV2>());
};

#[psio::interface]
pub trait Bank {
    #[pssz(max_dynamic_data = 1024)]
    fn transfer(&mut self, from: u64, to: u64, amount: u64, #[psio(max = 80)] memo: String);
    #[psio(returns(max = 8))]
    fn symbol(&self, token: u32) -> String;
}
```

TypeScript (`js/`): the bounds are the options of `field`; a `rawString`
with `utf8: true` is a JS `string`.

```typescript
import { argumentTuple, field, list, optional, rawString, record, u64 } from 'psio';

const accountFields = {
  id: u64,
  owner: field(rawString, { utf8: true, max: 32 }),
  tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),
  balance: u64,
};
const AccountV1 = record('AccountV1', accountFields, { maxDynamicData: 1024 });
const AccountV2 = record('AccountV2', {
  ...accountFields,
  email: field(optional(rawString), { utf8: true, max: 64 }),   // new: optional, last
}, { maxDynamicData: 1024 });
AccountV1.width;                                                // 2

const Transfer = argumentTuple(
  { from: u64, to: u64, amount: u64, memo: field(rawString, { utf8: true, max: 80 }) },
  { maxDynamicData: 1024 });
```

Python (`python/`): the bounds are `typing.Annotated` metadata on the
hints of a `@dataclass`; `str` is UTF-8 and `psio.RawString` is raw bytes;
the cap is a decorator.

```python
from dataclasses import dataclass
from typing import Annotated, NewType, Optional, Protocol
import psio
from psio import u32, u64

@psio.record(max_dynamic_data=1024)
@dataclass
class AccountV1:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64

@psio.record(max_dynamic_data=1024)
@dataclass
class AccountV2:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64
    email: Annotated[Optional[str], psio.max(64)] = None      # new: optional, last

psio.codec(AccountV1).width                                    # 2

AccountId = NewType("AccountId", u64)

@psio.interface(name="bank")
class Bank(Protocol):
    @psio.method(max_dynamic_data=1024)
    def transfer(self, from_: AccountId, to: AccountId, amount: u64,
                 memo: Annotated[str, psio.max(80)]) -> None: ...

    def symbol(self, token: u32) -> Annotated[str, psio.max(8)]: ...
```

Differences to know: Rust has no `exact` / `min`; TypeScript and Python
have no `pssz_evolves_from` (compare the widths and test old bytes against
the new type), and Rust's refuses every change of a bound where C++ allows
widening; TypeScript has no return-value annotation. The C++ behaviour of
the latest consistency round — enforced `maxFields`, `sorted` / `unique`,
the array form of JSON arguments, the round-trip rule of the name coders —
is marked "not yet available" per language in
[annotations.md §4](annotations.md#4-master-table) and
[§4.1](annotations.md#41-tooling-per-language) until its mirror is
committed.

## Step 11 — Reading: views, validated and trusted

**Write** nothing new. Choose how to read (`tut-11-run`):

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

**Get.**

- `psio::validate_pssz<T>(bytes)` checks everything: structure, bounds,
  `utf8`, the cap, invariants. It returns a status and never throws.
- `psio::from_pssz<T>(bytes)` validates, then decodes.
- `psio::pssz_view<T>{bytes}` reads members in place.
- `psio::from_pssz<T>(psio::trusted{bytes})` skips the validation pass.

**The trust rule.** Validate untrusted input — action arguments, data from
another service, anything a user supplied. Bytes your own service wrote
and stored may be read trusted or through a view. Views and the trusted
decode do not protect you (`tut-11-limits`):

```cpp
// A view checks nothing. Over bytes that were not validated it reads
// whatever is there: here a 33-byte owner under max<32>.
Account over{1, std::string(33, 'x'), {}, 0};
const auto bad = unchecked(over);
CHECK_FALSE(psio::validate_pssz<Account>(bad).ok());
const psio::pssz_view<Account> blind{bad};
CHECK(blind.get<1>().view_().size() == 33);

// validated_pssz_view builds the view only if the frame validates.
std::optional<psio::pssz_view<Account>> checked;
bool                                    has_unknown = false;
CHECK_FALSE(psio::validated_pssz_view(psio::pssz{}, std::span<const char>(bad), checked, has_unknown)
               .ok());
CHECK_FALSE(checked.has_value());

// The trusted decode skips invariants (and all structural checks).
std::vector<char> lower = psio::to_pssz(Symbol{"USD"});
lower.back()            = 'd';
CHECK(psio::from_pssz<Symbol>(psio::trusted{lower}).code == "USd");
CHECK_THROWS_AS(psio::from_pssz<Symbol>(lower), psio::codec_exception);
```

**Const safety.** A read view cannot write, and there is no conversion
from a read view to one that writes (`tut-11-const`):

```cpp
template <typename V>
concept can_set = requires(V v) { v.template get<0>().set(std::uint64_t{1}); };
static_assert(!can_set<psio::pssz_view<ref::Point>>);
static_assert(std::is_same_v<psio::pssz_view<const ref::Point>, psio::pssz_view<ref::Point>>);
```

Writing in place is a separate, explicit type, `psio::pssz_mutable_view<T>`,
limited to overwriting fixed-size values
([annotations.md §7.4](annotations.md#74-const-safety)).

## Where to go next

- One section per annotation and per kind of type:
  [annotations.md](annotations.md).
- The checklist for adding or changing a type:
  [annotations-for-agents.md](annotations-for-agents.md#checklist-adding-a-type).
- The wire format: [pssz-spec.md](pssz-spec.md); evolution in depth:
  [pssz-optional-evolution.md](pssz-optional-evolution.md).
