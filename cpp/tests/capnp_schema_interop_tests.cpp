// psio::capnp against the official library through the schema psio EMITS.
//
// The build writes psio::emit_capnp of the reflected types of
// capnp_shapes.hpp (capnp_schema_emit.cpp) and runs `capnp compile -oc++` on
// it. Nothing here was written by hand for Cap'n Proto: values are read and
// written through the compiled schema member by member (libcapnp's dynamic
// API over the generated types), so if the emitted schema described another
// layout than the codec's — an ordinal, a default, a union's discriminant,
// the place of an optional scalar — these would fail.
//
//   - a message psio writes is read by libcapnp, member by member, as the
//     value written;
//   - a message libcapnp writes from the same value validates and decodes
//     in psio to it;
//   - defaults: a member at its C++ initialiser is zero on the wire and
//     libcapnp reads the initialiser; an empty message decodes to the
//     default-constructed value;
//   - names: the generated JSON codec writes the C++ member names
//     (`$Json.name`).

#include <psio/capnp.hpp>

#include "emitted.capnp.h"
#include <capnp/compat/json.h>
#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>

#include <catch.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "capnp_shapes.hpp"

namespace
{
   namespace ci = psio::detail::capnp_impl;

   kj::Array<capnp::word> words_of(std::span<const char> bytes)
   {
      auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
      std::memcpy(words.begin(), bytes.data(), bytes.size());
      return words;
   }
   std::vector<char> bytes_of(capnp::MessageBuilder& mb)
   {
      auto flat = capnp::messageToFlatArray(mb);
      auto b    = flat.asBytes();
      return {reinterpret_cast<const char*>(b.begin()), reinterpret_cast<const char*>(b.end())};
   }

   // The fields of a struct or group in declaration order (the schema
   // lists them by ordinal).
   std::vector<capnp::StructSchema::Field> declared(capnp::StructSchema schema)
   {
      std::vector<capnp::StructSchema::Field> out;
      for (const auto f : schema.getFields())
         out.push_back(f);
      std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
         return a.getProto().getCodeOrder() < b.getProto().getCodeOrder();
      });
      return out;
   }
   // The member of a union with discriminant `value`.
   capnp::StructSchema::Field union_member(capnp::StructSchema schema, std::size_t value)
   {
      for (const auto f : schema.getUnionFields())
         if (f.getProto().getDiscriminantValue() == value)
            return f;
      FAIL("no union member " << value);
      throw 0;
   }

   // ── What libcapnp reads is the psio value ──────────────────────────────

   template <ci::Record T>
   void same(const T& value, capnp::DynamicStruct::Reader r);

   inline void same(bool value, capnp::DynamicValue::Reader r) { CHECK(r.as<bool>() == value); }
   template <typename S>
      requires (std::is_arithmetic_v<S> && !std::is_same_v<S, bool>)
   void same(S value, capnp::DynamicValue::Reader r)
   {
      CHECK(r.as<S>() == value);
   }
   template <typename E>
      requires std::is_enum_v<E>
   void same(E value, capnp::DynamicValue::Reader r)
   {
      // A C++ enum is its integer in the emitted schema.
      CHECK(r.as<std::underlying_type_t<E>>() == static_cast<std::underlying_type_t<E>>(value));
   }
   inline void same(const std::string& value, capnp::DynamicValue::Reader r)
   {
      const auto text = r.as<capnp::Text>();
      CHECK(std::string(text.begin(), text.size()) == value);
   }
   inline void same(const std::vector<std::uint8_t>& value, capnp::DynamicValue::Reader r)
   {
      const auto data = r.as<capnp::Data>();
      REQUIRE(data.size() == value.size());
      CHECK(std::memcmp(data.begin(), value.data(), value.size()) == 0);
   }
   template <ci::Record T>
   void same(const T& value, capnp::DynamicValue::Reader r)
   {
      same(value, r.as<capnp::DynamicStruct>());
   }
   template <typename C>
      requires (ci::is_vector_v<C> || ci::is_array_v<C>)
   void same(const C& value, capnp::DynamicValue::Reader r)
   {
      const auto list = r.as<capnp::DynamicList>();
      REQUIRE(list.size() == value.size());
      for (unsigned i = 0; i < list.size(); ++i)
      {
         const typename C::value_type element = value[i];  // (std::vector<bool>)
         same(element, list[i]);
      }
   }

   template <typename M>
   void same_member(const M& member, capnp::DynamicStruct::Reader r, capnp::StructSchema::Field f)
   {
      INFO(f.getProto().getName().cStr());
      if constexpr (ci::is_variant_v<M>)
      {
         const auto group  = r.get(f).as<capnp::DynamicStruct>();
         const auto active = KJ_ASSERT_NONNULL(group.which());
         REQUIRE(active.getProto().getDiscriminantValue() == member.index());
         std::visit(
            [&](const auto& alternative) {
               if constexpr (!std::is_same_v<std::remove_cvref_t<decltype(alternative)>, std::monostate>)
                  same(alternative, group.get(active));
            },
            member);
      }
      else if constexpr (ci::is_optional_v<M>)
      {
         if constexpr (ci::Scalar<typename M::value_type>)
         {
            // union { none :Void; some :T; }
            const auto group  = r.get(f).as<capnp::DynamicStruct>();
            const auto active = KJ_ASSERT_NONNULL(group.which());
            REQUIRE(active.getProto().getDiscriminantValue() == (member ? 1 : 0));
            if (member)
               same(*member, group.get(active));
         }
         else
         {
            // The pointer; null is "no value".
            REQUIRE(r.has(f, capnp::HasMode::NON_NULL) == member.has_value());
            if (member)
               same(*member, r.get(f));
         }
      }
      else
         same(member, r.get(f));
   }

   template <ci::Record T>
   void same(const T& value, capnp::DynamicStruct::Reader r)
   {
      using R           = psio::reflect<T>;
      const auto fields = declared(r.getSchema());
      REQUIRE(fields.size() == R::member_count);
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (same_member(value.*(R::template member_pointer<Is>), r, fields[Is]), ...);
      }(std::make_index_sequence<R::member_count>{});
   }

   // ── libcapnp writes the psio value ─────────────────────────────────────

   template <ci::Record T>
   void fill(const T& value, capnp::DynamicStruct::Builder b);
   template <typename C>
   void fill_list(const C& value, capnp::DynamicList::Builder list);

   template <typename X>
   void put(capnp::DynamicStruct::Builder b, capnp::StructSchema::Field f, const X& value)
   {
      if constexpr (std::is_same_v<X, std::monostate>)
         b.set(f, capnp::VOID);
      else if constexpr (std::is_enum_v<X>)
         b.set(f, static_cast<std::underlying_type_t<X>>(value));
      else if constexpr (std::is_arithmetic_v<X>)
         b.set(f, value);
      else if constexpr (std::is_same_v<X, std::string>)
         b.set(f, capnp::Text::Reader(value.c_str(), value.size()));
      else if constexpr (std::is_same_v<X, std::vector<std::uint8_t>>)
         b.set(f, capnp::Data::Reader(reinterpret_cast<const kj::byte*>(value.data()), value.size()));
      else if constexpr (ci::is_vector_v<X> || ci::is_array_v<X>)
         fill_list(value, b.init(f, static_cast<unsigned>(value.size())).template as<capnp::DynamicList>());
      else
         fill(value, b.init(f).template as<capnp::DynamicStruct>());
   }

   template <typename C>
   void fill_list(const C& value, capnp::DynamicList::Builder list)
   {
      using E = typename C::value_type;
      for (unsigned i = 0; i < list.size(); ++i)
      {
         const E element = value[i];
         if constexpr (std::is_enum_v<E>)
            list.set(i, static_cast<std::underlying_type_t<E>>(element));
         else if constexpr (std::is_arithmetic_v<E>)
            list.set(i, element);
         else if constexpr (std::is_same_v<E, std::string>)
            list.set(i, capnp::Text::Reader(element.c_str(), element.size()));
         else if constexpr (ci::is_vector_v<E> || ci::is_array_v<E>)
            fill_list(element, list.init(i, static_cast<unsigned>(element.size()))
                                  .template as<capnp::DynamicList>());
         else
            fill(element, list[i].template as<capnp::DynamicStruct>());
      }
   }

   template <typename M>
   void put_member(capnp::DynamicStruct::Builder b, capnp::StructSchema::Field f, const M& member)
   {
      if constexpr (ci::is_variant_v<M>)
      {
         auto group = b.get(f).as<capnp::DynamicStruct>();
         std::visit([&](const auto& alternative) {
            put(group, union_member(group.getSchema(), member.index()), alternative);
         }, member);
      }
      else if constexpr (ci::is_optional_v<M>)
      {
         if constexpr (ci::Scalar<typename M::value_type>)
         {
            auto group = b.get(f).as<capnp::DynamicStruct>();
            if (member)
               put(group, union_member(group.getSchema(), 1), *member);
            else
               put(group, union_member(group.getSchema(), 0), std::monostate{});
         }
         else if (member)
            put(b, f, *member);
      }
      else
         put(b, f, member);
   }

   template <ci::Record T>
   void fill(const T& value, capnp::DynamicStruct::Builder b)
   {
      using R           = psio::reflect<T>;
      const auto fields = declared(b.getSchema());
      REQUIRE(fields.size() == R::member_count);
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (put_member(b, fields[Is], value.*(R::template member_pointer<Is>)), ...);
      }(std::make_index_sequence<R::member_count>{});
   }

   // Both directions for one value; `Generated` is the compiled struct.
   template <typename Generated, typename T>
   void exchange(const T& value)
   {
      {
         const auto bytes = psio::to_capnp(value);
         auto       words = words_of(bytes);
         capnp::FlatArrayMessageReader reader(words.asPtr());
         same(value, capnp::DynamicStruct::Reader(capnp::toDynamic(reader.getRoot<Generated>())));
      }
      {
         capnp::MallocMessageBuilder mb;
         fill(value, capnp::toDynamic(mb.initRoot<Generated>()));
         const auto bytes = bytes_of(mb);
         const auto st    = psio::validate_capnp<T>(bytes);
         if (!st.ok())
            FAIL("validate: " << st.error().what);
         CHECK(psio::from_capnp<T>(bytes) == value);
      }
   }
}  // namespace

TEST_CASE("emitted capnp: messages both ways through the compiled schema", "[capnp][schema][interop]")
{
   exchange<cpe::CpOrder>(cp_sample_order());
   exchange<cpe::CpOrder>(CpOrder{});
   exchange<cpe::CpMixed>(cp_sample_mixed());
   exchange<cpe::CpMixed>(CpMixed{});
   {
      CpMixed m = cp_sample_mixed();
      m.payment = std::uint32_t{4000000000u};
      m.score.reset();
      m.memo = "";  // engaged and empty: a pointer that is not null
      m.extra.reset();
      m.maybe = true;
      exchange<cpe::CpMixed>(m);
      m.payment = std::string{"text"};
      exchange<cpe::CpMixed>(m);
      m.payment = -1e300;
      m.maybe.reset();
      exchange<cpe::CpMixed>(m);
      m.payment = std::monostate{};
      exchange<cpe::CpMixed>(m);
   }
   exchange<cpe::CpRenumbered>(CpRenumbered{-7, "renumbered", std::int8_t{-3}, 1ll << 40});
   exchange<cpe::CpRenumbered>(CpRenumbered{1, "", std::string{"alt"}, -1});
   exchange<cpe::CpDefaults>(CpDefaults{});
   exchange<cpe::CpDefaults>(CpDefaults{0, 0, 0, 0, false, CpGrade::low, 0, 0});
   exchange<cpe::CpDefaults>(CpDefaults{127, 1, 1e300, -1.5f, true, CpGrade::high, 1, -1});
}

TEST_CASE("emitted capnp: generated accessors, ordinals and defaults", "[capnp][schema][interop]")
{
   // The generated accessors, under Cap'n Proto's names.
   {
      const auto bytes = psio::to_capnp(cp_sample_order());
      auto       words = words_of(bytes);
      capnp::FlatArrayMessageReader reader(words.asPtr());
      const auto order = reader.getRoot<cpe::CpOrder>();
      CHECK(order.getId() == 0x1122334455667788ull);
      CHECK(order.getCustomer().getAge() == 37);
      REQUIRE(order.getItems().size() == 5);
      CHECK(order.getItems()[3].getUnitPrice() == 4.5);
      CHECK(order.getItems()[3].getQty() == 4);
      CHECK(std::string(order.getNote().cStr()) == "leave at the door");
   }

   // field<N>: ordinals in ascending field number, a union one per member.
   {
      const auto schema = capnp::Schema::from<cpe::CpRenumbered>();
      const auto ordinal = [&](const char* name) {
         return schema.getFieldByName(name).getProto().getOrdinal().getExplicit();
      };
      CHECK(ordinal("a") == 0);   // field<1>
      CHECK(ordinal("c") == 3);   // field<3>, after the union's two members
      CHECK(ordinal("b") == 4);   // field<4>
   }

   // A member at its initialiser is zero on the wire; libcapnp reads the
   // initialiser, because the emitted schema says it is the default.
   {
      const auto bytes = psio::to_capnp(CpDefaults{});
      auto       words = words_of(bytes);
      capnp::FlatArrayMessageReader reader(words.asPtr());
      const auto d = reader.getRoot<cpe::CpDefaults>();
      CHECK(d.getA() == -5);
      CHECK(d.getB() == ~std::uint64_t{0});
      CHECK(d.getD() == -2.5);
      CHECK(d.getF() == 0.25f);
      CHECK(d.getT() == true);
      CHECK(d.getG() == static_cast<std::uint8_t>(CpGrade::mid));
      CHECK(d.getU() == 65535);
      CHECK(d.getZ() == 0);
      // (the data section is all zero: value XOR default)
      for (std::size_t i = 16; i < bytes.size(); ++i)
         CHECK(bytes[i] == 0);
   }
   // An empty message from libcapnp is the default-constructed value.
   {
      capnp::MallocMessageBuilder mb;
      mb.initRoot<cpe::CpDefaults>();
      CHECK(psio::from_capnp<CpDefaults>(bytes_of(mb)) == CpDefaults{});
      capnp::MallocMessageBuilder mixed;
      mixed.initRoot<cpe::CpMixed>();
      CHECK(psio::from_capnp<CpMixed>(bytes_of(mixed)) == CpMixed{});
   }
}

TEST_CASE("emitted capnp: the C++ member names are the JSON names", "[capnp][schema][interop]")
{
   capnp::MallocMessageBuilder mb;
   auto                        item = mb.initRoot<cpe::CpItem>();
   item.setProduct("widget");
   item.setQty(3);
   item.setUnitPrice(9.5);
   capnp::JsonCodec codec;
   codec.handleByAnnotation<cpe::CpItem>();
   const kj::String  text = codec.encode(item.asReader());
   const std::string json(text.cStr());
   CHECK(json.find("\"unit_price\"") != std::string::npos);
   CHECK(json.find("unitPrice") == std::string::npos);
}
