// psio::flatbuf against the official FlatBuffers library, both directions.
// tests/flatbuf/interop.fbs is compiled by `flatc` (object API, mutable
// accessors); tests/flatbuf_shapes.hpp declares the same types for psio.
//
//   - layout: psio's vtable slots and struct sizes are flatc's;
//   - a buffer psio writes passes the official Verifier and unpacks to the
//     same values through the generated code;
//   - a buffer the official builder writes validates, decodes and views in
//     psio to the same values;
//   - the bytes psio writes are the bytes the official object API
//     (`T::Pack`) writes;
//   - in-place writes and key lookup, each side on the other's buffer;
//   - on corrupted buffers, whatever psio's verifier accepts the official
//     one accepts, and both then read the same values.

#include <psio/fbs_layout.hpp>
#include <psio/fbs_parser.hpp>
#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>

#include <catch.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuf_shapes.hpp"
#include "interop_generated.h"

namespace
{
   // ── psio value → official object ──────────────────────────────────────

   fbi::Vec3 to_native(const Vec3& v) { return fbi::Vec3{v.x, v.y, v.z}; }
   Vec3      from_native(const fbi::Vec3& v) { return Vec3{v.x(), v.y(), v.z()}; }

   fbi::Mixed to_native(const Mixed& m)
   {
      return fbi::Mixed{m.a, m.b, m.c, ::flatbuffers::span<const std::int32_t, 3>(m.d),
                        to_native(m.v), m.e};
   }
   Mixed from_native(const fbi::Mixed& m)
   {
      Mixed out;
      out.a = m.a();
      out.b = m.b();
      out.c = m.c();
      for (std::size_t i = 0; i < 3; ++i)
         out.d[i] = m.d()->Get(static_cast<::flatbuffers::uoffset_t>(i));
      out.v = from_native(m.v());
      out.e = m.e();
      return out;
   }

   std::unique_ptr<fbi::ProfileT> to_native(const Profile& p)
   {
      auto t      = std::make_unique<fbi::ProfileT>();
      t->id       = p.id;
      t->name     = p.name;
      t->email    = p.email;
      t->age      = p.age;
      t->verified = p.verified;
      return t;
   }
   Profile from_native(const fbi::ProfileT& t)
   {
      return Profile{t.id, t.name, t.email, t.age, t.verified};
   }
   std::unique_ptr<fbi::ItemT> to_native(const Item& i)
   {
      auto t        = std::make_unique<fbi::ItemT>();
      t->product    = i.product;
      t->qty        = i.qty;
      t->unit_price = i.unit_price;
      return t;
   }
   Item from_native(const fbi::ItemT& t) { return Item{t.product, t.qty, t.unit_price}; }

   fbi::CardT to_native(const Card& c)
   {
      fbi::CardT t;
      t.number = c.number;
      t.cvv    = c.cvv;
      return t;
   }
   fbi::WireT to_native(const Wire& w)
   {
      fbi::WireT t;
      t.iban = w.iban;
      return t;
   }

   std::unique_ptr<fbi::OrderT> to_native(const Order& o)
   {
      auto t      = std::make_unique<fbi::OrderT>();
      t->id       = o.id;
      t->customer = to_native(o.customer);
      for (const auto& i : o.items)
         t->items.push_back(to_native(i));
      t->total = o.total;
      t->note  = o.note.value_or("");
      if (const auto* c = std::get_if<Card>(&o.payment))
         t->payment.Set(to_native(*c));
      else if (const auto* w = std::get_if<Wire>(&o.payment))
         t->payment.Set(to_native(*w));
      t->position = std::make_unique<fbi::Vec3>(to_native(o.position));
      if (o.origin)
         t->origin = std::make_unique<fbi::Vec3>(to_native(*o.origin));
      for (const auto& p : o.path)
         t->path.push_back(to_native(p));
      t->tags  = o.tags;
      t->color = static_cast<fbi::Color>(o.color);
      if (o.discount)
         t->discount = *o.discount;
      t->labels.assign(o.labels.begin(), o.labels.end());
      for (const auto& [k, v] : o.stock)
      {
         auto e   = std::make_unique<fbi::StockEntryT>();
         e->key   = k;
         e->value = v;
         t->stock.push_back(std::move(e));
      }
      for (const auto& [k, v] : o.by_id)
      {
         auto e   = std::make_unique<fbi::ItemEntryT>();
         e->key   = k;
         e->value = to_native(v);
         t->by_id.push_back(std::move(e));
      }
      t->blob  = o.blob;
      t->flags = o.flags;
      t->quad.assign(o.quad.begin(), o.quad.end());
      for (const auto c : o.colors)
         t->colors.push_back(static_cast<fbi::Color>(c));
      t->ids.assign(o.ids.begin(), o.ids.end());
      if (const auto* c = std::get_if<Card>(&o.refund))
         t->refund.Set(to_native(*c));
      else if (const auto* w = std::get_if<Wire>(&o.refund))
         t->refund.Set(to_native(*w));
      t->mixed = std::make_unique<fbi::Mixed>(to_native(o.mixed));
      return t;
   }

   // What psio decodes from the buffer the official object would produce.
   // `note_present`: FlatBuffers' object API cannot tell an absent string
   // from an empty one; the caller says which the psio value has.
   Order from_native(const fbi::OrderT& t, bool note_present)
   {
      Order o;
      o.id = t.id;
      if (t.customer)
         o.customer = from_native(*t.customer);
      for (const auto& i : t.items)
         o.items.push_back(i ? from_native(*i) : Item{});
      o.total = t.total;
      if (note_present)
         o.note = t.note;
      if (const auto* c = t.payment.AsCard())
         o.payment = Card{c->number, c->cvv};
      else if (const auto* w = t.payment.AsWire())
         o.payment = Wire{w->iban};
      if (t.position)
         o.position = from_native(*t.position);
      if (t.origin)
         o.origin = from_native(*t.origin);
      for (const auto& p : t.path)
         o.path.push_back(from_native(p));
      o.tags  = t.tags;
      o.color = static_cast<Color>(t.color);
      if (t.discount)
         o.discount = *t.discount;
      o.labels.insert(t.labels.begin(), t.labels.end());
      for (const auto& e : t.stock)
         if (e)
            o.stock.emplace(e->key, e->value);
      for (const auto& e : t.by_id)
         if (e)
            o.by_id.emplace(e->key, e->value ? from_native(*e->value) : Item{});
      o.blob  = t.blob;
      o.flags = t.flags;
      for (std::size_t i = 0; i < o.quad.size() && i < t.quad.size(); ++i)
         o.quad[i] = t.quad[i];
      for (const auto c : t.colors)
         o.colors.push_back(static_cast<Color>(c));
      o.ids.insert(t.ids.begin(), t.ids.end());
      if (const auto* c = t.refund.AsCard())
         o.refund = Card{c->number, c->cvv};
      else if (const auto* w = t.refund.AsWire())
         o.refund = Wire{w->iban};
      if (t.mixed)
         o.mixed = from_native(*t.mixed);
      return o;
   }

   std::unique_ptr<fbi::TreeT> to_native(const Tree& t)
   {
      auto n   = std::make_unique<fbi::TreeT>();
      n->value = t.value;
      n->name  = t.name;
      for (const auto& c : t.children)
         n->children.push_back(to_native(c));
      return n;
   }

   std::string str(const ::flatbuffers::String* s) { return s ? s->str() : std::string{}; }

   struct write_opts
   {
      bool identifier    = false;
      bool size_prefixed = false;
   };

   std::vector<char> official_bytes(const Order& o, write_opts w = {})
   {
      ::flatbuffers::FlatBufferBuilder fbb;
      const auto                       native = to_native(o);
      const auto                       root   = fbi::Order::Pack(fbb, native.get());
      const char*                      id     = w.identifier ? fbi::OrderIdentifier() : nullptr;
      if (w.size_prefixed)
         fbb.FinishSizePrefixed(root, id);
      else
         fbb.Finish(root, id);
      const auto* p = reinterpret_cast<const char*>(fbb.GetBufferPointer());
      return {p, p + fbb.GetSize()};
   }

   bool official_verify(const std::vector<char>& b)
   {
      ::flatbuffers::Verifier v(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
      return v.VerifyBuffer<fbi::Order>(nullptr);
   }

   std::vector<Order> order_values()
   {
      std::vector<Order> out;
      out.push_back(Order{});
      out.push_back(sample_order());
      {
         Order o   = sample_order();
         o.payment = Wire{"DE00 1234 5678"};
         o.refund  = Wire{"FR11"};
         o.origin.reset();
         o.discount.reset();
         o.note.reset();
         o.tags.clear();
         o.blob.assign(3000, 0x42);  // past the builder's inline storage
         out.push_back(o);
      }
      {
         Order o   = sample_order();
         o.payment = std::monostate{};
         o.discount = 0;
         o.stock    = {{"", 1}, {"a", 0}};  // an empty string key
         o.by_id.clear();
         for (std::uint32_t i = 0; i < 40; ++i)
            o.by_id[i * 7] = Item{i % 2 ? "odd" : "", i % 3, i % 5 ? 0.0 : 2.5};
         for (int i = 0; i < 25; ++i)
            o.items.push_back(Item{std::string(static_cast<std::size_t>(i), 'p'),
                                   static_cast<std::uint32_t>(i), i * 0.5});
         out.push_back(o);
      }
      return out;
   }
}  // namespace

TEST_CASE("flatbuf interop: layout is flatc's", "[flatbuf][interop]")
{
   using L = psio::flatbuf::layout<Order>;
   static_assert(L::vtable_offset(0) == fbi::Order::VT_ID);
   static_assert(L::vtable_offset(1) == fbi::Order::VT_CUSTOMER);
   static_assert(L::vtable_offset(2) == fbi::Order::VT_ITEMS);
   static_assert(L::vtable_offset(3) == fbi::Order::VT_TOTAL);
   static_assert(L::vtable_offset(4) == fbi::Order::VT_NOTE);
   static_assert(L::vtable_offset(5) == fbi::Order::VT_PAYMENT_TYPE);
   static_assert(L::vtable_offset(5) + 2 == fbi::Order::VT_PAYMENT);
   static_assert(L::vtable_offset(6) == fbi::Order::VT_POSITION);
   static_assert(L::vtable_offset(7) == fbi::Order::VT_ORIGIN);
   static_assert(L::vtable_offset(8) == fbi::Order::VT_PATH);
   static_assert(L::vtable_offset(9) == fbi::Order::VT_TAGS);
   static_assert(L::vtable_offset(10) == fbi::Order::VT_COLOR);
   static_assert(L::vtable_offset(11) == fbi::Order::VT_DISCOUNT);
   static_assert(L::vtable_offset(12) == fbi::Order::VT_LABELS);
   static_assert(L::vtable_offset(13) == fbi::Order::VT_STOCK);
   static_assert(L::vtable_offset(14) == fbi::Order::VT_BY_ID);
   static_assert(L::vtable_offset(15) == fbi::Order::VT_BLOB);
   static_assert(L::vtable_offset(16) == fbi::Order::VT_FLAGS);
   static_assert(L::vtable_offset(17) == fbi::Order::VT_QUAD);
   static_assert(L::vtable_offset(18) == fbi::Order::VT_COLORS);
   static_assert(L::vtable_offset(19) == fbi::Order::VT_IDS);
   static_assert(L::vtable_offset(20) == fbi::Order::VT_REFUND_TYPE);
   static_assert(L::vtable_offset(21) == fbi::Order::VT_MIXED);

   // `(id: N)` is field<N + 1> here; `deprecated` keeps the slot.
   using R = psio::flatbuf::layout<Renumbered>;
   static_assert(R::vtable_offset(0) == fbi::Renumbered::VT_C);
   static_assert(R::vtable_offset(1) == fbi::Renumbered::VT_A);
   static_assert(R::vtable_offset(2) == fbi::Renumbered::VT_B);
   using D = psio::flatbuf::layout<WithDeprecated>;
   static_assert(D::vtable_offset(0) == fbi::WithDeprecated::VT_A);
   static_assert(D::vtable_offset(2) == fbi::WithDeprecated::VT_C);

   static_assert(psio::flatbuf::layout<Vec3>::size() == sizeof(fbi::Vec3));
   static_assert(psio::flatbuf::layout<Vec3>::align() == alignof(fbi::Vec3));
   static_assert(psio::flatbuf::layout<Mixed>::size() == sizeof(fbi::Mixed));
   static_assert(psio::flatbuf::layout<Mixed>::align() == alignof(fbi::Mixed));
   SUCCEED();
}

TEST_CASE("flatbuf interop: the layout psio computes from the .fbs text is flatc's",
          "[flatbuf][interop][schema]")
{
   std::ifstream     in(PSIO_FLATBUF_INTEROP_FBS);
   std::stringstream text;
   text << in.rdbuf();
   REQUIRE(in);
   const auto layouts = psio::fbs_layout(psio::parse_fbs(text.str()));
   const auto vt = [&](const char* table, const char* field) {
      const auto* t = layouts.find(table);
      REQUIRE(t != nullptr);
      const auto* f = t->field(field);
      REQUIRE(f != nullptr);
      return static_cast<int>(f->vtable_offset);
   };
   CHECK(vt("order", "id") == fbi::Order::VT_ID);
   CHECK(vt("order", "note") == fbi::Order::VT_NOTE);
   CHECK(vt("order", "payment") == fbi::Order::VT_PAYMENT_TYPE);
   CHECK(vt("order", "payment") + 2 == fbi::Order::VT_PAYMENT);
   CHECK(vt("order", "position") == fbi::Order::VT_POSITION);
   CHECK(vt("order", "discount") == fbi::Order::VT_DISCOUNT);
   CHECK(vt("order", "by_id") == fbi::Order::VT_BY_ID);
   CHECK(vt("order", "refund") == fbi::Order::VT_REFUND_TYPE);
   CHECK(vt("order", "refund") + 2 == fbi::Order::VT_REFUND);
   CHECK(vt("order", "mixed") == fbi::Order::VT_MIXED);
   CHECK(vt("profile", "verified") == fbi::Profile::VT_VERIFIED);
   CHECK(vt("item", "unit_price") == fbi::Item::VT_UNIT_PRICE);
   CHECK(vt("stock_entry", "key") == fbi::StockEntry::VT_KEY);
   CHECK(vt("item_entry", "value") == fbi::ItemEntry::VT_VALUE);
   CHECK(vt("tree", "children") == fbi::Tree::VT_CHILDREN);
   CHECK(vt("scalars", "ch") == fbi::Scalars::VT_CH);
   CHECK(vt("defaults", "ratio") == fbi::Defaults::VT_RATIO);
   CHECK(vt("renumbered", "c") == fbi::Renumbered::VT_C);
   CHECK(vt("renumbered", "a") == fbi::Renumbered::VT_A);
   CHECK(vt("renumbered", "b") == fbi::Renumbered::VT_B);
   CHECK(vt("with_deprecated", "c") == fbi::WithDeprecated::VT_C);
   REQUIRE(layouts.find("mixed") != nullptr);
   CHECK(layouts.find("mixed")->size == sizeof(fbi::Mixed));
   CHECK(layouts.find("mixed")->align == alignof(fbi::Mixed));
   CHECK(layouts.find("vec3")->size == sizeof(fbi::Vec3));
}

TEST_CASE("flatbuf interop: psio writes, the official library reads", "[flatbuf][interop]")
{
   for (const Order& o : order_values())
   {
      const auto bytes = psio::to_flatbuf(o);
      REQUIRE(official_verify(bytes));
      const auto* root = ::flatbuffers::GetRoot<fbi::Order>(bytes.data());
      std::unique_ptr<fbi::OrderT> native(root->UnPack());
      CHECK(from_native(*native, o.note.has_value()) == o);

      // Accessors, not only the object API.
      CHECK(root->id() == o.id);
      REQUIRE(root->customer() != nullptr);
      CHECK(str(root->customer()->name()) == o.customer.name);
      CHECK(root->mixed()->b() == o.mixed.b);
      CHECK(root->position()->z() == o.position.z);
      CHECK(root->color() == static_cast<fbi::Color>(o.color));
      CHECK(root->discount().has_value() == o.discount.has_value());
      if (const auto* c = std::get_if<Card>(&o.payment))
      {
         REQUIRE(root->payment_type() == fbi::Payment::Card);
         CHECK(str(root->payment_as_Card()->number()) == c->number);
      }
      if (std::holds_alternative<std::monostate>(o.payment))
         CHECK(root->payment_type() == fbi::Payment::NONE);

      // Key lookup by the official library on psio's sorted vectors.
      for (const auto& [k, v] : o.stock)
      {
         const auto* e = root->stock()->LookupByKey(k.c_str());
         REQUIRE(e != nullptr);
         CHECK(e->value() == v);
      }
      if (root->stock())
         CHECK(root->stock()->LookupByKey("no such key") == nullptr);
      for (const auto& [k, v] : o.by_id)
      {
         const auto* e = root->by_id()->LookupByKey(k);
         REQUIRE(e != nullptr);
         CHECK(str(e->value()->product()) == v.product);
      }
   }

   // File identifier, size prefix.
   const Order o = sample_order();
   {
      const auto b = psio::to_flatbuf(o, psio::flatbuf::options{.file_identifier = "ORDR"});
      ::flatbuffers::Verifier v(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
      CHECK(fbi::VerifyOrderBuffer(v));
      CHECK(fbi::OrderBufferHasIdentifier(b.data()));
   }
   {
      const auto b = psio::to_flatbuf(
         o, psio::flatbuf::options{.file_identifier = "ORDR", .size_prefixed = true});
      ::flatbuffers::Verifier v(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
      CHECK(fbi::VerifySizePrefixedOrderBuffer(v));
      std::unique_ptr<fbi::OrderT> native(fbi::GetSizePrefixedOrder(b.data())->UnPack());
      CHECK(from_native(*native, true) == o);
   }
}

TEST_CASE("flatbuf interop: the official library writes, psio reads", "[flatbuf][interop]")
{
   for (Order o : order_values())
   {
      // The object API does not write an empty string.
      if (o.note && o.note->empty())
         o.note.reset();
      const auto bytes = official_bytes(o);
      const auto st    = psio::validate_flatbuf<Order>(bytes);
      if (!st.ok())
         FAIL("validate: " << st.error().what << " at " << st.error().byte_offset);
      CHECK(psio::from_flatbuf<Order>(bytes) == o);

      const auto view = psio::validated_view<Order>(psio::flatbuf{}, bytes);
      REQUIRE(view.ok());
      CHECK(view->id() == o.id);
      CHECK(view->customer().name() == o.customer.name);
      CHECK(view->items().size() == o.items.size());
      CHECK(view->mixed().unpack() == o.mixed);
      CHECK(view->payment().unpack() == o.payment);
      CHECK(view->unpack() == o);
      for (const auto& [k, v] : o.stock)
         CHECK(view->stock().at(k) == v);
      for (const auto& [k, v] : o.by_id)
         CHECK(view->by_id().at(k)->product() == v.product);
      CHECK(psio::flatbuf_view<Order>{bytes}.unpack() == o);

      const auto with_id = official_bytes(o, {.identifier = true});
      CHECK(psio::validate_flatbuf<Order>(with_id,
                                          psio::flatbuf::options{.file_identifier = "ORDR"})
               .ok());
      const auto prefixed = official_bytes(o, {.identifier = true, .size_prefixed = true});
      CHECK(psio::validate_flatbuf<Order>(
               prefixed, psio::flatbuf::options{.file_identifier = "ORDR", .size_prefixed = true})
               .ok());
      CHECK(psio::flatbuf_view<Order>{psio::flatbuf_size_prefixed(prefixed)}.unpack() == o);
   }
}

TEST_CASE("flatbuf interop: psio's bytes are the official object API's", "[flatbuf][interop]")
{
   for (Order o : order_values())
   {
      if (o.note && o.note->empty())
         o.note.reset();
      CHECK(psio::to_flatbuf(o) == official_bytes(o));
      CHECK(psio::to_flatbuf(o, psio::flatbuf::options{.file_identifier = "ORDR"}) ==
            official_bytes(o, {.identifier = true}));
      CHECK(psio::to_flatbuf(o, psio::flatbuf::options{.size_prefixed = true}) ==
            official_bytes(o, {.size_prefixed = true}));
      CHECK(psio::to_flatbuf(
               o, psio::flatbuf::options{.file_identifier = "ORDR", .size_prefixed = true}) ==
            official_bytes(o, {.identifier = true, .size_prefixed = true}));
   }

   // A recursive table, shared vtables.
   Tree t;
   t.value = 1;
   t.name  = "root";
   t.children.resize(30);
   for (std::size_t i = 0; i < t.children.size(); ++i)
   {
      t.children[i].value = static_cast<std::int32_t>(i % 4);
      if (i % 3 == 0)
         t.children[i].name = "n" + std::to_string(i);
      if (i % 5 == 0)
         t.children[i].children.resize(2);
   }
   ::flatbuffers::FlatBufferBuilder fbb;
   const auto                       native = to_native(t);
   fbb.Finish(fbi::Tree::Pack(fbb, native.get()));
   const auto* p = reinterpret_cast<const char*>(fbb.GetBufferPointer());
   CHECK(psio::to_flatbuf(t) == std::vector<char>(p, p + fbb.GetSize()));

   // Every scalar type, defaults that are not zero, explicit ids, a
   // deprecated field.
   {
      Scalars s{true, -1, 2, -3, 4, -5, 6, -7, 8, 1.5f, -2.25, Color::blue, 'x'};
      fbi::ScalarsT n;
      n.b = s.b, n.i8 = s.i8, n.u8 = s.u8, n.i16 = s.i16, n.u16 = s.u16, n.i32 = s.i32;
      n.u32 = s.u32, n.i64 = s.i64, n.u64 = s.u64, n.f = s.f, n.d = s.d;
      n.c  = static_cast<fbi::Color>(s.c);
      n.ch = s.ch;
      ::flatbuffers::FlatBufferBuilder b;
      b.Finish(fbi::Scalars::Pack(b, &n));
      const auto* q = reinterpret_cast<const char*>(b.GetBufferPointer());
      CHECK(psio::to_flatbuf(s) == std::vector<char>(q, q + b.GetSize()));
   }
   for (const Defaults& d : {Defaults{}, Defaults{0, false, Color::red, 0.0}, Defaults{7, true, Color::blue, 0.5}})
   {
      fbi::DefaultsT n;
      n.level = d.level, n.on = d.on, n.c = static_cast<fbi::Color>(d.c), n.ratio = d.ratio;
      ::flatbuffers::FlatBufferBuilder b;
      b.Finish(fbi::Defaults::Pack(b, &n));
      const auto* q     = reinterpret_cast<const char*>(b.GetBufferPointer());
      const auto  bytes = psio::to_flatbuf(d);
      CHECK(bytes == std::vector<char>(q, q + b.GetSize()));
      const auto* root = ::flatbuffers::GetRoot<fbi::Defaults>(bytes.data());
      CHECK(root->level() == d.level);
      CHECK(root->on() == d.on);
      CHECK(root->ratio() == d.ratio);
   }
   {
      const Renumbered r{5, "five", -5};
      fbi::RenumberedT n;
      n.c = r.c, n.a = r.a, n.b = r.b;
      ::flatbuffers::FlatBufferBuilder b;
      b.Finish(fbi::Renumbered::Pack(b, &n));
      const auto* q = reinterpret_cast<const char*>(b.GetBufferPointer());
      CHECK(psio::to_flatbuf(r) == std::vector<char>(q, q + b.GetSize()));
      CHECK(psio::from_flatbuf<Renumbered>(std::span<const char>(q, b.GetSize())) == r);
   }
   {
      const WithDeprecated w{1, 0, 3};
      fbi::WithDeprecatedT n;
      n.a = 1, n.c = 3;
      ::flatbuffers::FlatBufferBuilder b;
      b.Finish(fbi::WithDeprecated::Pack(b, &n));
      const auto* q = reinterpret_cast<const char*>(b.GetBufferPointer());
      CHECK(psio::to_flatbuf(w) == std::vector<char>(q, q + b.GetSize()));
   }
}

TEST_CASE("flatbuf interop: in-place writes", "[flatbuf][interop]")
{
   Order o = sample_order();

   // psio writes into the official builder's buffer; the official reader sees it.
   auto bytes = official_bytes(o);
   psio::flatbuf_mutable_view<Order> m{bytes};
   REQUIRE(m.id().set(5));
   REQUIRE(m.customer().age().set(99));
   REQUIRE(m.mixed().d()[2].set(1234));
   REQUIRE(m.path()[2].x().set(-8.5f));
   REQUIRE(m.items()[0].unit_price().set(0.25));
   REQUIRE(official_verify(bytes));
   const auto* root = ::flatbuffers::GetRoot<fbi::Order>(bytes.data());
   CHECK(root->id() == 5);
   CHECK(root->customer()->age() == 99);
   CHECK(root->mixed()->d()->Get(2) == 1234);
   CHECK(root->path()->Get(2)->x() == -8.5f);
   CHECK(root->items()->Get(0)->unit_price() == 0.25);

   // The official mutators write into psio's buffer; psio reads it.
   auto  mine  = psio::to_flatbuf(o);
   auto* mroot = ::flatbuffers::GetMutableRoot<fbi::Order>(mine.data());
   REQUIRE(mroot->mutate_total(3.5));
   REQUIRE(mroot->mutable_customer()->mutate_verified(false));
   mroot->mutable_position()->mutate_y(77.0f);
   // Both sides refuse a scalar that is not stored (qty == its default).
   CHECK_FALSE(mroot->mutable_items()->GetMutableObject(1)->mutate_qty(9));
   CHECK_FALSE(psio::flatbuf_mutable_view<Order>{mine}.items()[1].qty().set(9));
   REQUIRE(psio::validate_flatbuf<Order>(mine).ok());
   const auto back = psio::from_flatbuf<Order>(mine);
   CHECK(back.total == 3.5);
   CHECK(back.customer.verified == false);
   CHECK(back.position.y == 77.0f);
}

TEST_CASE("flatbuf interop: corrupted buffers, both verifiers", "[flatbuf][interop][fuzz]")
{
   // psio's verifier is the stricter one (field offsets against the table
   // size, sorted keys, known union types). So: whatever psio accepts, the
   // official verifier accepts, and both then read the same values.
   const auto seeds = order_values();
   std::mt19937_64 rng(0x0f1a7b0f);
   std::size_t     both = 0, official_only = 0, neither = 0;
   std::map<std::string, std::size_t> stricter;  // why psio refused what the official one took
   for (int iter = 0; iter < 300000; ++iter)
   {
      const Order& o = seeds[static_cast<std::size_t>(iter) % seeds.size()];
      auto         b = iter % 2 ? psio::to_flatbuf(o) : official_bytes(o);
      const int    edits = 1 + static_cast<int>(rng() % 3);
      std::string  what;
      for (int e = 0; e < edits; ++e)
      {
         const std::size_t at  = rng() % b.size();
         const char        was = b[at];
         switch (rng() % 4)
         {
            case 0: b[at] = static_cast<char>(rng()); break;
            case 1: b[at] = static_cast<char>(b[at] + 1); break;
            case 2: b[at] = static_cast<char>(b[at] ^ (1u << (rng() % 8))); break;
            default: b[at] = 0; break;
         }
         what += " [" + std::to_string(at) + "]: " + std::to_string(static_cast<unsigned char>(was)) +
                 " -> " + std::to_string(static_cast<unsigned char>(b[at]));
      }
      const auto status = psio::validate_flatbuf<Order>(b);
      const bool mine   = status.ok();
      const bool theirs = official_verify(b);
      if (mine)
      {
         INFO("iteration " << iter << " (" << (iter % 2 ? "psio" : "official") << " bytes of value "
                           << static_cast<std::size_t>(iter) % seeds.size() << ", " << b.size()
                           << " bytes):" << what);
         REQUIRE(theirs);
         ++both;
         const Order                  decoded = psio::from_flatbuf<Order>(psio::trusted{b});
         std::unique_ptr<fbi::OrderT> native(::flatbuffers::GetRoot<fbi::Order>(b.data())->UnPack());
         // Compared as encodings: a corrupted float may be a NaN.
         const auto encoded = psio::to_flatbuf(decoded);
         REQUIRE(psio::to_flatbuf(from_native(*native, decoded.note.has_value())) == encoded);
         REQUIRE(psio::to_flatbuf(
                    psio::flatbuf_trusted_view<Order>{psio::trusted{b}}.unpack()) == encoded);
         REQUIRE(psio::to_flatbuf(psio::flatbuf_view<Order>{b}.unpack()) == encoded);
      }
      else if (theirs)
      {
         ++official_only;
         ++stricter[std::string(status.error().what)];
      }
      else
         ++neither;
   }
   std::string reasons;
   for (const auto& [why, n] : stricter)
      reasons += "\n  " + std::to_string(n) + "  " + why;
   WARN("accepted by both: " << both << ", by neither: " << neither
                             << ", by the official verifier only: " << official_only << reasons);
   CHECK(both > 1000);
}

TEST_CASE("flatbuf interop: strings the official builder shares are within the payload budget",
          "[flatbuf][interop][budget]")
{
   // FlatBufferBuilder::CreateSharedString writes a string once and hands
   // out the same offset for every equal string after it. psio's verifier
   // counts such a string each time it is reached (a decode copies it each
   // time): the default budget (1 MiB and 64 times the buffer) has room for
   // what a builder shares.
   auto shared_names = [](std::size_t children, const std::string& name, bool shared) {
      ::flatbuffers::FlatBufferBuilder                  fbb;
      std::vector<::flatbuffers::Offset<fbi::Tree>> kids;
      for (std::size_t i = 0; i < children; ++i)
      {
         const auto text = shared ? fbb.CreateSharedString(name) : fbb.CreateString(name);
         kids.push_back(fbi::CreateTree(fbb, static_cast<std::int32_t>(i), text));
      }
      const auto vec = fbb.CreateVector(kids);
      fbb.Finish(fbi::CreateTree(fbb, 7, fbb.CreateSharedString(name), vec));
      const auto* p = reinterpret_cast<const char*>(fbb.GetBufferPointer());
      return std::vector<char>(p, p + fbb.GetSize());
   };
   // 20,000 rows that share one 300-byte string (a message template, a
   // host name, a URL): 6 MB of strings from 0.4 MB.
   const std::string name(300, 'n');
   const auto        bytes = shared_names(20000, name, true);
   CHECK(bytes.size() < shared_names(200, name, false).size() * 20);
   ::flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                    bytes.size());
   REQUIRE(verifier.VerifyBuffer<fbi::Tree>(nullptr));
   REQUIRE(psio::validate_flatbuf<Tree>(bytes).ok());
   const Tree tree = psio::from_flatbuf<Tree>(bytes);
   REQUIRE(tree.children.size() == 20000);
   CHECK(tree.children[19999].name == name);
   CHECK(tree.name == name);
   // The expansion of this buffer: (4 + 300) bytes per reference against
   // about 20 bytes of table and offset each.
   const double expansion = 20001.0 * 304 / static_cast<double>(bytes.size());
   WARN("official CreateSharedString: " << bytes.size() << " bytes reach " << 20001 * 304
                                        << " bytes of strings (" << expansion << "x)");
   CHECK(expansion < 32);
   // Under a budget smaller than the sharing it is refused, for that reason.
   const auto tight = psio::validate_flatbuf<Tree>(bytes, psio::flatbuf::limits{.max_expansion = 4});
   REQUIRE_FALSE(tight.ok());
   CHECK(tight.error().what == "flatbuf: too many bytes referenced");
}
