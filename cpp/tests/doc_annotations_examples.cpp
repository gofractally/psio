// The compiled examples of docs/annotations.md, docs/tutorial-annotations.md
// and docs/annotations-for-agents.md.
//
// Every C++ snippet in those documents is a run of consecutive lines of this
// file (with the common indentation removed); the documents name the section
// (`tut-N`, `ref-...`) a snippet comes from. Change a snippet here and in the
// document together. Declarations are written at column 0 inside their
// namespace so that the text in the documents is the text compiled here.
//
// What is checked: offset widths and inferred bounds (static_assert), round
// trips, a rejection at bound + 1, the JSON / I-JSON text, key bytes, the
// Schema IR the annotations produce, and the evolution check.

#include <psio/emit_wit.hpp>
#include <psio/helpers.hpp>
#include <psio/invariant.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/max_size.hpp>
#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/structural.hpp>
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace
{
   std::string hex(std::span<const char> bytes)
   {
      static constexpr char digits[] = "0123456789abcdef";
      std::string           out;
      for (unsigned char c : bytes)
      {
         if (!out.empty())
            out += ' ';
         out += digits[c >> 4];
         out += digits[c & 15];
      }
      return out;
   }
   // psio::to_pssz throws for a value that has no encoding.
   template <typename T>
   bool encodes(const T& value)
   {
      try
      {
         (void)psio::to_pssz(value);
         return true;
      }
      catch (const psio::codec_exception&)
      {
         return false;
      }
   }
   // The frame a writer that checks nothing would produce (test only).
   template <typename T>
   std::vector<char> unchecked(const T& value)
   {
      return psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, value);
   }
   template <typename T>
   std::string schema_text(std::string_view name)
   {
      return psio::to_ijson(psio::SchemaBuilder{}.insert<T>(std::string{name}).build());
   }
   bool contains(std::string_view text, std::string_view part)
   {
      return text.find(part) != std::string_view::npos;
   }
}  // namespace

// ══ Tutorial ═══════════════════════════════════════════════════════════════
namespace tut
{
// ── tut-1: the plain declaration ───────────────────────────────────────────
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

// ── tut-2a: bound every variable-size part ─────────────────────────────────
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
// header 4 + id 8 + owner (slot 4 + 32) + tags (slot 4 + 64 x (16 + slot 4)) + balance 8
static_assert(psio::max_encoded_size<AccountWide>() == 4 + 8 + (4 + 32) + (4 + 64 * (16 + 4)) + 8);
static_assert(psio::max_encoded_size<AccountWide>() == 1340);   // <= 65535: 16-bit offsets
static_assert(psio::auto_pssz_width_v<AccountWide> == 2);

// ── tut-2b: a tighter bound, the next width down ───────────────────────────
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
static_assert(psio::max_encoded_size<Account>() == 4 + 8 + (4 + 32) + (4 + 8 * (16 + 4)) + 8);
static_assert(psio::max_encoded_size<Account>() == 220);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);

// ── tut-2c: bounding only part of a member bounds nothing ──────────────────
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

// ── tut-3: an explicit cap reserves room to grow ───────────────────────────
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
// The cap rules: 16-bit offsets, although today's fields need at most 220 bytes.
static_assert(psio::max_dynamic_data_v<AccountV1> == 1024);
static_assert(psio::auto_pssz_width_v<AccountV1> == 2);

// ── tut-4a: a compatible revision ──────────────────────────────────────────
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

// ── tut-4b: revisions that are not compatible ──────────────────────────────
// (1) No cap: the same new field pushes the inferred bound from 220 to 289,
//     the width from 1 to 2, and every existing record becomes unreadable.
struct AccountGrown
{
   std::uint64_t              id = 0;
   std::string                owner;
   std::vector<std::string>   tags;
   std::uint64_t              balance = 0;
   std::optional<std::string> email;
   PSIO_REFLECT(AccountGrown,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                attr(email, utf8 | max<64>))
};
static_assert(psio::max_encoded_size<AccountGrown>() == 220 + (4 + 1 + 64));
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

// ── tut-4c: what the check does not see ────────────────────────────────────
// A changed bound that keeps the width is accepted by pssz_evolves_from, but
// a reader with the tighter bound rejects records the old writer produced.
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
static_assert(psio::pssz_evolves_from<AccountV1, AccountTighter>);   // not a proof of compatibility

// ── tut-5: raw bytes or UTF-8 text ─────────────────────────────────────────
struct Note
{
   friend bool operator==(const Note&, const Note&) = default;
   std::string blob;   // raw bytes: any byte string
   std::string text;   // UTF-8 text
   PSIO_REFLECT(Note, attr(blob, max<64>), attr(text, utf8 | max<64>))
};

// ── tut-6: JSON presentation ───────────────────────────────────────────────
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

// ── tut-7: keys ────────────────────────────────────────────────────────────
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

// ── tut-8a: a transparent wrapper ──────────────────────────────────────────
struct AccountId
{
   friend auto operator<=>(const AccountId&, const AccountId&) = default;
   std::uint64_t value = 0;
   PSIO_REFLECT(AccountId, transparent(), value)
};
// It is its member: fixed-size, 8 bytes, no header.
static_assert(psio::is_transparent_v<AccountId>);
static_assert(psio::max_encoded_size<AccountId>() == 8);

// ── tut-8b: an invariant ───────────────────────────────────────────────────
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

// ── tut-9a: methods ────────────────────────────────────────────────────────
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
// open has no cap: header 4 + owner (4 + 32) + tags (4 + 8 x (16 + 4)) = 204, 8-bit.
static_assert(psio::max_encoded_size<OpenArgs>() == 4 + (4 + 32) + (4 + 8 * (16 + 4)));
static_assert(psio::auto_pssz_width_v<OpenArgs> == 1);
// transfer is capped: 16-bit for its lifetime.
static_assert(psio::max_dynamic_data_v<TransferArgs> == 1024);
static_assert(psio::auto_pssz_width_v<TransferArgs> == 2);

// ── tut-9b: a method gains an argument ─────────────────────────────────────
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
}  // namespace tut

// ══ Reference ══════════════════════════════════════════════════════════════
namespace ref
{
// ── ref-spellings: one constraint, three spellings ─────────────────────────
struct InReflect   // inside PSIO_REFLECT
{
   friend bool operator==(const InReflect&, const InReflect&) = default;
   std::string                title;
   std::vector<std::uint32_t> ids;
   PSIO_REFLECT(InReflect, attr(title, utf8 | max<32>), attr(ids, max<4>), maxDynamicData(200))
};

struct Separate    // separate statements, after the type
{
   std::string                title;
   std::vector<std::uint32_t> ids;
};
PSIO_REFLECT(Separate, title, ids)
PSIO_FIELD_ATTRS(Separate, title, psio::utf8 | psio::max<32>)
PSIO_FIELD_ATTRS(Separate, ids, psio::max<4>)
PSIO_TYPE_ATTRS(Separate, psio::max_dynamic_data_spec{200})

struct InTypes     // wrapper types
{
   psio::utf8_string<32>            title;
   psio::bounded<std::uint32_t, 4>  ids;
   PSIO_REFLECT(InTypes, title, ids, maxDynamicData(200))
};

static_assert(psio::auto_pssz_width_v<InReflect> == 1);
static_assert(psio::auto_pssz_width_v<Separate> == 1);
static_assert(psio::auto_pssz_width_v<InTypes> == 1);

// ── ref-length: max, exact, min ────────────────────────────────────────────
struct Limits
{
   friend bool operator==(const Limits&, const Limits&) = default;
   std::vector<std::uint8_t>  digest;
   std::string                code;
   std::vector<std::uint32_t> ids;
   PSIO_REFLECT(Limits,
                attr(digest, exact<32>),                                   // exactly 32 bytes
                attr(code, psio::length_bound{.max = 8, .min = 2}),        // 2..8 bytes
                attr(ids, max<4>))                                         // <= 4 elements
};
// max<N> and exact<N> give the inferred bound its N; min<N> alone bounds nothing.
// header 4 + digest (4 + 32) + code (4 + 8) + ids (4 + 4 x 4)
static_assert(psio::max_encoded_size<Limits>() == 4 + (4 + 32) + (4 + 8) + (4 + 4 * 4));
static_assert(psio::auto_pssz_width_v<Limits> == 1);

struct AtLeast
{
   std::string code;
   PSIO_REFLECT(AtLeast, attr(code, min<2>))
};
static_assert(!psio::max_encoded_size<AtLeast>().has_value());   // no upper bound: 32-bit

// ── ref-utf8: the annotation and the wrapper type ──────────────────────────
struct Names
{
   friend bool operator==(const Names&, const Names&) = default;
   std::string raw;       // raw bytes        (Rust RawString)
   std::string text;      // UTF-8            (Rust String)
   std::string label;     // UTF-8, <= 16 bytes
   PSIO_REFLECT(Names, raw, attr(text, utf8), attr(label, utf8 | max<16>))
};
struct NamesW
{
   std::string           raw;
   psio::utf8_string<>   text;
   psio::utf8_string<16> label;
   PSIO_REFLECT(NamesW, raw, text, label)
};

// ── ref-bytes: byte sequences ──────────────────────────────────────────────
struct Blob
{
   friend bool operator==(const Blob&, const Blob&) = default;
   std::vector<std::uint8_t>   data;      // variable: hex in I-JSON
   std::array<std::uint8_t, 4> tag{};     // fixed, 4 bytes, no offset
   PSIO_REFLECT(Blob, attr(data, max<64>), tag)
};
static_assert(psio::max_encoded_size<Blob>() == 4 + (4 + 64) + 4);

// ── ref-sorted: declared, not enforced ─────────────────────────────────────
struct Ranked
{
   std::vector<std::uint32_t> scores;
   PSIO_REFLECT(Ranked, attr(scores, sorted | max<4>))
};

// ── ref-positions: element, keys, values, at<I> ────────────────────────────
struct Post
{
   friend bool operator==(const Post&, const Post&) = default;
   std::string                               title;
   std::vector<std::string>                  tags;
   std::map<std::string, std::uint64_t>      votes;
   std::optional<std::vector<std::uint8_t>>  thumb;
   std::pair<std::string, std::uint32_t>     author;
   PSIO_REFLECT(Post,
                attr(title, utf8 | max<64>),                       // UTF-8, <= 64 bytes
                attr(tags, max<8> | element(utf8 | max<16>)),      // <= 8 strings of <= 16 bytes
                attr(votes, max<32> | keys(utf8 | max<16>)),       // <= 32 entries, keys <= 16 bytes
                attr(thumb, max<1024>),                            // when present: <= 1024 bytes
                attr(author, at<0>(utf8 | max<32>)))               // pair / tuple element 0
};
static_assert(psio::auto_pssz_width_v<Post> == 2);   // every part is bounded: 16-bit offsets

struct Grid   // a list of lists
{
   friend bool operator==(const Grid&, const Grid&) = default;
   std::vector<std::vector<std::uint16_t>> rows;
   PSIO_REFLECT(Grid, attr(rows, max<3> | element(max<4>)))
};
// header 4 + slot 4 + 3 x (4 x 2 + slot 4)
static_assert(psio::max_encoded_size<Grid>() == 4 + 4 + 3 * (4 * 2 + 4));
static_assert(psio::auto_pssz_width_v<Grid> == 1);

struct Choice   // a variant alternative
{
   friend bool operator==(const Choice&, const Choice&) = default;
   std::variant<std::uint32_t, std::string> v;
   PSIO_REFLECT(Choice, attr(v, at<1>(utf8 | max<16>)))
};
// header 4 + slot 4 + selector 1 + the widest alternative 16
static_assert(psio::max_encoded_size<Choice>() == 4 + 4 + 1 + 16);

// ── ref-integers: integers in JSON ─────────────────────────────────────────
struct Amounts
{
   friend bool operator==(const Amounts&, const Amounts&) = default;
   std::uint32_t count  = 0;
   std::uint64_t small  = 0;
   std::uint64_t large  = 0;
   std::int64_t  signed_ = 0;
   bool          frozen = false;
   PSIO_REFLECT(Amounts, count, small, large, signed_, frozen)
};
static_assert(psio::max_encoded_size<Amounts>() == 4 + 4 + 8 + 8 + 8 + 1);

struct AmountsNumbers   // every integer member a number in I-JSON
{
   std::uint64_t small = 0;
   std::uint64_t large = 0;
};
PSIO_REFLECT(AmountsNumbers, small, large)
PSIO_TYPE_ATTRS(AmountsNumbers, psio::ijson_number)

// ── ref-enum: enums ────────────────────────────────────────────────────────
enum class Status : std::uint8_t { pending, active = 2, closed };
PSIO_REFLECT_ENUM(Status, pending, active, closed)

struct Ticket
{
   friend bool operator==(const Ticket&, const Ticket&) = default;
   std::uint32_t id = 0;
   Status        status{};
   PSIO_REFLECT(Ticket, id, status, definitionWillNotChange())
};
// An enum is its underlying integer: 4 + 1 bytes, and no header (DWNC).
static_assert(psio::max_encoded_size<Ticket>() == 5);

// ── ref-arrays: fixed arrays ───────────────────────────────────────────────
struct Slots
{
   friend bool operator==(const Slots&, const Slots&) = default;
   std::array<std::uint32_t, 4> counts{};   // fixed: 16 bytes inline
   std::array<std::string, 3>   names;      // variable elements: its own offset table
   PSIO_REFLECT(Slots, counts, attr(names, element(utf8 | max<16>)))
};
// header 4 + 16 + names (slot 4 + 3 x (16 + slot 4))
static_assert(psio::max_encoded_size<Slots>() == 4 + 16 + (4 + 3 * (16 + 4)));
static_assert(psio::auto_pssz_width_v<Slots> == 1);

// ── ref-optional: optionals ────────────────────────────────────────────────
struct Profile
{
   friend bool operator==(const Profile&, const Profile&) = default;
   std::uint64_t                id = 0;
   std::optional<std::string>   bio;     // the annotation applies to the payload
   std::optional<std::uint32_t> age;
   PSIO_REFLECT(Profile, id, attr(bio, utf8 | max<100>), age)
};
// header 4 + id 8 + bio (slot 4 + selector 1 + 100) + age (slot 4 + selector 1 + 4)
static_assert(psio::max_encoded_size<Profile>() == 4 + 8 + (4 + 1 + 100) + (4 + 1 + 4));

// ── ref-maps: maps and sets ────────────────────────────────────────────────
struct Book
{
   friend bool operator==(const Book&, const Book&) = default;
   std::map<std::string, std::uint64_t> balances;
   std::set<std::uint32_t>              members;
   PSIO_REFLECT(Book, attr(balances, max<4> | keys(utf8 | max<8>)), attr(members, max<4>))
};
// balances: an entry is header 4 + key (4 + 8) + 8 = 24; 4 x (24 + slot 4)
static_assert(psio::max_encoded_size<Book>() == 4 + (4 + 4 * (24 + 4)) + (4 + 4 * 4));
static_assert(psio::auto_pssz_width_v<Book> == 1);

// ── ref-nested: nested values ──────────────────────────────────────────────
struct Memo
{
   friend bool operator==(const Memo&, const Memo&) = default;
   std::string text;
   PSIO_REFLECT(Memo, attr(text, utf8 | max<32>))
};
struct Action
{
   std::uint64_t      sender = 0;
   psio::nested<Memo> args;          // on the wire: a byte list holding a Memo
   PSIO_REFLECT(Action, sender, attr(args, max<64>))
};
// header 4 + sender 8 + args (slot 4 + 64)
static_assert(psio::max_encoded_size<Action>() == 4 + 8 + (4 + 64));
static_assert(psio::auto_pssz_width_v<Action> == 1);

// ── ref-records: record-level keywords ─────────────────────────────────────
struct Point   // fixed layout: no header, cannot gain fields
{
   friend bool operator==(const Point&, const Point&) = default;
   std::int32_t x = 0;
   std::int32_t y = 0;
   PSIO_REFLECT(Point, x, y, definitionWillNotChange())
};
struct PointOpen   // extensible: a header, may gain optional fields
{
   std::int32_t x = 0;
   std::int32_t y = 0;
   PSIO_REFLECT(PointOpen, x, y)
};
static_assert(psio::is_dwnc_v<Point> && !psio::is_dwnc_v<PointOpen>);
static_assert(psio::max_encoded_size<Point>() == 8);
static_assert(psio::max_encoded_size<PointOpen>() == 4 + 8);

struct Line   // a child's cap is what the parent reserves for it
{
   tut::AccountV1 a;   // maxDynamicData(1024)
   PSIO_REFLECT(Line, a)
};
static_assert(psio::max_encoded_size<Line>() == 4 + (4 + 1024));
static_assert(psio::auto_pssz_width_v<Line> == 2);

struct Capped
{
   std::string s;
   PSIO_REFLECT(Capped, s, maxDynamicData(16), maxFields(8))
};
static_assert(psio::max_dynamic_data_v<Capped> == 16);
static_assert(psio::max_fields_v<Capped> == 8);
static_assert(psio::auto_pssz_width_v<Capped> == 1);
struct CappedHolder
{
   Capped inner;
   PSIO_REFLECT(CappedHolder, inner)
};

// ── ref-transparent: transparent wrappers ──────────────────────────────────
struct PublicKey
{
   friend auto operator<=>(const PublicKey&, const PublicKey&) = default;
   psio::bounded<std::uint8_t, 65> data;   // the bound is in the member's type
   PSIO_REFLECT(PublicKey, transparent(), data)
};
static_assert(psio::max_encoded_size<PublicKey>() == 65);

// ── ref-adapter: a custom text form ────────────────────────────────────────
struct Color
{
   friend bool operator==(const Color&, const Color&) = default;
   std::uint8_t r = 0, g = 0, b = 0;
   PSIO_REFLECT(Color, r, g, b, definitionWillNotChange())
};
struct color_text   // "#rrggbb"
{
   static std::size_t packsize(const Color&) noexcept { return 7; }
   template <typename Sink>
   static void encode(const Color& c, Sink& s)
   {
      static constexpr char d[] = "0123456789abcdef";
      const char            text[] = {'#',         d[c.r >> 4], d[c.r & 15], d[c.g >> 4],
                                      d[c.g & 15], d[c.b >> 4], d[c.b & 15]};
      s.insert(s.end(), text, text + 7);
   }
   static int digit(char c) noexcept
   {
      return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
   }
   static psio::codec_status validate(std::span<const char> t) noexcept
   {
      if (t.size() != 7 || t[0] != '#')
         return psio::codec_fail("color: expected #rrggbb", 0, "Color");
      for (std::size_t i = 1; i < 7; ++i)
         if (digit(t[i]) < 0)
            return psio::codec_fail("color: expected #rrggbb", 0, "Color");
      return psio::codec_ok();
   }
   static psio::codec_status validate_strict(std::span<const char> t) noexcept
   {
      return validate(t);
   }
   static Color decode(std::span<const char> t) noexcept   // called on validated text
   {
      const auto byte = [&](std::size_t i) {
         return static_cast<std::uint8_t>(digit(t[i]) * 16 + digit(t[i + 1]));
      };
      return Color{byte(1), byte(3), byte(5)};
   }
};
}  // namespace ref
PSIO_ADAPTER(ref::Color, psio::text_category, ref::color_text)
namespace ref
{
struct Pixel
{
   friend bool operator==(const Pixel&, const Pixel&) = default;
   std::uint16_t x = 0;
   Color         color;
   PSIO_REFLECT(Pixel, x, color)
};

// ── ref-time: time values ──────────────────────────────────────────────────
struct Event
{
   friend bool operator==(const Event&, const Event&) = default;
   std::chrono::sys_time<std::chrono::milliseconds> at;    // a time point: a date in text
   std::chrono::seconds                             ttl{}; // a duration: its tick count
   PSIO_REFLECT(Event, at, ttl, definitionWillNotChange())
};
static_assert(psio::max_encoded_size<Event>() == 8 + 8);

// ── ref-field-number: protobuf field numbers ───────────────────────────────
struct Message
{
   std::uint32_t code = 0;
   std::string   text;
   PSIO_REFLECT(Message, attr(code, field<3>), attr(text, field<7>))
};
}  // namespace ref

using namespace tut;

// ══ Tutorial: run-time checks ══════════════════════════════════════════════

TEST_CASE("tutorial 2: a bounded record round-trips and refuses bound + 1", "[doc][tut-2]")
{
   // ── tut-2-run ──
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
   // ── /tut-2-run ──

   // The frame: 1-byte header and slots; the list's own table is 4 bytes wide.
   // [W1] (docs/annotations.md, "Pending wire change"): 08 00 00 00 and
   // 0b 00 00 00 are the list's table.
   CHECK(hex(bytes) ==
         "12 07 00 00 00 00 00 00 00 12 17 64 00 00 00 00 00 00 00 "
         "61 6c 69 63 65 08 00 00 00 0b 00 00 00 76 69 70 62 65 74 61");
   CHECK(bytes.size() == 39);
   // The same value as the plain type: 4-byte header and slots.
   CHECK(psio::to_pssz(AccountPlain{7, "alice", {"vip", "beta"}, 100}).size() == 48);

   // Readers refuse the frames an unchecking writer would have produced.
   for (const auto& bad : {unchecked(long_owner), unchecked(nine_tags), unchecked(not_text)})
   {
      CHECK_FALSE(psio::validate_pssz<Account>(bad).ok());
      CHECK_THROWS_AS(psio::from_pssz<Account>(bad), psio::codec_exception);
   }
   try
   {
      (void)psio::to_pssz(long_owner);
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(std::string_view{e.what()} == "pssz: member length_bound violated");
   }
   try
   {
      (void)psio::to_pssz(not_text);
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(std::string_view{e.what()} == "utf8_spec: invalid leading byte");
   }
}

TEST_CASE("tutorial 3: the cap is enforced on the encoded size", "[doc][tut-3]")
{
   // ── tut-3-run ──
   const AccountV1 alice{7, "alice", {"vip", "beta"}, 100};
   // 2-byte header and slots now: 3 bytes more than the 8-bit Account.
   CHECK(psio::to_pssz(alice).size() == psio::to_pssz(Account{7, "alice", {"vip", "beta"}, 100}).size() + 3);
   CHECK(psio::from_pssz<AccountV1>(psio::to_pssz(alice)) == alice);
   // ── /tut-3-run ──

   // ── tut-3-cap ──
   // A cap is the limit of the whole encoded value, header included.
   CHECK(psio::to_pssz(ref::Capped{std::string(14, 'x')}).size() == 16);     // 1 + 1 + 14
   CHECK_THROWS_AS(psio::to_pssz(ref::Capped{std::string(15, 'x')}), psio::codec_exception);
   auto frame = psio::to_pssz(ref::Capped{std::string(14, 'x')});
   frame.push_back('x');                                                     // 17 bytes
   CHECK_FALSE(psio::validate_pssz<ref::Capped>(frame).ok());
   // ── /tut-3-cap ──
   try
   {
      (void)psio::to_pssz(ref::Capped{std::string(15, 'x')});
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(std::string_view{e.what()} == "pssz: encoded size exceeds maxDynamicData cap");
   }
}

TEST_CASE("tutorial 4: an appended optional field is compatible both ways", "[doc][tut-4]")
{
   // ── tut-4-run ──
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
   // ── /tut-4-run ──

   // tut-4c: the old writer's record is refused by the tightened reader.
   const AccountV1 wide_owner{7, std::string(32, 'o'), {}, 0};
   CHECK_FALSE(psio::validate_pssz<AccountTighter>(psio::to_pssz(wide_owner)).ok());
}

TEST_CASE("tutorial 5: raw bytes and UTF-8 text", "[doc][tut-5]")
{
   // ── tut-5-run ──
   // A raw string holds any bytes in the binary formats.
   const Note binary{"\xff\xfe", "caf\xc3\xa9"};
   CHECK(psio::from_pssz<Note>(psio::to_pssz(binary)) == binary);
   // A utf8 string does not.
   CHECK_THROWS_AS(psio::to_pssz(Note{"", "\xff"}), psio::codec_exception);
   // JSON text is UTF-8: a raw string that is not UTF-8 has no JSON.
   CHECK_THROWS_AS(psio::to_json(binary), psio::codec_exception);
   CHECK(psio::to_json(Note{"bytes", "caf\xc3\xa9"}) == "{\"blob\":\"bytes\",\"text\":\"caf\xc3\xa9\"}");
   // ── /tut-5-run ──
   const auto frame = unchecked(Note{"", "\xff"});
   CHECK_FALSE(psio::validate_pssz<Note>(frame).ok());
}

TEST_CASE("tutorial 6: JSON presentation", "[doc][tut-6]")
{
   // ── tut-6-run ──
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
   // ── /tut-6-run ──
}

TEST_CASE("tutorial 7: keys", "[doc][tut-7]")
{
   // ── tut-7-run ──
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

   // Limit: the key ENCODER does not check member bounds; the key READER does.
   const auto too_long = psio::to_key(OwnerKey{std::string(33, 'x'), 7});   // no error
   CHECK_FALSE(psio::validate_key<OwnerKey>(too_long).ok());
   CHECK_THROWS_AS(psio::from_key<OwnerKey>(too_long), psio::codec_exception);
   // ── /tut-7-run ──
   const auto bad_utf8 = psio::to_key(OwnerKey{"\xff", 7});
   CHECK_FALSE(psio::validate_key<OwnerKey>(bad_utf8).ok());
}

TEST_CASE("tutorial 8: transparent wrappers and invariants", "[doc][tut-8]")
{
   // ── tut-8-run ──
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
   // ── /tut-8-run ──
}

TEST_CASE("tutorial 9: method arguments", "[doc][tut-9]")
{
   // ── tut-9-run ──
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
   // ── /tut-9-run ──

   CHECK_FALSE(psio::validate_pssz<TransferArgs>(
                  unchecked(TransferArgs{AccountId{1}, AccountId{2}, 500, std::string(81, 'm')}))
                  .ok());

   // The interface schema and WIT.
   const psio::Schema schema = psio::SchemaBuilder{}.insert_interface<Bank>().build();
   const std::string  text   = psio::to_ijson(schema);
   CHECK(contains(text, R"("name":"transfer")"));
   CHECK(contains(text, R"("attributes":[{"name":"maxDynamicData","value":"1024"}])"));
   CHECK(contains(text, R"("maxCount":80}}},"id":"utf8")"));
   const std::string wit = psio::emit_wit(schema);
   CHECK(contains(wit, "@maxDynamicData(1024)\n"));
   CHECK(contains(wit, "transfer: func(from: account-id, to: account-id, amount: u64, memo: string);"));
}

TEST_CASE("tutorial 11: views, validated and trusted reads", "[doc][tut-11]")
{
   // ── tut-11-run ──
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
   // ── /tut-11-run ──

   // ── tut-11-limits ──
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
   // ── /tut-11-limits ──
}

// A read view cannot write.
// ── tut-11-const ──
template <typename V>
concept can_set = requires(V v) { v.template get<0>().set(std::uint64_t{1}); };
static_assert(!can_set<psio::pssz_view<ref::Point>>);
static_assert(std::is_same_v<psio::pssz_view<const ref::Point>, psio::pssz_view<ref::Point>>);
// ── /tut-11-const ──

// ══ Reference: run-time checks ═════════════════════════════════════════════

TEST_CASE("reference: the three spellings are one type", "[doc][ref-spellings]")
{
   using namespace ref;
   const InReflect a{"title", {1, 2, 3}};
   const Separate  b{"title", {1, 2, 3}};
   InTypes         c;
   c.title = psio::utf8_string<32>{"title"};
   c.ids   = psio::bounded<std::uint32_t, 4>{1, 2, 3};
   CHECK(psio::to_pssz(a) == psio::to_pssz(b));
   CHECK(psio::to_pssz(a) == psio::to_pssz(c));
   CHECK(psio::to_json(a) == psio::to_json(b));
   CHECK(psio::to_json(a) == psio::to_json(c));
   CHECK(psio::to_key(a) == psio::to_key(b));
   CHECK(psio::to_key(a) == psio::to_key(c));
   CHECK_THROWS_AS(psio::to_pssz(Separate{std::string(33, 'x'), {}}), psio::codec_exception);
   // The wrapper also refuses at construction.
   CHECK_THROWS_AS(psio::utf8_string<32>{std::string(33, 'x')}, std::length_error);
   // The Schema IR of the two members is the same node in all three.
   for (const auto& text : {schema_text<InReflect>("T"), schema_text<Separate>("T"),
                            schema_text<InTypes>("T")})
   {
      CHECK(contains(text, R"("maxCount":32)"));
      CHECK(contains(text, R"("id":"utf8")"));
      CHECK(contains(text, R"("maxCount":4)"));
      CHECK(contains(text, R"({"name":"maxDynamicData","value":"200"})"));
   }
}

TEST_CASE("reference: length bounds", "[doc][ref-length]")
{
   using namespace ref;
   const Limits ok{std::vector<std::uint8_t>(32, 1), "ab", {1, 2, 3, 4}};
   CHECK(psio::from_pssz<Limits>(psio::to_pssz(ok)) == ok);
   Limits short_digest = ok;
   short_digest.digest.pop_back();                     // exact<32>: 31
   CHECK_FALSE(encodes(short_digest));
   Limits short_code = ok;
   short_code.code   = "a";                            // min 2
   CHECK_FALSE(encodes(short_code));
   Limits long_code = ok;
   long_code.code   = "123456789";                     // max 8
   CHECK_FALSE(encodes(long_code));
   Limits five = ok;
   five.ids.push_back(5);                              // max<4>
   CHECK_FALSE(encodes(five));
   for (const auto& bad : {unchecked(short_digest), unchecked(short_code), unchecked(long_code),
                           unchecked(five)})
      CHECK_FALSE(psio::validate_pssz<Limits>(bad).ok());

   // Schema IR: max alone is a bounded type; exact / min are the member
   // attribute lengthBound.
   const std::string text = schema_text<Limits>("Limits");
   CHECK(contains(text, R"("attributes":[{"name":"lengthBound","value":"exact=32"}])"));
   CHECK(contains(text, R"("attributes":[{"name":"lengthBound","value":"min=2,max=8"}])"));
   CHECK(contains(text, R"({"name":"ids","type":{"value":{"BoundedList":{"type":)"));
}

TEST_CASE("reference: utf8 annotation and wrapper", "[doc][ref-utf8]")
{
   using namespace ref;
   const Names n{"\xff", "caf\xc3\xa9", "ok"};
   NamesW      w;
   w.raw   = "\xff";
   w.text  = psio::utf8_string<>{"caf\xc3\xa9"};
   w.label = psio::utf8_string<16>{"ok"};
   CHECK(psio::to_pssz(n) == psio::to_pssz(w));
   CHECK(psio::from_pssz<Names>(psio::to_pssz(n)) == n);
   CHECK_FALSE(encodes(Names{"", "\xff", ""}));
   CHECK_FALSE(encodes(Names{"", "", "\xff"}));
   CHECK_FALSE(encodes(Names{"", "", std::string(17, 'x')}));
   // The schema says which is which.
   const std::string text = schema_text<Names>("Names");
   CHECK(contains(text, R"({"name":"text","type":{"value":{"Custom":{"type":{"value":{"List":)"));
   CHECK(contains(text, R"("maxCount":16}}},"id":"utf8")"));
   CHECK(contains(text, R"("id":"string")"));
}

TEST_CASE("reference: bytes", "[doc][ref-bytes]")
{
   using namespace ref;
   // ── ref-bytes-run ──
   const Blob b{{1, 255}, {0xde, 0xad, 0xbe, 0xef}};
   CHECK(psio::to_json(b) == R"({"data":[1,255],"tag":[222,173,190,239]})");
   CHECK(psio::to_ijson(b) == R"({"data":"01FF","tag":"DEADBEEF"})");
   CHECK(psio::from_ijson<Blob>(R"({"data":"01ff","tag":"deadbeef"})") == b);   // either case
   // ── /ref-bytes-run ──
   CHECK(psio::from_pssz<Blob>(psio::to_pssz(b)) == b);
   CHECK_FALSE(encodes(Blob{std::vector<std::uint8_t>(65, 0), {}}));
   CHECK(contains(schema_text<Blob>("Blob"), R"("id":"hex")"));
}

TEST_CASE("reference: sorted is declared, not enforced", "[doc][ref-sorted]")
{
   using namespace ref;
   // ── ref-sorted-run ──
   const Ranked unsorted{{3, 1, 2}};
   const auto   bytes = psio::to_pssz(unsorted);                                  // accepted
   CHECK(psio::validate_pssz<Ranked>(bytes).ok());                                // accepted
   CHECK(psio::validate_strict<Ranked>(psio::pssz{}, std::span<const char>(bytes)).ok());
   // ── /ref-sorted-run ──
   CHECK_FALSE(encodes(Ranked{{1, 2, 3, 4, 5}}));
}

TEST_CASE("reference: position specs", "[doc][ref-positions]")
{
   using namespace ref;
   Post p{"hello", {"a", "b"}, {{"alice", 3}}, std::vector<std::uint8_t>{1, 2}, {"bob", 9}};
   CHECK(psio::from_pssz<Post>(psio::to_pssz(p)) == p);
   CHECK(psio::to_ijson(p) ==
         R"({"title":"hello","tags":["a","b"],"votes":{"alice":3},"thumb":"0102","author":["bob",9]})");
   auto over = p;
   over.votes.emplace(std::string(17, 'k'), 1);
   CHECK_FALSE(encodes(over));
   over = p;
   over.thumb = std::vector<std::uint8_t>(1025, 0);
   CHECK_FALSE(encodes(over));
   over = p;
   over.author.first = std::string(33, 'a');
   CHECK_FALSE(encodes(over));
   over = p;
   over.thumb.reset();
   CHECK(encodes(over));

   CHECK(encodes(Grid{{{1, 2, 3, 4}, {}, {5}}}));
   CHECK_FALSE(encodes(Grid{{{1}, {1}, {1}, {1}}}));          // 4 rows
   CHECK_FALSE(encodes(Grid{{{1, 2, 3, 4, 5}}}));             // 5 in a row
   CHECK_FALSE(psio::validate_pssz<Grid>(unchecked(Grid{{{1, 2, 3, 4, 5}}})).ok());

   CHECK(encodes(Choice{std::string(16, 's')}));
   CHECK_FALSE(encodes(Choice{std::string(17, 's')}));
   CHECK(encodes(Choice{std::uint32_t{7}}));
}

TEST_CASE("reference: integers and bool in JSON", "[doc][ref-integers]")
{
   using namespace ref;
   // ── ref-integers-run ──
   const Amounts a{7, 9007199254740991ull, 9007199254740992ull, -9007199254740992ll, true};
   CHECK(psio::to_json(a) ==
         R"({"count":7,"small":9007199254740991,"large":9007199254740992,"signed_":-9007199254740992,"frozen":true})");
   CHECK(psio::to_ijson(a) ==
         R"({"count":7,"small":9007199254740991,"large":"9007199254740992","signed_":"-9007199254740992","frozen":true})");
   // The I-JSON reader accepts either form of a 64-bit integer.
   CHECK(psio::from_ijson<Amounts>(
            R"({"count":7,"small":"9007199254740991","large":"9007199254740992","signed_":"-9007199254740992","frozen":true})") ==
         a);
   // Type-level ijson_number: every integer member a number.
   CHECK(psio::to_ijson(AmountsNumbers{1, 9007199254740992ull}) == R"({"small":1,"large":9007199254740992})");
   // ── /ref-integers-run ──
   CHECK(psio::from_pssz<Amounts>(psio::to_pssz(a)) == a);
   // bool is one byte, 00 or 01.
   CHECK_FALSE(psio::validate_pssz<bool>(std::vector<char>{2}).ok());
   const std::string text = schema_text<AmountsNumbers>("N");
   CHECK(contains(text, R"("attributes":[{"name":"ijsonInt","value":"number"}])"));
   CHECK(contains(schema_text<Totals>("T"),
                  R"("attributes":[{"name":"jsonInt","value":"string"},{"name":"ijsonInt","value":"string"}])"));
   CHECK(contains(schema_text<Amounts>("A"), R"({"Custom":{"type":{"value":{"Int":{"bits":1,"isSigned":false}}},"id":"bool"}})"));
}

TEST_CASE("reference: enums", "[doc][ref-enum]")
{
   using namespace ref;
   // ── ref-enum-run ──
   const Ticket t{7, Status::active};
   CHECK(hex(psio::to_pssz(t)) == "07 00 00 00 02");                 // the underlying integer
   CHECK(psio::to_json(t) == R"({"id":7,"status":"active"})");       // the name in text
   CHECK(psio::from_json<Ticket>(R"({"id":7,"status":2})") == t);    // the number is read too
   // A value with no listed name is written as its number and read back.
   CHECK(psio::to_json(Ticket{7, Status{9}}) == R"({"id":7,"status":9})");
   CHECK(psio::from_pssz<Ticket>(psio::to_pssz(Ticket{7, Status{9}})) == Ticket{7, Status{9}});
   // ── /ref-enum-run ──
   CHECK(contains(schema_text<Ticket>("Ticket"),
                  R"("members":[{"name":"pending","value":0},{"name":"active","value":2},{"name":"closed","value":3}])"));
}

TEST_CASE("reference: arrays, optionals, maps, sets", "[doc][ref-containers]")
{
   using namespace ref;
   const Slots s{{1, 2, 3, 4}, {"a", "", std::string(16, 'n')}};
   CHECK(psio::from_pssz<Slots>(psio::to_pssz(s)) == s);
   CHECK_FALSE(encodes(Slots{{}, {"", std::string(17, 'n'), ""}}));
   // 1-byte header, 16 bytes, 1-byte slot; then the array's own 1-byte table.
   CHECK(psio::to_pssz(s).size() == 1 + 16 + 1 + 3 + 17);

   // ── ref-optional-run ──
   const Profile none{7, std::nullopt, std::nullopt};
   const Profile some{7, "hi", 30};
   CHECK(psio::to_pssz(none).size() == 1 + 8);                      // trailing None is pruned
   CHECK(psio::to_json(none) == R"({"id":7,"bio":null,"age":null})");
   CHECK(psio::from_pssz<Profile>(psio::to_pssz(some)) == some);
   CHECK_FALSE(encodes(Profile{7, std::string(101, 'b'), std::nullopt}));
   // ── /ref-optional-run ──

   // ── ref-maps-run ──
   const Book book{{{"bob", 5}, {"al", 7}}, {3, 1}};
   CHECK(psio::to_json(book) == R"({"balances":{"al":7,"bob":5},"members":[1,3]})");
   CHECK(psio::from_pssz<Book>(psio::to_pssz(book)) == book);
   CHECK_FALSE(encodes(Book{{{std::string(9, 'k'), 1}}, {}}));       // a key of 9 bytes
   CHECK_FALSE(encodes(Book{{}, {1, 2, 3, 4, 5}}));                  // 5 members
   // ── /ref-maps-run ──
   const std::string text = schema_text<Book>("Book");
   CHECK(contains(text, R"("maxCount":4}}},"id":"map")"));
   CHECK(contains(text, R"("maxCount":4}}},"id":"set")"));
}

TEST_CASE("reference: nested values", "[doc][ref-nested]")
{
   using namespace ref;
   // ── ref-nested-run ──
   const Action a{5, psio::nested<Memo>::from(Memo{"hi"})};
   // The member is a byte list holding the Memo's own encoding (01 01 68 69).
   CHECK(hex(psio::to_pssz(a)) == "09 05 00 00 00 00 00 00 00 09 01 01 68 69");
   // In text it is the inner value, not hex.
   CHECK(psio::to_ijson(a) == R"({"sender":5,"args":{"text":"hi"}})");
   CHECK(psio::from_pssz<Action>(psio::to_pssz(a)).args.decode() == Memo{"hi"});
   // ── /ref-nested-run ──
   const std::string text = schema_text<Action>("Action");
   CHECK(contains(text, R"("attributes":[{"name":"lengthBound","value":"max=64"}])"));
   CHECK(contains(text, R"({"Nested":{"type":)"));
}

TEST_CASE("reference: record keywords", "[doc][ref-records]")
{
   using namespace ref;
   // ── ref-records-run ──
   CHECK(psio::to_pssz(Point{1, 2}).size() == 8);          // DWNC: the fields, nothing else
   CHECK(psio::to_pssz(PointOpen{1, 2}).size() == 1 + 8);  // extensible: a 1-byte header
   // ── /ref-records-run ──
   const std::string text = psio::to_ijson(psio::SchemaBuilder{}
                                              .insert<Point>("Point")
                                              .insert<Capped>("Capped")
                                              .build());
   CHECK(contains(text, R"("attributes":[{"name":"definitionWillNotChange","value":""}])"));
   CHECK(contains(text, R"({"name":"maxFields","value":"8"})"));
   CHECK(contains(text, R"({"name":"maxDynamicData","value":"16"})"));
   const std::string wit = psio::emit_wit(psio::SchemaBuilder{}.insert<Capped>("Capped").build());
   CHECK(contains(wit, "@maxDynamicData(16)"));
   CHECK(contains(wit, "@maxFields(8)"));
}

TEST_CASE("reference: the cap and the text formats", "[doc][ref-cap-text]")
{
   using namespace ref;
   // ── ref-cap-text-run ──
   // maxDynamicData(16): every format's validate applies the cap to the size
   // of ITS input. The pssz encoding of this value is 16 bytes and valid; its
   // JSON text is 22 bytes, which the JSON reader refuses.
   const Capped value{std::string(14, 'x')};
   CHECK(psio::validate_pssz<Capped>(psio::to_pssz(value)).ok());
   const std::string text = psio::to_json(value);              // the writer does not apply the cap
   CHECK(text == R"({"s":"xxxxxxxxxxxxxx"})");
   CHECK_FALSE(psio::validate_json<Capped>(text).ok());        // 22 > 16
   CHECK(psio::validate_json<Capped>(R"({"s":"xxxxxxxx"})").ok());   // 16 bytes of text
   // ── /ref-cap-text-run ──
   const auto status = psio::validate_json<Capped>(text);
   CHECK(status.error().what == std::string_view{"encoded size exceeds maxDynamicData cap"});
   // A capped member of an uncapped root is not limited in text ...
   CHECK(psio::validate_json<CappedHolder>(R"({"inner":{"s":"xxxxxxxxxxxxxx"}})").ok());
   CHECK(psio::validate_json<CappedHolder>(R"({"inner":{"s":"xxxxxxxxxxxxxxx"}})").ok());
   // ... but is in pssz, wherever it is nested.
   CHECK_FALSE(encodes(CappedHolder{Capped{std::string(15, 'x')}}));
}

TEST_CASE("reference: transparent wrappers", "[doc][ref-transparent]")
{
   using namespace ref;
   PublicKey k;
   k.data = psio::bounded<std::uint8_t, 65>{1, 2, 3};
   CHECK(psio::to_pssz(k) == psio::to_pssz(std::vector<std::uint8_t>{1, 2, 3}));
   CHECK(psio::to_ijson(k) == R"("010203")");
   CHECK(psio::to_key(k) == psio::to_key(std::vector<std::uint8_t>{1, 2, 3}));
   // The Schema IR: a named alias of the member's type.
   CHECK(contains(schema_text<AccountId>("AccountId"),
                  R"({"name":"AccountId","type":{"value":{"Type":{"type":"@)"));
}

TEST_CASE("reference: custom text", "[doc][ref-adapter]")
{
   using namespace ref;
   // ── ref-adapter-run ──
   const Pixel p{3, Color{255, 0, 16}};
   CHECK(psio::to_json(p) == R"({"x":3,"color":"#ff0010"})");
   CHECK(psio::from_json<Pixel>(R"({"x":3,"color":"#ff0010"})") == p);
   CHECK_FALSE(psio::validate_json<Pixel>(R"({"x":3,"color":"red"})").ok());
   // The binary form is the record's own: 3 bytes.
   CHECK(psio::to_pssz(Color{255, 0, 16}).size() == 3);
   // ── /ref-adapter-run ──
   CHECK(contains(schema_text<Pixel>("Pixel"), R"("id":"Color")"));
}

TEST_CASE("reference: time", "[doc][ref-time]")
{
   using namespace ref;
   // ── ref-time-run ──
   const Event e{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{1795342530123}},
                 std::chrono::seconds{30}};
   CHECK(psio::to_pssz(e).size() == 16);                                    // two 64-bit tick counts
   CHECK(psio::to_ijson(e) == R"({"at":"2026-11-22T10:15:30.123Z","ttl":30})");
   CHECK(psio::from_ijson<Event>(psio::to_ijson(e)) == e);
   // ── /ref-time-run ──
   const std::string text = schema_text<Event>("Event");
   CHECK(contains(text, R"("id":"time-point-millis")"));
   CHECK(contains(text, R"("id":"duration-seconds")"));
}

TEST_CASE("reference: protobuf field numbers", "[doc][ref-field-number]")
{
   using namespace ref;
   // field 3 varint (tag 0x18), field 7 length-delimited (tag 0x3a)
   CHECK(hex(psio::to_protobuf(Message{1, "x"})) == "18 01 3a 01 78");
}

TEST_CASE("reference: the Schema IR of an annotated record", "[doc][ref-schema]")
{
   // ── ref-schema-run ──
   const psio::Schema schema = psio::SchemaBuilder{}.insert<AccountV2>("AccountV2").build();
   const std::string  text   = psio::to_ijson(schema);
   // owner: utf8 | max<32>
   CHECK(contains(text, R"({"name":"owner","type":{"value":{"Custom":{"type":{"value":{"BoundedList":{"type":{"value":{"Type":{"type":"@3"}}},"maxCount":32}}},"id":"utf8"}}},"attributes":[]})"));
   // tags: max<8> | element(utf8 | max<16>)
   CHECK(contains(text, R"({"name":"tags","type":{"value":{"BoundedList":{"type":{"value":{"Custom":{"type":{"value":{"BoundedList":{"type":{"value":{"Type":{"type":"@3"}}},"maxCount":16}}},"id":"utf8"}}},"maxCount":8}}},"attributes":[]})"));
   // maxDynamicData(1024)
   CHECK(contains(text, R"("attributes":[{"name":"maxDynamicData","value":"1024"}])"));
   // A reader with only the schema derives the same width.
   std::size_t width = 0;
   CHECK(psio::pssz_ir::auto_width(schema, "AccountV2", width).ok());
   CHECK(width == psio::auto_pssz_width_v<AccountV2>);
   // ── /ref-schema-run ──
   const AccountV2 a{7, "alice", {"vip"}, 100, std::nullopt};
   CHECK(psio::pssz_ir::validate_pssz(schema, "AccountV2", psio::to_pssz(a)).ok());
   AccountV2 over = a;
   over.owner     = std::string(33, 'x');
   CHECK_FALSE(psio::pssz_ir::validate_pssz(schema, "AccountV2", unchecked(over)).ok());
}

TEST_CASE("reference: mutable views write fixed-size values only", "[doc][ref-mutable-view]")
{
   // ── ref-mutable-view-run ──
   std::vector<char> bytes = psio::to_pssz(ref::Point{1, 2});
   psio::pssz_mutable_view<ref::Point> w{bytes};
   CHECK(w.get<0>().set(std::int32_t{42}));   // false if the value cannot be written
   CHECK(psio::from_pssz<ref::Point>(bytes) == ref::Point{42, 2});
   // ── /ref-mutable-view-run ──
}
