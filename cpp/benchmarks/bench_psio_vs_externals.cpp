// libraries/psio/cpp/benchmarks/bench_psio_vs_externals.cpp
//
// Unified head-to-head: psio's wire formats × canonical external
// libraries × shapes × ops.  Replaces the old v1↔v3 bench_perf_report
// matrix.  Writes one snapshot CSV per run to
//   bench_snapshots/perf_<UTC-ISO>_<commit>.csv
// (gitignored by default; commit a snapshot only when accepting it as
//  a new baseline).
//
// Phase 3 scaffold: psio-only columns first.  Adapter wiring for
// canonical external libs (msgpack-cxx, libprotobuf, libflatbuffers,
// libcapnp, sszpp) lands as separate commits, each gated behind a
// CMake find_package().  See .issues/psio-bench-vs-externals-plan.md.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/bson.hpp>
#include <psio/borsh.hpp>
#include <psio/capnp.hpp>
#include <psio/flatbuf.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/pssz_view.hpp>
#include <psio/view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include "harness.hpp"
#include "shapes.hpp"

#ifdef PSIO_HAVE_MSGPACK
#  include "adapters/msgpack_adapter.hpp"
#endif
#ifdef PSIO_HAVE_PROTOBUF
#  include "adapters/protobuf_adapter.hpp"
#endif
#ifdef PSIO_HAVE_FLATBUF
#  include "adapters/flatbuf_adapter.hpp"
#endif
#ifdef PSIO_HAVE_CAPNP
#  include "adapters/capnp_adapter.hpp"
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <vector>

using namespace psio_bench;

namespace {

   //  bench_view_target(decoded) — return one scalar from a meaningful
   //  place inside the decoded value, used by the view_one op.
   //
   //  Critical: the field MUST be deep enough that the optimiser can't
   //  shortcut the decode.  Earlier we used member_pointer<0> which
   //  for shapes whose first field is at byte offset 0 (e.g. ssz / bin
   //  ValidatorList → epoch at offset 0) let the compiler DCE the
   //  rest of the decode and read straight from the wire.  By
   //  targeting a deep field — vector[mid].field, customer.id,
   //  root.child.child.child.value — the bench measures honest
   //  decode-and-reach cost.
   //
   //  Default falls back to first arithmetic field for the shapes
   //  that are top-level scalars only (Point, NameRecord, …) — for
   //  those the optimisation we're worried about doesn't apply
   //  because there's nothing else to elide.
   template <typename T>
   inline std::uint64_t bench_view_target(const T& v)
   {
      using R  = ::psio::reflect<T>;
      constexpr auto p0 = R::template member_pointer<0>;
      using F0 = std::remove_cvref_t<typename R::template member_type<0>>;
      if constexpr (std::is_arithmetic_v<F0> ||
                     std::is_same_v<F0, bool>)
         return static_cast<std::uint64_t>(v.*p0);
      else
         return reinterpret_cast<std::uintptr_t>(&(v.*p0));
   }

   //  Specialisations for shapes where the first field is too
   //  shallow / too easily elided.

   inline std::uint64_t bench_view_target(const Order& v)
   { return v.customer.id; }
   inline std::uint64_t bench_view_target(const OrderBounded& v)
   { return v.customer.id; }

   inline std::uint64_t bench_view_target(const ValidatorList& v)
   {
      if (v.validators.empty()) return 0;
      return v.validators[v.validators.size() / 2].pubkey_lo;
   }
   inline std::uint64_t bench_view_target(const ValidatorListBounded& v)
   {
      if (v.validators.empty()) return 0;
      return v.validators[v.validators.size() / 2].pubkey_lo;
   }
   inline std::uint64_t bench_view_target(const ValidatorListDwnc& v)
   {
      if (v.validators.empty()) return 0;
      return v.validators[v.validators.size() / 2].pubkey_lo;
   }
   inline std::uint64_t bench_view_target(const OrderDwnc& v)
   { return v.customer.id; }

   //  Deep4 — the whole point is to access the value 4 nesting levels in.
   inline std::uint64_t bench_view_target(const Deep4Ext& v)
   { return v.root.child.child.child.value; }
   inline std::uint64_t bench_view_target(const Deep4Dwnc& v)
   { return v.root.child.child.child.value; }

   //  Variety shapes — reach into the vector / last field so the codec
   //  has to walk past field 0 (id) and actually touch the bulk payload.
   inline std::uint64_t bench_view_target(const MlEmbedding& v)
   {
      if (v.embedding.empty()) return v.id;
      float        f = v.embedding[v.embedding.size() / 2];
      std::uint32_t bits;
      std::memcpy(&bits, &f, sizeof(bits));
      return bits;
   }
   inline std::uint64_t bench_view_target(const BlobPayload& v)
   {
      if (v.bytes.empty()) return v.id;
      return v.bytes[v.bytes.size() / 2];
   }
   inline std::uint64_t bench_view_target(const WideRecord& v)
   { return v.f31; }


   //  ── view_target_via_view ──────────────────────────────────────────
   //
   //  Same scalar that bench_view_target reads (for the view_one op),
   //  but reached via psio::view<T, Fmt> chains — i.e., zero-copy
   //  field access through the format's record_field_span /
   //  vector_element_span traits.  Each shape gets one overload so
   //  the chain matches what bench_view_target does for the
   //  decode_then_view fallback.
   //
   //  Templates so the same overload covers ssz / pssz / any future
   //  format that supports has_record_support<Fmt, ShapeT>.

   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<Point>)
   {
      psio::view<Point, Fmt> v{sp};
      return static_cast<std::uint64_t>(v.template get<0>());
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<NameRecord>)
   {
      psio::view<NameRecord, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<Validator>)
   {
      psio::view<Validator, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<FlatRecord>)
   {
      psio::view<FlatRecord, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<FlatRecordDwnc>)
   {
      psio::view<FlatRecordDwnc, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<FlatRecordBounded>)
   {
      psio::view<FlatRecordBounded, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<Record>)
   {
      psio::view<Record, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<RecordDwnc>)
   {
      psio::view<RecordDwnc, Fmt> v{sp};
      return v.template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<RecordBounded>)
   {
      psio::view<RecordBounded, Fmt> v{sp};
      return v.template get<0>();
   }

   //  Order / OrderDwnc — bench_view_target returns customer.id
   //  (UserProfile is field index 1; UserProfile.id is field 0).
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<Order>)
   {
      psio::view<Order, Fmt> v{sp};
      return v.template get<1>().template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<OrderDwnc>)
   {
      psio::view<OrderDwnc, Fmt> v{sp};
      return v.template get<1>().template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<OrderBounded>)
   {
      psio::view<OrderBounded, Fmt> v{sp};
      return v.template get<1>().template get<0>();
   }

   //  ValidatorList* — bench_view_target returns
   //  validators[validators.size() / 2].pubkey_lo.
   //  validators is field index 1; pubkey_lo is field 0 of Validator.
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<ValidatorList>)
   {
      psio::view<ValidatorList, Fmt> v{sp};
      auto vec = v.template get<1>();
      const auto n = vec.size();
      if (n == 0) return 0;
      return vec[n / 2].template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<ValidatorListDwnc>)
   {
      psio::view<ValidatorListDwnc, Fmt> v{sp};
      auto vec = v.template get<1>();
      const auto n = vec.size();
      if (n == 0) return 0;
      return vec[n / 2].template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<ValidatorListBounded>)
   {
      psio::view<ValidatorListBounded, Fmt> v{sp};
      auto vec = v.template get<1>();
      const auto n = vec.size();
      if (n == 0) return 0;
      return vec[n / 2].template get<0>();
   }

   //  Deep4* — bench_view_target returns root.child.child.child.value.
   //  All five levels are field 0.
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<Deep4Ext>)
   {
      psio::view<Deep4Ext, Fmt> v{sp};
      return v.template get<0>().template get<0>()
              .template get<0>().template get<0>().template get<0>();
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<Deep4Dwnc>)
   {
      psio::view<Deep4Dwnc, Fmt> v{sp};
      return v.template get<0>().template get<0>()
              .template get<0>().template get<0>().template get<0>();
   }

   //  Variety shapes — reach into vector[mid] / last scalar field.
   //  Mirrors bench_view_target so the bench's view_one path measures
   //  the same hot read.  Element subscript yields a view<P, Fmt> for
   //  primitive elements; .get() pulls the value out.
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<MlEmbedding>)
   {
      psio::view<MlEmbedding, Fmt> v{sp};
      auto       vec = v.template get<1>();
      const auto n   = vec.size();
      if (n == 0) return v.template get<0>();
      float         f = vec[n / 2].get();
      std::uint32_t bits;
      std::memcpy(&bits, &f, sizeof(bits));
      return bits;
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<BlobPayload>)
   {
      psio::view<BlobPayload, Fmt> v{sp};
      auto       vec = v.template get<1>();
      const auto n   = vec.size();
      if (n == 0) return v.template get<0>();
      return static_cast<std::uint64_t>(vec[n / 2].get());
   }
   template <typename Fmt>
   inline std::uint64_t view_target_via_view(
      std::span<const char> sp, std::type_identity<WideRecord>)
   {
      psio::view<WideRecord, Fmt> v{sp};
      return v.template get<31>();
   }

   //  Concept: does view<T, Fmt> support the chain-traversal needed for
   //  this shape's view_target?  Detected via the well-formedness of
   //  view_target_via_view<Fmt>(span, type_identity<T>).
   template <typename Fmt, typename T>
   concept HasViewChain = requires(std::span<const char> sp) {
      view_target_via_view<Fmt>(sp, std::type_identity<T>{});
   };

   //  ── pjson_view_target ────────────────────────────────────────────
   //
   //  Reach the same scalar bench_view_target<T> reads, but via the
   //  pjson typed-view chain: from_pjson() validates canonical-form
   //  with a single memcmp on the precomputed hash template, then
   //  field<I>() is an O(1) slot-table read — no decode of the rest
   //  of the document.  For shapes whose target is nested (Order,
   //  ValidatorList, Deep4, etc.), each level constructs its own
   //  typed view of the inner pjson_view.
   //
   //  Non-canonical inputs would fall through to raw_.find(name)
   //  inside field<I>() — the bench encodes via from_struct, which
   //  is canonical-by-construction, so we measure the fast path.

   //  Default: top-level field 0 is the target (matches the default
   //  bench_view_target).  Non-arithmetic field 0 → 0 sink.
   template <typename T>
   inline std::uint64_t pjson_view_target(::psio::pjson_view raw)
   {
      auto tv = ::psio::view<T, ::psio::pjson_format>::from_pjson(raw);
      using F0 = typename ::psio::reflect<T>::template member_type<0>;
      if constexpr (std::is_arithmetic_v<F0> ||
                     std::is_same_v<F0, bool>)
         return static_cast<std::uint64_t>(tv.template get<0>());
      else
         return 0;
   }

   //  Order family — bench_view_target returns customer.id.
   //  customer is field 1; id is field 0 of UserProfile.
   template <>
   inline std::uint64_t pjson_view_target<Order>(::psio::pjson_view raw)
   {
      auto tv   = ::psio::view<Order, ::psio::pjson_format>::from_pjson(raw);
      auto cust = tv.template field<1>();
      auto cv   = ::psio::view<UserProfile, ::psio::pjson_format>::
                     from_pjson(cust);
      return cv.template get<0>();
   }
   template <>
   inline std::uint64_t pjson_view_target<OrderBounded>(::psio::pjson_view raw)
   {
      auto tv   =
         ::psio::view<OrderBounded, ::psio::pjson_format>::from_pjson(raw);
      auto cust = tv.template field<1>();
      auto cv   =
         ::psio::view<UserProfileBounded, ::psio::pjson_format>::
            from_pjson(cust);
      return cv.template get<0>();
   }
   template <>
   inline std::uint64_t pjson_view_target<OrderDwnc>(::psio::pjson_view raw)
   {
      auto tv   =
         ::psio::view<OrderDwnc, ::psio::pjson_format>::from_pjson(raw);
      auto cust = tv.template field<1>();
      auto cv   =
         ::psio::view<UserProfileDwnc, ::psio::pjson_format>::
            from_pjson(cust);
      return cv.template get<0>();
   }

   //  ValidatorList family — bench_view_target returns
   //  validators[mid].pubkey_lo.  validators is field 1; pubkey_lo is
   //  field 0 of Validator.  pjson stores the vector as a row_array,
   //  so .at(i) is O(1) indexing into the row table.
   template <>
   inline std::uint64_t pjson_view_target<ValidatorList>(
      ::psio::pjson_view raw)
   {
      auto tv  =
         ::psio::view<ValidatorList, ::psio::pjson_format>::from_pjson(raw);
      auto arr = tv.template field<1>();
      const std::size_t n = arr.count();
      if (n == 0) return 0;
      // Pass the row_array_record dynamic_view through unchanged —
      // it carries the parent_/parent_size_/form_ metadata the typed
      // view's find() needs to dispatch to row_record_find_.
      auto vv = ::psio::view<Validator, ::psio::pjson_format>::
                   from_pjson(arr.at(n / 2));
      return vv.template get<0>();
   }
   template <>
   inline std::uint64_t pjson_view_target<ValidatorListBounded>(
      ::psio::pjson_view raw)
   {
      auto tv  =
         ::psio::view<ValidatorListBounded, ::psio::pjson_format>::
            from_pjson(raw);
      auto arr = tv.template field<1>();
      const std::size_t n = arr.count();
      if (n == 0) return 0;
      // Pass the row_array_record dynamic_view through unchanged —
      // it carries the parent_/parent_size_/form_ metadata the typed
      // view's find() needs to dispatch to row_record_find_.
      auto vv = ::psio::view<Validator, ::psio::pjson_format>::
                   from_pjson(arr.at(n / 2));
      return vv.template get<0>();
   }
   template <>
   inline std::uint64_t pjson_view_target<ValidatorListDwnc>(
      ::psio::pjson_view raw)
   {
      auto tv  =
         ::psio::view<ValidatorListDwnc, ::psio::pjson_format>::
            from_pjson(raw);
      auto arr = tv.template field<1>();
      const std::size_t n = arr.count();
      if (n == 0) return 0;
      // Pass the row_array_record dynamic_view through unchanged —
      // it carries the parent_/parent_size_/form_ metadata the typed
      // view's find() needs to dispatch to row_record_find_.
      auto vv = ::psio::view<Validator, ::psio::pjson_format>::
                   from_pjson(arr.at(n / 2));
      return vv.template get<0>();
   }

   //  Deep4 — bench_view_target returns root.child.child.child.value.
   //  Five typed views, each field<0>() into the next level.
   template <>
   inline std::uint64_t pjson_view_target<Deep4Ext>(::psio::pjson_view raw)
   {
      auto v0 = ::psio::view<Deep4Ext, ::psio::pjson_format>::from_pjson(raw);
      auto p1 = v0.template field<0>();
      auto v1 = ::psio::view<Inner1Ext, ::psio::pjson_format>::from_pjson(p1);
      auto p2 = v1.template field<0>();
      auto v2 = ::psio::view<Inner2Ext, ::psio::pjson_format>::from_pjson(p2);
      auto p3 = v2.template field<0>();
      auto v3 = ::psio::view<Inner3Ext, ::psio::pjson_format>::from_pjson(p3);
      auto p4 = v3.template field<0>();
      auto v4 = ::psio::view<Inner4Ext, ::psio::pjson_format>::from_pjson(p4);
      return v4.template get<0>();
   }
   template <>
   inline std::uint64_t pjson_view_target<Deep4Dwnc>(::psio::pjson_view raw)
   {
      auto v0 =
         ::psio::view<Deep4Dwnc, ::psio::pjson_format>::from_pjson(raw);
      auto p1 = v0.template field<0>();
      auto v1 = ::psio::view<Inner1Dwnc, ::psio::pjson_format>::from_pjson(p1);
      auto p2 = v1.template field<0>();
      auto v2 = ::psio::view<Inner2Dwnc, ::psio::pjson_format>::from_pjson(p2);
      auto p3 = v2.template field<0>();
      auto v3 = ::psio::view<Inner3Dwnc, ::psio::pjson_format>::from_pjson(p3);
      auto p4 = v3.template field<0>();
      auto v4 = ::psio::view<Inner4Dwnc, ::psio::pjson_format>::from_pjson(p4);
      return v4.template get<0>();
   }

   //  WideRecord — bench_view_target returns f31.
   template <>
   inline std::uint64_t pjson_view_target<WideRecord>(::psio::pjson_view raw)
   {
      auto tv =
         ::psio::view<WideRecord, ::psio::pjson_format>::from_pjson(raw);
      return tv.template get<31>();
   }

   //  MlEmbedding / BlobPayload — bench_view_target reads
   //  vector[mid].  pjson stores these as typed_array (vec<f32>) /
   //  t_bytes (vec<u8>); .at(i) on typed_array yields a pjson_view
   //  whose value is the element at offset i.
   template <>
   inline std::uint64_t pjson_view_target<MlEmbedding>(::psio::pjson_view raw)
   {
      auto tv   =
         ::psio::view<MlEmbedding, ::psio::pjson_format>::from_pjson(raw);
      auto arr  = tv.template field<1>();
      const std::size_t n = arr.count();
      if (n == 0) return tv.template get<0>();
      // typed_array<f32>: bulk layout is [tag][f32 × n].  at(i) returns
      // a pjson_view of the i-th element scalar.
      auto elem = arr.at(n / 2);
      double      d = elem.as_double();
      float       f = static_cast<float>(d);
      std::uint32_t bits;
      std::memcpy(&bits, &f, sizeof(bits));
      return bits;
   }
   template <>
   inline std::uint64_t pjson_view_target<BlobPayload>(::psio::pjson_view raw)
   {
      auto tv  =
         ::psio::view<BlobPayload, ::psio::pjson_format>::from_pjson(raw);
      auto bv  = tv.template field<1>();
      // t_bytes payload is the raw byte sequence.
      auto sp  = bv.as_bytes();
      if (sp.empty()) return tv.template get<0>();
      return static_cast<std::uint64_t>(sp[sp.size() / 2]);
   }


   //  ── anti-DCE harness ──────────────────────────────────────────────
   //
   //  The earlier harness ran each timed op with a single loop-invariant
   //  encoded buffer and an asm-volatile clobber on the result.  For
   //  fixed-shape decode/validate/view, that lets the optimiser:
   //    1. specialise the template (T and Fmt are constants),
   //    2. observe that the input span is loop-invariant,
   //    3. constant-fold the result, and
   //    4. hoist the call out of the loop.
   //  The "measurement" then collapses to loop overhead (~1 cycle).
   //
   //  Two patches close that:
   //    A. Per-iteration input rotation — pre-encode K varied buffers
   //       and select with `i & (K-1)`.  The compiler can't fold
   //       results across distinct inputs.
   //    B. Value-dependent volatile sink — accumulate a value derived
   //       from the per-iter result into a `volatile` accumulator
   //       outside the lambda.  This forces every iteration to do
   //       observable work.
   //
   //  K = 16 is a small power of two (cheap modulo via &) that fits in
   //  cache; large enough to defeat constant-folding without distorting
   //  the working set.

   constexpr std::size_t kAntiDceK = 16;

   // Tighter than asm-volatile-on-stack: forces v through memory so the
   // compiler treats it as escaped, breaking dead-store elimination.
   template <typename T>
   inline void do_not_optimize(T&& v)
   {
      asm volatile("" : "+r,m"(v) : : "memory");
   }

   //  vary(v, i) — produce one of K perturbed copies of v.  For each
   //  shape we pick a scalar field and XOR it with i.  The chosen field
   //  is large enough that XOR-with-i (i ∈ [0, 15]) does not change the
   //  field's varint width, so wire-byte counts stay consistent across
   //  the K buffers for varint-using formats too.
   //
   //  For tiny shapes whose only fields are small (Point), the wire
   //  delta is at most ±1 byte for varint formats — within noise.
   inline Point      vary(Point v, std::size_t i)
   { v.x ^= static_cast<std::int32_t>(i); return v; }

   inline NameRecord vary(NameRecord v, std::size_t i)
   { v.account ^= i; return v; }

   inline FlatRecord vary(FlatRecord v, std::size_t i)
   { v.id ^= static_cast<std::uint32_t>(i); return v; }
   inline FlatRecordBounded vary(FlatRecordBounded v, std::size_t i)
   { v.id ^= static_cast<std::uint32_t>(i); return v; }
   inline FlatRecordDwnc    vary(FlatRecordDwnc v, std::size_t i)
   { v.id ^= static_cast<std::uint32_t>(i); return v; }

   inline Record vary(Record v, std::size_t i)
   { v.id ^= static_cast<std::uint32_t>(i); return v; }
   inline RecordBounded vary(RecordBounded v, std::size_t i)
   { v.id ^= static_cast<std::uint32_t>(i); return v; }
   inline RecordDwnc    vary(RecordDwnc v, std::size_t i)
   { v.id ^= static_cast<std::uint32_t>(i); return v; }

   inline Validator vary(Validator v, std::size_t i)
   { v.pubkey_lo ^= i; return v; }

   inline Order        vary(Order v, std::size_t i)
   { v.id ^= i; return v; }
   inline OrderBounded vary(OrderBounded v, std::size_t i)
   { v.id ^= i; return v; }
   inline OrderDwnc    vary(OrderDwnc v, std::size_t i)
   { v.id ^= i; return v; }

   inline ValidatorList vary(ValidatorList v, std::size_t i)
   { v.epoch ^= i; return v; }
   inline ValidatorListBounded vary(ValidatorListBounded v, std::size_t i)
   { v.epoch ^= i; return v; }
   inline ValidatorListDwnc    vary(ValidatorListDwnc v, std::size_t i)
   { v.epoch ^= i; return v; }

   inline Deep4Ext  vary(Deep4Ext v, std::size_t i)
   { v.root.child.child.child.value ^= i; return v; }
   inline Deep4Dwnc vary(Deep4Dwnc v, std::size_t i)
   { v.root.child.child.child.value ^= i; return v; }

   inline MlEmbedding vary(MlEmbedding v, std::size_t i)
   { v.id ^= i; return v; }
   inline BlobPayload vary(BlobPayload v, std::size_t i)
   { v.id ^= i; return v; }
   inline WideRecord  vary(WideRecord v, std::size_t i)
   { v.f00 ^= static_cast<std::uint32_t>(i); return v; }


   //  Per-(format, shape) support gate.  Some psio formats fail to
   //  encode certain shapes (e.g., frac doesn't yet support
   //  vector<variable-element>); the static_assert lives inside the
   //  template body, so SFINAE can't catch it — we have to skip
   //  explicitly here.  Default = supported; specialise to false_type
   //  for known-unsupported pairs.
   template <typename Fmt, typename T>
   struct fmt_supports : std::true_type {};

   //  frac32 / frac16 codec is missing the offset-table walker for
   //  vector<variable-element> (frac.hpp:530 + 1086 — explicit
   //  static_assert TODO; the wire format itself supports it, same
   //  pattern as legacy fracpack).  Order has vector<LineItem>,
   //  OrderBounded has vector<LineItemBounded>, OrderDwnc has
   //  vector<LineItemDwnc> — all blocked on the codec, not the spec.
   template <> struct fmt_supports<psio::frac32, Order>        : std::false_type {};
   template <> struct fmt_supports<psio::frac32, OrderBounded> : std::false_type {};
   template <> struct fmt_supports<psio::frac32, OrderDwnc>    : std::false_type {};
   template <> struct fmt_supports<psio::frac16, Order>        : std::false_type {};
   template <> struct fmt_supports<psio::frac16, OrderBounded> : std::false_type {};
   template <> struct fmt_supports<psio::frac16, OrderDwnc>    : std::false_type {};

   //  Per-(shape, format) op timings for the psio side.  Each call
   //  exercises one CPO and pushes one snapshot_row into `out`.
   //
   //  Ops covered here:
   //    size_of          — psio::size_of(fmt, v)
   //    encode_rvalue    — auto bytes = psio::encode(fmt, v)
   //    encode_sink      — psio::encode(fmt, v, sink) into a reused vector
   //    decode           — psio::decode<T>(fmt, bytes)
   //    validate         — psio::validate<T>(fmt, bytes)  (where supported)
   //
   //  view_one / view_all are added in a follow-up commit (task #66).

   template <typename Fmt, typename T>
   void bench_psio_cell(std::vector<snapshot_row>& out,
                        const std::string&         shape_name,
                        const std::string&         fmt_name,
                        Fmt                        fmt,
                        const T&                   v,
                        const std::string&         mode = "static")
   {
      // Pre-build K varied input values + their encoded bytes.  Every
      // timed op rotates through these via `i & (K-1)`, so the input
      // changes per iteration and the optimiser can't constant-fold
      // across the loop.  See the harness comments above the `vary`
      // overloads for the full rationale.
      //
      // buf_t is whatever psio::encode(fmt, …) returns for this format —
      // most are vector<char>, but psio::json returns std::string.
      using buf_t = std::remove_cvref_t<decltype(psio::encode(fmt, v))>;
      std::array<T, kAntiDceK>                     vals;
      std::array<buf_t, kAntiDceK>                 bufs;
      std::array<std::span<const char>, kAntiDceK> spans;
      for (std::size_t i = 0; i < kAntiDceK; ++i) {
         vals[i]  = vary(v, i);
         bufs[i]  = psio::encode(fmt, vals[i]);
         spans[i] = std::span<const char>{bufs[i].data(), bufs[i].size()};
      }
      const std::size_t wire = bufs[0].size();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = fmt_name,
            .library    = "psio",
            .mode       = mode,
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };

      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of — input rotates per iter; sink XOR-accumulates the
      // returned size to keep each call's result observable.
      volatile std::uint64_t sink_size = 0;
      auto t_size = ns_per_iter(0u, [&](std::size_t i) {
         sink_size ^= psio::size_of(fmt, vals[i & (kAntiDceK - 1)]);
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials);

      // encode_rvalue — rotated input prevents constant-folding the
      // entire encode call; sink XORs the byte count + first byte.
      volatile std::uint64_t sink_enc = 0;
      auto t_enc = ns_per_iter(0u, [&](std::size_t i) {
         auto b = psio::encode(fmt, vals[i & (kAntiDceK - 1)]);
         sink_enc ^= b.size();
         if (!b.empty()) sink_enc ^= static_cast<unsigned char>(b[0]);
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials);

      // encode_sink (reused buffer) — gated; capnp/flatbuf only have
      // the rvalue path.
      if constexpr (requires(std::vector<char>& s) {
                       psio::encode(fmt, v, s);
                    })
      {
         std::vector<char> sink_buf;
         sink_buf.reserve(wire * 2);
         volatile std::uint64_t sink_sk = 0;
         auto t_sink = ns_per_iter(0u, [&](std::size_t i) {
            sink_buf.clear();
            psio::encode(fmt, vals[i & (kAntiDceK - 1)], sink_buf);
            sink_sk ^= sink_buf.size();
            if (!sink_buf.empty())
               sink_sk ^= static_cast<unsigned char>(sink_buf[0]);
         });
         record("encode_sink", t_sink.min_ns, t_sink.median_ns,
                cv(t_sink), wire, t_sink.iters, t_sink.trials);
      }

      // decode — rotated input, sink fed by bench_view_target so the
      // decoded value's bytes have to be touched every iter.
      volatile std::uint64_t sink_dec = 0;
      auto t_dec = ns_per_iter(0u, [&](std::size_t i) {
         auto p = psio::decode<T>(fmt, spans[i & (kAntiDceK - 1)]);
         sink_dec ^= bench_view_target(p);
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials);

      // view_one — decode + access first reflected field.  Honest
      // framing: psio's Record-level zero-copy view machinery isn't
      // yet wired for every format, so what we time here is "decode
      // and reach in" — the full deserialisation cost, not a
      // pointer-into-buffer dereference.  Compare against
      // libcapnp/libflatbuffers' view_one to see the value of true
      // zero-copy for single-field access.
      if constexpr (::psio::Reflected<T>)
      {
         //  Honest framing per format:
         //
         //  psio::flatbuf and psio::capnp produce wire bytes that
         //  the canonical libraries (libflatbuffers, libcapnp) can
         //  read.  For these we use the canonical reader's
         //  zero-copy view path on psio-encoded bytes — that's how
         //  a real consumer of those formats would do it.
         //
         //  psio::ssz / psio::pssz expose vector / optional /
         //  variant zero-copy views through `psio::view<T, Fmt>`
         //  (their `view_layout::traits` specialisations now live
         //  in `psio::view_layout` — task #74 fix).  Record-level
         //  zero-copy view (per-field accessor on a Reflected
         //  struct) is the follow-up: until that lands, the
         //  fallback below pays the decode-and-reach cost on
         //  Reflected shapes.  psio::frac32 / psio::bin still
         //  await their own traits specialisations.
         //
         //  psio::msgpack, psio::protobuf, psio::borsh,
         //  psio::bincode, psio::avro, psio::json have no zero-copy
         //  semantics — full decode is what users pay.

#ifdef PSIO_HAVE_FLATBUF
         if constexpr (std::is_same_v<Fmt, psio::flatbuf> &&
                        requires {
                           typename fb_bench_adapter::fb_table_for<T>::type;
                        })
         {
            using FB = fb_bench_adapter::fb_table_t<T>;
            volatile std::uint64_t sink_v = 0;
            auto t_view = ns_per_iter(0u, [&](std::size_t i) {
               const auto& sb = spans[i & (kAntiDceK - 1)];
               auto root = flatbuffers::GetRoot<FB>(
                  reinterpret_cast<const std::uint8_t*>(sb.data()));
               sink_v ^=
                  static_cast<std::uint64_t>(
                     fb_bench_adapter::view_first_scalar(root));
            });
            record("view_one", t_view.min_ns, t_view.median_ns,
                   cv(t_view), wire, t_view.iters, t_view.trials,
                   "libflatbuffers GetRoot on psio bytes (zero-copy)");
            return;
         }
#endif
#ifdef PSIO_HAVE_CAPNP
         if constexpr (std::is_same_v<Fmt, psio::capnp> &&
                        requires {
                           typename cp_bench::cp_struct_for<T>::type;
                        })
         {
            using CP = cp_bench::cp_struct_t<T>;
            //  capnp wants word-aligned input; pre-build the K
            //  word-pointer arrays once.
            std::array<kj::ArrayPtr<const capnp::word>, kAntiDceK>
               word_arrs;
            for (std::size_t k = 0; k < kAntiDceK; ++k) {
               word_arrs[k] = kj::ArrayPtr<const capnp::word>(
                  reinterpret_cast<const capnp::word*>(
                     spans[k].data()),
                  spans[k].size() / sizeof(capnp::word));
            }
            volatile std::uint64_t sink_v = 0;
            auto t_view = ns_per_iter(0u, [&](std::size_t i) {
               capnp::FlatArrayMessageReader reader(
                  word_arrs[i & (kAntiDceK - 1)]);
               auto root = reader.getRoot<CP>();
               sink_v ^= static_cast<std::uint64_t>(
                  cp_bench::view_first_scalar(root));
            });
            record("view_one", t_view.min_ns, t_view.median_ns,
                   cv(t_view), wire, t_view.iters, t_view.trials,
                   "libcapnp FlatArrayMessageReader on psio bytes (zero-copy)");
            return;
         }
#endif

         //  Try psio::view<T, Fmt> — genuine zero-copy field access
         //  via the format's record_field_span / vector_element_span
         //  traits.  Only enabled when the trait support is detected
         //  AND a view-chain helper exists for this shape.
         if constexpr (
            ::psio::view_layout::has_record_support<Fmt, T>::value
            && HasViewChain<Fmt, T>)
         {
            volatile std::uint64_t sink_v = 0;
            auto t_view = ns_per_iter(0u, [&](std::size_t i) {
               sink_v ^= view_target_via_view<Fmt>(
                  spans[i & (kAntiDceK - 1)],
                  std::type_identity<T>{});
            });
            record("view_one", t_view.min_ns, t_view.median_ns,
                   cv(t_view), wire, t_view.iters, t_view.trials,
                   "psio::view<T, Fmt> chain (zero-copy)");
            return;
         }

         //  Fallback: decode-and-reach.  Captures the cost users
         //  actually pay today on formats whose zero-copy view
         //  trait specialisations aren't reachable — this is NOT
         //  a "view", it's a full decode followed by an in-struct
         //  field access.  Recorded under a distinct op name so
         //  the report doesn't conflate it with genuine zero-copy
         //  random access (psio::flatbuf / psio::capnp / lib*).
         volatile std::uint64_t sink_v = 0;
         auto t_view = ns_per_iter(0u, [&](std::size_t i) {
            auto decoded =
               psio::decode<T>(fmt, spans[i & (kAntiDceK - 1)]);
            sink_v ^= bench_view_target(decoded);
         });
         record("decode_then_view", t_view.min_ns, t_view.median_ns,
                cv(t_view), wire, t_view.iters, t_view.trials,
                "decode + reach (no native zero-copy view available)");
      }

      // validate — rotated input + bool-cast result XOR'd into a
      // volatile sink.  Without rotation, the validate result for
      // a constant span constant-folds and the call gets hoisted.
      try
      {
         volatile std::uint64_t sink_val = 0;
         auto t_val = ns_per_iter(0u, [&](std::size_t i) {
            auto st = psio::validate<T>(fmt, spans[i & (kAntiDceK - 1)]);
            sink_val ^= static_cast<std::uint64_t>(static_cast<bool>(st));
         });
         record("validate", t_val.min_ns, t_val.median_ns, cv(t_val),
                wire, t_val.iters, t_val.trials);
      }
      catch (...)
      {
         // No-op — some shapes/formats throw via codec_status::or_throw
         // semantics; the bench shouldn't abort.
      }
   }

#ifdef PSIO_HAVE_MSGPACK
   //  Time msgpack-cxx (the canonical lib) on a single shape, with
   //  the same op surface as bench_psio_cell.  library = "msgpack-cxx",
   //  format = "msgpack" (wire-compatible with psio::msgpack).
   template <typename T>
   void bench_msgpack_cxx_cell(std::vector<snapshot_row>& out,
                                const std::string& shape_name, const T& v)
   {
      // K-buffer rotation (anti-DCE — see `vary` overloads above).
      std::array<T, kAntiDceK>                vals;
      std::array<std::vector<char>, kAntiDceK> bufs;
      for (std::size_t i = 0; i < kAntiDceK; ++i) {
         vals[i] = vary(v, i);
         bufs[i] = mp_bench::encode(vals[i]);
      }
      const std::size_t wire = bufs[0].size();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = "msgpack",
            .library    = "msgpack-cxx",
            .mode       = "static",
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };

      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of  — msgpack-cxx has no two-pass sizer; the only honest
      // measurement is "pack to a throwaway sbuffer, take .size()".
      volatile std::uint64_t sink_size = 0;
      auto t_size = ns_per_iter(0u, [&](std::size_t i) {
         sink_size ^= mp_bench::size_of(vals[i & (kAntiDceK - 1)]);
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials,
             "pack-to-throwaway-sbuffer (no native sizer)");

      // encode_rvalue — fresh sbuffer + std::vector<char> copy.
      volatile std::uint64_t sink_enc = 0;
      auto t_enc = ns_per_iter(0u, [&](std::size_t i) {
         auto b = mp_bench::encode(vals[i & (kAntiDceK - 1)]);
         sink_enc ^= b.size();
         if (!b.empty()) sink_enc ^= static_cast<unsigned char>(b[0]);
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials);

      // encode_sink — reused sbuffer (msgpack-cxx's native buffer).
      msgpack::sbuffer sbuf;
      volatile std::uint64_t sink_sk = 0;
      auto t_sink = ns_per_iter(0u, [&](std::size_t i) {
         mp_bench::encode_into(vals[i & (kAntiDceK - 1)], sbuf);
         sink_sk ^= sbuf.size();
      });
      record("encode_sink", t_sink.min_ns, t_sink.median_ns, cv(t_sink),
             wire, t_sink.iters, t_sink.trials,
             "reused msgpack::sbuffer (not std::vector<char>)");

      // decode  — unpack + convert.  msgpack-cxx allocates internally
      // for each unpack call; this is the canonical pattern.
      volatile std::uint64_t sink_dec = 0;
      auto t_dec = ns_per_iter(0u, [&](std::size_t i) {
         auto& b = bufs[i & (kAntiDceK - 1)];
         auto p  = mp_bench::decode<T>(b.data(), b.size());
         sink_dec ^= bench_view_target(p);
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials);

      // No native validate — `unpack` IS the validation; report skipped.

      // decode_then_view — msgpack has no zero-copy; full unpack +
      // convert + reach the view_target field.  Recorded as
      // decode_then_view (NOT view_one), since a genuine zero-copy
      // view path doesn't exist here.
      if constexpr (::psio::Reflected<T>)
      {
         volatile std::uint64_t sink_v = 0;
         auto t_view = ns_per_iter(0u, [&](std::size_t i) {
            auto& b = bufs[i & (kAntiDceK - 1)];
            auto p  = mp_bench::decode<T>(b.data(), b.size());
            sink_v ^= bench_view_target(p);
         });
         record("decode_then_view", t_view.min_ns, t_view.median_ns,
                cv(t_view), wire, t_view.iters, t_view.trials,
                "unpack + reach view_target (no zero-copy)");
      }
   }
#endif

#ifdef PSIO_HAVE_PROTOBUF
   //  Time libprotobuf on a single shape.  library = "libprotobuf",
   //  format = "protobuf" (wire-compatible with psio::protobuf for the
   //  same shape).  Includes the `build()` cost (psio shape → pb
   //  message) on encode_rvalue, since that's what real callers do.
   //  encode_pure isolates SerializeToString on a pre-built reused
   //  pb message — closer to libprotobuf's best case.
   template <typename T>
   void bench_libprotobuf_cell(std::vector<snapshot_row>& out,
                               const std::string&         shape_name,
                               const T&                   v)
   {
      using PB = pb_bench::pb_message_t<T>;

      // K-buffer rotation: K varied input domain values, each
      // pre-built and pre-serialised.  Anti-DCE — see `vary` overloads.
      std::array<T, kAntiDceK>           vals;
      std::array<PB, kAntiDceK>          seeds;
      std::array<std::string, kAntiDceK> pres;
      for (std::size_t i = 0; i < kAntiDceK; ++i) {
         vals[i] = vary(v, i);
         pb_bench::build(seeds[i], vals[i]);
         (void)seeds[i].SerializeToString(&pres[i]);
      }
      const std::size_t wire = pres[0].size();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = "protobuf",
            .library    = "libprotobuf",
            .mode       = "static",
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };
      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of — libprotobuf's ByteSizeLong on a built message.
      volatile std::uint64_t sink_size = 0;
      auto t_size = ns_per_iter(0u, [&](std::size_t i) {
         sink_size ^= seeds[i & (kAntiDceK - 1)].ByteSizeLong();
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials,
             "ByteSizeLong on pre-built pb message");

      // encode_rvalue — fresh pb + build + SerializeToString, fair
      // domain-struct-to-bytes timing (matches what psio::encode does).
      volatile std::uint64_t sink_enc = 0;
      auto t_enc = ns_per_iter(0u, [&](std::size_t i) {
         PB          pb;
         pb_bench::build(pb, vals[i & (kAntiDceK - 1)]);
         std::string out_;
         (void)pb.SerializeToString(&out_);
         sink_enc ^= out_.size();
         if (!out_.empty()) sink_enc ^= static_cast<unsigned char>(out_[0]);
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials,
             "fresh pb + build + SerializeToString");

      // encode_sink — reused pb + reused std::string buffer.
      // Closest libprotobuf gets to a "reuse" path; analogous to the
      // psio sink path that reuses a vector<char>.
      PB          reused;
      std::string out_buf;
      out_buf.reserve(wire * 2);
      volatile std::uint64_t sink_sk = 0;
      auto t_sink = ns_per_iter(0u, [&](std::size_t i) {
         reused.Clear();
         pb_bench::build(reused, vals[i & (kAntiDceK - 1)]);
         out_buf.clear();
         (void)reused.SerializeToString(&out_buf);
         sink_sk ^= out_buf.size();
      });
      record("encode_sink", t_sink.min_ns, t_sink.median_ns,
             cv(t_sink), wire, t_sink.iters, t_sink.trials,
             "reused pb message + reused std::string");

      // decode — ParseFromString into a reused message.
      PB dec;
      volatile std::uint64_t sink_dec = 0;
      auto t_dec = ns_per_iter(0u, [&](std::size_t i) {
         dec.Clear();
         (void)dec.ParseFromString(pres[i & (kAntiDceK - 1)]);
         sink_dec ^= dec.ByteSizeLong();
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials,
             "ParseFromString into reused pb message");

      // No standalone validate — ParseFromString IS the validation.

      // decode_then_view — protobuf has no zero-copy; reach into
      // reused post-Parse pb message.  This is the canonical
      // libprotobuf pattern for "get one field after parsing".
      // Recorded as decode_then_view (NOT view_one) since a genuine
      // zero-copy view path doesn't exist for protobuf.
      if constexpr (::psio::Reflected<T>)
      {
         volatile std::uint64_t sink_v = 0;
         auto t_view = ns_per_iter(0u, [&](std::size_t i) {
            dec.Clear();
            (void)dec.ParseFromString(pres[i & (kAntiDceK - 1)]);
            // Touch one field via the message's accessors.  pb's
            // ByteSizeLong is a cheap-once-cached scalar read;
            // sufficient as a "reach in" signal.
            sink_v ^= dec.ByteSizeLong();
         });
         record("decode_then_view", t_view.min_ns, t_view.median_ns,
                cv(t_view), wire, t_view.iters, t_view.trials,
                "ParseFromString + ByteSizeLong (no zero-copy)");
      }
   }
#endif

#ifdef PSIO_HAVE_FLATBUF
   //  Time libflatbuffers.  Bottom-up build: children before parents,
   //  finish, get buffer pointer + size.  No streaming sink path —
   //  flatbuffers always builds into its own builder, then exposes
   //  the buffer at the end.
   template <typename T>
   void bench_libflatbuf_cell(std::vector<snapshot_row>& out,
                              const std::string&         shape_name,
                              const T&                   v)
   {
      using FB = fb_bench_adapter::fb_table_t<T>;

      // K varied input domain values + K pre-built builders.
      std::array<T, kAntiDceK>                                  vals;
      std::array<flatbuffers::FlatBufferBuilder, kAntiDceK>     seeds;
      std::array<const std::uint8_t*, kAntiDceK>                buf_ptrs;
      for (std::size_t i = 0; i < kAntiDceK; ++i) {
         vals[i] = vary(v, i);
         seeds[i].Finish(fb_bench_adapter::build(seeds[i], vals[i]));
         buf_ptrs[i] = seeds[i].GetBufferPointer();
      }
      const std::size_t wire = seeds[0].GetSize();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = "flatbuf",
            .library    = "libflatbuffers",
            .mode       = "static",
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };
      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of — flatbuffers has no separate sizer; build + GetSize
      // is the canonical "how big will this be" answer.  Includes
      // build cost; that's how callers measure it.
      volatile std::uint64_t sink_size = 0;
      auto t_size = ns_per_iter(0u, [&](std::size_t i) {
         flatbuffers::FlatBufferBuilder fbb;
         fbb.Finish(
            fb_bench_adapter::build(fbb, vals[i & (kAntiDceK - 1)]));
         sink_size ^= fbb.GetSize();
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials,
             "build + GetSize (no native sizer)");

      // encode_rvalue — fresh builder + build + Finish + Release().
      volatile std::uint64_t sink_enc = 0;
      auto t_enc = ns_per_iter(0u, [&](std::size_t i) {
         flatbuffers::FlatBufferBuilder fbb;
         fbb.Finish(
            fb_bench_adapter::build(fbb, vals[i & (kAntiDceK - 1)]));
         auto buf = fbb.Release();
         sink_enc ^= buf.size();
         if (buf.size()) sink_enc ^= buf.data()[0];
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials,
             "fresh builder + Finish");

      // encode_sink — reused builder; Clear() rather than reset.
      flatbuffers::FlatBufferBuilder fbb_reused;
      volatile std::uint64_t sink_sk = 0;
      auto t_sink = ns_per_iter(0u, [&](std::size_t i) {
         fbb_reused.Clear();
         fbb_reused.Finish(
            fb_bench_adapter::build(fbb_reused, vals[i & (kAntiDceK - 1)]));
         sink_sk ^= fbb_reused.GetSize();
      });
      record("encode_sink", t_sink.min_ns, t_sink.median_ns,
             cv(t_sink), wire, t_sink.iters, t_sink.trials,
             "reused FlatBufferBuilder.Clear()");

      // decode — full materialisation of the native C++ shape from the
      // flatbuffer (every field walked, strings allocated, vectors
      // populated).  GetRoot alone is a zero-copy view; calling it
      // "decode" would conflate it with the materialise-T cost that
      // psio::decode<T>(fmt, …) actually pays.
      volatile std::uint64_t sink_dec = 0;
      auto t_dec = ns_per_iter(0u, [&](std::size_t i) {
         auto root = flatbuffers::GetRoot<FB>(buf_ptrs[i & (kAntiDceK - 1)]);
         T native = fb_bench_adapter::to_native<T>(root);
         sink_dec ^= bench_view_target(native);
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials,
             "GetRoot + materialise to native T (apples-to-apples decode)");

      // view_one — zero-copy random access of one scalar field.
      volatile std::uint64_t sink_v = 0;
      auto t_view = ns_per_iter(0u, [&](std::size_t i) {
         auto root = flatbuffers::GetRoot<FB>(buf_ptrs[i & (kAntiDceK - 1)]);
         sink_v ^= static_cast<std::uint64_t>(
            fb_bench_adapter::view_first_scalar(root));
      });
      record("view_one", t_view.min_ns, t_view.median_ns, cv(t_view),
             wire, t_view.iters, t_view.trials,
             "GetRoot + first-scalar getter (zero-copy)");
   }
#endif

#ifdef PSIO_HAVE_CAPNP
   //  Time libcapnp.  encode = MallocMessageBuilder + setters +
   //  messageToFlatArray.  decode = FlatArrayMessageReader +
   //  getRoot<>() (zero-copy, just pointer arithmetic).
   template <typename T>
   void bench_libcapnp_cell(std::vector<snapshot_row>& out,
                            const std::string&         shape_name,
                            const T&                   v)
   {
      using CP = cp_bench::cp_struct_t<T>;

      // K varied input domain values + K pre-built flat-array buffers.
      std::array<T, kAntiDceK>                            vals;
      std::array<kj::Array<capnp::word>, kAntiDceK>       seed_arrs;
      std::array<kj::ArrayPtr<const capnp::word>, kAntiDceK> word_arrs;
      for (std::size_t i = 0; i < kAntiDceK; ++i) {
         vals[i] = vary(v, i);
         capnp::MallocMessageBuilder mb;
         cp_bench::build(mb.initRoot<CP>(), vals[i]);
         seed_arrs[i] = capnp::messageToFlatArray(mb);
         word_arrs[i] = seed_arrs[i].asPtr();
      }
      const std::size_t wire = seed_arrs[0].asBytes().size();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = "capnp",
            .library    = "libcapnp",
            .mode       = "static",
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };
      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of — capnp's "size" is the flat-array word count after
      // building.  Closest cheap proxy: build, then sizeInWords.
      volatile std::uint64_t sink_size = 0;
      auto t_size = ns_per_iter(0u, [&](std::size_t i) {
         capnp::MallocMessageBuilder b;
         cp_bench::build(b.initRoot<CP>(), vals[i & (kAntiDceK - 1)]);
         sink_size ^= capnp::computeSerializedSizeInWords(b) * 8;
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials,
             "build + computeSerializedSizeInWords (no native sizer)");

      // encode_rvalue — fresh builder + build + messageToFlatArray.
      volatile std::uint64_t sink_enc = 0;
      auto t_enc = ns_per_iter(0u, [&](std::size_t i) {
         capnp::MallocMessageBuilder b;
         cp_bench::build(b.initRoot<CP>(), vals[i & (kAntiDceK - 1)]);
         auto out_arr = capnp::messageToFlatArray(b);
         sink_enc ^= out_arr.asBytes().size();
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials,
             "fresh MallocMessageBuilder + messageToFlatArray");

      // decode — full materialisation of the native C++ shape from the
      // capnp reader (every field walked, strings allocated, lists
      // populated).  getRoot alone is a zero-copy view; calling it
      // "decode" would conflate it with the materialise-T cost that
      // psio::decode<T>(fmt, …) actually pays.
      volatile std::uint64_t sink_dec = 0;
      auto t_dec = ns_per_iter(0u, [&](std::size_t i) {
         capnp::FlatArrayMessageReader reader(word_arrs[i & (kAntiDceK - 1)]);
         auto root = reader.getRoot<CP>();
         T native = cp_bench::to_native<T>(root);
         sink_dec ^= bench_view_target(native);
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials,
             "getRoot + materialise to native T (apples-to-apples decode)");

      // view_one — zero-copy random access of one scalar field.
      volatile std::uint64_t sink_v = 0;
      auto t_view = ns_per_iter(0u, [&](std::size_t i) {
         capnp::FlatArrayMessageReader reader(word_arrs[i & (kAntiDceK - 1)]);
         auto root = reader.getRoot<CP>();
         sink_v ^= static_cast<std::uint64_t>(
            cp_bench::view_first_scalar(root));
      });
      record("view_one", t_view.min_ns, t_view.median_ns, cv(t_view),
             wire, t_view.iters, t_view.trials,
             "FlatArrayMessageReader + first-scalar getter (zero-copy)");
   }
#endif

   //  pjson — psio's tagged-binary self-describing format.  Doesn't
   //  plug into the encode/decode CPO (it uses its own
   //  from_struct/to_struct API), so it gets a dedicated cell.
   //  Available unconditionally — no external library required.
   template <typename T>
   void bench_pjson_cell(std::vector<snapshot_row>& out,
                         const std::string&         shape_name,
                         const T&                   v)
   {
      // K varied input values + K pre-encoded buffers.
      std::array<T, kAntiDceK>                            vals;
      std::array<std::vector<std::uint8_t>, kAntiDceK>    bufs;
      std::array<std::span<const std::uint8_t>, kAntiDceK> spans;
      for (std::size_t i = 0; i < kAntiDceK; ++i) {
         vals[i]  = vary(v, i);
         bufs[i]  = ::psio::from_struct(vals[i]);
         spans[i] = std::span<const std::uint8_t>{bufs[i].data(),
                                                  bufs[i].size()};
      }
      const std::size_t wire = bufs[0].size();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b,
                         std::size_t iters, int trials,
                         std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = "pjson",
            .library    = "psio",
            .mode       = "static",
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };
      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of: pjson lacks a sizer that runs on T (its sizer takes a
      // pjson_value).  Closest honest measurement: re-run from_struct
      // (which internally runs the size pass) and report the resulting
      // buffer size.
      volatile std::uint64_t sink_size = 0;
      auto t_size = ns_per_iter(0u, [&](std::size_t i) {
         auto b = ::psio::from_struct(vals[i & (kAntiDceK - 1)]);
         sink_size ^= b.size();
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials,
             "from_struct + .size() (no native typed sizer)");

      // encode_rvalue
      volatile std::uint64_t sink_enc = 0;
      auto t_enc = ns_per_iter(0u, [&](std::size_t i) {
         auto b = ::psio::from_struct(vals[i & (kAntiDceK - 1)]);
         sink_enc ^= b.size();
         if (!b.empty()) sink_enc ^= b[0];
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials);

      // encode_sink: reused buffer via to_pjson(t, sink&).
      std::vector<std::uint8_t> sink_buf;
      sink_buf.reserve(wire * 2);
      volatile std::uint64_t sink_sk = 0;
      auto t_sink = ns_per_iter(0u, [&](std::size_t i) {
         sink_buf.clear();
         ::psio::to_pjson(vals[i & (kAntiDceK - 1)], sink_buf);
         sink_sk ^= sink_buf.size();
      });
      record("encode_sink", t_sink.min_ns, t_sink.median_ns,
             cv(t_sink), wire, t_sink.iters, t_sink.trials,
             "to_pjson into reused vector<u8>");

      // decode: full materialisation via view<T, pjson_format>::to_struct().
      volatile std::uint64_t sink_dec = 0;
      auto t_dec = ns_per_iter(0u, [&](std::size_t i) {
         const auto& sp = spans[i & (kAntiDceK - 1)];
         auto raw = ::psio::pjson_view{sp.data(), sp.size()};
         auto tv  = ::psio::view<T, ::psio::pjson_format>::from_pjson(raw);
         T native = tv.to_struct();
         sink_dec ^= bench_view_target(native);
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials,
             "from_pjson + to_struct (full materialisation)");

      // validate
      try {
         volatile std::uint64_t sink_val = 0;
         auto t_val = ns_per_iter(0u, [&](std::size_t i) {
            const auto& sp = spans[i & (kAntiDceK - 1)];
            sink_val ^=
               static_cast<std::uint64_t>(::psio::pjson::validate(sp));
         });
         record("validate", t_val.min_ns, t_val.median_ns, cv(t_val),
                wire, t_val.iters, t_val.trials);
      } catch (...) {}

      // view_one: typed pjson view + chain to the same field
      // bench_view_target reads.  Canonical fast path: from_pjson()
      // runs a single memcmp on the schema-hash template, then every
      // field<I>() is an O(1) slot-table read — no decode of the rest
      // of the document.  Non-canonical falls back to find().  The
      // bench encodes via from_struct → output is canonical, so this
      // measures the canonical fast path.
      if constexpr (::psio::Reflected<T>) {
         volatile std::uint64_t sink_v = 0;
         auto t_view = ns_per_iter(0u, [&](std::size_t i) {
            const auto& sp = spans[i & (kAntiDceK - 1)];
            auto raw = ::psio::pjson_view{sp.data(), sp.size()};
            sink_v ^= pjson_view_target<T>(raw);
         });
         record("view_one", t_view.min_ns, t_view.median_ns,
                cv(t_view), wire, t_view.iters, t_view.trials,
                "view<T,pjson_format> + chain (canonical fast path)");
      }
   }

   //  Run all psio formats against one shape value.  Each cell is
   //  gated by fmt_supports<Fmt, T> so unsupported pairs are skipped
   //  rather than triggering a static_assert at compile time.
   template <typename T>
   void run_shape(std::vector<snapshot_row>& out,
                  const std::string&         shape_name,
                  const T&                   v)
   {
      auto cell = [&]<typename Fmt>(const std::string& fmt_name, Fmt fmt) {
         if constexpr (fmt_supports<Fmt, T>::value)
            bench_psio_cell(out, shape_name, fmt_name, fmt, v);
      };
      cell("ssz",      psio::ssz{});
      cell("pssz",     psio::pssz{});
      cell("frac32",   psio::frac32{});
      cell("bin",      psio::bin{});
      cell("borsh",    psio::borsh{});
      cell("bincode",  psio::bincode{});
      cell("avro",     psio::avro{});
      cell("protobuf", psio::protobuf{});
      cell("msgpack",  psio::msgpack{});
      cell("capnp",    psio::capnp{});
      cell("flatbuf",  psio::flatbuf{});
      // WIT canonical-ABI memory layout (Component Model).  Sink path
      // is policy-driven (StorePolicy/LoadPolicy), not the streaming
      // CPO, so wit times the in-memory lower/lift round-trip.
      cell("wit",      psio::wit{});
      // Self-describing formats (no schema needed at decode time).
      cell("json",     psio::json{});
      // BSON — MongoDB Binary JSON; self-describing key/value document.
      cell("bson",     psio::bson{});

      // pjson — psio's tagged-binary self-describing format.  Has its
      // own from_struct / to_struct API (NOT the encode/decode CPO),
      // so it gets a dedicated cell rather than going through `cell()`.
      bench_pjson_cell(out, shape_name, v);

#ifdef PSIO_HAVE_MSGPACK
      bench_msgpack_cxx_cell(out, shape_name, v);
#endif
#ifdef PSIO_HAVE_PROTOBUF
      if constexpr (requires {
                       typename pb_bench::pb_message_for<T>::type;
                    })
         bench_libprotobuf_cell(out, shape_name, v);
#endif
#ifdef PSIO_HAVE_FLATBUF
      if constexpr (requires {
                       typename fb_bench_adapter::fb_table_for<T>::type;
                    })
         bench_libflatbuf_cell(out, shape_name, v);
#endif
#ifdef PSIO_HAVE_CAPNP
      if constexpr (requires {
                       typename cp_bench::cp_struct_for<T>::type;
                    })
         bench_libcapnp_cell(out, shape_name, v);
#endif
   }

}  // namespace

int main(int argc, char** argv)
{
   const auto plat = detect_platform_info();

   std::printf("# psio vs externals — perf snapshot\n\n");
   std::printf("commit: %s, os: %s, arch: %s, compiler: %s, "
               "build: %s\n\n",
               plat.commit_short.c_str(), plat.os.c_str(),
               plat.arch.c_str(), plat.compiler.c_str(),
               plat.build_type.c_str());

   std::vector<snapshot_row> rows;

   // Tier 1-7 + bounded
   run_shape(rows, "Point",                psio_bench::point());
   run_shape(rows, "NameRecord",           psio_bench::namerec());
   run_shape(rows, "FlatRecord",           psio_bench::flatrec());
   run_shape(rows, "Record",               psio_bench::record());
   run_shape(rows, "Validator",            psio_bench::validator());
   run_shape(rows, "Order",                psio_bench::order());
   run_shape(rows, "ValidatorList(100)",   psio_bench::vlist(100));
   run_shape(rows, "FlatRecordBounded",    psio_bench::flatrec_bounded());
   run_shape(rows, "RecordBounded",        psio_bench::record_bounded());
   run_shape(rows, "OrderBounded",         psio_bench::order_bounded());
   run_shape(rows, "ValidatorListBounded(100)",
             psio_bench::vlist_bounded(100));

   // ── Dwnc twins of the variable-shape tiers ─────────────────────
   // For each non-DWNC shape that already exists, time a paired
   // DWNC variant — apples-to-apples for extensibility-aware
   // formats (frac32, pssz) so they aren't penalised for header
   // overhead the user opted out of.
   run_shape(rows, "FlatRecordDwnc",        psio_bench::flatrec_dwnc());
   run_shape(rows, "RecordDwnc",            psio_bench::record_dwnc());
   run_shape(rows, "OrderDwnc",             psio_bench::order_dwnc());
   run_shape(rows, "ValidatorListDwnc(100)",psio_bench::vlist_dwnc(100));

   // Depth-4 nested — Ext vs Dwnc head-to-head shows the cost of the
   // extensibility prefix at every nesting level.
   run_shape(rows, "Deep4Ext",   psio_bench::deep4_ext());
   run_shape(rows, "Deep4Dwnc",  psio_bench::deep4_dwnc());

   // ── Variety stress shapes ───────────────────────────────────────
   //   MlEmbedding : id + vec<f32>(64)  — exercises typed-array fast
   //                                       paths (BSON Vector subtype
   //                                       0x09 dtype 0x10 (FLOAT32),
   //                                       msgpack typed-array,
   //                                       protobuf packed repeated).
   //   BlobPayload : id + vec<u8>(256)  — exercises bytes / blob
   //                                       paths (BSON binary 0x00,
   //                                       msgpack `bin`, protobuf
   //                                       `bytes`).
   //   WideRecord  : 32 × u32           — stresses vtable size in
   //                                       flatbuf, slot tables in
   //                                       pjson, fixed-region offset
   //                                       arithmetic in pssz/ssz.
   run_shape(rows, "MlEmbedding",  psio_bench::ml_embedding());
   run_shape(rows, "BlobPayload",  psio_bench::blob_payload());
   run_shape(rows, "WideRecord",   psio_bench::wide_record());

   // Where to write the snapshot.  Default: bench_snapshots/.  Caller
   // can override via PSIO_BENCH_SNAPSHOT_DIR or argv[1].
   std::string snap_dir = "bench_snapshots";
   if (const char* env = std::getenv("PSIO_BENCH_SNAPSHOT_DIR");
       env && *env)
      snap_dir = env;
   if (argc > 1)
      snap_dir = argv[1];

   std::filesystem::create_directories(snap_dir);
   std::string snap_name = "perf_" + plat.timestamp_utc + "_" +
                            plat.commit_short + ".csv";
   std::filesystem::path snap_path =
      std::filesystem::path{snap_dir} / snap_name;

   std::ofstream snap{snap_path};
   if (!snap)
   {
      std::fprintf(stderr, "failed to open %s for writing\n",
                    snap_path.c_str());
      return 1;
   }
   write_snapshot_csv(snap, plat, rows);
   snap.close();

   std::printf("wrote %zu rows to %s\n", rows.size(),
               snap_path.c_str());

   // ── Wire-size summary table ─────────────────────────────────────
   //
   // Prints one row per (shape, format) showing the byte count.
   // Useful at a glance to compare format compactness on the same
   // payload — orthogonal to the speed numbers, often the real
   // selection criterion.

   //  Pull (shape, format, library) → size from the rows we just
   //  collected.  encode_rvalue rows always carry wire_bytes; we use
   //  those.
   struct size_key
   {
      std::string shape, format, library;
      bool        operator<(const size_key& o) const
      {
         if (shape != o.shape) return shape < o.shape;
         if (format != o.format) return format < o.format;
         return library < o.library;
      }
   };
   std::map<size_key, std::size_t> sizes;
   for (const auto& r : rows)
   {
      if (r.op != "encode_rvalue" && r.op != "encode_sink")
         continue;
      size_key k{r.shape, r.format, r.library};
      auto [it, inserted] = sizes.emplace(k, r.wire_bytes);
      if (!inserted && r.wire_bytes > 0 && it->second == 0)
         it->second = r.wire_bytes;
   }

   //  Group into one column per (format, library) pair and emit a
   //  matrix-style table.  Columns are sorted by the median size
   //  across shapes (compact formats first).
   std::map<std::string, std::map<std::string, std::size_t>> table;
   //  table[shape][format-pair] = bytes
   std::set<std::string> col_set;
   for (const auto& [k, b] : sizes)
   {
      std::string col =
         k.library == "psio" ? k.format
                              : (k.library + "/" + k.format);
      table[k.shape][col] = b;
      col_set.insert(col);
   }

   std::printf("\n# Wire-size matrix (bytes per encoded shape)\n\n");
   std::printf("| %-26s |", "shape");
   for (const auto& c : col_set)
      std::printf(" %12s |", c.c_str());
   std::printf("\n|%-28s|", "----------------------------");
   for (size_t i = 0; i < col_set.size(); ++i)
      std::printf("--------------|");
   std::printf("\n");
   for (const auto& [shape, cols] : table)
   {
      std::printf("| %-26s |", shape.c_str());
      for (const auto& c : col_set)
      {
         auto it = cols.find(c);
         if (it == cols.end())
            std::printf(" %12s |", "  -");
         else
            std::printf(" %12zu |", it->second);
      }
      std::printf("\n");
   }

   return 0;
}
