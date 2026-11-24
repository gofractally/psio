// psio::flatbuf against code `flatc` generates from the schema psio EMITS.
//
// The build writes psio::emit_fbs of the reflected types of
// flatbuf_shapes.hpp (fbs_schema_emit.cpp, flatbuf_schema_shapes.hpp) and
// runs `flatc --cpp --gen-object-api` on it. Nothing here was written by
// hand for FlatBuffers: if the emitted schema described another layout than
// the codec's — a field id, a default, a union's discriminants, an optional
// scalar, a struct's packing, an enum's width — these would fail.
//
//   - the constants flatc generates (vtable offsets, struct sizes, union
//     discriminants) are the codec's;
//   - a buffer psio writes passes the generated verifier and reads, field
//     by field, as the value written;
//   - the generated object API unpacks it and packs it back to the same
//     bytes, so what the official builder writes for this schema is what
//     psio writes, and psio reads it back to the value;
//   - defaults: a member at its C++ initialiser is not in the buffer, and
//     the generated accessor returns the initialiser.

#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>

#include <catch.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "emitted_generated.h"
#include "flatbuf_shapes.hpp"

namespace
{
   const std::uint8_t* data(const std::vector<char>& b)
   {
      return reinterpret_cast<const std::uint8_t*>(b.data());
   }

   // An empty string that is not optional is not written: it reads as
   // absent.
   std::string str(const ::flatbuffers::String* s) { return s ? s->str() : std::string{}; }

   // psio writes `value`; the generated code verifies, unpacks and packs it
   // back to the same bytes; psio reads those.
   template <typename Table, typename T>
   void exchange(const T& value, const char* identifier = nullptr)
   {
      const auto bytes = identifier ? psio::to_flatbuf(value, {.file_identifier = identifier})
                                    : psio::to_flatbuf(value);
      ::flatbuffers::Verifier verifier(data(bytes), bytes.size());
      REQUIRE(verifier.VerifyBuffer<Table>(identifier));

      const std::unique_ptr<typename Table::NativeTableType> native(
         ::flatbuffers::GetRoot<Table>(bytes.data())->UnPack());
      ::flatbuffers::FlatBufferBuilder fbb;
      fbb.Finish(Table::Pack(fbb, native.get()), identifier);
      const std::vector<char> official(
         reinterpret_cast<const char*>(fbb.GetBufferPointer()),
         reinterpret_cast<const char*>(fbb.GetBufferPointer()) + fbb.GetSize());
      CHECK(official == bytes);

      const auto st = psio::validate_flatbuf<T>(official);
      REQUIRE(st.ok());
      CHECK(psio::from_flatbuf<T>(official) == value);
      CHECK(psio::flatbuf_view<T>{official}.unpack() == value);
   }
}  // namespace

TEST_CASE("emitted fbs: flatc's constants are the codec's layout", "[flatbuf][schema][interop]")
{
   using psio::flatbuf;
   CHECK(sizeof(fbe::Vec3) == flatbuf::layout<Vec3>::size());
   CHECK(alignof(fbe::Vec3) == flatbuf::layout<Vec3>::align());
   CHECK(sizeof(fbe::Mixed) == flatbuf::layout<Mixed>::size());
   CHECK(alignof(fbe::Mixed) == flatbuf::layout<Mixed>::align());

   using O = flatbuf::layout<Order>;
   CHECK(fbe::Order::VT_ID == O::vtable_offset(0));
   CHECK(fbe::Order::VT_CUSTOMER == O::vtable_offset(1));
   CHECK(fbe::Order::VT_ITEMS == O::vtable_offset(2));
   CHECK(fbe::Order::VT_TOTAL == O::vtable_offset(3));
   CHECK(fbe::Order::VT_NOTE == O::vtable_offset(4));
   CHECK(fbe::Order::VT_PAYMENT_TYPE == O::vtable_offset(5));  // the union's type
   CHECK(fbe::Order::VT_PAYMENT == O::vtable_offset(5) + 2);   // and its value
   CHECK(fbe::Order::VT_POSITION == O::vtable_offset(6));
   CHECK(fbe::Order::VT_ORIGIN == O::vtable_offset(7));
   CHECK(fbe::Order::VT_PATH == O::vtable_offset(8));
   CHECK(fbe::Order::VT_TAGS == O::vtable_offset(9));
   CHECK(fbe::Order::VT_COLOR == O::vtable_offset(10));
   CHECK(fbe::Order::VT_DISCOUNT == O::vtable_offset(11));
   CHECK(fbe::Order::VT_LABELS == O::vtable_offset(12));
   CHECK(fbe::Order::VT_STOCK == O::vtable_offset(13));
   CHECK(fbe::Order::VT_BY_ID == O::vtable_offset(14));
   CHECK(fbe::Order::VT_BLOB == O::vtable_offset(15));
   CHECK(fbe::Order::VT_FLAGS == O::vtable_offset(16));
   CHECK(fbe::Order::VT_QUAD == O::vtable_offset(17));
   CHECK(fbe::Order::VT_COLORS == O::vtable_offset(18));
   CHECK(fbe::Order::VT_IDS == O::vtable_offset(19));
   CHECK(fbe::Order::VT_REFUND_TYPE == O::vtable_offset(20));
   CHECK(fbe::Order::VT_REFUND == O::vtable_offset(20) + 2);
   CHECK(fbe::Order::VT_MIXED == O::vtable_offset(21));

   // attr(member, field<N>): ids that are not the declaration order.
   using R = flatbuf::layout<Renumbered>;
   CHECK(fbe::Renumbered::VT_C == R::vtable_offset(0));
   CHECK(fbe::Renumbered::VT_A == R::vtable_offset(1));
   CHECK(fbe::Renumbered::VT_B == R::vtable_offset(2));
   CHECK(fbe::Renumbered::VT_A == 4);
   CHECK(fbe::Renumbered::VT_C == 8);

   // A deprecated member keeps its slot.
   using D = flatbuf::layout<WithDeprecated>;
   CHECK(fbe::WithDeprecated::VT_A == D::vtable_offset(0));
   CHECK(fbe::WithDeprecated::VT_C == D::vtable_offset(2));

   // std::variant<std::monostate, Card, Wire>: NONE, then the alternatives.
   CHECK(static_cast<int>(fbe::Order_payment::NONE) == 0);
   CHECK(static_cast<int>(fbe::Order_payment::Card) == 1);
   CHECK(static_cast<int>(fbe::Order_payment::Wire) == 2);
   // std::variant<Card, Wire>: alternative i is discriminant i + 1.
   CHECK(static_cast<int>(fbe::Order_refund::Card) == 1);
   CHECK(static_cast<int>(fbe::Order_refund::Wire) == 2);
}

TEST_CASE("emitted fbs: generated code reads what psio writes", "[flatbuf][schema][interop]")
{
   const Order value = sample_order();
   const auto  bytes = psio::to_flatbuf(value, {.file_identifier = "ORDR"});
   ::flatbuffers::Verifier verifier(data(bytes), bytes.size());
   REQUIRE(fbe::VerifyOrderBuffer(verifier));
   REQUIRE(fbe::OrderBufferHasIdentifier(bytes.data()));
   const fbe::Order* o = fbe::GetOrder(bytes.data());

   CHECK(o->id() == value.id);
   CHECK(str(o->customer()->name()) == value.customer.name);
   CHECK(o->customer()->age() == value.customer.age);
   CHECK(o->customer()->verified() == value.customer.verified);
   REQUIRE(o->items()->size() == value.items.size());
   for (std::uint32_t i = 0; i < o->items()->size(); ++i)
   {
      CHECK(str(o->items()->Get(i)->product()) == value.items[i].product);
      CHECK(o->items()->Get(i)->qty() == value.items[i].qty);
      CHECK(o->items()->Get(i)->unit_price() == value.items[i].unit_price);
   }
   CHECK(o->total() == value.total);
   CHECK(str(o->note()) == *value.note);
   REQUIRE(o->payment_type() == fbe::Order_payment::Card);
   CHECK(str(o->payment_as_Card()->number()) == std::get<Card>(value.payment).number);
   CHECK(o->payment_as_Card()->cvv() == std::get<Card>(value.payment).cvv);
   CHECK(o->position()->z() == value.position.z);
   CHECK(o->origin()->x() == value.origin->x);
   REQUIRE(o->path()->size() == value.path.size());
   CHECK(o->path()->Get(1)->y() == value.path[1].y);
   REQUIRE(o->tags()->size() == value.tags.size());
   CHECK(str(o->tags()->Get(2)) == value.tags[2]);
   CHECK(o->color() == static_cast<std::uint8_t>(value.color));
   REQUIRE(o->discount().has_value());
   CHECK(*o->discount() == *value.discount);
   REQUIRE(o->labels()->size() == value.labels.size());
   CHECK(str(o->labels()->Get(0)) == "gift");
   // std::map: entry tables sorted by key, found by the generated lookup.
   REQUIRE(o->stock()->size() == value.stock.size());
   REQUIRE(o->stock()->LookupByKey("washer") != nullptr);
   CHECK(o->stock()->LookupByKey("washer")->value() == 250);
   CHECK(o->stock()->LookupByKey("absent") == nullptr);
   REQUIRE(o->by_id()->LookupByKey(4000000000u) != nullptr);
   CHECK(str(o->by_id()->LookupByKey(4000000000u)->value()->product()) == "big");
   REQUIRE(o->blob()->size() == value.blob.size());
   CHECK(std::memcmp(o->blob()->data(), value.blob.data(), value.blob.size()) == 0);
   REQUIRE(o->flags()->size() == 3);
   CHECK(o->flags()->Get(0) == 1);
   CHECK(o->flags()->Get(1) == 0);
   REQUIRE(o->quad()->size() == 4);
   CHECK(o->quad()->Get(3) == 65535);
   REQUIRE(o->colors()->size() == 2);
   CHECK(o->colors()->Get(1) == static_cast<std::uint8_t>(Color::blue));
   REQUIRE(o->ids()->size() == 3);
   CHECK(o->ids()->Get(2) == 500);
   REQUIRE(o->refund_type() == fbe::Order_refund::Card);
   CHECK(str(o->refund_as_Card()->number()) == "refund");
   CHECK(o->mixed()->a() == value.mixed.a);
   CHECK(o->mixed()->b() == value.mixed.b);
   CHECK(o->mixed()->c() == value.mixed.c);
   CHECK(o->mixed()->d()->Get(2) == value.mixed.d[2]);
   CHECK(o->mixed()->v().y() == value.mixed.v.y);
   CHECK(o->mixed()->e() == value.mixed.e);

   // An absent optional scalar, an absent union.
   Order plain;
   const auto plain_bytes = psio::to_flatbuf(plain);
   const fbe::Order* p    = ::flatbuffers::GetRoot<fbe::Order>(plain_bytes.data());
   CHECK_FALSE(p->discount().has_value());
   CHECK(p->payment_type() == fbe::Order_payment::NONE);
   CHECK(p->note() == nullptr);
   CHECK(p->color() == static_cast<std::uint8_t>(Color::green));  // the initialiser
}

TEST_CASE("emitted fbs: defaults are the member initialisers", "[flatbuf][schema][interop]")
{
   // psio does not write a member that equals its initialiser; the
   // generated accessors return the default of the EMITTED schema.
   const auto            bytes = psio::to_flatbuf(Defaults{});
   const fbe::Defaults*  d     = ::flatbuffers::GetRoot<fbe::Defaults>(bytes.data());
   CHECK(d->level() == 7);
   CHECK(d->on() == true);
   CHECK(d->c() == static_cast<std::uint8_t>(Color::green));
   CHECK(d->ratio() == 0.5);
   const auto* table = reinterpret_cast<const ::flatbuffers::Table*>(d);
   CHECK(table->GetOptionalFieldOffset(fbe::Defaults::VT_LEVEL) == 0);
   CHECK(table->GetOptionalFieldOffset(fbe::Defaults::VT_RATIO) == 0);

   // The official builder leaves them out too; psio reads the initialisers.
   ::flatbuffers::FlatBufferBuilder fbb;
   fbb.Finish(fbe::CreateDefaults(fbb));
   const std::vector<char> official(
      reinterpret_cast<const char*>(fbb.GetBufferPointer()),
      reinterpret_cast<const char*>(fbb.GetBufferPointer()) + fbb.GetSize());
   CHECK(official == bytes);
   CHECK(psio::from_flatbuf<Defaults>(official) == Defaults{});

   // Zero is then a value that is written.
   Defaults zero{0, false, Color::red, 0.0};
   const auto zero_bytes = psio::to_flatbuf(zero);
   const fbe::Defaults* z = ::flatbuffers::GetRoot<fbe::Defaults>(zero_bytes.data());
   CHECK(z->level() == 0);
   CHECK(z->on() == false);
   CHECK(z->c() == 0);
   CHECK(z->ratio() == 0.0);

   const auto item = psio::to_flatbuf(Item{"x", 1, 2.0});
   CHECK(::flatbuffers::GetRoot<fbe::Item>(item.data())->qty() == 1);
   CHECK(reinterpret_cast<const ::flatbuffers::Table*>(
            ::flatbuffers::GetRoot<fbe::Item>(item.data()))
            ->GetOptionalFieldOffset(fbe::Item::VT_QTY) == 0);
}

TEST_CASE("emitted fbs: the official object API writes psio's bytes", "[flatbuf][schema][interop]")
{
   exchange<fbe::Order>(sample_order());
   exchange<fbe::Order>(sample_order(), "ORDR");
   exchange<fbe::Order>(compact_order());
   exchange<fbe::Order>(Order{});
   {
      Order o   = compact_order();
      o.payment = Wire{"DE00"};
      o.refund  = Wire{"FR00"};
      o.discount.reset();
      o.origin.reset();
      o.note.reset();
      exchange<fbe::Order>(o);
   }
   exchange<fbe::Profile>(sample_profile());
   exchange<fbe::Point>(Point{-3, 9});
   exchange<fbe::Tree>(Tree{1, "root", {Tree{2, "a", {}}, Tree{3, "b", {Tree{4, "", {}}}}}});
   exchange<fbe::Scalars>(Scalars{true, -8, 200, -3000, 60000, -70000, 4000000000u,
                                  -9000000000000000000ll, 18000000000000000000ull, 1.5f, -2.25,
                                  Color::blue, 'x'});
   exchange<fbe::Scalars>(Scalars{});
   exchange<fbe::Defaults>(Defaults{});
   exchange<fbe::Defaults>(Defaults{0, false, Color::red, 0.0});
   exchange<fbe::Defaults>(Defaults{-1, true, Color::blue, 1e300});
   exchange<fbe::Renumbered>(Renumbered{-7, "renumbered", 1ll << 40});
   exchange<fbe::WithDeprecated>(WithDeprecated{1, 0, 3});
   exchange<fbe::Sparse>(Sparse{{SparseRow{1, 0, 0, 0, 0, 6}, SparseRow{}, SparseRow{0, 2, 0, 4, 0, 0}}});
   exchange<fbe::Numbers>(Numbers{true, -1, 2, -3, 4, -5, 6, -7, 8, 9.5f, -10.25});
   exchange<fbe::Basket>(Basket{9, sample_profile(), {{"a", 1, 2.5}, {"", 0, 0}}, {"x", "y"},
                                {1, 2, 65535}, {0, 255}, 12.5, true});
   exchange<fbe::Longs>(Longs{{1, 2, ~0ull}, {0.5, -1e-300}});
}
