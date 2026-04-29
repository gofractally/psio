#pragma once
//
// adapters/protobuf_adapter.hpp — drive libprotobuf (the canonical
// Google C++ protobuf library) on the unified bench shapes via the
// generated bench_schemas.pb.h.  Wire bytes match psio::protobuf for
// the same shape value; the comparison is purely about library
// performance.

#include "bench_schemas.pb.h"

#include "../shapes.hpp"

namespace pb_bench {

   // ── build_<Shape>(pb_msg, shape) ─ copy psio shape into pb message ──

   inline void build(pb_Point& pb, const Point& v)
   {
      pb.set_x(v.x);
      pb.set_y(v.y);
   }

   inline void build(pb_NameRecord& pb, const NameRecord& v)
   {
      pb.set_account(v.account);
      pb.set_limit(v.limit);
   }

   inline void build(pb_FlatRecord& pb, const FlatRecord& v)
   {
      pb.set_id(v.id);
      pb.set_label(v.label);
      pb.mutable_values()->Reserve(static_cast<int>(v.values.size()));
      for (auto x : v.values)
         pb.add_values(x);
   }

   inline void build(pb_FlatRecord& pb, const FlatRecordBounded& v)
   {
      pb.set_id(v.id);
      pb.set_label(v.label);
      pb.mutable_values()->Reserve(static_cast<int>(v.values.size()));
      for (auto x : v.values)
         pb.add_values(x);
   }

   inline void build(pb_Record& pb, const Record& v)
   {
      pb.set_id(v.id);
      pb.set_label(v.label);
      pb.mutable_values()->Reserve(static_cast<int>(v.values.size()));
      for (auto x : v.values)
         pb.add_values(x);
      if (v.score)
         pb.set_score(*v.score);
   }

   inline void build(pb_Record& pb, const RecordBounded& v)
   {
      pb.set_id(v.id);
      pb.set_label(v.label);
      pb.mutable_values()->Reserve(static_cast<int>(v.values.size()));
      for (auto x : v.values)
         pb.add_values(x);
      if (v.score)
         pb.set_score(*v.score);
   }

   inline void build(pb_Validator& pb, const Validator& v)
   {
      pb.set_pubkey_lo(v.pubkey_lo);
      pb.set_pubkey_hi(v.pubkey_hi);
      pb.set_withdrawal_lo(v.withdrawal_lo);
      pb.set_withdrawal_hi(v.withdrawal_hi);
      pb.set_effective_balance(v.effective_balance);
      pb.set_slashed(v.slashed);
      pb.set_activation_epoch(v.activation_epoch);
      pb.set_exit_epoch(v.exit_epoch);
      pb.set_withdrawable_epoch(v.withdrawable_epoch);
   }

   inline void build(pb_LineItem& pb, const LineItem& v)
   {
      pb.set_product(v.product);
      pb.set_qty(v.qty);
      pb.set_unit_price(v.unit_price);
   }
   inline void build(pb_LineItem& pb, const LineItemBounded& v)
   {
      pb.set_product(v.product);
      pb.set_qty(v.qty);
      pb.set_unit_price(v.unit_price);
   }

   inline void build(pb_UserProfile& pb, const UserProfile& v)
   {
      pb.set_id(v.id);
      pb.set_name(v.name);
      pb.set_email(v.email);
      pb.set_age(v.age);
      pb.set_verified(v.verified);
   }
   inline void build(pb_UserProfile& pb, const UserProfileBounded& v)
   {
      pb.set_id(v.id);
      pb.set_name(v.name);
      pb.set_email(v.email);
      pb.set_age(v.age);
      pb.set_verified(v.verified);
   }

   inline void build(pb_Order& pb, const Order& v)
   {
      pb.set_id(v.id);
      build(*pb.mutable_customer(), v.customer);
      for (const auto& it : v.items)
      {
         auto* p = pb.add_items();
         build(*p, it);
      }
      pb.set_total(v.total);
      if (v.note)
         pb.set_note(*v.note);
   }
   inline void build(pb_Order& pb, const OrderBounded& v)
   {
      pb.set_id(v.id);
      build(*pb.mutable_customer(), v.customer);
      for (const auto& it : v.items)
      {
         auto* p = pb.add_items();
         build(*p, it);
      }
      pb.set_total(v.total);
      if (v.note)
         pb.set_note(*v.note);
   }

   inline void build(pb_ValidatorList& pb, const ValidatorList& v)
   {
      pb.set_epoch(v.epoch);
      for (const auto& vd : v.validators)
      {
         auto* p = pb.add_validators();
         build(*p, vd);
      }
   }
   inline void build(pb_ValidatorList& pb, const ValidatorListBounded& v)
   {
      pb.set_epoch(v.epoch);
      for (const auto& vd : v.validators)
      {
         auto* p = pb.add_validators();
         build(*p, vd);
      }
   }

   // ── shape → pb message type ──
   //  Map each psio shape to its libprotobuf message counterpart so
   //  templated bench code can pick the right type without per-call
   //  branching.
   template <typename Shape>
   struct pb_message_for;

   template <> struct pb_message_for<Point>                  { using type = pb_Point;          };
   template <> struct pb_message_for<NameRecord>             { using type = pb_NameRecord;     };
   template <> struct pb_message_for<FlatRecord>             { using type = pb_FlatRecord;     };
   template <> struct pb_message_for<FlatRecordBounded>      { using type = pb_FlatRecord;     };
   template <> struct pb_message_for<Record>                 { using type = pb_Record;         };
   template <> struct pb_message_for<RecordBounded>          { using type = pb_Record;         };
   template <> struct pb_message_for<Validator>              { using type = pb_Validator;      };
   template <> struct pb_message_for<Order>                  { using type = pb_Order;          };
   template <> struct pb_message_for<OrderBounded>           { using type = pb_Order;          };
   template <> struct pb_message_for<ValidatorList>          { using type = pb_ValidatorList;  };
   template <> struct pb_message_for<ValidatorListBounded>   { using type = pb_ValidatorList;  };
   //  No competing pb message for LineItem / UserProfile (they're
   //  reachable only as nested inside Order); skip stand-alone bench.

   template <typename Shape>
   using pb_message_t = typename pb_message_for<Shape>::type;

}  // namespace pb_bench
