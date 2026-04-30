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

}  // namespace psio_bench
