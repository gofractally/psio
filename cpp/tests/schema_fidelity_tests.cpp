// Schema fidelity: what a reader holding only the Schema IR derives must be
// what the typed codec does.
//
//   - a boxed value has no inferred size bound (offset widths);
//   - a raw string ("string") is any bytes, a UTF-8 string ("utf8") is
//     validated;
//   - an explicit-width profile enforces maxDynamicData on the top-level
//     type only.

#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace fidelity
{
   template <typename T>
   using Box = psio::schema_types::Box<T>;

   struct Leaf
   {
      std::uint16_t        n = 0;
      psio::utf8_string<4> t;
   };
   PSIO_REFLECT(Leaf, n, t)

   struct Direct
   {
      std::uint32_t id = 0;
      Leaf          leaf;
   };
   PSIO_REFLECT(Direct, id, leaf)
   struct BoxMember
   {
      std::uint32_t id = 0;
      Box<Leaf>     leaf;
   };
   PSIO_REFLECT(BoxMember, id, leaf)
   struct BoxMemberCapped
   {
      std::uint32_t id = 0;
      Box<Leaf>     leaf;
   };
   PSIO_REFLECT(BoxMemberCapped, id, leaf)
   struct BoxOptional
   {
      std::optional<Box<Leaf>> leaf;
      std::uint8_t             tail = 0;
   };
   PSIO_REFLECT(BoxOptional, leaf, tail)
   struct BoxOptionalCapped
   {
      std::optional<Box<Leaf>> leaf;
      std::uint8_t             tail = 0;
   };
   PSIO_REFLECT(BoxOptionalCapped, leaf, tail)
   struct BoxVector
   {
      std::vector<Box<Leaf>> leaves;
   };
   PSIO_REFLECT(BoxVector, leaves)
   struct BoxVectorCapped
   {
      std::vector<Box<Leaf>> leaves;
   };
   PSIO_REFLECT(BoxVectorCapped, leaves)
   struct DirectVector
   {
      std::vector<Leaf> leaves;
   };
   PSIO_REFLECT(DirectVector, leaves)
   struct Outer
   {
      BoxMemberCapped capped;
      BoxMember       open;
      Direct          direct;
   };
   PSIO_REFLECT(Outer, capped, open, direct)
   // A recursive type is unbounded with or without the marker.
   struct Node
   {
      std::uint8_t             value = 0;
      std::optional<Box<Node>> next;
   };
   PSIO_REFLECT(Node, value, next)

   struct Strings
   {
      std::string          raw;
      psio::utf8_string<>  text;
      psio::utf8_string<8> bounded;
      std::vector<std::string> raws;
   };
   PSIO_REFLECT(Strings, raw, text, bounded, raws)

   struct Inner
   {
      std::string note;
   };
   PSIO_REFLECT(Inner, note)
   struct Capped
   {
      std::uint8_t id = 0;
      std::string  memo;
      Inner        inner;
   };
   PSIO_REFLECT(Capped, id, memo, inner)
   // The same layouts without caps: to write frames the capped types refuse.
   struct InnerTwin
   {
      std::string note;
   };
   PSIO_REFLECT(InnerTwin, note)
   struct CappedTwin
   {
      std::uint8_t id = 0;
      std::string  memo;
      InnerTwin    inner;
   };
   PSIO_REFLECT(CappedTwin, id, memo, inner)
}  // namespace fidelity

// Annotations of a namespaced type are attached at global scope.
PSIO_TYPE_ATTRS(fidelity::BoxMemberCapped, psio::max_dynamic_data_spec{200})
PSIO_TYPE_ATTRS(fidelity::BoxOptionalCapped, psio::max_dynamic_data_spec{60000})
PSIO_FIELD_ATTRS(fidelity::BoxVector, leaves, psio::length_bound{.max = 3})
PSIO_TYPE_ATTRS(fidelity::BoxVectorCapped, psio::max_dynamic_data_spec{250})
PSIO_FIELD_ATTRS(fidelity::DirectVector, leaves, psio::length_bound{.max = 3})
PSIO_TYPE_ATTRS(fidelity::Inner, psio::max_dynamic_data_spec{8})
PSIO_TYPE_ATTRS(fidelity::Capped, psio::max_dynamic_data_spec{40})

namespace
{
   using namespace fidelity;
   namespace ir = psio::pssz_ir;
   using Bytes  = std::vector<char>;

   template <typename T>
   psio::Schema schema_of()
   {
      return psio::SchemaBuilder{}.insert<T>("Root").build();
   }
   const psio::schema_types::AnyType root = psio::schema_types::Type{"Root"};

   // Width from the IR == typed width, and the typed bytes of `value` are
   // valid, decode and re-encode identically through the IR.
   template <typename T>
   void agrees(const T& value, std::size_t expected_width)
   {
      INFO(psio::reflect<T>::name);
      const auto  schema = schema_of<T>();
      std::size_t width  = 0;
      REQUIRE(ir::auto_width(schema, root, width).ok());
      CHECK(width == psio::auto_pssz_width_v<T>);
      CHECK(width == expected_width);

      Bytes typed;
      REQUIRE(psio::encode_pssz_checked(psio::pssz{}, value, typed).ok());
      bool unknown = true;
      REQUIRE(ir::validate_pssz(schema, root, std::span<const char>{typed}, &unknown).ok());
      CHECK_FALSE(unknown);
      ir::Value decoded;
      REQUIRE(ir::decode_auto(schema, root, std::span<const char>{typed}, decoded, unknown).ok());
      Bytes again;
      REQUIRE(ir::encode_auto(schema, root, decoded, again).ok());
      CHECK(again == typed);
      // Through text and back.
      std::string text;
      REQUIRE(ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{typed}, text)
                 .ok());
      Bytes from_text;
      REQUIRE(ir::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{text}, from_text)
                 .ok());
      CHECK(from_text == typed);
   }

   Leaf leaf(std::uint16_t n, const char* t) { return Leaf{n, psio::utf8_string<4>{t}}; }
}  // namespace

TEST_CASE("a boxed value has no inferred bound: IR width == typed width",
          "[schema][fidelity][box]")
{
   agrees(Direct{1, leaf(2, "ab")}, 1);
   agrees(BoxMember{1, Box<Leaf>{leaf(2, "ab")}}, 4);
   agrees(BoxMemberCapped{1, Box<Leaf>{leaf(2, "abcd")}}, 1);
   agrees(BoxOptional{Box<Leaf>{leaf(3, "x")}, 9}, 4);
   agrees(BoxOptional{std::nullopt, 9}, 4);
   agrees(BoxOptionalCapped{Box<Leaf>{leaf(3, "x")}, 9}, 2);
   {
      BoxVector v;
      v.leaves.push_back(Box<Leaf>{leaf(1, "a")});
      v.leaves.push_back(Box<Leaf>{leaf(2, "")});
      agrees(v, 4);
      agrees(BoxVector{}, 4);
      BoxVectorCapped c;
      c.leaves.push_back(Box<Leaf>{leaf(1, "a")});
      agrees(c, 1);
      DirectVector d;
      d.leaves.push_back(leaf(1, "a"));
      agrees(d, 1);
   }
   agrees(Outer{BoxMemberCapped{1, Box<Leaf>{leaf(2, "c")}}, BoxMember{3, Box<Leaf>{leaf(4, "o")}},
                Direct{5, leaf(6, "d")}},
          4);
   Node chain{1, Box<Node>{Node{2, Box<Node>{Node{3, std::nullopt}}}}};
   agrees(chain, 4);

   // The IR says so: the member is Custom{..., "box"}.
   const auto schema = schema_of<BoxMember>();
   const auto* type  = root.resolve(schema);
   REQUIRE(type);
   const auto& object = std::get<psio::schema_types::Object>(type->value);
   const auto* member = object.members[1].type->resolve(schema);
   REQUIRE(member);
   const auto* custom = std::get_if<psio::schema_types::Custom>(&member->value);
   REQUIRE(custom);
   CHECK(custom->id == "box");

   // The same schema with the box removed (how it was recorded before the
   // marker) derives the narrower width: that is the mismatch the marker
   // closes.
   psio::Schema unmarked = schema;
   for (auto& named : unmarked.types)
      if (auto* c = std::get_if<psio::schema_types::Custom>(&named.type->value);
          c && c->id == "box")
      {
         psio::schema_types::AnyType inner = *c->type;
         *named.type                        = std::move(inner);
      }
   std::size_t width = 0;
   REQUIRE(ir::auto_width(unmarked, root, width).ok());
   CHECK(width == 1);
}

TEST_CASE("raw strings and UTF-8 strings are distinct in the IR", "[schema][fidelity][string]")
{
   const auto schema = schema_of<Strings>();
   const auto& object =
      std::get<psio::schema_types::Object>(root.resolve(schema)->value);
   auto id_of = [&](std::size_t i) {
      const auto* t = object.members[i].type->resolve(schema);
      return std::get<psio::schema_types::Custom>(t->value).id;
   };
   CHECK(id_of(0) == "string");  // std::string: raw bytes
   CHECK(id_of(1) == "utf8");    // utf8_string<>
   CHECK(id_of(2) == "utf8");    // utf8_string<8>

   Strings good{"plain", psio::utf8_string<>{"t\xC3\xA9xt"}, psio::utf8_string<8>{"ok"}, {"a"}};
   agrees(good, psio::auto_pssz_width_v<Strings>);

   // A raw string holding bytes that are not UTF-8: valid PSSZ for the type
   // and through the schema, and it round-trips; it has no JSON text.
   Strings raw{std::string("\xff\xfe", 2), psio::utf8_string<>{"t"}, psio::utf8_string<8>{"b"},
               {std::string("\x80", 1)}};
   Bytes bytes;
   REQUIRE(psio::encode_pssz_checked(psio::pssz{}, raw, bytes).ok());
   REQUIRE(psio::validate<Strings>(psio::pssz{}, std::span<const char>{bytes}).ok());
   REQUIRE(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());
   ir::Value value;
   bool      unknown = false;
   REQUIRE(ir::decode_auto(schema, root, std::span<const char>{bytes}, value, unknown).ok());
   Bytes again;
   REQUIRE(ir::encode_auto(schema, root, value, again).ok());
   CHECK(again == bytes);
   std::string text = "keep";
   for (bool i_json : {false, true})
   {
      const auto st = i_json ? ir::pssz_to_json(schema, root, psio::ijson{},
                                                std::span<const char>{bytes}, text)
                             : ir::pssz_to_json(schema, root, psio::json{},
                                                std::span<const char>{bytes}, text);
      REQUIRE_FALSE(st.ok());
      CHECK(std::string(st.error().what).find("string is not valid UTF-8") != std::string::npos);
      CHECK(text == "keep");
      std::string typed_text;
      CHECK_FALSE((i_json ? psio::encode_json_checked(psio::ijson{}, raw, typed_text)
                          : psio::encode_json_checked(psio::json{}, raw, typed_text))
                     .ok());
   }

   // The same bytes in a UTF-8 string are rejected, by the type and by the
   // schema.
   Strings shifted{"a", psio::utf8_string<>{"b"}, psio::utf8_string<8>{"c"}, {}};
   Bytes   frame;
   REQUIRE(psio::encode_pssz_checked(psio::pssz{}, shifted, frame).ok());
   // The payload of `text` is the single byte 'b'.
   const auto at = std::find(frame.begin(), frame.end(), 'b');
   REQUIRE(at != frame.end());
   *at = static_cast<char>(0xff);
   CHECK_FALSE(psio::validate<Strings>(psio::pssz{}, std::span<const char>{frame}).ok());
   CHECK_FALSE(ir::validate_pssz(schema, root, std::span<const char>{frame}).ok());
   // ...and the raw member's byte may be anything.
   *at = 'b';
   const auto raw_at = std::find(frame.begin(), frame.end(), 'a');
   REQUIRE(raw_at != frame.end());
   *raw_at = static_cast<char>(0xff);
   CHECK(psio::validate<Strings>(psio::pssz{}, std::span<const char>{frame}).ok());
   CHECK(ir::validate_pssz(schema, root, std::span<const char>{frame}).ok());
}

TEST_CASE("explicit widths enforce maxDynamicData on the top-level type only",
          "[schema][fidelity][cap]")
{
   const auto schema = schema_of<Capped>();
   auto typed = [](const Capped& v, Bytes& out) {
      return psio::encode_pssz_checked(psio::pssz16{}, v, out).ok();
   };
   auto through_schema = [&](const Bytes& bytes) {
      ir::Value value;
      bool      unknown = false;
      return ir::decode_compatible<2>(schema, root, std::span<const char>{bytes}, value, unknown)
         .ok();
   };

   // Within both caps.
   Capped fits{1, "memo", Inner{"ok"}};
   Bytes  bytes;
   REQUIRE(typed(fits, bytes));
   REQUIRE(bytes.size() <= 40);
   CHECK(psio::validate<Capped>(psio::pssz16{}, std::span<const char>{bytes}).ok());
   CHECK(through_schema(bytes));
   {
      ir::Value value;
      bool      unknown = false;
      REQUIRE(ir::decode_compatible<2>(schema, root, std::span<const char>{bytes}, value, unknown)
                 .ok());
      Bytes again;
      REQUIRE(ir::encode<2>(schema, root, value, again).ok());
      CHECK(again == bytes);
   }

   // The nested record over ITS cap (8), the whole still within 40: the
   // typed explicit-width codec does not look a nested cap up, and neither
   // does the schema codec.
   Capped nested{1, "", Inner{"0123456789abcdef"}};
   bytes.clear();
   REQUIRE(typed(nested, bytes));
   REQUIRE(bytes.size() <= 40);
   CHECK(psio::validate<Capped>(psio::pssz16{}, std::span<const char>{bytes}).ok());
   CHECK(through_schema(bytes));
   // (The automatic profile checks every record: the same value has no
   // psio::pssz encoding.)
   CHECK_THROWS_AS(psio::encode(psio::pssz{}, nested), psio::codec_exception);

   // The top-level type over its cap: refused by both, on write and read.
   Capped over{1, std::string(60, 'm'), Inner{"ok"}};
   bytes.clear();
   CHECK_FALSE(typed(over, bytes));
   const Bytes raw =
      psio::encode(psio::pssz16{}, CappedTwin{1, std::string(60, 'm'), InnerTwin{"ok"}});
   REQUIRE(raw.size() > 40);
   CHECK_FALSE(psio::validate<Capped>(psio::pssz16{}, std::span<const char>{raw}).ok());
   CHECK_FALSE(through_schema(raw));
   {
      // Building the over-cap value through the schema fails on encode too.
      ir::Value value;
      bool      unknown = false;
      const auto wide_schema = schema;
      Bytes      within;
      REQUIRE(typed(fits, within));
      REQUIRE(ir::decode_compatible<2>(wide_schema, root, std::span<const char>{within}, value,
                                       unknown)
                 .ok());
      auto& fields = std::get<ir::Record>(value.value);
      for (auto& [name, field] : fields)
         if (name == "memo")
            field = std::string(60, 'm');
      Bytes out;
      const auto st = ir::encode<2>(wide_schema, root, value, out);
      REQUIRE_FALSE(st.ok());
      CHECK(std::string(st.error().what) == "pssz IR: encoded size exceeds maxDynamicData cap");
   }
}

// ── Custom text presentations ──────────────────────────────────────────────

namespace fidelity
{
   // A u64 with a text form (an enum is the strong typedef): "#<hex>".
   enum class Tag : std::uint64_t
   {
   };
   struct TagText
   {
      static constexpr std::string_view schema_id = "HexTag";
      static bool parse(std::span<const char> in, Tag& out) noexcept
      {
         if (in.size() < 2 || in.size() > 17 || in[0] != '#') return false;
         std::uint64_t v = 0;
         for (std::size_t i = 1; i < in.size(); ++i)
         {
            const char c = in[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            v = (v << 4) | static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
         }
         if (in.size() > 2 && in[1] == '0') return false;  // one text per value
         out = static_cast<Tag>(v);
         return true;
      }
      static std::size_t packsize(const Tag& v)
      {
         std::string s;
         encode(v, s);
         return s.size();
      }
      static void encode(const Tag& t, std::string& s)
      {
         static const char digits[] = "0123456789abcdef";
         auto              v        = static_cast<std::uint64_t>(t);
         std::string       hex;
         do
         {
            hex.insert(hex.begin(), digits[v & 15]);
            v >>= 4;
         } while (v);
         s += '#';
         s += hex;
      }
      static Tag decode(std::span<const char> in)
      {
         Tag t{};
         (void)parse(in, t);
         return t;
      }
      static psio::codec_status validate(std::span<const char> in) noexcept
      {
         Tag t{};
         return parse(in, t) ? psio::codec_ok() : psio::codec_fail("HexTag: expected #hex", 0, "HexTag");
      }
      static psio::codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   // A record with a text form: "x,y". No schema_id: the id is the
   // reflected name.
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      friend bool  operator==(const Point&, const Point&) = default;
   };
   PSIO_REFLECT(Point, x, y)
   struct PointText
   {
      static bool parse(std::span<const char> in, Point& out) noexcept
      {
         const std::string_view text{in.data(), in.size()};
         const auto             comma = text.find(',');
         if (comma == std::string_view::npos) return false;
         auto number = [](std::string_view part, std::int32_t& v) {
            return psio::detail::json_impl::parse_integer(part, v);
         };
         return number(text.substr(0, comma), out.x) && number(text.substr(comma + 1), out.y);
      }
      static std::size_t packsize(const Point& v)
      {
         std::string s;
         encode(v, s);
         return s.size();
      }
      static void encode(const Point& p, std::string& s)
      {
         s += std::to_string(p.x) + "," + std::to_string(p.y);
      }
      static Point decode(std::span<const char> in)
      {
         Point p;
         (void)parse(in, p);
         return p;
      }
      static psio::codec_status validate(std::span<const char> in) noexcept
      {
         Point p;
         return parse(in, p) ? psio::codec_ok() : psio::codec_fail("Point: expected x,y", 0, "Point");
      }
      static psio::codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   struct Shape
   {
      Tag                  tag{};
      Point                at;
      std::vector<Point>   path;
      std::optional<Tag>   parent;
      std::string          label;
   };
   PSIO_REFLECT(Shape, tag, at, path, parent, label)
}  // namespace fidelity

PSIO_ADAPTER(fidelity::Tag, psio::text_category, fidelity::TagText)
PSIO_ADAPTER(fidelity::Point, psio::text_category, fidelity::PointText)

TEST_CASE("a custom text form is Custom{wire type, id} and a registered handler writes it",
          "[schema][fidelity][custom]")
{
   static_assert(psio::schema_custom_v<Tag>);
   static_assert(psio::schema_custom_v<Point>);
   static_assert(!psio::schema_custom_v<Shape>);
   static_assert(!psio::schema_custom_v<psio::uint128>);  // decimal in JSON, not a custom
   static_assert(psio::schema_custom_id<Tag>() == "HexTag");
   static_assert(psio::schema_custom_id<Point>() == "Point");

   const auto  schema = schema_of<Shape>();
   const auto& object = std::get<psio::schema_types::Object>(root.resolve(schema)->value);
   {
      // The IR: the id over the wire type.
      const auto& tag = std::get<psio::schema_types::Custom>(
         object.members[0].type->resolve(schema)->value);
      CHECK(tag.id == "HexTag");
      const auto& integer = std::get<psio::schema_types::Int>(tag.type->resolve(schema)->value);
      CHECK(integer.bits == 64);
      CHECK_FALSE(integer.isSigned);
      const auto& point = std::get<psio::schema_types::Custom>(
         object.members[1].type->resolve(schema)->value);
      CHECK(point.id == "Point");
      CHECK(std::holds_alternative<psio::schema_types::Object>(
         point.type->resolve(schema)->value));
   }

   const Shape value{static_cast<Tag>(0xbeef), Point{-1, 2}, {Point{3, 4}, Point{0, 0}},
                     static_cast<Tag>(0), "label"};
   const std::string typed_json  = psio::encode(psio::json{}, value);
   const std::string typed_ijson = psio::encode(psio::ijson{}, value);
   CHECK(typed_ijson ==
         R"({"tag":"#beef","at":"-1,2","path":["3,4","0,0"],"parent":"#0","label":"label"})");
   Bytes bytes;
   REQUIRE(psio::encode_pssz_checked(psio::pssz{}, value, bytes).ok());
   // The width and the bytes never depend on the presentation.
   std::size_t width = 0;
   REQUIRE(ir::auto_width(schema, root, width).ok());
   CHECK(width == psio::auto_pssz_width_v<Shape>);
   REQUIRE(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());

   // One registration call per type: its text adapter is the handler.
   ir::CustomHandlers handlers;
   handlers.add<Tag>().add<Point>();

   auto text_of = [&](bool i_json, const ir::CustomHandlers* h, const ir::JsonNames* names) {
      std::string out;
      const auto  st = i_json ? ir::pssz_to_json(schema, root, psio::ijson{},
                                                 std::span<const char>{bytes}, out, nullptr, h,
                                                 names)
                              : ir::pssz_to_json(schema, root, psio::json{},
                                                 std::span<const char>{bytes}, out, nullptr, h,
                                                 names);
      return st.ok() ? out : std::string("error: ") + std::string(st.error().what);
   };
   auto bytes_of = [&](const std::string& text, const ir::CustomHandlers* h,
                       const ir::JsonNames* names) {
      Bytes out;
      const auto st = ir::json_to_pssz(schema, root, psio::ijson{}, std::span<const char>{text},
                                       out, names, psio::json_parser::automatic, h);
      return st.ok() ? out : Bytes{'!'};
   };

   // With the handlers: the typed text, both ways.
   CHECK(text_of(false, &handlers, nullptr) == typed_json);
   CHECK(text_of(true, &handlers, nullptr) == typed_ijson);
   CHECK(bytes_of(typed_ijson, &handlers, nullptr) == bytes);
   // Bound once per schema through JsonNames: the same.
   const ir::JsonNames names(schema, handlers);
   CHECK(text_of(true, nullptr, &names) == typed_ijson);
   CHECK(bytes_of(typed_ijson, nullptr, &names) == bytes);

   // Without a handler: the wire type's own text, which reads back to the
   // same bytes.
   const std::string plain =
      R"({"tag":48879,"at":{"x":-1,"y":2},"path":[{"x":3,"y":4},{"x":0,"y":0}],"parent":0,"label":"label"})";
   CHECK(text_of(true, nullptr, nullptr) == plain);
   CHECK(bytes_of(plain, nullptr, nullptr) == bytes);
   // One id registered, the other not: each is decided on its own.
   ir::CustomHandlers only_tag;
   only_tag.add<Tag>();
   CHECK(text_of(true, &only_tag, nullptr) ==
         R"({"tag":"#beef","at":{"x":-1,"y":2},"path":[{"x":3,"y":4},{"x":0,"y":0}],"parent":"#0","label":"label"})");
   // A caller that needs the exact presentation makes a missing handler an
   // error.
   only_tag.require_all = true;
   CHECK(text_of(true, &only_tag, nullptr) ==
         "error: pssz IR: no handler registered for a custom type");
   CHECK(bytes_of(typed_ijson, &only_tag, nullptr) == Bytes{'!'});

   // With a handler the presentation is exactly its text: the plain form
   // and a malformed text are rejected.
   CHECK(bytes_of(plain, &handlers, nullptr) == Bytes{'!'});
   CHECK(bytes_of(R"({"tag":"beef","at":"-1,2","path":[],"parent":null,"label":""})", &handlers,
                  nullptr) == Bytes{'!'});
   CHECK(bytes_of(R"({"tag":"#beef","at":"1;2","path":[],"parent":null,"label":""})", &handlers,
                  nullptr) == Bytes{'!'});

   // A handler written by hand, for an id no C++ type declares.
   psio::Schema hand;
   hand.insert("Root", psio::schema_types::Custom{
                          psio::schema_types::AnyType{psio::schema_types::Int{32, false}},
                          "Percent"});
   ir::CustomHandlers percent;
   percent.add("Percent",
               {[](std::span<const char> pssz, std::string& text) -> psio::codec_status {
                   if (pssz.size() != 4) return psio::codec_fail("Percent: 4 bytes", 0, "Percent");
                   std::uint32_t v;
                   std::memcpy(&v, pssz.data(), 4);
                   if (v > 100) return psio::codec_fail("Percent: over 100", 0, "Percent");
                   text += std::to_string(v) + "%";
                   return psio::codec_ok();
                },
                [](std::string_view text, std::vector<char>& pssz) -> psio::codec_status {
                   std::uint32_t v = 0;
                   if (text.empty() || text.back() != '%' ||
                       !psio::detail::json_impl::parse_integer(text.substr(0, text.size() - 1), v) ||
                       v > 100)
                      return psio::codec_fail("Percent: expected N%", 0, "Percent");
                   pssz.resize(4);
                   std::memcpy(pssz.data(), &v, 4);
                   return psio::codec_ok();
                }});
   const Bytes  forty_two{42, 0, 0, 0};
   std::string  out;
   REQUIRE(ir::pssz_to_json(hand, root, psio::json{}, std::span<const char>{forty_two}, out,
                            nullptr, &percent)
              .ok());
   CHECK(out == R"("42%")");
   Bytes back;
   REQUIRE(ir::json_to_pssz(hand, root, psio::json{}, std::span<const char>{out}, back, nullptr,
                            psio::json_parser::automatic, &percent)
              .ok());
   CHECK(back == forty_two);
   // The handler validates in both directions.
   const Bytes two_hundred{static_cast<char>(200), 0, 0, 0};
   out.clear();
   CHECK_FALSE(ir::pssz_to_json(hand, root, psio::json{}, std::span<const char>{two_hundred}, out,
                                nullptr, &percent)
                  .ok());
   CHECK(out.empty());
   CHECK_FALSE(ir::json_to_pssz(hand, root, psio::json{},
                                std::span<const char>{std::string_view{R"("200%")"}}, back,
                                nullptr, psio::json_parser::automatic, &percent)
                  .ok());
   // The bytes themselves stay valid whatever the handler thinks of them.
   CHECK(ir::validate_pssz(hand, root, std::span<const char>{two_hundred}).ok());
}
