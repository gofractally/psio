// The key / time features and the schema-fidelity features together:
//
//   - a time id is a built-in presentation of the Custom-id mechanism: the
//     tick integer on the wire, its text from time.hpp, never a handler;
//   - an enum is its underlying integer in a key; its names are JSON only;
//   - raw and UTF-8 strings are the same bytes in pssz and key, distinct
//     ids in the schema, and only the UTF-8 one is validated;
//   - reflected methods are not part of a record's pssz, key or JSON;
//   - a member's length_bound is enforced when a key is read.

#include <psio/annotate.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>
#include <psio/time.hpp>

#include <catch.hpp>

#include "validated_write.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kts
{
   using sys_micros = std::chrono::sys_time<std::chrono::microseconds>;
   using sys_secs   = std::chrono::sys_time<std::chrono::seconds>;

   enum class Status : std::int32_t
   {
      pending = -1,
      active  = 2,
      closed  = 7,
   };
   PSIO_REFLECT_ENUM(Status, pending, active, closed)

   struct Order
   {
      Status                    status = Status::pending;
      psio::utf8_string<16>     label;
      std::string               raw;
      sys_micros                at{};
      std::chrono::milliseconds ttl{};
      friend bool               operator==(const Order&, const Order&) = default;
   };
   PSIO_REFLECT(Order, status, label, raw, at, ttl)

   // Order's fields and nothing else on the wire: the methods are metadata.
   struct OrderWithMethods
   {
      Status                    status = Status::pending;
      psio::utf8_string<16>     label;
      std::string               raw;
      sys_micros                at{};
      std::chrono::milliseconds ttl{};
      bool                      open() const { return status != Status::closed; }
      void                      close(std::uint32_t reason) { (void)reason, status = Status::closed; }
      static std::uint32_t      version() { return 3; }
      PSIO_REFLECT(OrderWithMethods, status, label, raw, at, ttl, method(open),
                   method(close, reason), method(version))
   };

   struct Bounded
   {
      std::uint32_t              shard = 0;
      std::string                owner;
      std::optional<std::string> note;
      std::vector<std::uint16_t> tags;
   };
   PSIO_REFLECT(Bounded, shard, owner, note, tags)
}  // namespace kts
PSIO_FIELD_ATTRS(kts::Bounded, owner, psio::length_bound{.max = 3})
PSIO_FIELD_ATTRS(kts::Bounded, note, psio::length_bound{.max = 2})
PSIO_FIELD_ATTRS(kts::Bounded, tags, psio::length_bound{.max = 1})

namespace
{
   using Bytes = std::vector<char>;
   namespace ir = psio::pssz_ir;
   namespace st = psio::schema_types;

   template <typename T>
   Bytes key_of(const T& v)
   {
      return psio::encode(psio::key{}, v);
   }
   template <typename T>
   bool key_ok(const Bytes& bytes)
   {
      return psio::validate<T>(psio::key{}, std::span<const char>{bytes}).ok();
   }
   Bytes cat(std::initializer_list<Bytes> parts)
   {
      Bytes out;
      for (const auto& p : parts)
         out.insert(out.end(), p.begin(), p.end());
      return out;
   }
   const st::AnyType& named(const psio::Schema& schema, std::string_view name)
   {
      for (const auto& member : schema.types)
         if (member.name == name)
            return *member.type;
      FAIL("no type " << name);
      static const st::AnyType none;
      return none;
   }
   const st::AnyType& member_type(const psio::Schema& schema, std::string_view type,
                                  std::string_view member)
   {
      const st::AnyType* at = named(schema, type).resolve(schema);
      const auto*        o  = std::get_if<st::Object>(&at->value);
      const auto*        s  = std::get_if<st::Struct>(&at->value);
      for (const auto& m : o ? o->members : s->members)
         if (m.name == member)
            return *m.type->resolve(schema);
      FAIL("no member " << member);
      return *at;
   }
   kts::Order sample()
   {
      return kts::Order{kts::Status::active, psio::utf8_string<16>{"caf\xc3\xa9"},
                        std::string("a\0\xff", 3),
                        kts::sys_micros{std::chrono::microseconds{1795342530123456}},
                        std::chrono::milliseconds{-1500}};
   }
}  // namespace

TEST_CASE("an enum is its underlying integer in a key; names are JSON only", "[key][enum]")
{
   using kts::Status;
   for (auto s : {Status::pending, Status::active, Status::closed, static_cast<Status>(1234),
                  static_cast<Status>(-99)})
   {
      CHECK(key_of(s) == key_of(static_cast<std::int32_t>(s)));
      CHECK(psio::decode<Status>(psio::key{}, std::span<const char>{key_of(s)}) == s);
      CHECK(psio::encode(psio::pssz{}, s) == psio::encode(psio::pssz{}, static_cast<std::int32_t>(s)));
   }
   // Key order is the order of the values, not of the names or declarations.
   auto before = [](Status a, Status b) {
      const auto ka = key_of(a), kb = key_of(b);
      return std::memcmp(ka.data(), kb.data(), ka.size()) < 0;
   };
   CHECK(before(Status::pending, Status::active));
   CHECK(before(static_cast<Status>(-99), Status::pending));
   CHECK(before(Status::closed, static_cast<Status>(1234)));
   // The name appears in text only.
   CHECK(psio::encode(psio::json{}, Status::active) == "\"active\"");
   CHECK(psio::encode(psio::ijson{}, Status::active) == "\"active\"");
   CHECK(psio::encode(psio::json{}, static_cast<Status>(1234)) == "1234");
   // The fixed key size of the integer.
   STATIC_REQUIRE(*psio::detail::key_impl::key_fixed_size<Status>() == 4);
}

TEST_CASE("raw and UTF-8 strings: one byte form, two schema ids, one validated", "[key][utf8]")
{
   const std::string text = "caf\xc3\xa9";
   const std::string junk = std::string("a\0\xff", 3);
   // Same bytes -> same pssz and same key under either type.
   CHECK(key_of(psio::utf8_string<16>{text}) == key_of(text));
   CHECK(psio::encode(psio::pssz{}, psio::utf8_string<16>{text}) == psio::encode(psio::pssz{}, text));
   // A raw string holds any bytes in pssz and key; the UTF-8 type rejects them.
   const auto raw_key = key_of(junk);
   CHECK(key_ok<std::string>(raw_key));
   CHECK(psio::decode<std::string>(psio::key{}, std::span<const char>{raw_key}) == junk);
   CHECK_FALSE(key_ok<psio::utf8_string<16>>(raw_key));
   const auto raw_pssz = psio::encode(psio::pssz{}, junk);
   CHECK(psio::validate<std::string>(psio::pssz{}, std::span<const char>{raw_pssz}).ok());
   CHECK_FALSE(
      psio::validate<psio::utf8_string<16>>(psio::pssz{}, std::span<const char>{raw_pssz}).ok());
   // The UTF-8 bound is enforced in the key too.
   CHECK_FALSE(key_ok<psio::utf8_string<2>>(key_of(std::string{"abc"})));

   // The schema says which is which.
   psio::SchemaBuilder builder;
   builder.insert<kts::Order>("Order");
   const psio::Schema schema = std::move(builder).build();
   const auto* label = std::get_if<st::Custom>(&member_type(schema, "Order", "label").value);
   const auto* raw   = std::get_if<st::Custom>(&member_type(schema, "Order", "raw").value);
   REQUIRE(label);
   REQUIRE(raw);
   CHECK(label->id == "utf8");
   CHECK(raw->id == "string");
   const auto* status = std::get_if<st::Enum>(&member_type(schema, "Order", "status").value);
   REQUIRE(status);
   CHECK(status->members.size() == 3);
   const auto* at = std::get_if<st::Custom>(&member_type(schema, "Order", "at").value);
   REQUIRE(at);
   CHECK(at->id == "time-point-micros");
   const auto* ttl = std::get_if<st::Custom>(&member_type(schema, "Order", "ttl").value);
   REQUIRE(ttl);
   CHECK(ttl->id == "duration-millis");
}

TEST_CASE("typed and schema-driven codecs agree on enum, strings and time together",
          "[time][schema][enum]")
{
   psio::SchemaBuilder builder;
   builder.insert<kts::Order>("Order");
   const psio::Schema schema = std::move(builder).build();
   const st::AnyType  root{st::Type{"Order"}};

   // A raw string that is UTF-8, so that it has text.
   auto v = sample();
   v.raw  = "plain";
   const auto        bytes = psio::encode(psio::pssz{}, v);
   const std::string json  = psio::encode(psio::json{}, v);
   const std::string ijson = psio::encode(psio::ijson{}, v);
   CHECK(json == R"({"status":"active","label":"café","raw":"plain",)"
                 R"("at":"2026-11-22T10:15:30.123456Z","ttl":-1500})");
   CHECK(ijson == json);

   CHECK(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());
   for (bool strict : {false, true})
   {
      // The time ids are built in: they need no handler, also when every
      // custom id must have one.
      ir::CustomHandlers handlers;
      handlers.require_all = strict;
      std::string out;
      REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, out,
                               nullptr, &handlers)
                 .ok());
      CHECK(out == json);
      out.clear();
      REQUIRE(ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{bytes}, out,
                               nullptr, &handlers)
                 .ok());
      CHECK(out == ijson);
      Bytes back;
      REQUIRE(ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{json}, back,
                               nullptr, psio::json_parser::automatic, &handlers)
                 .ok());
      CHECK(back == bytes);
      // Bound once through JsonNames as well.
      const ir::JsonNames names(schema, handlers);
      out.clear();
      REQUIRE(ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{bytes}, out,
                               nullptr, nullptr, &names)
                 .ok());
      CHECK(out == ijson);
      back.clear();
      REQUIRE(ir::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{ijson}, back,
                               &names)
                 .ok());
      CHECK(back == bytes);
   }

   // A handler cannot be registered under a time id: the presentation is
   // the codec's own.
   ir::CustomHandlers hijack;
   bool               called = false;
   CHECK_FALSE(hijack
                  .try_add("time-point-micros",
                           {[&](std::span<const char>, std::string& text) {
                               called = true;
                               text   = "hijacked";
                               return psio::codec_ok();
                            },
                            [&](std::string_view, std::vector<char>&) {
                               called = true;
                               return psio::codec_ok();
                            }})
                  .ok());
   std::string out;
   REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, out, nullptr,
                            &hijack)
              .ok());
   CHECK(out == json);
   CHECK_FALSE(called);

   // A raw string that is not UTF-8 has pssz and key but no text, in both
   // the typed and the schema-driven path.
   const auto  odd       = sample();
   const auto  odd_bytes = psio::encode(psio::pssz{}, odd);
   std::string none;
   CHECK_FALSE(psio::encode_json_checked(psio::json{}, odd, none).ok());
   CHECK(ir::validate_pssz(schema, root, std::span<const char>{odd_bytes}).ok());
   CHECK_FALSE(
      ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{odd_bytes}, none).ok());
   CHECK(key_ok<kts::Order>(key_of(odd)));
   CHECK(psio::decode<kts::Order>(psio::key{}, std::span<const char>{key_of(odd)}) == odd);
}

TEST_CASE("time ids are reserved: a malformed one, or one over a non-tick type, is refused",
          "[time][schema]")
{
   auto schema_of = [](std::string id, st::AnyType inner) {
      psio::Schema schema;
      schema.types.push_back(
         {"T", st::Box<st::AnyType>{st::Custom{st::Box<st::AnyType>{std::move(inner)}, std::move(id)}}});
      return schema;
   };
   const st::AnyType root{st::Type{"T"}};
   const Bytes       eight(8, '\0');
   auto accepts = [&](const psio::Schema& schema) {
      std::string text;
      Bytes       back;
      const bool  binary = ir::validate_pssz(schema, root, std::span<const char>{eight}).ok();
      const bool  write =
         ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{eight}, text).ok();
      const std::string zero = "0";
      const bool        read =
         ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{zero}, back).ok();
      CHECK(binary == write);
      return binary || write || read;
   };
   // Well-formed.
   CHECK(accepts(schema_of("duration-millis", st::Int{64, true})));
   CHECK(accepts(schema_of("duration-60/1", st::Int{64, false})));
   CHECK(accepts(schema_of("time-point-seconds", st::Int{64, true})));
   // An ordinary custom id: its inner type.
   CHECK(accepts(schema_of("AccountNumber", st::Int{64, false})));
   // Reserved spellings that are not time ids.
   for (const char* id : {"time-point", "duration", "time-point-", "duration-", "duration-minutes",
                          "duration-1/1000", "duration-2/4", "time-point-01/7", "duration-0/1"})
   {
      INFO(id);
      CHECK_FALSE(accepts(schema_of(id, st::Int{64, true})));
   }
   // The tick type is an 8/16/32/64-bit integer.
   CHECK_FALSE(accepts(schema_of("duration-millis", st::Int{128, true})));
   CHECK_FALSE(accepts(schema_of("duration-millis", st::Float{11, 53})));
   CHECK_FALSE(accepts(schema_of(
      "time-point-seconds", st::Custom{st::Box<st::AnyType>{st::List{st::Box<st::AnyType>{st::Int{8, false}}}}, "utf8"})));
}

TEST_CASE("reflected methods are not part of a record's pssz, key or JSON", "[reflect][key]")
{
   const auto        a = sample();
   kts::OrderWithMethods b;
   b.status = a.status, b.label = a.label, b.raw = a.raw, b.at = a.at, b.ttl = a.ttl;
   STATIC_REQUIRE(psio::reflect<kts::OrderWithMethods>::member_count == 5);
   STATIC_REQUIRE(psio::reflect<kts::OrderWithMethods>::method_count == 3);
   CHECK(psio::encode(psio::pssz{}, a) == psio::encode(psio::pssz{}, b));
   CHECK(key_of(a) == key_of(b));
   CHECK(psio::size_of(psio::key{}, a) == psio::size_of(psio::key{}, b));
   STATIC_REQUIRE(psio::auto_pssz_width<kts::Order>() ==
                  psio::auto_pssz_width<kts::OrderWithMethods>());
   STATIC_REQUIRE(psio::pssz_evolves_from<kts::Order, kts::Order>);
   auto text = a;
   kts::OrderWithMethods text_b = b;
   text.raw = text_b.raw = "x";
   CHECK(psio::encode(psio::json{}, text) == psio::encode(psio::json{}, text_b));
   CHECK(psio::encode(psio::ijson{}, text) == psio::encode(psio::ijson{}, text_b));

   // And not part of its schema node's members.
   psio::SchemaBuilder builder;
   builder.insert<kts::Order>("A");
   builder.insert<kts::OrderWithMethods>("B");
   const psio::Schema schema = std::move(builder).build();
   const st::AnyType  ra{st::Type{"A"}}, rb{st::Type{"B"}};
   const auto         bytes = psio::encode(psio::pssz{}, b);
   CHECK(ir::validate_pssz(schema, ra, std::span<const char>{bytes}).ok());
   CHECK(ir::validate_pssz(schema, rb, std::span<const char>{bytes}).ok());
   std::string ja, jb;
   const auto  tb = psio::encode(psio::pssz{}, text_b);
   REQUIRE(ir::pssz_to_json(schema, ra, psio::json{}, std::span<const char>{tb}, ja).ok());
   REQUIRE(ir::pssz_to_json(schema, rb, psio::json{}, std::span<const char>{tb}, jb).ok());
   CHECK(ja == jb);
   CHECK(ja == psio::encode(psio::json{}, text_b));
}

TEST_CASE("a member's length_bound is enforced when a key is read", "[key][bound]")
{
   using kts::Bounded;
   const Bounded ok{7, "bob", std::string{"hi"}, {5}};
   const auto    key = key_of(ok);
   CHECK(key_ok<Bounded>(key));
   const auto back = psio::decode<Bounded>(psio::key{}, std::span<const char>{key});
   CHECK(back.owner == "bob");
   CHECK(back.note == std::optional<std::string>{"hi"});
   CHECK(back.tags == std::vector<std::uint16_t>{5});
   // An absent optional satisfies its bound.
   CHECK(key_ok<Bounded>(key_of(Bounded{7, "", std::nullopt, {}})));

   // The same bytes without the bounds are a valid key of the unbounded
   // tuple; each bound is checked on its own member.
   using Plain = std::tuple<std::uint32_t, std::string, std::optional<std::string>,
                            std::vector<std::uint16_t>>;
   const auto over_owner = key_of(Plain{7, "bobs", std::string{"hi"}, {5}});
   const auto over_note  = key_of(Plain{7, "bob", std::string{"hey"}, {5}});
   const auto over_tags  = key_of(Plain{7, "bob", std::string{"hi"}, {5, 6}});
   for (const auto* bad : {&over_owner, &over_note, &over_tags})
   {
      CHECK(key_ok<Plain>(*bad));
      const auto status = psio::validate<Bounded>(psio::key{}, std::span<const char>{*bad});
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what.find("length_bound") !=
            std::string_view::npos);
      CHECK_THROWS(psio_test::decode_validated<Bounded>(psio::key{}, std::span<const char>{*bad}));
   }
   // pssz reads enforce the same bounds on the same values.
   CHECK_FALSE(psio::validate<Bounded>(
                  psio::pssz{},
                  std::span<const char>{psio::detail::pssz_impl::encode_ignoring_length_bounds(
                     psio::pssz{}, Bounded{7, "bobs", std::nullopt, {}})})
                  .ok());
}
