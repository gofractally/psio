#pragma once
//
// libraries/psio/cpp/benchmarks/shapes.hpp — bench shape library.
//
// Reflected struct definitions used by the benchmark harness, ranging
// from a fixed-size 8-byte Point through a 100-element ValidatorList
// at ~12 KB.  Each tier exists as both an unbounded form (plain
// std::string / std::vector) and a `psio::length_bound`-annotated
// `*Bounded` form so codecs that key off bounds can be measured both
// ways.
//
// Naming policy: types live at global scope so PSIO_REFLECT works
// without namespace gymnastics; factories live in `psio_bench::`.
// (Was previously `psio_bench::` when the library was called psio —
// renamed alongside the psio → psio rename.)

#include <psio/annotate.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── Tier 1: Point — 8 B fixed, DWNC ───────────────────────────────────
struct Point
{
   std::int32_t x = 0, y = 0;
   friend bool operator==(const Point&, const Point&) = default;
};
PSIO_REFLECT(Point, x, y, definitionWillNotChange())

// ── Tier 2: NameRecord — 16 B fixed, DWNC ─────────────────────────────
struct NameRecord
{
   std::uint64_t account = 0, limit = 0;
   friend bool operator==(const NameRecord&, const NameRecord&) = default;
};
PSIO_REFLECT(NameRecord, account, limit, definitionWillNotChange())

// ── Tier 3: FlatRecord — non-DWNC, variable ───────────────────────────
struct FlatRecord
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const FlatRecord&, const FlatRecord&) = default;
};
PSIO_REFLECT(FlatRecord, id, label, values)

// ── Tier 4: Record — non-DWNC, variable + optional ────────────────────
struct Record
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const Record&, const Record&) = default;
};
PSIO_REFLECT(Record, id, label, values, score)

// ── Tier 5: Validator — 65 B fixed, DWNC ──────────────────────────────
//
// Packed with __attribute__((packed)) so sum(field_sizes) == sizeof(T).
// Matches the canonical Ethereum Validator memory layout for wire
// correctness with pssz/bin/ssz DWNC memcpy fast paths.
struct __attribute__((packed)) Validator
{
   std::uint64_t pubkey_lo          = 0;
   std::uint64_t pubkey_hi          = 0;
   std::uint64_t withdrawal_lo      = 0;
   std::uint64_t withdrawal_hi      = 0;
   std::uint64_t effective_balance  = 0;
   bool          slashed            = false;
   std::uint64_t activation_epoch   = 0;
   std::uint64_t exit_epoch         = 0;
   std::uint64_t withdrawable_epoch = 0;
   friend bool operator==(const Validator&, const Validator&) = default;
};
PSIO_REFLECT(Validator,
             pubkey_lo, pubkey_hi, withdrawal_lo, withdrawal_hi,
             effective_balance, slashed, activation_epoch, exit_epoch,
             withdrawable_epoch, definitionWillNotChange())

// ── Tier 6: Order — nested records + vector + optional ────────────────
struct LineItem
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const LineItem&, const LineItem&) = default;
};
PSIO_REFLECT(LineItem, product, qty, unit_price)

struct UserProfile
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const UserProfile&, const UserProfile&) = default;
};
PSIO_REFLECT(UserProfile, id, name, email, age, verified)

struct Order
{
   std::uint64_t              id = 0;
   UserProfile                customer;
   std::vector<LineItem>      items;
   double                     total = 0.0;
   std::optional<std::string> note;
   friend bool operator==(const Order&, const Order&) = default;
};
PSIO_REFLECT(Order, id, customer, items, total, note)

// ── Tier 7: ValidatorList — N Validators (~12 KB at N=100) ────────────
struct ValidatorList
{
   std::uint64_t          epoch = 0;
   std::vector<Validator> validators;
   friend bool operator==(const ValidatorList&,
                           const ValidatorList&) = default;
};
PSIO_REFLECT(ValidatorList, epoch, validators)

// ── Tier 8: bounded variants of the unbounded shapes ──────────────────
//
// Same data as Tier 3-7, with explicit length_bound annotations
// attached via PSIO_FIELD_ATTRS-style `attr(name, max<N>)`.

struct FlatRecordBounded
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint32_t> values;
   friend bool operator==(const FlatRecordBounded&,
                           const FlatRecordBounded&) = default;
};
PSIO_REFLECT(FlatRecordBounded, id, attr(label, max<63>),
             attr(values, max<255>))

struct RecordBounded
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint32_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const RecordBounded&,
                           const RecordBounded&) = default;
};
PSIO_REFLECT(RecordBounded, id, attr(label, max<63>),
             attr(values, max<255>), score)

struct LineItemBounded
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const LineItemBounded&,
                           const LineItemBounded&) = default;
};
PSIO_REFLECT(LineItemBounded, attr(product, max<63>), qty, unit_price)

struct UserProfileBounded
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const UserProfileBounded&,
                           const UserProfileBounded&) = default;
};
PSIO_REFLECT(UserProfileBounded, id, attr(name, max<63>),
             attr(email, max<255>), age, verified)

struct OrderBounded
{
   std::uint64_t                  id = 0;
   UserProfileBounded             customer;
   std::vector<LineItemBounded>   items;
   double                         total = 0.0;
   std::optional<std::string>     note;
   friend bool operator==(const OrderBounded&,
                           const OrderBounded&) = default;
};
PSIO_REFLECT(OrderBounded, id, customer, attr(items, max<255>), total,
             attr(note, max<255>))

struct ValidatorListBounded
{
   std::uint64_t          epoch = 0;
   std::vector<Validator> validators;
   friend bool operator==(const ValidatorListBounded&,
                           const ValidatorListBounded&) = default;
};
PSIO_REFLECT(ValidatorListBounded, epoch, attr(validators, max<1024>))

// ── Dwnc twins of the variable-shape tiers ────────────────────────────
//
// definitionWillNotChange() tells extensible binary formats (frac,
// pssz) they can drop the per-record header that normally lets old
// readers tolerate new fields.  Same data; ~2 bytes smaller wire and
// the static-offset memcpy fast path in pssz/bin.
//
// We pair each non-DWNC shape with a Dwnc twin so format comparisons
// stay apples-to-apples — extensible-aware formats shouldn't be
// penalised for header overhead when the user has explicitly opted
// into a frozen layout.

struct FlatRecordDwnc
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const FlatRecordDwnc&,
                           const FlatRecordDwnc&) = default;
};
PSIO_REFLECT(FlatRecordDwnc, id, label, values, definitionWillNotChange())

struct RecordDwnc
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const RecordDwnc&, const RecordDwnc&) = default;
};
PSIO_REFLECT(RecordDwnc, id, label, values, score,
             definitionWillNotChange())

struct LineItemDwnc
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const LineItemDwnc&,
                           const LineItemDwnc&) = default;
};
PSIO_REFLECT(LineItemDwnc, product, qty, unit_price,
             definitionWillNotChange())

struct UserProfileDwnc
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const UserProfileDwnc&,
                           const UserProfileDwnc&) = default;
};
PSIO_REFLECT(UserProfileDwnc, id, name, email, age, verified,
             definitionWillNotChange())

struct OrderDwnc
{
   std::uint64_t                id = 0;
   UserProfileDwnc              customer;
   std::vector<LineItemDwnc>    items;
   double                       total = 0.0;
   std::optional<std::string>   note;
   friend bool operator==(const OrderDwnc&, const OrderDwnc&) = default;
};
PSIO_REFLECT(OrderDwnc, id, customer, items, total, note,
             definitionWillNotChange())

struct ValidatorListDwnc
{
   std::uint64_t          epoch = 0;
   std::vector<Validator> validators;
   friend bool operator==(const ValidatorListDwnc&,
                           const ValidatorListDwnc&) = default;
};
PSIO_REFLECT(ValidatorListDwnc, epoch, validators,
             definitionWillNotChange())

// ── Deep nesting: 4 levels, extensible vs DWNC ────────────────────────
//
// For view_one fairness — accessing `outer.a.a.a.value` forces every
// nesting layer to be traversed, exposing the cost of offset-table
// walks in extensible formats vs static-offset reads in DWNC formats.
//
// Two parallel chains so we can compare side-by-side:
//   - Inner4Ext / Inner3Ext / Inner2Ext / Inner1Ext / Deep4Ext —
//     no DWNC at any level → variable-size record header per layer.
//   - Inner4Dwnc / .../Deep4Dwnc — definitionWillNotChange() at every
//     level → no header, fast static offsets.

struct Inner4Ext { std::uint64_t value = 0; };
PSIO_REFLECT(Inner4Ext, value)

struct Inner3Ext { Inner4Ext child; std::uint64_t pad3 = 0; };
PSIO_REFLECT(Inner3Ext, child, pad3)

struct Inner2Ext { Inner3Ext child; std::uint64_t pad2 = 0; };
PSIO_REFLECT(Inner2Ext, child, pad2)

struct Inner1Ext { Inner2Ext child; std::uint64_t pad1 = 0; };
PSIO_REFLECT(Inner1Ext, child, pad1)

struct Deep4Ext { Inner1Ext root; };
PSIO_REFLECT(Deep4Ext, root)

struct Inner4Dwnc { std::uint64_t value = 0; };
PSIO_REFLECT(Inner4Dwnc, value, definitionWillNotChange())

struct Inner3Dwnc { Inner4Dwnc child; std::uint64_t pad3 = 0; };
PSIO_REFLECT(Inner3Dwnc, child, pad3, definitionWillNotChange())

struct Inner2Dwnc { Inner3Dwnc child; std::uint64_t pad2 = 0; };
PSIO_REFLECT(Inner2Dwnc, child, pad2, definitionWillNotChange())

struct Inner1Dwnc { Inner2Dwnc child; std::uint64_t pad1 = 0; };
PSIO_REFLECT(Inner1Dwnc, child, pad1, definitionWillNotChange())

struct Deep4Dwnc { Inner1Dwnc root; };
PSIO_REFLECT(Deep4Dwnc, root, definitionWillNotChange())

// ── Variety stress shapes — primitives BSON Vector + msgpack bin / ────
//
// MlEmbedding: vec<float> as a 64-d ML embedding.  Exercises BSON's
// Vector subtype 0x09 dtype 0x10 (FLOAT32) — and shows the wire-size
// gap between mongo-idiomatic encoders and naive array-of-double
// fallbacks.
//
// BlobPayload: vec<uint8_t> raw bytes.  Hits BSON generic binary
// subtype 0x00, protobuf `bytes`, msgpack `bin`.  Common in transit /
// blob-store workloads.
//
// WideRecord: 32 u32 fields.  Stresses vtable size in flatbuf, slot
// tables in pjson, and fixed-region offset arithmetic in pssz/ssz —
// places where N=9 (Validator) wasn't enough.

struct MlEmbedding
{
   std::uint64_t      id = 0;
   std::vector<float> embedding;
   friend bool operator==(const MlEmbedding&, const MlEmbedding&) = default;
};
PSIO_REFLECT(MlEmbedding, id, embedding, definitionWillNotChange())

struct BlobPayload
{
   std::uint64_t             id = 0;
   std::vector<std::uint8_t> bytes;
   friend bool operator==(const BlobPayload&, const BlobPayload&) = default;
};
PSIO_REFLECT(BlobPayload, id, bytes, definitionWillNotChange())

struct WideRecord
{
   std::uint32_t f00 = 0, f01 = 0, f02 = 0, f03 = 0;
   std::uint32_t f04 = 0, f05 = 0, f06 = 0, f07 = 0;
   std::uint32_t f08 = 0, f09 = 0, f10 = 0, f11 = 0;
   std::uint32_t f12 = 0, f13 = 0, f14 = 0, f15 = 0;
   std::uint32_t f16 = 0, f17 = 0, f18 = 0, f19 = 0;
   std::uint32_t f20 = 0, f21 = 0, f22 = 0, f23 = 0;
   std::uint32_t f24 = 0, f25 = 0, f26 = 0, f27 = 0;
   std::uint32_t f28 = 0, f29 = 0, f30 = 0, f31 = 0;
   friend bool operator==(const WideRecord&, const WideRecord&) = default;
};
PSIO_REFLECT(WideRecord,
             f00, f01, f02, f03, f04, f05, f06, f07,
             f08, f09, f10, f11, f12, f13, f14, f15,
             f16, f17, f18, f19, f20, f21, f22, f23,
             f24, f25, f26, f27, f28, f29, f30, f31,
             definitionWillNotChange())

// ── Realistic shapes for the compressed-wire-size bench ───────────────
//
// The shapes above stress format mechanics (offset tables, DWNC fast
// paths, varint widths) but produce wire bytes that are either too
// small or too uniform for compressors to learn meaningful structure
// from.  These five shapes are populated with deterministic but
// compression-friendly data — repeated key strings, monotonic
// timestamps, low-cardinality enums, structural redundancy at depth —
// so lz4/zstd have something to express the format-vs-format gap on
// after compression.
//
// Naming: Realistic* prefix.  Sizes target 1–50 KB raw — large enough
// for the compressor to amortise its fixed overhead, small enough to
// keep the bench tractable on every shape × format × op combination.

// ── HttpApiResponse — mixed types in a nested record ──────────────────
//
// One status code, one optional error string, a request id, a small
// header map, a body payload.  Realistic header keys repeat across
// requests (content-type, x-request-id, …) → high compression ratio
// on key columns even at this size.  Target ~1–2 KB raw.

struct HttpHeader
{
   std::string key;
   std::string value;
   friend bool operator==(const HttpHeader&, const HttpHeader&) = default;
};
PSIO_REFLECT(HttpHeader, key, value)

struct HttpApiResponse
{
   std::uint64_t              request_id = 0;
   std::uint64_t              session_id = 0;
   std::uint32_t              status     = 0;
   std::optional<std::string> error;
   std::vector<HttpHeader>    headers;
   std::string                body;
   friend bool operator==(const HttpApiResponse&,
                          const HttpApiResponse&) = default;
};
PSIO_REFLECT(HttpApiResponse,
             request_id, session_id, status, error, headers, body)

// ── BlockOfTransactions — column-friendly ledger block ────────────────
//
// 100 transactions.  from/to addresses cluster around a small set
// (high inter-record redundancy), nonce monotonically increases (most
// bytes zero), amount stays in u64 territory (top half always 0), data
// is mostly empty/short (about half the records carry a 32-byte
// payload).  Target ~50 KB raw.  Canonical "ledger block" — the kind
// of payload pssz/ssz/borsh compress very well on because the columnar
// patterns repeat exactly the way a block-based compressor wants.

struct Transaction
{
   std::uint64_t             from   = 0;
   std::uint64_t             to     = 0;
   std::uint64_t             nonce  = 0;
   std::uint64_t             amount = 0;
   std::vector<std::uint8_t> data;
   friend bool operator==(const Transaction&,
                          const Transaction&) = default;
};
PSIO_REFLECT(Transaction, from, to, nonce, amount, data)

struct BlockOfTransactions
{
   std::uint64_t            block_number = 0;
   std::uint64_t            timestamp    = 0;
   std::vector<Transaction> txs;
   friend bool operator==(const BlockOfTransactions&,
                          const BlockOfTransactions&) = default;
};
PSIO_REFLECT(BlockOfTransactions, block_number, timestamp, txs)

// ── ConfigTree — recursive config-style document ──────────────────────
//
// Three to four levels of nesting, lots of repeated string keys, mixed
// optional leaves of each scalar type.  std::variant of recursive
// types is hard to reflect cleanly, so this uses a flat record per
// node with an optional of each leaf type and a vector of children.
// Compressors love this shape because the same key strings recur
// many times across siblings + descendants.  Target ~5 KB raw.

struct ConfigEntry
{
   std::string                  key;
   std::optional<std::string>   string_val;
   std::optional<std::int64_t>  int_val;
   std::optional<double>        double_val;
   std::optional<bool>          bool_val;
   friend bool operator==(const ConfigEntry&,
                          const ConfigEntry&) = default;
};
PSIO_REFLECT(ConfigEntry, key, string_val, int_val, double_val, bool_val)

struct ConfigSection
{
   std::string              name;
   std::vector<ConfigEntry> entries;
   friend bool operator==(const ConfigSection&,
                          const ConfigSection&) = default;
};
PSIO_REFLECT(ConfigSection, name, entries)

struct ConfigGroup
{
   std::string                name;
   std::vector<ConfigSection> sections;
   friend bool operator==(const ConfigGroup&,
                          const ConfigGroup&) = default;
};
PSIO_REFLECT(ConfigGroup, name, sections)

struct ConfigTree
{
   std::string              schema_version;
   std::vector<ConfigGroup> groups;
   friend bool operator==(const ConfigTree&, const ConfigTree&) = default;
};
PSIO_REFLECT(ConfigTree, schema_version, groups)

// ── TimeSeriesChunk — metrics-database shape ──────────────────────────
//
// 1024 time points: timestamp monotonically increases by ~1000ns each
// (top bits all zero), value drifts via a smooth function (most f64
// bits stay in a narrow exponent range — good for delta-friendly
// compressors), label_idx is one of ~10 distinct values (low
// cardinality).  Target ~24 KB raw.  Tests how each format handles
// the metrics / OLAP / time-series workload that production telemetry
// systems run all day.

struct TimePoint
{
   std::uint64_t timestamp = 0;
   double        value     = 0.0;
   std::uint32_t label_idx = 0;
   friend bool operator==(const TimePoint&, const TimePoint&) = default;
};
PSIO_REFLECT(TimePoint, timestamp, value, label_idx,
             definitionWillNotChange())

struct TimeSeriesChunk
{
   std::uint64_t            series_id = 0;
   std::uint64_t            start_ns  = 0;
   std::vector<TimePoint>   points;
   std::vector<std::string> labels;
   friend bool operator==(const TimeSeriesChunk&,
                          const TimeSeriesChunk&) = default;
};
PSIO_REFLECT(TimeSeriesChunk, series_id, start_ns, points, labels)

// ── MixedDocument — JSON-equivalent payload ───────────────────────────
//
// User profile + activity feed.  Typical "what would normally be JSON
// over HTTP" shape: nested objects, mixed scalar types, string-keyed
// records, optional, vector-of-vectors via the actions list.  Target
// ~2 KB raw.  Compressors find structural redundancy in the field
// names and the repeated action types.

struct UserPrefEntry
{
   std::string key;
   std::string value;
   friend bool operator==(const UserPrefEntry&,
                          const UserPrefEntry&) = default;
};
PSIO_REFLECT(UserPrefEntry, key, value)

struct UserAction
{
   std::uint64_t timestamp = 0;
   std::string   verb;
   std::string   target;
   std::vector<std::string> tags;
   friend bool operator==(const UserAction&, const UserAction&) = default;
};
PSIO_REFLECT(UserAction, timestamp, verb, target, tags)

struct MixedDocument
{
   std::uint64_t                  id = 0;
   std::string                    name;
   std::string                    email;
   std::optional<std::string>     bio;
   std::vector<UserPrefEntry>     prefs;
   std::vector<UserAction>        recent_actions;
   friend bool operator==(const MixedDocument&,
                          const MixedDocument&) = default;
};
PSIO_REFLECT(MixedDocument,
             id, name, email, bio, prefs, recent_actions)

// ── Sample factories ──────────────────────────────────────────────────
namespace psio_bench {

   inline Point      point() { return {.x = -42, .y = 77}; }

   inline NameRecord namerec()
   {
      return {.account = 0x0123'4567'89AB'CDEFull, .limit = 1'000'000};
   }

   inline FlatRecord flatrec()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }

   inline Record record()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }

   inline Validator make_validator(std::uint64_t i)
   {
      Validator v;
      v.pubkey_lo          = i * 7;
      v.pubkey_hi          = i * 11;
      v.withdrawal_lo      = i * 13;
      v.withdrawal_hi      = i * 17;
      v.effective_balance  = 32'000'000'000ull;
      v.slashed            = (i % 50) == 0;
      v.activation_epoch   = 100;
      v.exit_epoch         = 0xFFFFFFFFull;
      v.withdrawable_epoch = 0xFFFFFFFFull;
      return v;
   }
   inline Validator validator() { return make_validator(1); }

   inline Order order()
   {
      Order o;
      o.id       = 10042;
      o.customer = UserProfile{.id = 77, .name = "Alice Stone",
                                .email = "alice@example.com",
                                .age = 34, .verified = true};
      o.items    = {
         LineItem{.product = "Widget",      .qty = 2, .unit_price = 9.99},
         LineItem{.product = "Sprocket",    .qty = 1, .unit_price = 14.50},
         LineItem{.product = "Gizmo-Mk.II", .qty = 5, .unit_price = 3.25},
      };
      o.total = 2 * 9.99 + 14.50 + 5 * 3.25;
      o.note  = std::string{"gift-wrap please"};
      return o;
   }

   inline ValidatorList vlist(std::uint32_t n = 100)
   {
      ValidatorList l;
      l.epoch = 42;
      l.validators.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
         l.validators.push_back(make_validator(i));
      return l;
   }

   // ── Bounded factories ────────────────────────────────────────────
   inline FlatRecordBounded flatrec_bounded()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }

   inline RecordBounded record_bounded()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }

   inline OrderBounded order_bounded()
   {
      OrderBounded o;
      o.id        = 10042;
      o.customer  = UserProfileBounded{.id = 77, .name = "Alice Stone",
                                        .email = "alice@example.com",
                                        .age = 34, .verified = true};
      o.items     = {
         LineItemBounded{.product = "Widget",
                          .qty = 2, .unit_price = 9.99},
         LineItemBounded{.product = "Sprocket",
                          .qty = 1, .unit_price = 14.50},
         LineItemBounded{.product = "Gizmo-Mk.II",
                          .qty = 5, .unit_price = 3.25},
      };
      o.total = 2 * 9.99 + 14.50 + 5 * 3.25;
      o.note  = std::string{"gift-wrap please"};
      return o;
   }

   inline ValidatorListBounded vlist_bounded(std::uint32_t n = 100)
   {
      ValidatorListBounded l;
      l.epoch = 42;
      l.validators.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
         l.validators.push_back(make_validator(i));
      return l;
   }

   inline FlatRecordDwnc flatrec_dwnc()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }
   inline RecordDwnc record_dwnc()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }
   inline OrderDwnc order_dwnc()
   {
      OrderDwnc o;
      o.id        = 10042;
      o.customer  = UserProfileDwnc{.id = 77, .name = "Alice Stone",
                                     .email = "alice@example.com",
                                     .age = 34, .verified = true};
      o.items     = {
         LineItemDwnc{.product = "Widget",
                       .qty = 2, .unit_price = 9.99},
         LineItemDwnc{.product = "Sprocket",
                       .qty = 1, .unit_price = 14.50},
         LineItemDwnc{.product = "Gizmo-Mk.II",
                       .qty = 5, .unit_price = 3.25},
      };
      o.total = 2 * 9.99 + 14.50 + 5 * 3.25;
      o.note  = std::string{"gift-wrap please"};
      return o;
   }
   inline ValidatorListDwnc vlist_dwnc(std::uint32_t n = 100)
   {
      ValidatorListDwnc l;
      l.epoch = 42;
      l.validators.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
         l.validators.push_back(make_validator(i));
      return l;
   }

   inline MlEmbedding ml_embedding()
   {
      MlEmbedding e;
      e.id = 0xCAFE'BABE'DEAD'BEEFull;
      e.embedding.resize(64);
      // Deterministic non-zero floats so varint formats see real bytes.
      for (std::size_t i = 0; i < 64; ++i)
         e.embedding[i] = static_cast<float>(i) * 0.0125f - 0.4f;
      return e;
   }

   inline BlobPayload blob_payload()
   {
      BlobPayload b;
      b.id = 0x0123'4567'89AB'CDEFull;
      b.bytes.resize(256);
      for (std::size_t i = 0; i < 256; ++i)
         b.bytes[i] = static_cast<std::uint8_t>(i ^ 0xA5);
      return b;
   }

   inline WideRecord wide_record()
   {
      WideRecord w;
      // Spread values so varint formats see varying widths.
      std::uint32_t* fields[] = {
         &w.f00, &w.f01, &w.f02, &w.f03, &w.f04, &w.f05, &w.f06, &w.f07,
         &w.f08, &w.f09, &w.f10, &w.f11, &w.f12, &w.f13, &w.f14, &w.f15,
         &w.f16, &w.f17, &w.f18, &w.f19, &w.f20, &w.f21, &w.f22, &w.f23,
         &w.f24, &w.f25, &w.f26, &w.f27, &w.f28, &w.f29, &w.f30, &w.f31,
      };
      for (std::size_t i = 0; i < 32; ++i)
         *fields[i] = static_cast<std::uint32_t>(0x1000'0000u * (i + 1));
      return w;
   }

   inline Deep4Ext deep4_ext()
   {
      return Deep4Ext{
         .root = Inner1Ext{
            .child = Inner2Ext{
               .child = Inner3Ext{
                  .child = Inner4Ext{.value = 0xDEAD'BEEF'CAFE'BABEull},
                  .pad3 = 3},
               .pad2 = 2},
            .pad1 = 1}};
   }
   inline Deep4Dwnc deep4_dwnc()
   {
      return Deep4Dwnc{
         .root = Inner1Dwnc{
            .child = Inner2Dwnc{
               .child = Inner3Dwnc{
                  .child = Inner4Dwnc{.value = 0xDEAD'BEEF'CAFE'BABEull},
                  .pad3 = 3},
               .pad2 = 2},
            .pad1 = 1}};
   }

   // ── Realistic-shape factories ───────────────────────────────────────
   //
   // Deterministic content — same call always returns the same bytes.
   // Patterns chosen so each shape gives the compressor real structure
   // to find: repeated key strings, monotonic timestamps,
   // low-cardinality enums, narrow value distributions.

   inline HttpApiResponse realistic_http_response()
   {
      // 14 typical HTTP headers — keys repeat across requests in real
      // traffic; values vary in length and content.  Body is a small
      // JSON-like payload.  Target ~1–2 KB raw.
      HttpApiResponse r;
      r.request_id = 0xABCD'1234'5678'90EFull;
      r.session_id = 0xDEAD'BEEF'C0FF'EE77ull;
      r.status     = 200;
      r.error.reset();
      r.headers = {
         {"content-type",        "application/json; charset=utf-8"},
         {"content-length",      "1024"},
         {"x-request-id",        "f47ac10b-58cc-4372-a567-0e02b2c3d479"},
         {"x-trace-id",          "00-0af7651916cd43dd-b7ad6b7169203331-01"},
         {"cache-control",       "no-cache, no-store, must-revalidate"},
         {"server",              "psiserve/0.1.0"},
         {"date",                "Wed, 30 Apr 2026 12:34:56 GMT"},
         {"x-ratelimit-limit",   "1000"},
         {"x-ratelimit-remaining","997"},
         {"x-ratelimit-reset",   "1714485356"},
         {"access-control-allow-origin", "*"},
         {"access-control-allow-methods","GET, POST, PUT, DELETE, OPTIONS"},
         {"x-content-type-options","nosniff"},
         {"strict-transport-security","max-age=31536000; includeSubDomains"},
      };
      r.body =
         "{\"ok\":true,\"data\":{\"users\":["
         "{\"id\":1001,\"name\":\"alice\",\"role\":\"admin\"},"
         "{\"id\":1002,\"name\":\"bob\",\"role\":\"viewer\"},"
         "{\"id\":1003,\"name\":\"carol\",\"role\":\"editor\"},"
         "{\"id\":1004,\"name\":\"dave\",\"role\":\"viewer\"},"
         "{\"id\":1005,\"name\":\"erin\",\"role\":\"viewer\"},"
         "{\"id\":1006,\"name\":\"frank\",\"role\":\"editor\"}],"
         "\"page\":1,\"per_page\":50,\"total\":6}}";
      return r;
   }

   inline BlockOfTransactions realistic_block(std::uint32_t n = 100)
   {
      BlockOfTransactions b;
      b.block_number = 19'500'000;
      b.timestamp    = 1'714'485'356'000'000'000ull;  // 2024-04-30 ns

      // Cluster from/to around a few hot addresses (~8 each); real
      // ledger traffic has a long-tail distribution that compresses
      // well at the column level.
      static constexpr std::uint64_t hot_from[8] = {
         0x0000'1111'2222'3333ull, 0x0000'4444'5555'6666ull,
         0x0000'7777'8888'9999ull, 0x0000'AAAA'BBBB'CCCCull,
         0x0000'DEAD'BEEF'0001ull, 0x0000'DEAD'BEEF'0002ull,
         0x0000'CAFE'BABE'0001ull, 0x0000'CAFE'BABE'0002ull,
      };
      static constexpr std::uint64_t hot_to[8] = {
         0x0000'1234'5678'9ABCull, 0x0000'2222'3333'4444ull,
         0x0000'5555'6666'7777ull, 0x0000'8888'9999'AAAAull,
         0x0000'F00D'CAFE'0001ull, 0x0000'F00D'CAFE'0002ull,
         0x0000'BEEF'DEAD'0001ull, 0x0000'BEEF'DEAD'0002ull,
      };

      b.txs.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
      {
         Transaction t;
         t.from   = hot_from[i % 8];
         t.to     = hot_to[(i / 2) % 8];
         t.nonce  = 1'000'000ull + i;             // monotonic
         t.amount = 1'000'000ull * ((i % 7) + 1); // small u64
         if (i % 2 == 0)
         {
            // 32-byte payload, deterministic but varied per index.
            t.data.resize(32);
            for (std::size_t j = 0; j < 32; ++j)
               t.data[j] = static_cast<std::uint8_t>((i * 31 + j) & 0xFF);
         }
         b.txs.push_back(std::move(t));
      }
      return b;
   }

   inline ConfigTree realistic_config_tree()
   {
      // 4 groups × 3 sections × 6 entries = 72 leaf records.  Repeated
      // key prefixes ("max_", "default_", "enable_") and section names
      // give compressors plenty to learn.  Target ~5 KB raw.
      static constexpr const char* group_names[4] = {
         "network", "storage", "compute", "auth",
      };
      static constexpr const char* section_names[3] = {
         "limits", "defaults", "feature_flags",
      };
      static constexpr const char* entry_keys[6] = {
         "max_concurrent_requests", "max_queue_depth",
         "default_timeout_ms",      "default_retries",
         "enable_metrics_export",   "enable_debug_logging",
      };

      ConfigTree t;
      t.schema_version = "config-v1.4.2";
      t.groups.reserve(4);
      for (std::size_t gi = 0; gi < 4; ++gi)
      {
         ConfigGroup g;
         g.name = group_names[gi];
         g.sections.reserve(3);
         for (std::size_t si = 0; si < 3; ++si)
         {
            ConfigSection s;
            s.name = section_names[si];
            s.entries.reserve(6);
            for (std::size_t ei = 0; ei < 6; ++ei)
            {
               ConfigEntry e;
               e.key = entry_keys[ei];
               // Fill exactly one of the four leaf types based on the
               // entry index so every config row has a populated leaf
               // (compressors see the same field-name pattern over
               // and over with different leaf occupancy).
               switch (ei)
               {
                  case 0:
                  case 1:
                     e.int_val =
                        static_cast<std::int64_t>(1024 * (gi + 1) * (si + 1));
                     break;
                  case 2:
                     e.int_val = 30'000;  // ms
                     break;
                  case 3:
                     e.int_val = 5;
                     break;
                  case 4:
                  case 5:
                     e.bool_val = ((gi + si + ei) % 2) == 0;
                     break;
               }
               // Add a description string on every other entry so
               // string-key reuse compresses too.
               if ((ei & 1) == 0)
                  e.string_val =
                     std::string{"applies to "} + group_names[gi] +
                     std::string{"/"} + section_names[si];
               s.entries.push_back(std::move(e));
            }
            g.sections.push_back(std::move(s));
         }
         t.groups.push_back(std::move(g));
      }
      return t;
   }

   inline TimeSeriesChunk realistic_time_series(std::uint32_t n = 1024)
   {
      TimeSeriesChunk c;
      c.series_id = 0xC001'CAFE'D00D'5EEDull;
      c.start_ns  = 1'714'485'000'000'000'000ull;
      c.points.reserve(n);
      // ~1000 ns spacing with small jitter — high-bit zeros stay
      // identical across all records, low-bit drift gives compressors
      // delta structure.  Value drifts via a smooth sinusoidal-style
      // function that keeps f64 exponent bits near-constant.
      for (std::uint32_t i = 0; i < n; ++i)
      {
         TimePoint p;
         p.timestamp = c.start_ns +
                       static_cast<std::uint64_t>(i) * 1000ull +
                       (i * 13ull) % 7ull;        // jitter
         // Smooth drift in [-1.0, 1.0] range — most mantissa bits
         // change slowly, exponent stays near 1.0.
         const double phase = static_cast<double>(i) * 0.006;
         p.value = 0.5 + 0.4 * (
            // Cheap sinusoid approximation without <cmath> dependency
            // ordering pain — Taylor series at small phase.
            phase - (phase * phase * phase) / 6.0);
         p.label_idx = i % 10;  // 10 distinct labels
         c.points.push_back(p);
      }
      // Label dictionary — short, low-cardinality strings repeated
      // across the chunk via label_idx.
      c.labels = {
         "cpu.user",       "cpu.system",     "cpu.iowait",
         "memory.rss",     "memory.virt",    "disk.read_bytes",
         "disk.write_bytes","net.rx_bytes",  "net.tx_bytes",
         "gc.pause_ms",
      };
      return c;
   }

   inline MixedDocument realistic_mixed_document()
   {
      MixedDocument d;
      d.id    = 42'857'913;
      d.name  = "Alice Stone";
      d.email = "alice.stone@example.com";
      d.bio   = std::string{
         "Engineer; works on distributed systems; based in Berlin."};
      d.prefs = {
         {"locale",           "en-US"},
         {"timezone",         "Europe/Berlin"},
         {"theme",            "dark"},
         {"notifications",    "email,push"},
         {"two_factor",       "totp"},
         {"items_per_page",   "50"},
         {"sort_order",       "recent"},
         {"experimental",     "beta-channel"},
      };
      // Activity feed: 24 actions across 4 verbs × 6 targets, with
      // tag overlap.  Verbs and tag strings repeat → compressor
      // expresses that as ~3-4× ratio on the activity portion.
      static constexpr const char* verbs[4]   = {
         "viewed", "edited", "commented_on", "starred",
      };
      static constexpr const char* targets[6] = {
         "doc/architecture-overview", "doc/api-reference",
         "issue/perf-regression-12",  "issue/release-checklist",
         "pr/payments-refactor",      "pr/observability-rollout",
      };
      static constexpr const char* tag_pool[6] = {
         "review", "follow-up", "urgent", "doc", "infra", "perf",
      };
      d.recent_actions.reserve(24);
      for (std::uint32_t i = 0; i < 24; ++i)
      {
         UserAction a;
         a.timestamp = 1'714'400'000'000'000'000ull +
                       static_cast<std::uint64_t>(i) * 60'000'000'000ull;
         a.verb   = verbs[i % 4];
         a.target = targets[i % 6];
         a.tags   = {tag_pool[i % 6], tag_pool[(i + 2) % 6]};
         d.recent_actions.push_back(std::move(a));
      }
      return d;
   }

}  // namespace psio_bench
