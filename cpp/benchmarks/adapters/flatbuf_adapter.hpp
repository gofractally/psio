#pragma once
//
// adapters/flatbuf_adapter.hpp — drive libflatbuffers (Google's
// canonical FlatBuffers C++ library) on the unified bench shapes via
// the generated bench_schemas_generated.h.
//
// Construction is bottom-up: children (strings, vectors, sub-tables)
// must be created before their parent's CreateXxx() call so the
// builder knows their offsets.  These build_*() helpers encapsulate
// that pattern — caller passes a FlatBufferBuilder and a psio shape,
// gets back the root offset.

#include "bench_schemas_generated.h"

#include "../shapes.hpp"

#include <flatbuffers/flatbuffers.h>

namespace fb_bench_adapter {

   using fbs = flatbuffers::FlatBufferBuilder;

   inline flatbuffers::Offset<fb_bench::fb_Point>
   build(fbs& b, const Point& v)
   {
      return fb_bench::Createfb_Point(b, v.x, v.y);
   }

   inline flatbuffers::Offset<fb_bench::fb_NameRecord>
   build(fbs& b, const NameRecord& v)
   {
      return fb_bench::Createfb_NameRecord(b, v.account, v.limit);
   }

   inline flatbuffers::Offset<fb_bench::fb_FlatRecord>
   build(fbs& b, const FlatRecord& v)
   {
      auto label  = b.CreateString(v.label);
      std::vector<uint32_t> vals_v(v.values.begin(), v.values.end());
      auto values = b.CreateVector(vals_v);
      return fb_bench::Createfb_FlatRecord(b, v.id, label, values);
   }
   inline flatbuffers::Offset<fb_bench::fb_FlatRecord>
   build(fbs& b, const FlatRecordBounded& v)
   {
      auto label  = b.CreateString(v.label);
      auto values = b.CreateVector(v.values);
      return fb_bench::Createfb_FlatRecord(b, v.id, label, values);
   }

   inline flatbuffers::Offset<fb_bench::fb_Record>
   build(fbs& b, const Record& v)
   {
      auto label  = b.CreateString(v.label);
      std::vector<uint32_t> vals_v(v.values.begin(), v.values.end());
      auto values = b.CreateVector(vals_v);
      return fb_bench::Createfb_Record(b, v.id, label, values,
                                        v.score ? *v.score : 0u);
   }
   inline flatbuffers::Offset<fb_bench::fb_Record>
   build(fbs& b, const RecordBounded& v)
   {
      auto label  = b.CreateString(v.label);
      auto values = b.CreateVector(v.values);
      return fb_bench::Createfb_Record(b, v.id, label, values,
                                        v.score ? *v.score : 0u);
   }

   inline flatbuffers::Offset<fb_bench::fb_Validator>
   build(fbs& b, const Validator& v)
   {
      return fb_bench::Createfb_Validator(
         b, v.pubkey_lo, v.pubkey_hi, v.withdrawal_lo, v.withdrawal_hi,
         v.effective_balance, v.slashed, v.activation_epoch,
         v.exit_epoch, v.withdrawable_epoch);
   }

   inline flatbuffers::Offset<fb_bench::fb_LineItem>
   build(fbs& b, const LineItem& v)
   {
      auto product = b.CreateString(v.product);
      return fb_bench::Createfb_LineItem(b, product, v.qty, v.unit_price);
   }
   inline flatbuffers::Offset<fb_bench::fb_LineItem>
   build(fbs& b, const LineItemBounded& v)
   {
      auto product = b.CreateString(v.product);
      return fb_bench::Createfb_LineItem(b, product, v.qty, v.unit_price);
   }

   inline flatbuffers::Offset<fb_bench::fb_UserProfile>
   build(fbs& b, const UserProfile& v)
   {
      auto name  = b.CreateString(v.name);
      auto email = b.CreateString(v.email);
      return fb_bench::Createfb_UserProfile(b, v.id, name, email, v.age,
                                             v.verified);
   }
   inline flatbuffers::Offset<fb_bench::fb_UserProfile>
   build(fbs& b, const UserProfileBounded& v)
   {
      auto name  = b.CreateString(v.name);
      auto email = b.CreateString(v.email);
      return fb_bench::Createfb_UserProfile(b, v.id, name, email, v.age,
                                             v.verified);
   }

   inline flatbuffers::Offset<fb_bench::fb_Order>
   build(fbs& b, const Order& v)
   {
      auto customer = build(b, v.customer);
      std::vector<flatbuffers::Offset<fb_bench::fb_LineItem>> items_v;
      items_v.reserve(v.items.size());
      for (const auto& it : v.items)
         items_v.push_back(build(b, it));
      auto items = b.CreateVector(items_v);
      flatbuffers::Offset<flatbuffers::String> note;
      if (v.note)
         note = b.CreateString(*v.note);
      return fb_bench::Createfb_Order(b, v.id, customer, items, v.total,
                                       note);
   }
   inline flatbuffers::Offset<fb_bench::fb_Order>
   build(fbs& b, const OrderBounded& v)
   {
      auto customer = build(b, v.customer);
      std::vector<flatbuffers::Offset<fb_bench::fb_LineItem>> items_v;
      items_v.reserve(v.items.size());
      for (const auto& it : v.items)
         items_v.push_back(build(b, it));
      auto items = b.CreateVector(items_v);
      flatbuffers::Offset<flatbuffers::String> note;
      if (v.note)
         note = b.CreateString(*v.note);
      return fb_bench::Createfb_Order(b, v.id, customer, items, v.total,
                                       note);
   }

   inline flatbuffers::Offset<fb_bench::fb_ValidatorList>
   build(fbs& b, const ValidatorList& v)
   {
      std::vector<flatbuffers::Offset<fb_bench::fb_Validator>> vs_v;
      vs_v.reserve(v.validators.size());
      for (const auto& vd : v.validators)
         vs_v.push_back(build(b, vd));
      auto validators = b.CreateVector(vs_v);
      return fb_bench::Createfb_ValidatorList(b, v.epoch, validators);
   }
   inline flatbuffers::Offset<fb_bench::fb_ValidatorList>
   build(fbs& b, const ValidatorListBounded& v)
   {
      std::vector<flatbuffers::Offset<fb_bench::fb_Validator>> vs_v;
      vs_v.reserve(v.validators.size());
      for (const auto& vd : v.validators)
         vs_v.push_back(build(b, vd));
      auto validators = b.CreateVector(vs_v);
      return fb_bench::Createfb_ValidatorList(b, v.epoch, validators);
   }

   //  Map each psio shape to its fb_bench root table type.
   template <typename Shape>
   struct fb_table_for;

   template <> struct fb_table_for<Point>                  { using type = fb_bench::fb_Point;          };
   template <> struct fb_table_for<NameRecord>             { using type = fb_bench::fb_NameRecord;     };
   template <> struct fb_table_for<FlatRecord>             { using type = fb_bench::fb_FlatRecord;     };
   template <> struct fb_table_for<FlatRecordBounded>      { using type = fb_bench::fb_FlatRecord;     };
   template <> struct fb_table_for<Record>                 { using type = fb_bench::fb_Record;         };
   template <> struct fb_table_for<RecordBounded>          { using type = fb_bench::fb_Record;         };
   template <> struct fb_table_for<Validator>              { using type = fb_bench::fb_Validator;      };
   template <> struct fb_table_for<Order>                  { using type = fb_bench::fb_Order;          };
   template <> struct fb_table_for<OrderBounded>           { using type = fb_bench::fb_Order;          };
   template <> struct fb_table_for<ValidatorList>          { using type = fb_bench::fb_ValidatorList;  };
   template <> struct fb_table_for<ValidatorListBounded>   { using type = fb_bench::fb_ValidatorList;  };

   template <typename Shape>
   using fb_table_t = typename fb_table_for<Shape>::type;

   //  view_first_scalar(reader) — first scalar field per shape, for
   //  view_one bench op.  flatbuffers is zero-copy: the call expands
   //  to a couple of pointer dereferences + a fixed-offset load.
   inline std::int32_t view_first_scalar(const fb_bench::fb_Point* r)
   { return r->x(); }
   inline std::uint64_t view_first_scalar(const fb_bench::fb_NameRecord* r)
   { return r->account(); }
   inline std::uint32_t view_first_scalar(const fb_bench::fb_FlatRecord* r)
   { return r->id(); }
   inline std::uint32_t view_first_scalar(const fb_bench::fb_Record* r)
   { return r->id(); }
   inline std::uint64_t view_first_scalar(const fb_bench::fb_Validator* r)
   { return r->pubkey_lo(); }
   inline std::uint32_t view_first_scalar(const fb_bench::fb_LineItem* r)
   { return r->qty(); }
   inline std::uint64_t view_first_scalar(const fb_bench::fb_UserProfile* r)
   { return r->id(); }
   //  Order / ValidatorList — reach the same deep field that
   //  `bench_view_target` reads in the psio fallback path, so the
   //  view_one numbers compare apples-to-apples.  Without this,
   //  flatbuf/capnp would report "first outer scalar" while psio
   //  formats reported "decode + reach into nested element".
   inline std::uint64_t view_first_scalar(const fb_bench::fb_Order* r)
   { return r->customer()->id(); }
   inline std::uint64_t view_first_scalar(const fb_bench::fb_ValidatorList* r)
   {
      auto* vs = r->validators();
      if (!vs || vs->size() == 0) return 0;
      return vs->Get(vs->size() / 2)->pubkey_lo();
   }

   // ── to_native — full materialization from a flatbuffer reader ─────────
   //
   // GetRoot is zero-copy (a few pointer arithmetic ops).  A real
   // libflatbuffers consumer that wants a native C++ struct still has to
   // walk every field, allocate strings, and populate vectors.  These
   // overloads do that work explicitly so the bench's "decode" cell
   // measures honest materialisation, not just a vtable read.

   template <typename Shape>
   Shape to_native(const fb_table_t<Shape>* r);

   template <> inline Point to_native<Point>(const fb_bench::fb_Point* r)
   { return Point{r->x(), r->y()}; }

   template <>
   inline NameRecord to_native<NameRecord>(const fb_bench::fb_NameRecord* r)
   { return NameRecord{r->account(), r->limit()}; }

   template <>
   inline FlatRecord to_native<FlatRecord>(const fb_bench::fb_FlatRecord* r)
   {
      FlatRecord o;
      o.id = r->id();
      if (auto* lab = r->label())
         o.label.assign(lab->c_str(), lab->size());
      if (auto* vs = r->values()) {
         o.values.reserve(vs->size());
         for (auto v : *vs)
            o.values.push_back(static_cast<std::uint16_t>(v));
      }
      return o;
   }
   template <>
   inline FlatRecordBounded
   to_native<FlatRecordBounded>(const fb_bench::fb_FlatRecord* r)
   {
      FlatRecordBounded o;
      o.id = r->id();
      if (auto* lab = r->label())
         o.label.assign(lab->c_str(), lab->size());
      if (auto* vs = r->values()) {
         o.values.reserve(vs->size());
         for (auto v : *vs)
            o.values.push_back(v);
      }
      return o;
   }

   template <>
   inline Record to_native<Record>(const fb_bench::fb_Record* r)
   {
      Record o;
      o.id = r->id();
      if (auto* lab = r->label())
         o.label.assign(lab->c_str(), lab->size());
      if (auto* vs = r->values()) {
         o.values.reserve(vs->size());
         for (auto v : *vs)
            o.values.push_back(static_cast<std::uint16_t>(v));
      }
      // The flatbuf schema represents `score` as a non-optional default-0
      // u32; treat 0 as "no value" to match the Record default.
      if (r->score() != 0) o.score = r->score();
      return o;
   }
   template <>
   inline RecordBounded
   to_native<RecordBounded>(const fb_bench::fb_Record* r)
   {
      RecordBounded o;
      o.id = r->id();
      if (auto* lab = r->label())
         o.label.assign(lab->c_str(), lab->size());
      if (auto* vs = r->values()) {
         o.values.reserve(vs->size());
         for (auto v : *vs)
            o.values.push_back(v);
      }
      if (r->score() != 0) o.score = r->score();
      return o;
   }

   template <>
   inline Validator to_native<Validator>(const fb_bench::fb_Validator* r)
   {
      Validator v;
      v.pubkey_lo          = r->pubkey_lo();
      v.pubkey_hi          = r->pubkey_hi();
      v.withdrawal_lo      = r->withdrawal_lo();
      v.withdrawal_hi      = r->withdrawal_hi();
      v.effective_balance  = r->effective_balance();
      v.slashed            = r->slashed();
      v.activation_epoch   = r->activation_epoch();
      v.exit_epoch         = r->exit_epoch();
      v.withdrawable_epoch = r->withdrawable_epoch();
      return v;
   }

   inline LineItem
   to_native_lineitem(const fb_bench::fb_LineItem* r)
   {
      LineItem li;
      if (auto* p = r->product())
         li.product.assign(p->c_str(), p->size());
      li.qty        = r->qty();
      li.unit_price = r->unit_price();
      return li;
   }
   inline LineItemBounded
   to_native_lineitem_bounded(const fb_bench::fb_LineItem* r)
   {
      LineItemBounded li;
      if (auto* p = r->product())
         li.product.assign(p->c_str(), p->size());
      li.qty        = r->qty();
      li.unit_price = r->unit_price();
      return li;
   }
   inline UserProfile
   to_native_userprofile(const fb_bench::fb_UserProfile* r)
   {
      UserProfile up;
      up.id = r->id();
      if (auto* n = r->name())  up.name.assign(n->c_str(), n->size());
      if (auto* e = r->email()) up.email.assign(e->c_str(), e->size());
      up.age      = r->age();
      up.verified = r->verified();
      return up;
   }
   inline UserProfileBounded
   to_native_userprofile_bounded(const fb_bench::fb_UserProfile* r)
   {
      UserProfileBounded up;
      up.id = r->id();
      if (auto* n = r->name())  up.name.assign(n->c_str(), n->size());
      if (auto* e = r->email()) up.email.assign(e->c_str(), e->size());
      up.age      = r->age();
      up.verified = r->verified();
      return up;
   }

   template <>
   inline Order to_native<Order>(const fb_bench::fb_Order* r)
   {
      Order o;
      o.id       = r->id();
      if (auto* c = r->customer()) o.customer = to_native_userprofile(c);
      if (auto* its = r->items()) {
         o.items.reserve(its->size());
         for (auto* it : *its)
            o.items.push_back(to_native_lineitem(it));
      }
      o.total = r->total();
      if (auto* nt = r->note())
         o.note = std::string{nt->c_str(), nt->size()};
      return o;
   }
   template <>
   inline OrderBounded to_native<OrderBounded>(const fb_bench::fb_Order* r)
   {
      OrderBounded o;
      o.id       = r->id();
      if (auto* c = r->customer())
         o.customer = to_native_userprofile_bounded(c);
      if (auto* its = r->items()) {
         o.items.reserve(its->size());
         for (auto* it : *its)
            o.items.push_back(to_native_lineitem_bounded(it));
      }
      o.total = r->total();
      if (auto* nt = r->note())
         o.note = std::string{nt->c_str(), nt->size()};
      return o;
   }

   template <>
   inline ValidatorList
   to_native<ValidatorList>(const fb_bench::fb_ValidatorList* r)
   {
      ValidatorList vl;
      vl.epoch = r->epoch();
      if (auto* vs = r->validators()) {
         vl.validators.reserve(vs->size());
         for (auto* v : *vs)
            vl.validators.push_back(to_native<Validator>(v));
      }
      return vl;
   }
   template <>
   inline ValidatorListBounded
   to_native<ValidatorListBounded>(const fb_bench::fb_ValidatorList* r)
   {
      ValidatorListBounded vl;
      vl.epoch = r->epoch();
      if (auto* vs = r->validators()) {
         vl.validators.reserve(vs->size());
         for (auto* v : *vs)
            vl.validators.push_back(to_native<Validator>(v));
      }
      return vl;
   }

}  // namespace fb_bench_adapter
