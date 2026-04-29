#pragma once
//
// adapters/capnp_adapter.hpp — drive libcapnp (Sandstorm's Cap'n Proto
// C++ library) on the unified bench shapes via the generated
// bench_schemas.capnp.h.
//
// capnp is zero-copy: encode = MallocMessageBuilder + initRoot<T>() +
// setters + messageToFlatArray; decode = FlatArrayMessageReader +
// getRoot<T>() (just pointer arithmetic).  For a fair comparison
// against the streaming codecs we measure a "decode + walk" path
// that touches at least one field, since GetRoot alone optimises
// to nearly zero.

#include "bench_schemas.capnp.h"

#include "../shapes.hpp"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>

namespace cp_bench {

   //  build_<Shape>(builder, shape) — populate the capnp Builder
   //  using the schema's setX/initY methods.

   inline void build(CpPoint::Builder b, const Point& v)
   {
      b.setX(v.x);
      b.setY(v.y);
   }

   inline void build(CpNameRecord::Builder b, const NameRecord& v)
   {
      b.setAccount(v.account);
      b.setLimit(v.limit);
   }

   inline void build(CpFlatRecord::Builder b, const FlatRecord& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, v.values[i]);
   }
   inline void build(CpFlatRecord::Builder b, const FlatRecordBounded& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, static_cast<uint16_t>(v.values[i]));
   }

   inline void build(CpRecord::Builder b, const Record& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, v.values[i]);
      b.setScore(v.score ? *v.score : 0u);
   }
   inline void build(CpRecord::Builder b, const RecordBounded& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, static_cast<uint16_t>(v.values[i]));
      b.setScore(v.score ? *v.score : 0u);
   }

   inline void build(CpValidator::Builder b, const Validator& v)
   {
      b.setPubkeyLo(v.pubkey_lo);
      b.setPubkeyHi(v.pubkey_hi);
      b.setWithdrawalLo(v.withdrawal_lo);
      b.setWithdrawalHi(v.withdrawal_hi);
      b.setEffectiveBalance(v.effective_balance);
      b.setSlashed(v.slashed);
      b.setActivationEpoch(v.activation_epoch);
      b.setExitEpoch(v.exit_epoch);
      b.setWithdrawableEpoch(v.withdrawable_epoch);
   }

   inline void build(CpLineItem::Builder b, const LineItem& v)
   {
      b.setProduct(v.product);
      b.setQty(v.qty);
      b.setUnitPrice(v.unit_price);
   }
   inline void build(CpLineItem::Builder b, const LineItemBounded& v)
   {
      b.setProduct(v.product);
      b.setQty(v.qty);
      b.setUnitPrice(v.unit_price);
   }

   inline void build(CpUserProfile::Builder b, const UserProfile& v)
   {
      b.setId(v.id);
      b.setName(v.name);
      b.setEmail(v.email);
      b.setAge(v.age);
      b.setVerified(v.verified);
   }
   inline void build(CpUserProfile::Builder b,
                     const UserProfileBounded& v)
   {
      b.setId(v.id);
      b.setName(v.name);
      b.setEmail(v.email);
      b.setAge(v.age);
      b.setVerified(v.verified);
   }

   inline void build(CpOrder::Builder b, const Order& v)
   {
      b.setId(v.id);
      build(b.initCustomer(), v.customer);
      auto its = b.initItems(static_cast<unsigned>(v.items.size()));
      for (unsigned i = 0; i < v.items.size(); ++i)
         build(its[i], v.items[i]);
      b.setTotal(v.total);
      if (v.note)
         b.setNote(*v.note);
   }
   inline void build(CpOrder::Builder b, const OrderBounded& v)
   {
      b.setId(v.id);
      build(b.initCustomer(), v.customer);
      auto its = b.initItems(static_cast<unsigned>(v.items.size()));
      for (unsigned i = 0; i < v.items.size(); ++i)
         build(its[i], v.items[i]);
      b.setTotal(v.total);
      if (v.note)
         b.setNote(*v.note);
   }

   inline void build(CpValidatorList::Builder b, const ValidatorList& v)
   {
      b.setEpoch(v.epoch);
      auto vs = b.initValidators(
         static_cast<unsigned>(v.validators.size()));
      for (unsigned i = 0; i < v.validators.size(); ++i)
         build(vs[i], v.validators[i]);
   }
   inline void build(CpValidatorList::Builder b,
                     const ValidatorListBounded& v)
   {
      b.setEpoch(v.epoch);
      auto vs = b.initValidators(
         static_cast<unsigned>(v.validators.size()));
      for (unsigned i = 0; i < v.validators.size(); ++i)
         build(vs[i], v.validators[i]);
   }

   //  Map each psio shape to its capnp struct type.
   template <typename Shape>
   struct cp_struct_for;

   template <> struct cp_struct_for<Point>                  { using type = CpPoint;          };
   template <> struct cp_struct_for<NameRecord>             { using type = CpNameRecord;     };
   template <> struct cp_struct_for<FlatRecord>             { using type = CpFlatRecord;     };
   template <> struct cp_struct_for<FlatRecordBounded>      { using type = CpFlatRecord;     };
   template <> struct cp_struct_for<Record>                 { using type = CpRecord;         };
   template <> struct cp_struct_for<RecordBounded>          { using type = CpRecord;         };
   template <> struct cp_struct_for<Validator>              { using type = CpValidator;      };
   template <> struct cp_struct_for<Order>                  { using type = CpOrder;          };
   template <> struct cp_struct_for<OrderBounded>           { using type = CpOrder;          };
   template <> struct cp_struct_for<ValidatorList>          { using type = CpValidatorList;  };
   template <> struct cp_struct_for<ValidatorListBounded>   { using type = CpValidatorList;  };

   template <typename Shape>
   using cp_struct_t = typename cp_struct_for<Shape>::type;

   //  view_first_scalar(reader) — return one representative scalar
   //  field per shape, for the view_one bench op.  Forces the
   //  reader to actually deference into the wire bytes.
   inline std::int64_t view_first_scalar(CpPoint::Reader r)
   { return r.getX(); }
   inline std::uint64_t view_first_scalar(CpNameRecord::Reader r)
   { return r.getAccount(); }
   inline std::uint64_t view_first_scalar(CpFlatRecord::Reader r)
   { return r.getId(); }
   inline std::uint64_t view_first_scalar(CpRecord::Reader r)
   { return r.getId(); }
   inline std::uint64_t view_first_scalar(CpValidator::Reader r)
   { return r.getPubkeyLo(); }
   inline std::uint64_t view_first_scalar(CpLineItem::Reader r)
   { return r.getQty(); }
   inline std::uint64_t view_first_scalar(CpUserProfile::Reader r)
   { return r.getId(); }
   //  Order / ValidatorList — reach the same deep field that
   //  `bench_view_target` reads in the psio fallback path, so the
   //  view_one numbers compare apples-to-apples.  Without this,
   //  capnp/flatbuf would report "first outer scalar" while psio
   //  formats reported "decode + reach into nested element".
   inline std::uint64_t view_first_scalar(CpOrder::Reader r)
   { return r.getCustomer().getId(); }
   inline std::uint64_t view_first_scalar(CpValidatorList::Reader r)
   {
      auto vs = r.getValidators();
      if (vs.size() == 0) return 0;
      return vs[vs.size() / 2].getPubkeyLo();
   }

   // ── to_native — full materialization from a capnp reader ─────────────
   //
   // FlatArrayMessageReader + getRoot are zero-copy.  A real libcapnp
   // consumer that wants a native C++ struct still has to walk every
   // field, allocate strings, and populate vectors.  These overloads do
   // that work explicitly so the bench's "decode" cell measures honest
   // materialisation, not just a pointer-arithmetic getRoot.

   template <typename Shape>
   Shape to_native(typename cp_struct_t<Shape>::Reader r);

   template <>
   inline Point to_native<Point>(CpPoint::Reader r)
   {
      return Point{static_cast<std::int32_t>(r.getX()),
                   static_cast<std::int32_t>(r.getY())};
   }

   template <>
   inline NameRecord to_native<NameRecord>(CpNameRecord::Reader r)
   { return NameRecord{r.getAccount(), r.getLimit()}; }

   template <>
   inline FlatRecord to_native<FlatRecord>(CpFlatRecord::Reader r)
   {
      FlatRecord o;
      o.id    = static_cast<std::uint32_t>(r.getId());
      auto lab = r.getLabel();
      o.label.assign(lab.cStr(), lab.size());
      auto vs = r.getValues();
      o.values.reserve(vs.size());
      for (auto v : vs)
         o.values.push_back(static_cast<std::uint16_t>(v));
      return o;
   }
   template <>
   inline FlatRecordBounded
   to_native<FlatRecordBounded>(CpFlatRecord::Reader r)
   {
      FlatRecordBounded o;
      o.id    = static_cast<std::uint32_t>(r.getId());
      auto lab = r.getLabel();
      o.label.assign(lab.cStr(), lab.size());
      auto vs = r.getValues();
      o.values.reserve(vs.size());
      for (auto v : vs)
         o.values.push_back(static_cast<std::uint32_t>(v));
      return o;
   }

   template <>
   inline Record to_native<Record>(CpRecord::Reader r)
   {
      Record o;
      o.id    = static_cast<std::uint32_t>(r.getId());
      auto lab = r.getLabel();
      o.label.assign(lab.cStr(), lab.size());
      auto vs = r.getValues();
      o.values.reserve(vs.size());
      for (auto v : vs)
         o.values.push_back(static_cast<std::uint16_t>(v));
      auto sc = r.getScore();
      if (sc != 0) o.score = static_cast<std::uint32_t>(sc);
      return o;
   }
   template <>
   inline RecordBounded to_native<RecordBounded>(CpRecord::Reader r)
   {
      RecordBounded o;
      o.id    = static_cast<std::uint32_t>(r.getId());
      auto lab = r.getLabel();
      o.label.assign(lab.cStr(), lab.size());
      auto vs = r.getValues();
      o.values.reserve(vs.size());
      for (auto v : vs)
         o.values.push_back(static_cast<std::uint32_t>(v));
      auto sc = r.getScore();
      if (sc != 0) o.score = static_cast<std::uint32_t>(sc);
      return o;
   }

   template <>
   inline Validator to_native<Validator>(CpValidator::Reader r)
   {
      Validator v;
      v.pubkey_lo          = r.getPubkeyLo();
      v.pubkey_hi          = r.getPubkeyHi();
      v.withdrawal_lo      = r.getWithdrawalLo();
      v.withdrawal_hi      = r.getWithdrawalHi();
      v.effective_balance  = r.getEffectiveBalance();
      v.slashed            = r.getSlashed();
      v.activation_epoch   = r.getActivationEpoch();
      v.exit_epoch         = r.getExitEpoch();
      v.withdrawable_epoch = r.getWithdrawableEpoch();
      return v;
   }

   inline LineItem to_native_lineitem(CpLineItem::Reader r)
   {
      LineItem li;
      auto p = r.getProduct();
      li.product.assign(p.cStr(), p.size());
      li.qty        = static_cast<std::uint32_t>(r.getQty());
      li.unit_price = r.getUnitPrice();
      return li;
   }
   inline LineItemBounded to_native_lineitem_bounded(CpLineItem::Reader r)
   {
      LineItemBounded li;
      auto p = r.getProduct();
      li.product.assign(p.cStr(), p.size());
      li.qty        = static_cast<std::uint32_t>(r.getQty());
      li.unit_price = r.getUnitPrice();
      return li;
   }
   inline UserProfile to_native_userprofile(CpUserProfile::Reader r)
   {
      UserProfile up;
      up.id       = r.getId();
      auto n = r.getName();   up.name.assign(n.cStr(), n.size());
      auto e = r.getEmail();  up.email.assign(e.cStr(), e.size());
      up.age      = static_cast<std::uint32_t>(r.getAge());
      up.verified = r.getVerified();
      return up;
   }
   inline UserProfileBounded
   to_native_userprofile_bounded(CpUserProfile::Reader r)
   {
      UserProfileBounded up;
      up.id       = r.getId();
      auto n = r.getName();   up.name.assign(n.cStr(), n.size());
      auto e = r.getEmail();  up.email.assign(e.cStr(), e.size());
      up.age      = static_cast<std::uint32_t>(r.getAge());
      up.verified = r.getVerified();
      return up;
   }

   template <>
   inline Order to_native<Order>(CpOrder::Reader r)
   {
      Order o;
      o.id       = r.getId();
      o.customer = to_native_userprofile(r.getCustomer());
      auto its   = r.getItems();
      o.items.reserve(its.size());
      for (auto it : its) o.items.push_back(to_native_lineitem(it));
      o.total = r.getTotal();
      if (r.hasNote())
      {
         auto n = r.getNote();
         o.note = std::string{n.cStr(), n.size()};
      }
      return o;
   }
   template <>
   inline OrderBounded to_native<OrderBounded>(CpOrder::Reader r)
   {
      OrderBounded o;
      o.id       = r.getId();
      o.customer = to_native_userprofile_bounded(r.getCustomer());
      auto its   = r.getItems();
      o.items.reserve(its.size());
      for (auto it : its)
         o.items.push_back(to_native_lineitem_bounded(it));
      o.total = r.getTotal();
      if (r.hasNote())
      {
         auto n = r.getNote();
         o.note = std::string{n.cStr(), n.size()};
      }
      return o;
   }

   template <>
   inline ValidatorList
   to_native<ValidatorList>(CpValidatorList::Reader r)
   {
      ValidatorList vl;
      vl.epoch = r.getEpoch();
      auto vs  = r.getValidators();
      vl.validators.reserve(vs.size());
      for (auto v : vs)
         vl.validators.push_back(to_native<Validator>(v));
      return vl;
   }
   template <>
   inline ValidatorListBounded
   to_native<ValidatorListBounded>(CpValidatorList::Reader r)
   {
      ValidatorListBounded vl;
      vl.epoch = r.getEpoch();
      auto vs  = r.getValidators();
      vl.validators.reserve(vs.size());
      for (auto v : vs)
         vl.validators.push_back(to_native<Validator>(v));
      return vl;
   }

}  // namespace cp_bench
