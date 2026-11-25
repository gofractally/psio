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
// Schema IR the annotations produce, the evolution check, the JSON forms of an
// argument list, result encoding and the name coders.

#include <psio/compress_name.hpp>
#include <psio/emit_wit.hpp>
#include <psio/capnp.hpp>
#include <psio/ext_int.hpp>
#include <psio/flatbuf.hpp>
#include <psio/get_type_name.hpp>
#include <psio/helpers.hpp>
#include <psio/invariant.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/max_size.hpp>
#include <psio/mutable_view.hpp>
#include <psio/name.hpp>
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
   // The schema of T with T's own name blanked: equal for two types that are
   // one type but for the name.
   template <typename T>
   std::string shape_text()
   {
      std::string            text{schema_text<T>("T")};
      const std::string      quoted = '"' + std::string{psio::reflect<T>::name} + '"';
      for (auto at = text.find(quoted); at != std::string::npos; at = text.find(quoted, at + 3))
         text.replace(at, quoted.size(), "\"_\"");
      return text;
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
static_assert(!psio::inferred_bound<AccountPlain>().has_value());
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
// tags:   a table of 64 slots, then 64 strings of <= 16 bytes. With 1-byte slots that
//         would be 64 + 1024 = 1088 > 255, so the slots are 2 bytes: 2 x 64 + 1024 = 1152.
// record: a header and 2 slots (owner, tags), then id 8 + owner 32 + tags 1152 + balance 8
//         = 1200. 3 + 1200 > 255, so header and slots are 2 bytes: 2 x 3 + 1200 = 1206.
static_assert(psio::inferred_bound<AccountWide>() == 2 * 3 + 8 + 32 + (2 * 64 + 64 * 16) + 8);
static_assert(psio::inferred_bound<AccountWide>() == 1206);   // <= 65535: 16-bit offsets
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
// tags:   8 slots + 8 x 16 = 136 <= 255: the list's own table is 1 byte per slot.
// record: (header + 2 slots) x 1 byte + id 8 + owner 32 + tags 136 + balance 8.
static_assert(psio::inferred_bound<Account>() == 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::inferred_bound<Account>() == 187);        // <= 255: 8-bit offsets
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
static_assert(!psio::inferred_bound<AccountCountOnly>().has_value());
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
// The cap rules: 16-bit offsets, although today's fields need at most 187 bytes.
static_assert(psio::max_dynamic_data_v<AccountV1> == 1024);
static_assert(psio::auto_pssz_width_v<AccountV1> == 2);
// The inferred bound stays what the fields add up to; the cap is what rules.
static_assert(psio::inferred_bound<AccountV1>() == 187);
static_assert(psio::effective_max_dynamic_v<AccountV1> == 1024);   // the cap, when one is declared
static_assert(psio::effective_max_dynamic_v<Account> == 187);      // else the inferred bound
// The largest AccountV1 is 190 bytes: its header and two slots are 2 bytes each.
static_assert(psio::max_encoded_size<AccountV1>() == 2 * 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::max_encoded_size<Account>() == 187);           // no cap: the inferred bound

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
static_assert(psio::inferred_bound<AccountGrown>() == 2 * 4 + 8 + 32 + 136 + 8 + (1 + 100));
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

// ── tut-4c: a published bound may only widen ───────────────────────────────
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
static_assert(psio::inferred_bound<AccountId>() == 8);

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

// ── tut-8c: a first-class bounded string ───────────────────────────────────
struct Memo
{
   friend auto operator<=>(const Memo&, const Memo&) = default;
   std::string contents;
   PSIO_REFLECT(Memo, transparent(), attr(contents, utf8 | max<80>))
};
// Memo is the bounded type its member's annotations describe: on the wire it
// is psio::utf8_string<80>.
static_assert(psio::is_transparent_v<Memo>);
static_assert(psio::inferred_bound<Memo>() == 80);
static_assert(psio::inferred_bound<Memo>() == psio::inferred_bound<psio::utf8_string<80>>());

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
static_assert(psio::inferred_bound<Receipt>() == 1 + 8 + 1 + 80);
static_assert(psio::auto_pssz_width_v<Receipt> == 1);

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
// open has no cap: (header + 2 slots) x 1 byte + owner 32 + tags 136 = 171, 8-bit.
static_assert(psio::inferred_bound<OpenArgs>() == 3 + 32 + (8 + 8 * 16));
static_assert(psio::auto_pssz_width_v<OpenArgs> == 1);
// transfer is capped: 16-bit for its lifetime.
static_assert(psio::max_dynamic_data_v<TransferArgs> == 1024);
static_assert(psio::auto_pssz_width_v<TransferArgs> == 2);
// The inferred bound is that of the parameters and has no cap in it:
// header 1 + slot 1 + from 8 + to 8 + amount 8 + memo 80.
static_assert(psio::inferred_bound<TransferArgs>() == 2 + 8 + 8 + 8 + 80);
// The largest argument list is 108 bytes: under the cap the header and the
// slot are 2 bytes each.
static_assert(psio::max_encoded_size<TransferArgs>() == 2 * 2 + 8 + 8 + 8 + 80);

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

// ── tut-9c: a first-class type as a parameter ──────────────────────────────
struct Payments
{
   void pay(AccountId from, AccountId to, std::uint64_t amount, Memo memo);
   PSIO_REFLECT(Payments, method(pay, from, to, amount, memo, maxDynamicData(1024)))
};
using PayArgs = psio::reflect<Payments>::method_arg_tuple<0>;
// The bound travels with the type: no attr on the parameter, and the argument
// list is the one of Bank::transfer.
static_assert(psio::inferred_bound<PayArgs>() == psio::inferred_bound<TransferArgs>());
static_assert(psio::max_dynamic_data_v<PayArgs> == 1024);
static_assert(psio::auto_pssz_width_v<PayArgs> == 2);
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
                attr(code, min<2> | max<8>),                               // 2..8 bytes
                attr(ids, max<4>))                                         // <= 4 elements
};
// max<N> and exact<N> give the inferred bound its N; min<N> alone bounds nothing.
// (header + 3 slots) x 1 byte + digest 32 + code 8 + ids 4 x 4
static_assert(psio::inferred_bound<Limits>() == 4 + 32 + 8 + 4 * 4);
static_assert(psio::auto_pssz_width_v<Limits> == 1);
// min<A> | max<B> is one spec, the range psio::length_bound{.max = B, .min = A}.
static_assert(std::get<psio::length_bound>(psio::min<2> | psio::max<8>) ==
              psio::length_bound{.max = 8, .min = 2});

struct AtLeast
{
   std::string code;
   PSIO_REFLECT(AtLeast, attr(code, min<2>))
};
static_assert(!psio::inferred_bound<AtLeast>().has_value());   // no upper bound: 32-bit

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

struct NamesSpelled   // psio::utf8_spec{} is psio::utf8
{
   std::string text;
   PSIO_REFLECT(NamesSpelled, attr(text, psio::utf8_spec{}))
};

// ── ref-bytes: byte sequences ──────────────────────────────────────────────
struct Blob
{
   friend bool operator==(const Blob&, const Blob&) = default;
   std::vector<std::uint8_t>   data;      // variable: hex in I-JSON
   std::array<std::uint8_t, 4> tag{};     // fixed, 4 bytes, no offset
   PSIO_REFLECT(Blob, attr(data, max<64>), tag)
};
static_assert(psio::inferred_bound<Blob>() == 2 + 64 + 4);   // header + slot, data, tag

// ── ref-sorted: order constraints ──────────────────────────────────────────
struct Ranked
{
   friend bool operator==(const Ranked&, const Ranked&) = default;
   std::vector<std::uint32_t>                         scores;    // non-descending
   std::vector<std::string>                           names;     // strictly ascending: no duplicate
   std::vector<std::pair<std::string, std::uint32_t>> entries;   // no two with the same .first
   PSIO_REFLECT(Ranked,
                attr(scores, sorted | max<4>),
                attr(names, unique),
                attr(entries, unique_keys))
};
struct Unranked   // the same members without the constraints
{
   std::vector<std::uint32_t>                         scores;
   std::vector<std::string>                           names;
   std::vector<std::pair<std::string, std::uint32_t>> entries;
   PSIO_REFLECT(Unranked, attr(scores, max<4>), names, entries)
};
// An order constraint is part of the type: it cannot be added or dropped later.
static_assert(!psio::pssz_evolves_from<Unranked, Ranked>);
static_assert(!psio::pssz_evolves_from<Ranked, Unranked>);

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
// header 1 + slot 1 + rows: 3 slots of 1 byte + 3 x (4 x 2)
static_assert(psio::inferred_bound<Grid>() == 1 + 1 + (3 + 3 * (4 * 2)));
static_assert(psio::auto_pssz_width_v<Grid> == 1);

struct Choice   // a variant alternative
{
   friend bool operator==(const Choice&, const Choice&) = default;
   std::variant<std::uint32_t, std::string> v;
   PSIO_REFLECT(Choice, attr(v, at<1>(utf8 | max<16>)))
};
// header 1 + slot 1 + selector 1 + the widest alternative 16
static_assert(psio::inferred_bound<Choice>() == 1 + 1 + 1 + 16);

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
static_assert(psio::inferred_bound<Amounts>() == 1 + 4 + 8 + 8 + 8 + 1);   // a 1-byte header

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
static_assert(psio::inferred_bound<Ticket>() == 5);

// ── ref-arrays: fixed arrays ───────────────────────────────────────────────
struct Slots
{
   friend bool operator==(const Slots&, const Slots&) = default;
   std::array<std::uint32_t, 4> counts{};   // fixed: 16 bytes inline
   std::array<std::string, 3>   names;      // variable elements: its own offset table
   PSIO_REFLECT(Slots, counts, attr(names, element(utf8 | max<16>)))
};
// header 1 + counts 16 + slot 1 + names (3 slots of 1 byte + 3 x 16)
static_assert(psio::inferred_bound<Slots>() == 1 + 16 + 1 + (3 + 3 * 16));
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
// header 1 + id 8 + bio (slot 1 + selector 1 + 100) + age (slot 1 + selector 1 + 4)
static_assert(psio::inferred_bound<Profile>() == 1 + 8 + (1 + 1 + 100) + (1 + 1 + 4));

// ── ref-maps: maps and sets ────────────────────────────────────────────────
struct Book
{
   friend bool operator==(const Book&, const Book&) = default;
   std::map<std::string, std::uint64_t> balances;
   std::set<std::uint32_t>              members;
   PSIO_REFLECT(Book, attr(balances, max<4> | keys(utf8 | max<8>)), attr(members, max<4>))
};
// balances: an entry is header 1 + key (slot 1 + 8) + 8 = 18; the map is 4 slots + 4 x 18 = 76
// record:   (header + 2 slots) x 1 byte + balances 76 + members 4 x 4
static_assert(psio::inferred_bound<Book>() == 3 + (4 + 4 * 18) + 4 * 4);
static_assert(psio::auto_pssz_width_v<Book> == 1);

// ── ref-nested: nested values ──────────────────────────────────────────────
struct Payload
{
   friend bool operator==(const Payload&, const Payload&) = default;
   std::string text;
   PSIO_REFLECT(Payload, attr(text, utf8 | max<32>))
};
struct Action
{
   std::uint64_t         sender = 0;
   psio::nested<Payload> args;       // on the wire: a byte list holding a Payload
   PSIO_REFLECT(Action, sender, attr(args, max<64>))
};
// header 1 + sender 8 + args (slot 1 + 64)
static_assert(psio::inferred_bound<Action>() == 1 + 8 + (1 + 64));
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
static_assert(psio::inferred_bound<Point>() == 8);
static_assert(psio::inferred_bound<PointOpen>() == 1 + 8);

struct Line   // a child's cap is what the parent reserves for it
{
   tut::AccountV1 a;   // maxDynamicData(1024)
   PSIO_REFLECT(Line, a)
};
// (header + slot) x 2 bytes + the child's cap
static_assert(psio::inferred_bound<Line>() == 2 * 2 + 1024);
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

// ── ref-max-fields: a cap on the number of fields ──────────────────────────
struct Settings
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   PSIO_REFLECT(Settings, volume, balance, maxFields(3), maxDynamicData(64))
};
static_assert(psio::max_fields_v<Settings> == 3);
static_assert(psio::effective_max_fields_v<Settings> == 3);   // the cap; without one, today's count
static_assert(psio::effective_max_fields_v<Point> == 2);
static_assert(psio::max_record_fields == 255);                // every record; N of maxFields(N) too

struct SettingsGrown   // a third field: within the cap
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   std::optional<std::uint32_t> treble;
   PSIO_REFLECT(SettingsGrown, volume, balance, treble, maxFields(3), maxDynamicData(64))
};
static_assert(psio::pssz_evolves_from<Settings, SettingsGrown>);

struct SettingsRaised   // a revision may not raise the cap ...
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   std::optional<std::uint32_t> treble;
   PSIO_REFLECT(SettingsRaised, volume, balance, treble, maxFields(8), maxDynamicData(64))
};
static_assert(!psio::pssz_evolves_from<Settings, SettingsRaised>);

struct SettingsDropped   // ... or drop it
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   std::optional<std::uint32_t> treble;
   PSIO_REFLECT(SettingsDropped, volume, balance, treble, maxDynamicData(64))
};
static_assert(!psio::pssz_evolves_from<Settings, SettingsDropped>);

// A method's argument list takes the same cap.
struct Mixer
{
   void set(std::uint32_t channel, std::uint32_t level);
   PSIO_REFLECT(Mixer, method(set, channel, level, maxFields(4), maxDynamicData(64)))
};
using SetArgs = psio::reflect<Mixer>::method_arg_tuple<0>;
static_assert(psio::reflect<Mixer>::method_max_fields<0> == 4);
static_assert(psio::max_fields_v<SetArgs> == 4);

// ── ref-thresholds: where the width changes ────────────────────────────────
struct Raw253   // header 1 + slot 1 + 253 = 255: the last 1-byte record of this shape
{
   std::string s;
   PSIO_REFLECT(Raw253, attr(s, max<253>))
};
struct Raw254   // 1 + 1 + 254 = 256 does not fit: header and slot are 2 bytes
{
   std::string s;
   PSIO_REFLECT(Raw254, attr(s, max<254>))
};
static_assert(psio::inferred_bound<Raw253>() == 255 && psio::auto_pssz_width_v<Raw253> == 1);
static_assert(psio::inferred_bound<Raw254>() == 258 && psio::auto_pssz_width_v<Raw254> == 2);
struct Raw65531   // 2 x 2 + 65531 = 65535: the last 2-byte record of this shape
{
   std::string s;
   PSIO_REFLECT(Raw65531, attr(s, max<65531>))
};
struct Raw65532   // header and slot are 4 bytes
{
   std::string s;
   PSIO_REFLECT(Raw65532, attr(s, max<65532>))
};
static_assert(psio::inferred_bound<Raw65531>() == 65535 && psio::auto_pssz_width_v<Raw65531> == 2);
static_assert(psio::inferred_bound<Raw65532>() == 65540 && psio::auto_pssz_width_v<Raw65532> == 4);
// A list measures itself the same way: one slot per element, then the elements.
using Tags15x16 = psio::bounded<psio::utf8_string<16>, 15>;   // 15 + 15 x 16 = 255
using Tags15x17 = psio::bounded<psio::utf8_string<17>, 15>;   // 15 + 255 > 255: 2 x 15 + 255
static_assert(psio::inferred_bound<Tags15x16>() == 255 && psio::auto_pssz_width_v<Tags15x16> == 1);
static_assert(psio::inferred_bound<Tags15x17>() == 285 && psio::auto_pssz_width_v<Tags15x17> == 2);

// ── ref-cap-transfer: what fits under the largest 1-byte cap ───────────────
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

// ── ref-cap-positions: a cap narrows the containers beneath it ─────────────
struct Batch   // plain member types; the cap is the only bound
{
   friend bool operator==(const Batch&, const Batch&) = default;
   std::uint32_t                      id = 0;
   std::vector<std::string>           tags;     // under the cap: a 1-byte offset table
   std::map<std::string, std::string> labels;   // 1-byte table, 1-byte entry header and slots
   PSIO_REFLECT(Batch, id, tags, labels, maxDynamicData(255))
};
struct BatchOpen   // the same members with no cap: everything is 4 bytes wide
{
   std::uint32_t                      id = 0;
   std::vector<std::string>           tags;
   std::map<std::string, std::string> labels;
   PSIO_REFLECT(BatchOpen, id, tags, labels)
};
static_assert(psio::auto_pssz_width_v<Batch> == 1);
static_assert(psio::auto_pssz_width_v<BatchOpen> == 4);

// ── ref-cap-child: a record beneath a cap keeps its own width ──────────────
struct Item   // unbounded: 4-byte offsets, wherever it appears
{
   friend bool operator==(const Item&, const Item&) = default;
   std::uint32_t id = 0;
   std::string   name;
   PSIO_REFLECT(Item, id, name)
};
struct Order
{
   std::uint32_t     id = 0;
   std::vector<Item> items;   // the list's table is 1 byte; each Item still has 4-byte offsets
   PSIO_REFLECT(Order, id, items, maxDynamicData(255))
};
static_assert(psio::auto_pssz_width_v<Item> == 4);
static_assert(psio::pssz_child_wider_than_cap_v<Order>);   // Item cannot use its width here

struct SmallItem   // bounded: header 1 + id 4 + slot 1 + 32 = 38, 1-byte offsets
{
   friend bool operator==(const SmallItem&, const SmallItem&) = default;
   std::uint32_t id = 0;
   std::string   name;
   PSIO_REFLECT(SmallItem, id, attr(name, utf8 | max<32>))
};
struct SmallOrder
{
   std::uint32_t          id = 0;
   std::vector<SmallItem> items;
   PSIO_REFLECT(SmallOrder, id, items, maxDynamicData(255))
};
static_assert(psio::auto_pssz_width_v<SmallItem> == 1);
static_assert(!psio::pssz_child_wider_than_cap_v<SmallOrder>);

// ── ref-bits: bit types are bounded ────────────────────────────────────────
struct Flags
{
   std::uint8_t        kind = 0;
   psio::bitvector<12> mask;   // fixed: 2 bytes inline, no slot
   psio::bitlist<12>   seen;   // variable: at most 12 bits and the delimiter bit
   PSIO_REFLECT(Flags, kind, mask, seen)
};
// header 1 + kind 1 + mask 2 + seen (slot 1 + 2 + 1)
static_assert(psio::inferred_bound<Flags>() == 1 + 1 + 2 + (1 + 2 + 1));
static_assert(psio::auto_pssz_width_v<Flags> == 1);

// ── ref-tuple: a tuple is the extensible record of its elements ────────────
using Entry = std::tuple<std::uint32_t, psio::utf8_string<16>>;
// header 1 + u32 4 + slot 1 + 16
static_assert(psio::inferred_bound<Entry>() == 1 + 4 + 1 + 16);
static_assert(psio::auto_pssz_width_v<Entry> == 1);
static_assert(psio::max_tuple_elements == 255);   // a larger tuple does not compile

// ── ref-transparent: transparent wrappers ──────────────────────────────────
struct PublicKey
{
   friend auto operator<=>(const PublicKey&, const PublicKey&) = default;
   psio::bounded<std::uint8_t, 65> data;   // the bound is in the member's type
   PSIO_REFLECT(PublicKey, transparent(), data)
};
static_assert(psio::inferred_bound<PublicKey>() == 65);

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

// ── ref-as: a text presentation for one member ─────────────────────────────
struct Wide
{
   friend bool operator==(const Wide&, const Wide&) = default;
   std::uint32_t id = 0;
   psio::uint128 amount{};
   PSIO_REFLECT(Wide, id, attr(amount, as<hex_tag>))   // "0x..." in JSON and I-JSON
};
struct WidePlain   // the same record without the presentation
{
   std::uint32_t id = 0;
   psio::uint128 amount{};
   PSIO_REFLECT(WidePlain, id, amount)
};
// A text-only adapter does not reach the binary formats: the same bound and width.
static_assert(psio::inferred_bound<Wide>() == 1 + 4 + 16);
static_assert(psio::inferred_bound<Wide>() == psio::inferred_bound<WidePlain>());
static_assert(psio::auto_pssz_width_v<Wide> == 1);

// ── ref-result: a result is encoded as its returns(...) type ───────────────
struct Listing
{
   friend bool operator==(const Listing&, const Listing&) = default;
   std::uint64_t id = 0;
   std::string   note;
   PSIO_REFLECT(Listing, id, attr(note, utf8 | max<32>))
};
struct Directory
{
   std::vector<std::string> names(std::uint32_t limit) const;
   std::optional<Listing>     find(std::uint64_t id) const;
   void                     touch(std::uint64_t id);
   PSIO_REFLECT(Directory,
                method(names, limit, returns(max<4> | element(utf8 | max<16>))),
                method(find, id),
                method(touch, id))
};

// ── ref-names: names as numbers ────────────────────────────────────────────
// An account name: a number is given only to text it decodes back to.
static_assert(psio::name_to_number("alice") == 6697231);
static_assert(psio::name_to_number("x--b") == 0);    // no number decodes to it: not a name
static_assert(psio::name_to_number("Alice") == 0);   // outside the alphabet
// A method name: compressed when the number decodes to the case-folded name ...
static_assert(psio::detail::method_to_number("transfer") == psio::detail::method_to_number("Transfer"));
static_assert(!psio::detail::is_hash_name(psio::detail::method_to_number("transfer")));
// ... and a hash of the name as written otherwise (here the code needs more than 64 bits).
static_assert(psio::detail::method_to_number("getSymbolType") == 0xd55df3070b8758ebull);
static_assert(psio::detail::is_hash_name(psio::detail::method_to_number("getSymbolType")));
static_assert(psio::hash_name("getSymbolType") == 0xd55df3070b8758ebull);
static_assert(psio::detail::method_to_number("#abc") == 0);   // malformed hash text: not a name

// ── ref-time: time values ──────────────────────────────────────────────────
struct Event
{
   friend bool operator==(const Event&, const Event&) = default;
   std::chrono::sys_time<std::chrono::milliseconds> at;    // a time point: a date in text
   std::chrono::seconds                             ttl{}; // a duration: its tick count
   PSIO_REFLECT(Event, at, ttl, definitionWillNotChange())
};
static_assert(psio::inferred_bound<Event>() == 8 + 8);

// ── ref-field-number: protobuf field numbers ───────────────────────────────
struct Message
{
   std::uint32_t code = 0;
   std::string   text;
   PSIO_REFLECT(Message, attr(code, field<3>), attr(text, field<7>))
};

// ── ref-schema-id: a schema id and nothing else ────────────────────────────
struct Call
{
   std::uint64_t     sender  = 0;
   std::uint64_t     service = 0;
   std::uint32_t     method  = 0;
   std::vector<char> data;   // the arguments, packed: only the service's schema explains them
   PSIO_REFLECT(Call, schemaId("Action"), sender, service, method, data)
};
static_assert(psio::schema_custom_v<Call>);                 // Custom{record, "Action"} in a schema
static_assert(psio::schema_custom_id<Call>() == "Action");
static_assert(!psio::has_invariant_v<Call>);                // no check
static_assert(!psio::detail::mutation_blocked_v<Call>);     // addressable by psio::mutable_view
}  // namespace ref

namespace ref
{
// ── ref-labels: the label of a variant alternative ─────────────────────────
struct Circle
{
   friend bool operator==(const Circle&, const Circle&) = default;
   std::uint32_t radius = 0;
   PSIO_REFLECT(Circle, radius)
};
struct Square
{
   friend bool operator==(const Square&, const Square&) = default;
   std::uint32_t side = 0;
   PSIO_REFLECT(Square, side)
};
// The name of the alternative's type: a reflected type's name, the table for the rest.
using Shape = std::variant<Circle, Square, std::uint32_t, std::vector<std::string>>;
static_assert(psio::variant_label<Shape, 0>() == "Circle");
static_assert(psio::variant_label<Shape, 2>() == "uint32");
static_assert(psio::variant_label<Shape, 3>() == "string[]");
// A type name two alternatives share is no label: they are labelled by position.
using Repeated = std::variant<std::uint32_t, std::string, std::string>;
static_assert(psio::variant_label<Repeated, 0>() == "uint32");
static_assert(psio::variant_label<Repeated, 1>() == "1");
static_assert(psio::variant_label<Repeated, 2>() == "2");
using Sized = std::variant<Circle, Square, std::uint32_t>;   // its labels are stated below
}  // namespace ref
// Stated labels: one per alternative, in order.
template <>
struct psio::variant_labels<ref::Sized>
{
   static constexpr std::array<std::string_view, 3> names{"circle", "square", "side"};
};
static_assert(psio::variant_label<ref::Sized, 2>() == "side");

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

   // The frame: 1-byte header and slots, and a 1-byte table in the list
   // (02 and 05, the offsets of "vip" and "beta" from the start of the list).
   CHECK(hex(bytes) ==
         "12 07 00 00 00 00 00 00 00 12 17 64 00 00 00 00 00 00 00 "
         "61 6c 69 63 65 02 05 76 69 70 62 65 74 61");
   CHECK(bytes.size() == 33);
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
   // 2-byte header and slots now: 3 bytes more than the 8-bit Account. The
   // list keeps its 1-byte table: its own bound (136) is tighter than the cap.
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

   // ── tut-4c-run ──
   // What the check promises, on bytes: an old record at its bound is a record
   // of the widened type, and is not one of the tightened type.
   const AccountV1 wide_owner{7, std::string(32, 'o'), {}, 0};
   CHECK(psio::validate_pssz<AccountWider>(psio::to_pssz(wide_owner)).ok());
   CHECK_FALSE(psio::validate_pssz<AccountTighter>(psio::to_pssz(wide_owner)).ok());
   // One direction only: a new writer can produce what an old reader refuses.
   const AccountWider wider{7, std::string(33, 'o'), {}, 0};
   CHECK_FALSE(psio::validate_pssz<AccountV1>(psio::to_pssz(wider)).ok());
   // ── /tut-4c-run ──
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
   // A raw string is any bytes for every reader, the exact-schema check included.
   CHECK(psio::validate_pssz<Note>(psio::to_pssz(binary)).ok());
   CHECK(psio::validate_strict<Note>(psio::pssz{}, std::span<const char>(psio::to_pssz(binary))).ok());
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
   // ── /tut-7-run ──
   const auto status = psio::encode_key_checked(too_long, out);
   CHECK(status.error().what == std::string_view{"key: member length_bound violated"});
   const auto bad_utf8 = psio::to_key(ByOwner{"\xff", 7});
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

TEST_CASE("tutorial 8: a transparent type with a bounded member", "[doc][tut-8]")
{
   // ── tut-8c-run ──
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
   // ── /tut-8c-run ──
   CHECK_FALSE(psio::validate_pssz<Receipt>(unchecked(Receipt{1, Memo{std::string(81, 'm')}})).ok());
   // Evolution: the bound of a transparent member may only widen, like any other.
   static_assert(psio::pssz_evolves_from<Memo, Memo>);
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

   // ── tut-9-json ──
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
   // ── /tut-9-json ──
   CHECK_FALSE(psio::validate_json<TransferArgs>(
                  (R"([1,2,500,")" + std::string(81, 'm') + R"("])").c_str())
                  .ok());

   // ── tut-9c-run ──
   // A Memo parameter: the same bytes and the same text as Bank::transfer.
   const PayArgs pay{AccountId{1}, AccountId{2}, 500, Memo{"rent"}};
   CHECK(psio::to_pssz(pay) == bytes);
   CHECK(psio::to_json(pay) == psio::to_json(args));
   CHECK_THROWS_AS(psio::to_pssz(PayArgs{AccountId{1}, AccountId{2}, 500, Memo{std::string(81, 'm')}}),
                   psio::codec_exception);
   // ── /tut-9c-run ──
   {
      // The interface schema names the type; its node is the bounded string.
      const std::string pay_schema =
         psio::to_ijson(psio::SchemaBuilder{}.insert<PayArgs>("PayArgs").build());
      CHECK(contains(pay_schema, R"("maxCount":80}}},"id":"utf8")"));
      CHECK(contains(pay_schema, R"({"name":"tuple","value":"named"})"));
   }

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

   // ── tut-11-foreign ──
   // The declaration is the type in FlatBuffers, Cap'n Proto and protobuf
   // too. A writer that does not know it (here: AccountPlain, the same
   // members without annotations) writes a 33-byte owner as valid
   // FlatBuffers; read as an Account, the buffer is refused.
   const AccountPlain      wide{1, std::string(33, 'x'), {}, 0};
   const std::vector<char> fb = psio::to_flatbuf(wide);
   CHECK(psio::validate_flatbuf<AccountPlain>(fb).ok());
   const psio::codec_status refused = psio::validate_flatbuf<Account>(fb);
   REQUIRE_FALSE(refused.ok());
   CHECK(refused.error().what == std::string_view{"flatbuf: member length_bound violated"});
   CHECK_THROWS_AS(psio::from_flatbuf<Account>(fb), psio::codec_exception);
   CHECK_FALSE(psio::validate_capnp<Account>(psio::to_capnp(wide)).ok());
   CHECK_FALSE(psio::validate_protobuf<Account>(psio::to_protobuf(wide)).ok());

   // Inside the bounds, the same bytes are an Account.
   const AccountPlain      fits{1, "alice", {"vip"}, 100};
   CHECK(psio::from_flatbuf<Account>(psio::to_flatbuf(fits)) == Account{1, "alice", {"vip"}, 100});
   // ── /tut-11-foreign ──
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
   // utf8_spec{} spelled out is the same statement: enforced everywhere.
   CHECK(encodes(NamesSpelled{"caf\xc3\xa9"}));
   CHECK_FALSE(encodes(NamesSpelled{"\xff"}));
   CHECK_FALSE(psio::validate_pssz<NamesSpelled>(unchecked(NamesSpelled{"\xff"})).ok());
   CHECK(contains(schema_text<NamesSpelled>("N"), R"("id":"utf8")"));
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

TEST_CASE("reference: sorted / unique / unique_keys are enforced", "[doc][ref-sorted]")
{
   using namespace ref;
   // ── ref-sorted-run ──
   const Ranked ok{{1, 1, 2}, {"a", "ab", "b"}, {{"x", 1}, {"a", 1}}};
   CHECK(psio::from_pssz<Ranked>(psio::to_pssz(ok)) == ok);
   // The constraints change no byte: the frame is the unconstrained record's.
   CHECK(psio::to_pssz(ok) == psio::to_pssz(Unranked{ok.scores, ok.names, ok.entries}));

   // A value out of order has no encoding, in pssz, JSON and key alike.
   const Ranked descending{{3, 1, 2}, {}, {}};
   CHECK_THROWS_AS(psio::to_pssz(descending), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_json(descending), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_key(descending), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_pssz(Ranked{{}, {"a", "a"}, {}}), psio::codec_exception);            // a duplicate
   CHECK_THROWS_AS(psio::to_pssz(Ranked{{}, {}, {{"a", 1}, {"a", 2}}}), psio::codec_exception);  // a key twice

   // Every checking reader rejects what a writer without the constraint produces.
   const auto bytes = psio::to_pssz(Unranked{{3, 1, 2}, {}, {}});
   CHECK_FALSE(psio::validate_pssz<Ranked>(bytes).ok());
   CHECK_THROWS_AS(psio::from_pssz<Ranked>(bytes), psio::codec_exception);
   CHECK_FALSE(psio::validate_json<Ranked>(R"({"scores":[3,1,2],"names":[],"entries":[]})").ok());
   // ── /ref-sorted-run ──
   CHECK(psio::validate_pssz<Ranked>(bytes).error().what ==
         std::string_view{"sorted: elements are not in order"});
   CHECK_FALSE(psio::validate_strict<Ranked>(psio::pssz{}, std::span<const char>(bytes)).ok());
   CHECK_FALSE(psio::validate_key<Ranked>(psio::to_key(Unranked{{3, 1, 2}, {}, {}})).ok());
   {
      std::vector<char> out;
      const auto        status = psio::encode_pssz_checked(psio::pssz{}, Ranked{{}, {"a", "a"}, {}}, out);
      CHECK(status.error().what ==
            std::string_view{"sorted: elements are not in strictly ascending order"});
      const auto twice =
         psio::encode_pssz_checked(psio::pssz{}, Ranked{{}, {}, {{"a", 1}, {"a", 2}}}, out);
      CHECK(twice.error().what == std::string_view{"unique_keys: two elements have the same key"});
   }
   CHECK_FALSE(encodes(Ranked{{1, 2, 3, 4, 5}, {}, {}}));
   // Not in the Schema IR: the schema of the two records is the same.
   CHECK(shape_text<Ranked>() == shape_text<Unranked>());
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
   const Action a{5, psio::nested<Payload>::from(Payload{"hi"})};
   // The member is a byte list holding the Payload's own encoding (01 01 68 69).
   CHECK(hex(psio::to_pssz(a)) == "09 05 00 00 00 00 00 00 00 09 01 01 68 69");
   // In text it is the inner value, not hex.
   CHECK(psio::to_ijson(a) == R"({"sender":5,"args":{"text":"hi"}})");
   CHECK(psio::from_pssz<Action>(psio::to_pssz(a)).args.decode() == Payload{"hi"});
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

TEST_CASE("reference: maxFields is a cap on the number of fields", "[doc][ref-max-fields]")
{
   using namespace ref;
   // ── ref-max-fields-run ──
   // The cap is in the schema: the record's, and the argument list's.
   CHECK(contains(schema_text<Settings>("Settings"), R"({"name":"maxFields","value":"3"})"));
   CHECK(contains(schema_text<SetArgs>("SetArgs"), R"({"name":"maxFields","value":"4"})"));

   // A schema-driven reader refuses a schema whose record has more members
   // than its maxFields, or a maxFields above 255, before reading any value.
   using namespace psio::schema_types;
   const auto reads = [](std::size_t members, const char* max_fields) {
      Object record;
      for (std::size_t i = 0; i < members; ++i)
         record.members.push_back(Member{"m" + std::to_string(i), Box<AnyType>{AnyType{Int{8, false}}}, {}});
      record.attributes.push_back(Attribute{"maxFields", max_fields});
      std::size_t width = 0;
      return psio::pssz_ir::auto_width(Schema{}, AnyType{std::move(record)}, width).ok();
   };
   CHECK(reads(3, "3"));
   CHECK_FALSE(reads(4, "3"));
   CHECK_FALSE(reads(3, "256"));
   // ── /ref-max-fields-run ──
   // maxFields changes no byte and no width.
   CHECK(psio::to_pssz(Settings{7, 9}).size() == 1 + 4 + 1 + 4);
   CHECK(psio::to_pssz(SettingsGrown{7, 9, std::nullopt}) == psio::to_pssz(Settings{7, 9}));
   const std::string wit = psio::emit_wit(psio::SchemaBuilder{}.insert<Settings>("Settings").build());
   CHECK(contains(wit, "@maxFields(3)"));
}

TEST_CASE("reference: the cap and the text formats", "[doc][ref-cap-text]")
{
   using namespace ref;
   // ── ref-cap-text-run ──
   // maxDynamicData(16) limits the pssz encoding. It is not a limit on the
   // length of the JSON text: this value is 16 bytes of pssz and 22 of text.
   const Capped value{std::string(14, 'x')};
   CHECK(psio::to_pssz(value).size() == 16);
   const std::string text = psio::to_json(value);
   CHECK(text == R"({"s":"xxxxxxxxxxxxxx"})");
   CHECK(psio::validate_json<Capped>(text).ok());
   CHECK(psio::from_json<Capped>(text).s.size() == 14);
   // The text codecs do not apply the cap at all: a text can hold a value
   // that has no pssz encoding. The cap is met when the value is written as pssz.
   const Capped over = psio::from_json<Capped>(R"({"s":"xxxxxxxxxxxxxxx"})");   // 15: 17 bytes of pssz
   CHECK_THROWS_AS(psio::to_pssz(over), psio::codec_exception);
   // ── /ref-cap-text-run ──
   CHECK(psio::validate_json<Capped>(R"({"s":"xxxxxxxxxxxxxxx"})").ok());
   CHECK(psio::to_json(over) == R"({"s":"xxxxxxxxxxxxxxx"})");
   // The cap of a nested record is applied in pssz wherever it is nested.
   CHECK_FALSE(encodes(CappedHolder{Capped{std::string(15, 'x')}}));
   CHECK(psio::validate_json<CappedHolder>(R"({"inner":{"s":"xxxxxxxxxxxxxxx"}})").ok());
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

TEST_CASE("reference: as<hex_tag> is a text presentation", "[doc][ref-as]")
{
   using namespace ref;
   // ── ref-as-run ──
   const Wide w{7, psio::uint128{255}};
   CHECK(psio::to_json(w) == R"({"id":7,"amount":"0xff"})");
   CHECK(psio::to_ijson(w) == R"({"id":7,"amount":"0xff"})");
   CHECK(psio::to_json(WidePlain{7, psio::uint128{255}}) == R"({"id":7,"amount":255})");
   CHECK(psio::from_json<Wide>(psio::to_json(w)) == w);
   // Binary and key: exactly the record without the annotation.
   CHECK(psio::to_pssz(w) == psio::to_pssz(WidePlain{7, psio::uint128{255}}));
   CHECK(psio::to_key(w) == psio::to_key(WidePlain{7, psio::uint128{255}}));
   CHECK(psio::from_pssz<Wide>(psio::to_pssz(w)) == w);
   // ── /ref-as-run ──
   // The Schema IR does not carry it either.
   CHECK(shape_text<Wide>() == shape_text<WidePlain>());
}

TEST_CASE("reference: a method result has the bytes of its returns(...) type", "[doc][ref-result]")
{
   using namespace ref;
   // ── ref-result-run ──
   const std::vector<std::string> two{"alice", "bob"};
   std::vector<char>              bytes;
   REQUIRE(psio::encode_method_result<Directory, 0>(two, bytes).ok());
   // returns(max<4> | element(utf8 | max<16>)): a 1-byte offset table (02 07) ...
   CHECK(hex(bytes) == "02 07 61 6c 69 63 65 62 6f 62");
   // ... where the plain std::vector<std::string> has a 4-byte one.
   CHECK(psio::to_pssz(two).size() == 2 * 4 + 8);
   CHECK(psio::validate_method_result<Directory, 0>(bytes).ok());
   std::vector<std::string> back;
   REQUIRE(psio::decode_method_result<Directory, 0>(bytes, back).ok());
   CHECK(back == two);
   // A result outside returns(...) has no encoding.
   std::vector<char> none;
   CHECK_FALSE(psio::encode_method_result<Directory, 0>(std::vector<std::string>(5, "a"), none).ok());
   CHECK_FALSE(psio::encode_method_result<Directory, 0>(std::vector<std::string>{"\xff"}, none).ok());
   CHECK(none.empty());
   // ── /ref-result-run ──
   // The same bytes as the wrapper-type spelling of the result.
   psio::bounded<psio::utf8_string<16>, 4> same;
   same.push_back(psio::utf8_string<16>{"alice"});
   same.push_back(psio::utf8_string<16>{"bob"});
   CHECK(psio::to_pssz(same) == bytes);
   // A string result has no width of its own: tut::Bank::symbol.
   std::vector<char> symbol;
   REQUIRE(psio::encode_method_result<Bank, 2>(std::string("USD"), symbol).ok());
   CHECK(hex(symbol) == "55 53 44");
   CHECK_FALSE(psio::encode_method_result<Bank, 2>(std::string(9, 'X'), symbol).ok());
}

TEST_CASE("reference: a method result in one call", "[doc][ref-result]")
{
   using namespace ref;
   // ── ref-result-call-run ──
   const std::vector<std::string> two{"alice", "bob"};
   // The callee: the result as returns(...) types it.
   const std::vector<char> bytes = psio::method_result_to_pssz<Directory, 0>(two);
   CHECK(hex(bytes) == "02 07 61 6c 69 63 65 62 6f 62");
   // The caller: validate and decode, decode what is already validated, or view in place.
   CHECK(psio::method_result_from_pssz<Directory, 0>(std::span<const char>{bytes}) == two);
   CHECK(psio::method_result_from_pssz<Directory, 0>(psio::trusted{bytes}) == two);
   const auto view = psio::validated_method_result_view<Directory, 0>(bytes);
   REQUIRE(view.ok());
   CHECK(view->size() == 2);
   CHECK(std::string_view{(*view)[1]} == "bob");
   // An empty optional result is zero bytes when its payload cannot be empty ...
   CHECK(psio::method_result_to_pssz<Directory, 1>(std::optional<Listing>{}).empty());
   CHECK(psio::method_result_from_pssz<Directory, 1>(std::span<const char>{}) == std::nullopt);
   // ... and a present one is the value's own encoding.
   CHECK(psio::method_result_to_pssz<Directory, 1>(std::optional<Listing>{Listing{7, "ok"}}) ==
         psio::to_pssz(Listing{7, "ok"}));
   // A method that returns nothing has no result bytes.
   CHECK(psio::validate_method_result<Directory, 2>({}).ok());
   // ── /ref-result-call-run ──
   CHECK_THROWS_AS((psio::method_result_to_pssz<Directory, 0>(std::vector<std::string>(5, "a"))),
                   psio::codec_exception);

   // ── ref-result-schema-run ──
   // A schema that lists functions its own way, without the Interface envelope.
   psio::SchemaBuilder                builder;
   const psio::schema_types::FunctionType names = builder.insert_method<Directory, 0>();
   const psio::schema_types::FunctionType touch = builder.insert_method<Directory, 2>();
   const psio::Schema                 schema = std::move(builder).build();
   // The result node is the bounded type of returns(...): at most 4 strings ...
   const auto& list = std::get<psio::schema_types::BoundedList>((*names.result)->value);
   CHECK(list.maxCount == 4);
   // ... each UTF-8 of at most 16 bytes.
   CHECK(std::get<psio::schema_types::Custom>(list.type->value).id == "utf8");
   CHECK_FALSE(touch.result.has_value());
   // A schema-driven reader checks a result against that node.
   bool unknown = false;
   CHECK(psio::pssz_ir::validate_auto(schema, **names.result, bytes, &unknown).ok());
   // ── /ref-result-schema-run ──
}

TEST_CASE("reference: variant alternative labels", "[doc][ref-labels]")
{
   using namespace ref;
   // ── ref-labels-run ──
   // The label is the key of the tagged text form ...
   CHECK(psio::encode(psio::ijson{}, Shape{Circle{3}}) == R"({"Circle":{"radius":3}})");
   CHECK(psio::encode(psio::ijson{}, Shape{std::uint32_t{7}}) == R"({"uint32":7})");
   CHECK(psio::encode(psio::ijson{}, Repeated{std::in_place_index<2>, "b"}) == R"({"2":"b"})");
   CHECK(psio::encode(psio::ijson{}, Sized{Square{4}}) == R"({"square":{"side":4}})");
   CHECK(psio::decode<Sized>(psio::ijson{}, std::string_view{R"({"side":5})"}) ==
         Sized{std::uint32_t{5}});
   // ... and the name of the alternative in the schema.
   psio::SchemaBuilder builder;
   const auto          type   = builder.insert<Sized>();
   const psio::Schema  schema = std::move(builder).build();
   const auto& members = std::get<psio::schema_types::Variant>(type.resolve(schema)->value).members;
   CHECK(members[0].name == "circle");
   CHECK(members[2].name == "side");
   // Binary has no labels: an alternative is its position.
   CHECK(hex(psio::to_pssz(Sized{std::uint32_t{5}})) == "02 05 00 00 00");
   // ── /ref-labels-run ──
}

TEST_CASE("reference: names as numbers", "[doc][ref-names]")
{
   // ── ref-names-run ──
   CHECK(psio::number_to_name(6697231) == "alice");
   CHECK(psio::detail::number_to_method(psio::detail::method_to_number("Transfer")) == "transfer");
   // A hash number is shown as '#' and 16 letters, and reads back.
   CHECK(psio::detail::number_to_method(0xd55df3070b8758ebull) == "#hmlnslheseoypnnp");
   CHECK(psio::detail::method_to_number("#hmlnslheseoypnnp") == 0xd55df3070b8758ebull);
   // ── /ref-names-run ──
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

TEST_CASE("reference: a schema id and nothing else", "[doc][ref-schema-id]")
{
   using ref::Call;
   // ── ref-schema-id-run ──
   const psio::Schema schema = psio::SchemaBuilder{}.insert<Call>("Call").build();
   CHECK(contains(psio::to_ijson(schema), R"("id":"Action")"));

   Call call{1, 2, 3, {'h', 'i'}};
   auto bytes = psio::to_pssz(call);
   // Without a handler the node is its wire type: the record's own text.
   std::string text;
   CHECK(psio::pssz_ir::pssz_to_json(schema, "Call", psio::json{}, std::span<const char>{bytes}, text).ok());
   CHECK(text == psio::to_json(call));
   // A handler registered for the id presents it: any one JSON value.
   psio::pssz_ir::CustomHandlers handlers;
   handlers.add("Action",
                {.to_json = [](std::span<const char> pssz, const psio::pssz_ir::JsonContext&,
                               std::string& json) -> psio::codec_status {
                    const Call c = psio::from_pssz<Call>(psio::trusted{pssz});
                    json += R"({"call":")" + std::to_string(c.service) + "::" +
                            std::to_string(c.method) + R"(","args":")" +
                            std::string{c.data.begin(), c.data.end()} + R"("})";
                    return psio::codec_ok();
                 }});
   text.clear();
   CHECK(psio::pssz_ir::pssz_to_json(schema, "Call", psio::json{}, std::span<const char>{bytes}, text,
                                     nullptr, &handlers)
            .ok());
   CHECK(text == R"({"call":"2::3","args":"hi"})");
   // The record is overwritten in place like any other.
   CHECK(psio::mutable_view<Call, psio::pssz>{bytes}.get<0>().set(9));
   call.sender = 9;
   CHECK(bytes == psio::to_pssz(call));
   // ── /ref-schema-id-run ──
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

TEST_CASE("reference: what fits under a cap of 255", "[doc][ref-cap-transfer]")
{
   using namespace ref;
   // ── ref-cap-transfer-run ──
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
   // ── /ref-cap-transfer-run ──
   try
   {
      (void)psio::to_pssz(Transfer{1, 2, 3, memo, std::vector<std::uint8_t>(152, 0)});
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(std::string_view{e.what()} == "pssz: encoded size exceeds maxDynamicData cap");
   }
   auto frame = psio::to_pssz(full);
   frame.push_back(0);
   CHECK_FALSE(psio::validate_pssz<Transfer>(frame).ok());
}

TEST_CASE("reference: a cap narrows the containers beneath it", "[doc][ref-cap-positions]")
{
   using namespace ref;
   // ── ref-cap-positions-run ──
   const Batch     capped{1, {"a", "bc"}, {{"k", "v"}}};
   const BatchOpen open{1, {"a", "bc"}, {{"k", "v"}}};
   // tags is the table 02 03, then "a" "bc"; labels is the table 01, then one
   // entry: header 02, slots 02 03, "k" "v". Every one of them is 1 byte wide.
   CHECK(hex(psio::to_pssz(capped)) ==
         "06 01 00 00 00 06 0b 02 03 61 62 63 01 02 02 03 6b 76");
   CHECK(psio::to_pssz(capped).size() == 18);
   // Without the cap the same value has 4-byte headers, slots and tables.
   CHECK(psio::to_pssz(open).size() == 45);
   CHECK(psio::from_pssz<Batch>(psio::to_pssz(capped)) == capped);
   // A view follows the same widths.
   const auto                   bytes = psio::to_pssz(capped);
   const psio::pssz_view<Batch> view{bytes};
   CHECK(view.get<1>()[1].view_() == "bc");
   // ── /ref-cap-positions-run ──

   // ── ref-cap-child-run ──
   // The list's table is 1 byte wide under the cap; each Item is the same
   // bytes it is everywhere else, with its own 4-byte header and slot.
   const Item item{7, "x"};
   CHECK(psio::to_pssz(item).size() == 4 + 4 + 4 + 1);
   CHECK(psio::to_pssz(Order{1, {item}}).size() == (1 + 4 + 1) + 1 + 13);
   // The bounded child is 1 + 4 + 1 + 1 = 7 bytes.
   CHECK(psio::to_pssz(SmallOrder{1, {SmallItem{7, "x"}}}).size() == (1 + 4 + 1) + 1 + 7);
   // ── /ref-cap-child-run ──
}

TEST_CASE("reference: views read members under annotations", "[doc][ref-view-annotated]")
{
   using namespace ref;
   // ── ref-view-annotated-run ──
   // names: std::array<std::string, 3> under element(utf8 | max<16>) has a
   // 1-byte table; the view reads it at that width.
   const Slots s{{1, 2, 3, 4}, {"a", "", "last"}};
   const auto  slot_bytes = psio::to_pssz(s);
   const psio::pssz_view<Slots> slots{slot_bytes};
   CHECK(slots.get<1>()[2].view_() == "last");

   // votes: a map under max<32> | keys(utf8 | max<16>); author: a pair under at<0>(...).
   const Post p{"hello", {"a", "b"}, {{"alice", 3}}, std::nullopt, {"bob", 9}};
   const auto post_bytes = psio::to_pssz(p);
   const psio::pssz_view<Post> post{post_bytes};
   CHECK(post.get<1>()[1].view_() == "b");
   CHECK(post.get<2>()[0].get<0>().view_() == "alice");
   CHECK(post.get<2>()[0].get<1>() == 3u);
   CHECK(post.get<4>().get<0>().view_() == "bob");
   // ── /ref-view-annotated-run ──

   // A variant whose at<I>(...) narrows no width is viewable.
   const auto choice_bytes = psio::to_pssz(Choice{std::string("s")});
   CHECK(psio::pssz_view<Choice>{choice_bytes}.get<0>().index() == 1);
}

TEST_CASE("reference: bit types", "[doc][ref-bits]")
{
   using namespace ref;
   Flags f;
   f.kind = 1;
   const auto bytes = psio::to_pssz(f);
   CHECK(psio::validate_pssz<Flags>(bytes).ok());
   CHECK(bytes.size() <= *psio::inferred_bound<Flags>());
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
