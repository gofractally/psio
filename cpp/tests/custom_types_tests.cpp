// Transparent wrapper types (psio/transparent.hpp), custom text forms for a
// type of any shape (adapter.hpp schema_custom_v) and per-type invariants
// (psio/invariant.hpp), in the core formats — pssz, key, json, ijson, the
// Schema IR — typed and schema-driven.

#include "custom_conformance_types.hpp"

#include <psio/graphql.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace custom_conf;
namespace ir = psio::pssz_ir;
namespace st = psio::schema_types;
namespace pi = psio::detail::pssz_impl;

// ── Declarations in every form ─────────────────────────────────────────────
namespace ct::deep::er
{
   // In the class body, the keyword anywhere in the list, a private member.
   class Secret
   {
      std::string text_;

    public:
      Secret() = default;
      explicit Secret(std::string t) : text_(std::move(t)) {}
      const std::string& text() const { return text_; }
      friend bool        operator==(const Secret&, const Secret&) = default;
      PSIO_REFLECT(Secret, text_, transparent(), method(text))
   };

   // After the type, at namespace scope.
   struct Height
   {
      std::uint32_t value = 0;
      friend bool   operator==(const Height&, const Height&) = default;
   };
   PSIO_REFLECT(Height, transparent(), value)

   // With the separate statement.
   struct Weight
   {
      std::uint16_t grams = 0;
      friend bool   operator==(const Weight&, const Weight&) = default;
   };
   PSIO_REFLECT(Weight, grams)
   PSIO_TYPE_ATTRS(Weight, psio::transparent)

   // A record with a budget, and its transparent wrapper: the budget (and
   // with it the offset width) is the member's.
   struct Capped
   {
      std::uint16_t id = 0;
      std::string   memo;
      friend bool   operator==(const Capped&, const Capped&) = default;
      PSIO_REFLECT(Capped, id, memo, maxDynamicData(300))
   };
   struct CappedBox
   {
      Capped      inner;
      friend bool operator==(const CappedBox&, const CappedBox&) = default;
      PSIO_REFLECT(CappedBox, transparent(), inner)
   };
   // Wrappers nest.
   struct Tall
   {
      Height      height;
      friend bool operator==(const Tall&, const Tall&) = default;
      PSIO_REFLECT(Tall, transparent(), height)
   };

   // A record holding transparent members, and the same record spelled
   // with the members' types.
   struct Holder
   {
      Height                 h;
      Secret                 s;
      std::optional<Height>  oh;
      std::optional<Secret>  os;
      std::vector<Height>    vh;
      std::array<Weight, 2>  aw{};
      CappedBox              c;
      std::tuple<Tall, Secret> t;
      friend bool operator==(const Holder&, const Holder&) = default;
      PSIO_REFLECT(Holder, h, s, oh, os, vh, aw, c, t)
   };
   struct Plain
   {
      std::uint32_t                        h = 0;
      std::string                          s;
      std::optional<std::uint32_t>         oh;
      std::optional<std::string>           os;
      std::vector<std::uint32_t>           vh;
      std::array<std::uint16_t, 2>         aw{};
      Capped                               c;
      std::tuple<std::uint32_t, std::string> t;
      PSIO_REFLECT(Plain, h, s, oh, os, vh, aw, c, t)
   };

   // An integer member of a transparent type takes the enclosing member's
   // integer text form.
   struct Forms
   {
      Id            a;
      Id            b;
      std::uint64_t c = 0;
      PSIO_REFLECT(Forms, attr(a, json_string), attr(b, ijson_number), c)
   };

   // ── Invariants in every form ──────────────────────────────────────────
   // A static member declared AFTER the macro (the class is complete where
   // the keyword is used).
   struct Even
   {
      std::uint32_t n = 0;
      friend bool   operator==(const Even&, const Even&) = default;
      PSIO_REFLECT(Even, n, invariant(is_even))
      static psio::codec_status is_even(const Even& e) noexcept
      {
         return e.n % 2 == 0 ? psio::codec_ok() : psio::codec_fail("Even: odd", 3, "Even");
      }
   };

   // Records what ran, to observe the order of checks.
   inline std::vector<std::string>& log()
   {
      static std::vector<std::string> entries;
      return entries;
   }
   struct Leaf
   {
      std::uint8_t v = 0;
      friend bool  operator==(const Leaf&, const Leaf&) = default;
      static psio::codec_status check(const Leaf& l) noexcept
      {
         log().push_back("Leaf");
         return l.v != 0xff ? psio::codec_ok() : psio::codec_fail("Leaf: ff", 0, "Leaf");
      }
      PSIO_REFLECT(Leaf, definitionWillNotChange(), invariant(check), v)
   };
   // Its check relies on its members' checks having passed.
   struct Branch
   {
      Leaf        left;
      Leaf        right;
      friend bool operator==(const Branch&, const Branch&) = default;
      static psio::codec_status check(const Branch& b) noexcept
      {
         log().push_back("Branch");
         return b.left.v <= b.right.v ? psio::codec_ok()
                                      : psio::codec_fail("Branch: unordered", 0, "Branch");
      }
      PSIO_REFLECT(Branch, left, right, invariant(check))
   };
   // A transparent wrapper with an invariant over a type with an invariant:
   // one value, two custom ids.
   struct Twig
   {
      Leaf        leaf;
      friend bool operator==(const Twig&, const Twig&) = default;
      static psio::codec_status check(const Twig& t) noexcept
      {
         log().push_back("Twig");
         return t.leaf.v != 0 ? psio::codec_ok() : psio::codec_fail("Twig: zero", 0, "Twig");
      }
      PSIO_REFLECT(Twig, transparent(), invariant(check), leaf)
   };

   // Types that are not reflected records: PSIO_INVARIANT.
   using Span = std::tuple<std::uint32_t, std::uint32_t>;  // begin <= end
   inline psio::codec_status check_span(const Span& s) noexcept
   {
      return std::get<0>(s) <= std::get<1>(s) ? psio::codec_ok()
                                              : psio::codec_fail("Span: reversed", 0, "Span");
   }
   using Choice = std::variant<std::uint8_t, std::string>;  // a string is not "bad"
   enum class Parity : std::uint8_t {};                     // no enumerator names

   // No invariant anywhere.
   struct Free
   {
      std::vector<std::optional<std::tuple<std::uint8_t, std::string>>> items;
      PSIO_REFLECT(Free, items)
   };
   // A recursive type with an invariant below.
   struct Node
   {
      Even              value;
      std::vector<Node> children;
      PSIO_REFLECT(Node, value, children)
   };
   template <typename T>
   struct holder_of
   {
      T held;
   };
   template <typename T>
   concept has_inner = requires { typename psio::transparent_inner_t<T>; };
   // A transparent wrapper of a composite key.
   struct Wrapped
   {
      std::tuple<std::uint32_t, std::string> key;
      PSIO_REFLECT(Wrapped, transparent(), key)
   };
}  // namespace ct::deep::er

PSIO_INVARIANT(ct::deep::er::Span, ct::deep::er::check_span, "Span")
PSIO_INVARIANT(ct::deep::er::Choice,
               ([](const ct::deep::er::Choice& c) noexcept {
                  const auto* s = std::get_if<std::string>(&c);
                  return s && *s == "bad" ? psio::codec_fail("Choice: bad", 0, "Choice")
                                          : psio::codec_ok();
               }),
               "Choice")
PSIO_INVARIANT(ct::deep::er::Parity,
               ([](const ct::deep::er::Parity& p) noexcept {
                  return static_cast<unsigned>(p) % 2 == 0
                            ? psio::codec_ok()
                            : psio::codec_fail("Parity: odd", 0, "Parity");
               }),
               "Parity")
// The extension point of the transitive query.
template <typename T>
struct psio::invariant_children<ct::deep::er::holder_of<T>>
{
   using type = std::tuple<T>;
};

using namespace ct::deep::er;

namespace
{
   template <typename T>
   std::vector<char> pssz_of(const T& v)
   {
      return psio::encode(psio::pssz{}, v);
   }
   template <typename T>
   std::vector<char> unchecked(const T& v)
   {
      std::vector<char> out;
      REQUIRE(pi::encode_ignoring_invariants(psio::pssz{}, v, out).ok());
      return out;
   }
   std::span<const char> sp(const std::vector<char>& b) { return {b.data(), b.size()}; }
   std::span<const char> sp(const std::string& s) { return {s.data(), s.size()}; }

   const st::AnyType& named(const psio::Schema& schema, std::string_view name)
   {
      for (const auto& t : schema.types)
         if (t.name == name) return *t.type;
      FAIL("no type " << name);
      static const st::AnyType none;
      return none;
   }
   // What `type` is once named aliases are followed.
   const st::AnyType& resolved(const psio::Schema& schema, const st::AnyType& type)
   {
      const st::AnyType* at = &type;
      while (const auto* ref = std::get_if<st::Type>(&at->value)) at = &named(schema, ref->type);
      return *at;
   }
}  // namespace

// ── G3: transparent types ──────────────────────────────────────────────────

TEST_CASE("transparent: the traits", "[transparent]")
{
   static_assert(psio::is_transparent_v<Id>);
   static_assert(psio::is_transparent_v<Secret>);
   static_assert(psio::is_transparent_v<Height>);
   static_assert(psio::is_transparent_v<Weight>);
   static_assert(psio::is_transparent_v<const Tall&>);
   static_assert(!psio::is_transparent_v<Capped>);
   static_assert(!psio::is_transparent_v<std::uint64_t>);
   static_assert(!psio::is_transparent_v<std::vector<Id>>);
   static_assert(!psio::is_transparent_v<Level>);
   static_assert(std::is_same_v<psio::transparent_inner_t<Id>, std::uint64_t>);
   static_assert(std::is_same_v<psio::transparent_inner_t<Secret>, std::string>);
   static_assert(std::is_same_v<psio::transparent_inner_t<Tall>, Height>);
   static_assert(has_inner<Id> && !has_inner<Capped> && !has_inner<int>);  // SFINAE-friendly

   Id id{7};
   psio::transparent_inner(id) = 9;
   CHECK(id.value == 9);
   const Secret s{"x"};
   CHECK(psio::transparent_inner(s) == "x");
   // It stays a reflected type with its own name, members and methods.
   static_assert(psio::Reflected<Secret>);
   CHECK(psio::reflect<Secret>::name == "Secret");
   CHECK(psio::reflect<Secret>::member_count == 1);
   CHECK(psio::reflect<Secret>::method_count == 1);
   CHECK(std::string_view{psio::get_type_name(static_cast<const Id*>(nullptr))} == "Id");
}

TEST_CASE("transparent: pssz layout is the member's", "[transparent][pssz]")
{
   static_assert(pi::is_fixed_v<Id> && pi::fixed_size_of<Id>() == 8);
   static_assert(pi::is_fixed_v<Tall> && pi::fixed_size_of<Tall>() == 4);
   static_assert(!pi::is_fixed_v<Secret>);
   static_assert(!pi::is_fixed_v<CappedBox>);
   static_assert(!pi::Record<Id>);
   static_assert(psio::auto_pssz_width_v<Id> == psio::auto_pssz_width_v<std::uint64_t>);
   static_assert(psio::auto_pssz_width_v<Secret> == psio::auto_pssz_width_v<std::string>);
   // The member's declared budget carries through.
   static_assert(psio::auto_pssz_width_v<Capped> == 2);
   static_assert(psio::auto_pssz_width_v<CappedBox> == 2);
   static_assert(psio::inferred_bound<Id>() == psio::inferred_bound<std::uint64_t>());
   static_assert(psio::inferred_bound<CappedBox>() == 300);
   static_assert(psio::auto_pssz_width_v<Holder> == psio::auto_pssz_width_v<Plain>);
   static_assert(psio::inferred_bound<Holder>() == psio::inferred_bound<Plain>());
   // The optional selector rule is the member's: none for a positive
   // minimum size, one for a payload that can be empty.
   static_assert(!pi::optional_uses_selector_v<Id>);
   static_assert(pi::optional_uses_selector_v<Secret>);
   static_assert(pi::optional_uses_selector_v<Blob>);

   CHECK(pssz_of(Id{0x0102030405060708}) == pssz_of(std::uint64_t{0x0102030405060708}));
   CHECK(pssz_of(Secret{"hello"}) == pssz_of(std::string{"hello"}));
   CHECK(pssz_of(Secret{}) == pssz_of(std::string{}));
   CHECK(pssz_of(Tall{Height{77}}) == pssz_of(std::uint32_t{77}));
   CHECK(pssz_of(CappedBox{Capped{5, "memo"}}) == pssz_of(Capped{5, "memo"}));
   CHECK(pssz_of(std::optional<Id>{}).empty());
   CHECK(pssz_of(std::optional<Id>{Id{1}}).size() == 8);
   CHECK(pssz_of(std::optional<Secret>{Secret{}}) == std::vector<char>{1});
   CHECK(pssz_of(std::optional<Secret>{}) == std::vector<char>{0});
   // A list of fixed wrappers is the contiguous list of members.
   const std::vector<Id>            ids{Id{1}, Id{2}, Id{3}};
   const std::vector<std::uint64_t> raw{1, 2, 3};
   CHECK(pssz_of(ids) == pssz_of(raw));
   CHECK(psio::decode<std::vector<Id>>(psio::pssz{}, sp(pssz_of(raw))) == ids);

   const Holder h{Height{1}, Secret{"s"}, Height{2}, Secret{"t"}, {Height{3}, Height{4}},
                  {Weight{5}, Weight{6}}, CappedBox{Capped{7, "m"}}, {Tall{Height{8}}, Secret{"u"}}};
   const Plain  p{1, "s", 2, "t", {3, 4}, {5, 6}, Capped{7, "m"}, {8, "u"}};
   for (int explicit_width = 0; explicit_width < 2; ++explicit_width)
   {
      const auto a = explicit_width ? psio::encode(psio::pssz16{}, h) : pssz_of(h);
      const auto b = explicit_width ? psio::encode(psio::pssz16{}, p) : pssz_of(p);
      CHECK(a == b);
   }
   const auto bytes = pssz_of(h);
   CHECK(psio::size_of(psio::pssz{}, h) == bytes.size());
   CHECK(psio::validate<Holder>(psio::pssz{}, sp(bytes)).ok());
   CHECK(psio::validate_strict<Holder>(psio::pssz{}, sp(bytes)).ok());
   CHECK(psio::decode<Holder>(psio::pssz{}, sp(bytes)) == h);
   Holder back;
   bool   unknown = true;
   REQUIRE(psio::decode_pssz_checked(psio::pssz{}, sp(bytes), back, unknown).ok());
   CHECK(back == h);
   CHECK_FALSE(unknown);
   std::vector<char> checked;
   REQUIRE(psio::encode_pssz_checked(psio::pssz{}, h, checked).ok());
   CHECK(checked == bytes);
   // A struct-framed value is not the wrapper's encoding.
   CHECK_FALSE(psio::validate<Id>(psio::pssz{}, sp(std::vector<char>(9, 0))).ok());
}

TEST_CASE("transparent: the view is the member's view", "[transparent][pssz][view]")
{
   using V = psio::storage;
   static_assert(std::is_base_of_v<psio::view<std::uint64_t, psio::pssz, V::const_borrow>,
                                   psio::view<Id, psio::pssz, V::const_borrow>>);
   static_assert(std::is_base_of_v<psio::view<std::string, psio::pssz, V::const_borrow>,
                                   psio::view<Secret, psio::pssz, V::const_borrow>>);
   const Holder h{Height{1}, Secret{"secret"}, Height{2}, {}, {Height{3}, Height{4}},
                  {Weight{5}, Weight{6}}, CappedBox{Capped{7, "m"}}, {Tall{Height{8}}, Secret{"u"}}};
   const auto   bytes = pssz_of(h);
   const psio::view<Holder, psio::pssz, V::const_borrow> view{sp(bytes)};
   CHECK(view.get<0>().get() == 1);                          // Height: a u32
   CHECK(psio::bytes(view.get<1>()).size() == 6);            // Secret: a string
   CHECK(view.get<4>().size() == 2);                         // list of Height
   CHECK(view.get<4>()[1].get() == 4);
   CHECK(view.get<6>().get<0>() == 7);                       // CappedBox: Capped's fields
   const auto id_bytes = pssz_of(Id{99});
   CHECK(psio::view<Id, psio::pssz, V::const_borrow>{sp(id_bytes)}.get() == 99);
}

TEST_CASE("transparent: the evolution check keeps the type's identity", "[transparent][pssz]")
{
   static_assert(psio::pssz_evolves_from<Id, Id>);
   static_assert(!psio::pssz_evolves_from<Id, std::uint64_t>);
   static_assert(!psio::pssz_evolves_from<std::uint64_t, Id>);
   static_assert(!psio::pssz_evolves_from<Height, Tall>);  // different names
   static_assert(!psio::pssz_evolves_from<Blob, DerKey>);  // same member type, different types
   static_assert(psio::pssz_evolves_from<std::vector<Id>, std::vector<Id>>);
   static_assert(!psio::pssz_evolves_from<std::vector<Id>, std::vector<std::uint64_t>>);
   SUCCEED();
}

TEST_CASE("transparent: key is the member's key", "[transparent][key]")
{
   namespace ki = psio::detail::key_impl;
   static_assert(ki::key_fixed_size<Id>() == std::size_t{8});
   static_assert(!ki::key_fixed_size<Secret>().has_value());
   static_assert(!ki::Record<Id>);
   CHECK(psio::encode(psio::key{}, Id{258}) == psio::encode(psio::key{}, std::uint64_t{258}));
   CHECK(psio::encode(psio::key{}, Secret{"a\0b"}) ==
         psio::encode(psio::key{}, std::string{"a\0b"}));
   const auto fixed = psio::to_key(Tall{Height{3}});
   static_assert(std::tuple_size_v<decltype(fixed)> == 4);
   const auto key = psio::encode(psio::key{}, Secret{"hello"});
   CHECK(psio::validate<Secret>(psio::key{}, sp(key)).ok());
   CHECK(psio::decode<Secret>(psio::key{}, sp(key)) == Secret{"hello"});
   CHECK(psio::size_of(psio::key{}, Secret{"hello"}) == key.size());
   // A transparent wrapper of a composite key is that composite key; of
   // anything else it is not composite.
   static_assert(ki::Composite<Wrapped>);
   static_assert(!ki::Composite<Id>);
   static_assert(!ki::Composite<Secret>);
   CHECK(psio::key_prefix<Wrapped>(7u) ==
         psio::key_prefix<std::tuple<std::uint32_t, std::string>>(7u));
   CHECK(psio::key_prefix<Wrapped>(7u, "a") == psio::encode(psio::key{}, Wrapped{{7, "a"}}));
}

TEST_CASE("transparent: json and ijson are the member's text", "[transparent][json]")
{
   CHECK(psio::encode(psio::json{}, Id{42}) == "42");
   CHECK(psio::encode(psio::ijson{}, Id{~std::uint64_t{0}}) == "\"18446744073709551615\"");
   CHECK(psio::encode(psio::json{}, Secret{"s"}) == "\"s\"");
   CHECK(psio::encode(psio::json{}, Tall{Height{5}}) == "5");
   CHECK(psio::encode(psio::json{}, CappedBox{Capped{1, "m"}}) == R"({"id":1,"memo":"m"})");
   CHECK(psio::encode(psio::ijson{}, DerKey{{0x30, 0xff}}) == "\"30FF\"");
   CHECK(psio::encode(psio::json{}, DerKey{{0x30, 0xff}}) == "[48,255]");
   Id id;
   REQUIRE(psio::decode_json_checked(psio::json{}, sp(std::string{"7"}), id).ok());
   CHECK(id == Id{7});
   CHECK_FALSE(psio::validate<Id>(psio::json{}, sp(std::string{R"({"value":7})"})).ok());
   // The enclosing member's integer form reaches the wrapped integer.
   const Forms f{Id{5}, Id{~std::uint64_t{0}}, 9};
   CHECK(psio::encode(psio::json{}, f) == R"({"a":"5","b":18446744073709551615,"c":9})");
   CHECK(psio::encode(psio::ijson{}, f) == R"({"a":5,"b":18446744073709551615,"c":9})");
   Forms back;
   REQUIRE(psio::decode_json_checked(psio::json{}, sp(psio::encode(psio::json{}, f)), back).ok());
   CHECK(back.a == f.a);
   CHECK_FALSE(psio::validate<Forms>(psio::json{}, sp(std::string{R"({"a":5,"b":1,"c":9})"})).ok());
   // An I-JSON variant names a transparent alternative by its own name.
   const std::variant<Id, Secret> v{Secret{"x"}};
   CHECK(psio::encode(psio::ijson{}, v) == R"({"Secret":"x"})");
}

TEST_CASE("transparent: the Schema IR node is a named alias", "[transparent][schema]")
{
   const auto schema = psio::SchemaBuilder{}.insert<Holder>("Holder").insert<Id>("Id").build();
   // Id -> (an alias of) Int{64}: nothing marks it.
   const auto& id = named(schema, "Id");
   REQUIRE(std::holds_alternative<st::Type>(id.value));
   const auto* integer = std::get_if<st::Int>(&resolved(schema, id).value);
   REQUIRE(integer);
   CHECK(integer->bits == 64);
   CHECK_FALSE(integer->isSigned);
   // Tall -> Height -> u32.
   const auto& tall = named(schema, "Tall");
   REQUIRE(std::get_if<st::Type>(&tall.value));
   CHECK(std::get<st::Type>(tall.value).type == "Height");
   // The schema-driven codec reads the typed bytes and writes the typed text.
   const Holder h{Height{1}, Secret{"s"}, Height{2}, Secret{"t"}, {Height{3}, Height{4}},
                  {Weight{5}, Weight{6}}, CappedBox{Capped{7, "m"}}, {Tall{Height{8}}, Secret{"u"}}};
   const auto   bytes = pssz_of(h);
   std::size_t  width = 0;
   REQUIRE(ir::auto_width(schema, st::Type{"Holder"}, width).ok());
   CHECK(width == psio::auto_pssz_width_v<Holder>);
   REQUIRE(ir::validate_pssz(schema, st::Type{"Holder"}, sp(bytes)).ok());
   for (int ij = 0; ij < 2; ++ij)
   {
      std::string       text;
      std::vector<char> again;
      if (ij)
      {
         REQUIRE(ir::pssz_to_json(schema, st::Type{"Holder"}, psio::ijson{}, sp(bytes), text).ok());
         CHECK(text == psio::encode(psio::ijson{}, h));
         REQUIRE(ir::json_to_pssz(schema, st::Type{"Holder"}, psio::ijson{}, sp(text), again).ok());
      }
      else
      {
         REQUIRE(ir::pssz_to_json(schema, st::Type{"Holder"}, psio::json{}, sp(bytes), text).ok());
         CHECK(text == psio::encode(psio::json{}, h));
         REQUIRE(ir::json_to_pssz(schema, st::Type{"Holder"}, psio::json{}, sp(text), again).ok());
      }
      CHECK(again == bytes);
   }
   // A strict registry has nothing to say about a plain alias.
   ir::CustomHandlers strict;
   strict.require_all = true;
   CHECK(ir::validate_pssz(schema, st::Type{"Holder"}, sp(bytes), nullptr, &strict).ok());
}

// ── G4: a custom text form for a type of any shape ─────────────────────────

TEST_CASE("custom text: any shape, typed", "[custom][json]")
{
   static_assert(psio::schema_custom_v<Endpoint>);  // a variant
   static_assert(psio::schema_custom_v<Ratio>);     // a tuple
   static_assert(psio::schema_custom_v<Blob>);      // a transparent wrapper
   static_assert(psio::schema_custom_v<Ref>);       // a transparent wrapper of a variant
   static_assert(!psio::schema_custom_v<Id>);
   static_assert(!psio::schema_custom_v<std::variant<std::uint64_t, std::string>>);
   static_assert(!psio::schema_custom_v<psio::uint128>);
   static_assert(!psio::schema_custom_v<psio::uint256>);
   CHECK(psio::schema_custom_id<Endpoint>() == "Endpoint");
   CHECK(psio::schema_custom_id<Ratio>() == "Ratio");
   CHECK(psio::schema_custom_id<Blob>() == "Blob");

   const Endpoint local{V4{{127, 0, 0, 1}, 8080}};
   for (int ij = 0; ij < 2; ++ij)
   {
      INFO(ij);
      auto text = [&](const auto& v) {
         return ij ? psio::encode(psio::ijson{}, v) : psio::encode(psio::json{}, v);
      };
      CHECK(text(local) == R"("127.0.0.1:8080")");
      CHECK(text(Ratio{-3, 4}) == R"("-3/4")");
      CHECK(text(Blob{{'M', 'a'}}) == R"("TWE=")");
      CHECK(text(Ref{std::string{"alice"}}) == R"("@alice")");
      CHECK(text(Ref{std::uint64_t{7}}) == R"("#7")");
      // The adapter applies to that C++ type wherever it is used ...
      CHECK(text(std::vector<Endpoint>{local}) == R"(["127.0.0.1:8080"])");
      CHECK(text(std::optional<Ratio>{Ratio{1, 2}}) == R"("1/2")");
      CHECK(text(std::tuple<Ratio, Blob>{Ratio{1, 2}, Blob{}}) == R"(["1/2",""])");
   }
   // ... while the bare variant a wrapper holds keeps its own form: the
   // wrapper, not the variant, is the type with the identity.
   CHECK(psio::encode(psio::json{}, std::variant<std::uint64_t, std::string>{std::uint64_t{7}}) ==
         "[0,7]");
   // Binary and key forms do not involve the text.
   CHECK(pssz_of(local) == std::vector<char>{0, 127, 0, 0, 1, char(0x90), 0x1f});
   CHECK(psio::encode(psio::key{}, Ratio{-3, 4}) ==
         psio::encode(psio::key{}, std::tuple<std::int16_t, std::uint16_t>{-3, 4}));

   Endpoint e;
   REQUIRE(psio::decode_json_checked(psio::ijson{},
                                     sp(std::string{R"("[0000:0000:0000:0000:0000:0000:0000:0001]:9")"}),
                                     e)
              .ok());
   REQUIRE(std::holds_alternative<V6>(e));
   CHECK(std::get<V6>(e).address[15] == 1);
   CHECK(std::get<V6>(e).port == 9);
   auto bad = psio::validate<Endpoint>(psio::json{}, sp(std::string{R"("1.2.3.4")"}));
   REQUIRE_FALSE(bad.ok());
   CHECK(bad.error().what == "Endpoint: expected address:port");
   CHECK_FALSE(psio::validate<Endpoint>(psio::json{}, sp(std::string{R"([0,{}])"})).ok());
   CHECK_FALSE(psio::validate<Ratio>(psio::json{}, sp(std::string{"[1,2]"})).ok());
}

TEST_CASE("custom text: any shape, in the schema and through it", "[custom][schema]")
{
   const auto schema = psio::SchemaBuilder{}
                          .insert<Guarded>("Guarded")
                          .insert<Endpoint>("Endpoint")
                          .insert<Ratio>("Ratio")
                          .insert<Level>("Level")
                          .build();
   // Custom{the type's wire type, id}, whatever the shape.
   auto custom = [&](std::string_view name) -> const st::Custom& {
      const auto* c = std::get_if<st::Custom>(&resolved(schema, named(schema, name)).value);
      REQUIRE(c);
      return *c;
   };
   CHECK(custom("Endpoint").id == "Endpoint");
   CHECK(std::holds_alternative<st::Variant>(custom("Endpoint").type->value));
   CHECK(custom("Ratio").id == "Ratio");
   CHECK(std::holds_alternative<st::Tuple>(custom("Ratio").type->value));
   // A transparent wrapper with a text form: Custom over the member's type.
   CHECK(custom("Blob").id == "Blob");
   {
      const auto* inner = std::get_if<st::Custom>(&resolved(schema, *custom("Blob").type).value);
      REQUIRE(inner);
      CHECK(inner->id == "hex");
   }
   CHECK(custom("Ref").id == "Ref");
   CHECK(std::holds_alternative<st::Variant>(resolved(schema, *custom("Ref").type).value));
   // An invariant alone makes a type custom too.
   CHECK(custom("Name").id == "Name");
   CHECK(std::holds_alternative<st::Object>(custom("Name").type->value));
   CHECK(custom("Level").id == "Level");
   CHECK(std::holds_alternative<st::Enum>(custom("Level").type->value));
   CHECK(custom("DerKey").id == "DerKey");

   ir::CustomHandlers handlers;
   for_each_custom([&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
   const Guarded value = guarded_sample();
   const auto    bytes = pssz_of(value);
   const st::AnyType root = st::Type{"Guarded"};
   std::string text, plain;
   REQUIRE(ir::pssz_to_json(schema, root, psio::ijson{}, sp(bytes), text, nullptr, &handlers).ok());
   CHECK(text == psio::encode(psio::ijson{}, value));
   REQUIRE(ir::pssz_to_json(schema, root, psio::ijson{}, sp(bytes), plain).ok());
   CHECK(plain != text);
   CHECK(plain.find(R"("endpoint":{"V4":{"address":"7F000001","port":8080}})") != plain.npos);
   CHECK(plain.find(R"("ratios":[[-3,4],[0,1]])") != plain.npos);
   CHECK(plain.find(R"("ref":{"string":"alice"})") != plain.npos);
   std::vector<char> again;
   REQUIRE(ir::json_to_pssz(schema, root, psio::ijson{}, sp(text), again, nullptr,
                            psio::json_parser::automatic, &handlers)
              .ok());
   CHECK(again == bytes);
   again.clear();
   REQUIRE(ir::json_to_pssz(schema, root, psio::ijson{}, sp(plain), again).ok());
   CHECK(again == bytes);
   // Handlers bound once to the schema's nodes give the same text.
   const ir::JsonNames names(schema, handlers);
   std::string         bound;
   REQUIRE(
      ir::pssz_to_json(schema, root, psio::ijson{}, sp(bytes), bound, nullptr, nullptr, &names)
         .ok());
   CHECK(bound == text);
   // A typed text is not the wire type's text and the reverse.
   again.clear();
   CHECK_FALSE(ir::json_to_pssz(schema, root, psio::ijson{}, sp(text), again).ok());
   CHECK_FALSE(ir::json_to_pssz(schema, root, psio::ijson{}, sp(plain), again, nullptr,
                                psio::json_parser::automatic, &handlers)
                  .ok());
   CHECK(again.empty());
}

// ── G5: invariants ─────────────────────────────────────────────────────────

TEST_CASE("invariant: the query surface", "[invariant]")
{
   static_assert(psio::has_invariant_v<Name>);
   static_assert(psio::has_invariant_v<const Name&>);
   static_assert(psio::has_invariant_v<DerKey>);
   static_assert(psio::has_invariant_v<Range>);
   static_assert(psio::has_invariant_v<Level>);
   static_assert(psio::has_invariant_v<Even>);
   static_assert(psio::has_invariant_v<Span>);
   static_assert(psio::has_invariant_v<Choice>);
   static_assert(psio::has_invariant_v<Parity>);
   static_assert(!psio::has_invariant_v<Id>);
   static_assert(!psio::has_invariant_v<Guarded>);
   static_assert(!psio::has_invariant_v<std::vector<Name>>);
   static_assert(!psio::has_invariant_v<std::uint64_t>);
   static_assert(!psio::has_invariant_v<std::tuple<std::uint32_t, std::uint64_t>>);

   static_assert(psio::contains_invariant_v<Name>);
   static_assert(psio::contains_invariant_v<Guarded>);
   static_assert(psio::contains_invariant_v<std::vector<std::optional<Name>>>);
   static_assert(psio::contains_invariant_v<std::array<Level, 3>>);
   static_assert(psio::contains_invariant_v<std::variant<int, std::tuple<Id, DerKey>>>);
   static_assert(psio::contains_invariant_v<std::pair<int, Range>>);
   static_assert(psio::contains_invariant_v<std::map<std::string, Even>>);
   static_assert(psio::contains_invariant_v<Twig>);
   static_assert(psio::contains_invariant_v<Node>);  // recursive
   static_assert(psio::contains_invariant_v<st::Box<Even>>);
   static_assert(psio::contains_invariant_v<holder_of<Even>>);  // the extension point
   static_assert(!psio::contains_invariant_v<holder_of<int>>);
   static_assert(!psio::contains_invariant_v<Free>);
   static_assert(!psio::contains_invariant_v<Id>);
   static_assert(!psio::contains_invariant_v<Endpoint>);
   static_assert(!psio::contains_invariant_v<std::string>);

   // "A leaf reached through these types sits inside a value an invariant
   // constrains."
   static_assert(psio::any_has_invariant_v<Guarded, Range, std::uint32_t>);
   static_assert(!psio::any_has_invariant_v<Guarded, Id, std::uint64_t>);
   static_assert(!psio::any_has_invariant_v<>);

   CHECK(psio::check_invariant(Name{1}).ok());
   auto bad = psio::check_invariant(Even{3});
   REQUIRE_FALSE(bad.ok());
   CHECK(bad.error().what == "Even: odd");
   CHECK(bad.error().byte_offset == 3);
   CHECK(bad.error().format_name == "Even");
   CHECK(psio::check_invariant(std::uint64_t{7}).ok());  // no invariant: ok
   CHECK(psio::check_invariant(Free{}).ok());
   CHECK_FALSE(psio::check_invariant(Span{2, 1}).ok());
   CHECK_FALSE(psio::check_invariant(Choice{std::string{"bad"}}).ok());
   CHECK(psio::check_invariant(Choice{std::string{"good"}}).ok());
   CHECK_FALSE(psio::check_invariant(static_cast<Parity>(3)).ok());
   CHECK(psio::invariant_schema_id<Span>() == "Span");
   CHECK(psio::invariant_schema_id<Name>().empty());
   CHECK(psio::schema_custom_id<Span>() == "Span");
   CHECK(psio::schema_custom_id<Even>() == "Even");
   CHECK(psio::schema_custom_id<Level>() == "Level");
}

namespace
{
   // Every checked pssz entry point rejects `bytes` with `reason`; the
   // trusted ones read it.
   template <typename Fmt, typename T>
   void pssz_rejects(const std::vector<char>& bytes, std::string_view reason)
   {
      auto status = psio::validate<T>(Fmt{}, sp(bytes));
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      status = psio::validate_strict<T>(Fmt{}, sp(bytes));
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      bool unknown = false;
      status       = psio::validate_pssz_compatible<T>(Fmt{}, sp(bytes), unknown);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      T value{};
      status = psio::decode_pssz_checked(Fmt{}, sp(bytes), value, unknown);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      try
      {
         psio::validate_or_throw<T>(Fmt{}, sp(bytes));
         FAIL("validate_or_throw accepted");
      }
      catch (const psio::codec_exception& e)
      {
         CHECK(e.error().what == reason);
      }
      // Skippable only by the explicitly unchecked readers.
      value      = psio::decode<T>(Fmt{}, sp(bytes));
      auto boxed = psio::make_boxed<T>(Fmt{}, sp(bytes));
      REQUIRE(boxed);
      // No writer writes the value back.
      std::vector<char> out{'x'};
      status = psio::encode_pssz_checked(Fmt{}, value, out);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      CHECK(out == std::vector<char>{'x'});
      try
      {
         (void)psio::encode(Fmt{}, value);
         FAIL("encode wrote the value");
      }
      catch (const psio::codec_exception& e)
      {
         CHECK(e.error().what == reason);
      }
      try
      {
         psio::encode(Fmt{}, value, out);
         FAIL("sink encode wrote the value");
      }
      catch (const psio::codec_exception&)
      {
      }
      CHECK(out == std::vector<char>{'x'});
   }
   template <typename T>
   void pssz_violates(const T& value, std::string_view reason)
   {
      std::vector<char> bytes;
      REQUIRE(pi::encode_ignoring_invariants(psio::pssz{}, value, bytes).ok());
      pssz_rejects<psio::pssz, T>(bytes, reason);
      for (int w = 0; w < 3; ++w)
      {
         bytes.clear();
         if (w == 0)
         {
            if (!pi::encode_ignoring_invariants(psio::pssz8{}, value, bytes).ok()) continue;
            pssz_rejects<psio::pssz8, T>(bytes, reason);
         }
         else if (w == 1)
         {
            REQUIRE(pi::encode_ignoring_invariants(psio::pssz16{}, value, bytes).ok());
            pssz_rejects<psio::pssz16, T>(bytes, reason);
         }
         else
         {
            REQUIRE(pi::encode_ignoring_invariants(psio::pssz32{}, value, bytes).ok());
            pssz_rejects<psio::pssz32, T>(bytes, reason);
         }
      }
   }
}  // namespace

TEST_CASE("invariant: pssz readers and writers", "[invariant][pssz]")
{
   pssz_violates(Name{std::uint64_t{1} << 60}, "Name: value is not a canonical name");
   pssz_violates(DerKey{}, "DerKey: not a DER sequence");
   pssz_violates(DerKey{{0x31}}, "DerKey: not a DER sequence");
   pssz_violates(Range{2, 1}, "Range: lo is greater than hi");
   pssz_violates(static_cast<Level>(9), "Level: not a listed level");
   pssz_violates(Even{3}, "Even: odd");
   pssz_violates(Span{2, 1}, "Span: reversed");
   pssz_violates(Choice{std::string{"bad"}}, "Choice: bad");
   pssz_violates(static_cast<Parity>(1), "Parity: odd");
   pssz_violates(Ref{std::string{}}, "Ref: a name is not empty");
   // Where the value occurs: lists, arrays, optionals, variants, tuples,
   // records, transparent wrappers, boxes.
   pssz_violates(std::vector<Name>{Name{1}, Name{~std::uint64_t{0}}},
                 "Name: value is not a canonical name");
   pssz_violates(std::optional<Even>{Even{1}}, "Even: odd");
   pssz_violates(std::array<Even, 2>{Even{0}, Even{1}}, "Even: odd");
   // An enum with an invariant is checked element by element, never as a
   // block of integers.
   pssz_violates(std::vector<Level>{Level::low, static_cast<Level>(4)}, "Level: not a listed level");
   pssz_violates(std::array<Level, 3>{Level::low, Level::top, static_cast<Level>(4)},
                 "Level: not a listed level");
   pssz_violates(std::vector<Parity>{static_cast<Parity>(2), static_cast<Parity>(3)},
                 "Parity: odd");
   pssz_violates(std::variant<std::uint8_t, Range>{Range{5, 4}}, "Range: lo is greater than hi");
   pssz_violates(std::tuple<Id, Span>{Id{1}, Span{9, 8}}, "Span: reversed");
   pssz_violates(std::vector<std::optional<std::tuple<Choice>>>{std::nullopt,
                                                               std::tuple<Choice>{"bad"}},
                 "Choice: bad");
   pssz_violates(st::Box<Even>{Even{5}}, "Even: odd");
   {
      Guarded g = guarded_sample();
      g.key     = DerKey{{0}};
      pssz_violates(g, "DerKey: not a DER sequence");
      g        = guarded_sample();
      g.either = Name{std::uint64_t{1} << 61};
      pssz_violates(g, "Name: value is not a canonical name");
   }
   // Values that satisfy their invariants are unaffected.
   const Guarded good  = guarded_sample();
   const auto    bytes = pssz_of(good);
   CHECK(psio::validate<Guarded>(psio::pssz{}, sp(bytes)).ok());
   CHECK(psio::validate_strict<Guarded>(psio::pssz{}, sp(bytes)).ok());
   CHECK(psio::decode<Guarded>(psio::pssz{}, sp(bytes)) == good);
   CHECK(unchecked(good) == bytes);
   // A structural error is reported as such: the invariant never sees a
   // malformed value.
   auto status = psio::validate<Name>(psio::pssz{}, sp(std::vector<char>(7, char(0xff))));
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().what == "pssz: record fixed region truncated");
   // Views do not run invariants: they assume validated bytes.
   const auto bad = unchecked(Even{3});
   CHECK(psio::view<Even, psio::pssz, psio::storage::const_borrow>{sp(bad)}.get<0>() == 3);
}

TEST_CASE("invariant: innermost first, each value once", "[invariant][pssz]")
{
   const Branch ok{Leaf{1}, Leaf{2}};
   log().clear();
   const auto bytes = pssz_of(ok);
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   log().clear();
   REQUIRE(psio::validate<Branch>(psio::pssz{}, sp(bytes)).ok());
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   // A member that fails stops the walk: the enclosing check does not run.
   log().clear();
   const auto bad_leaf = unchecked(Branch{Leaf{1}, Leaf{0xff}});
   auto       status   = psio::validate<Branch>(psio::pssz{}, sp(bad_leaf));
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().what == "Leaf: ff");
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf"});
   log().clear();
   status = psio::validate<Branch>(psio::pssz{}, sp(unchecked(Branch{Leaf{2}, Leaf{1}})));
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().what == "Branch: unordered");
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   // A transparent wrapper: its member's check, then its own.
   log().clear();
   REQUIRE(psio::validate<Twig>(psio::pssz{}, sp(pssz_of(Twig{Leaf{4}}))).ok());
   CHECK(log() == std::vector<std::string>{"Leaf", "Twig", "Leaf", "Twig"});  // encode + validate
   log().clear();
   // The same order in text and in keys.
   const std::string text = psio::encode(psio::json{}, ok);
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   log().clear();
   REQUIRE(psio::validate<Branch>(psio::json{}, sp(text)).ok());
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   log().clear();
   const auto key = psio::encode(psio::key{}, ok);
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   log().clear();
   REQUIRE(psio::validate<Branch>(psio::key{}, sp(key)).ok());
   CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Branch"});
   log().clear();
   // A sizing pass checks nothing; the write does.
   CHECK(psio::size_of(psio::key{}, ok) == key.size());
   CHECK(log().empty());
}

TEST_CASE("invariant: key encode, validate and decode", "[invariant][key]")
{
   auto rejects = [](const auto& plain, auto type, std::string_view reason) {
      using T        = typename decltype(type)::type;
      const auto key = psio::encode(psio::key{}, plain);
      auto       st  = psio::validate<T>(psio::key{}, sp(key));
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == reason);
      st = psio::validate_strict<T>(psio::key{}, sp(key));
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == reason);
      CHECK_THROWS_AS(psio::decode<T>(psio::key{}, sp(key)), psio::codec_exception);
   };
   rejects(std::uint64_t{1} << 60, std::type_identity<Name>{},
           "Name: value is not a canonical name");
   rejects(std::vector<std::uint8_t>{}, std::type_identity<DerKey>{},
           "DerKey: not a DER sequence");
   rejects(std::uint8_t{200}, std::type_identity<Level>{}, "Level: not a listed level");
   // (Span is std::tuple<u32, u32> itself: the same key bytes from an array.)
   rejects(std::array<std::uint32_t, 2>{3, 2}, std::type_identity<Span>{}, "Span: reversed");
   rejects(std::tuple<std::string, std::optional<std::uint64_t>>{"k", ~std::uint64_t{0}},
           std::type_identity<std::tuple<std::string, std::optional<Name>>>{},
           "Name: value is not a canonical name");

   const Name bad{~std::uint64_t{0}};
   CHECK_THROWS_AS(psio::encode(psio::key{}, bad), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_key(bad), psio::codec_exception);
   std::vector<char> sink;
   CHECK_THROWS_AS(psio::encode(psio::key{}, std::tuple<std::string, Name>{"k", bad}, sink),
                   psio::codec_exception);
   try
   {
      (void)psio::to_key(std::vector<Even>{Even{2}, Even{3}});
      FAIL("to_key wrote the value");
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(e.error().what == "Even: odd");
      CHECK(e.error().format_name == "Even");
   }
   // A prefix encodes elements: each is checked as the value it is.
   using Key = std::tuple<Name, std::string>;
   CHECK_THROWS_AS(psio::key_prefix<Key>(bad), psio::codec_exception);
   CHECK(psio::key_prefix<Key>(Name{1}).size() == 8);
   // Good keys are unaffected, and a structural error stays structural.
   const auto key = psio::to_key(Name{5});
   CHECK(psio::decode<Name>(psio::key{}, std::span<const char>{key.data(), key.size()}) == Name{5});
   auto st = psio::validate<Name>(psio::key{}, sp(std::vector<char>(7, 0)));
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == "key: truncated");
   CHECK(st.error().format_name == "key");
}

namespace
{
   template <typename Fmt, typename T>
   void text_rejects(const std::string& text, std::string_view reason)
   {
      INFO(text);
      for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text,
                                psio::json_parser::simdjson, psio::json_parser::simdjson_index})
      {
         T    value{};
         auto status = psio::decode_json_checked(Fmt{}, sp(text), value, parser);
         REQUIRE_FALSE(status.ok());
         CHECK(status.error().what == reason);
         status = psio::validate_json<T>(Fmt{}, sp(text), parser);
         REQUIRE_FALSE(status.ok());
         CHECK(status.error().what == reason);
         std::string padded = text;
         status = psio::decode_json_checked(Fmt{}, psio::padded_json::of(padded), value, parser);
         REQUIRE_FALSE(status.ok());
         CHECK(status.error().what == reason);
         status = psio::validate_json<T>(Fmt{}, psio::padded_json::of(padded), parser);
         REQUIRE_FALSE(status.ok());
         CHECK(status.error().what == reason);
      }
      auto status = psio::validate<T>(Fmt{}, sp(text));
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      status = psio::validate_strict<T>(Fmt{}, sp(text));
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      CHECK(psio::make_boxed<T>(Fmt{}, sp(text)) == nullptr);
      try
      {
         (void)psio::decode<T>(Fmt{}, sp(text));
         FAIL("decode accepted");
      }
      catch (const psio::codec_exception& e)
      {
         CHECK(e.error().what == reason);
      }
   }
   template <typename T>
   void text_refuses(const T& value, std::string_view reason)
   {
      for (int ij = 0; ij < 2; ++ij)
      {
         std::string out = "keep";
         auto status = ij ? psio::encode_json_checked(psio::ijson{}, value, out)
                          : psio::encode_json_checked(psio::json{}, value, out);
         REQUIRE_FALSE(status.ok());
         CHECK(status.error().what == reason);
         CHECK(out == "keep");
         if (ij)
         {
            CHECK_THROWS_AS(psio::encode(psio::ijson{}, value), psio::codec_exception);
            CHECK_THROWS_AS(psio::size_of(psio::ijson{}, value), psio::codec_exception);
         }
         else
         {
            CHECK_THROWS_AS(psio::encode(psio::json{}, value), psio::codec_exception);
            CHECK_THROWS_AS(psio::encode(psio::json{}, value, out), psio::codec_exception);
         }
      }
   }
}  // namespace

TEST_CASE("invariant: json and ijson readers and writers", "[invariant][json]")
{
   text_rejects<psio::json, Name>(R"({"value":1152921504606846976})",
                                  "Name: value is not a canonical name");
   text_rejects<psio::ijson, Name>(R"({"value":"1152921504606846976"})",
                                   "Name: value is not a canonical name");
   text_rejects<psio::json, DerKey>("[]", "DerKey: not a DER sequence");
   text_rejects<psio::ijson, DerKey>(R"("31")", "DerKey: not a DER sequence");
   text_rejects<psio::json, Level>("4", "Level: not a listed level");
   text_rejects<psio::ijson, Level>(R"("9")", "Level: not a listed level");
   text_rejects<psio::json, Span>("[2,1]", "Span: reversed");
   text_rejects<psio::json, Choice>(R"([1,"bad"])", "Choice: bad");
   text_rejects<psio::ijson, Choice>(R"({"string":"bad"})", "Choice: bad");
   // A text adapter validates the text; then the invariant sees the value.
   text_rejects<psio::json, Ref>(R"("@")", "Ref: a name is not empty");
   text_rejects<psio::json, Ref>(R"("alice")", "Ref: expected #number or @name");
   // Nested: lists, optionals, members in any order, repeated keys.
   text_rejects<psio::json, std::vector<Even>>(R"([{"n":2},{"n":4},{"n":5}])", "Even: odd");
   text_rejects<psio::ijson, std::optional<Range>>(R"({"hi":1,"lo":2})",
                                                   "Range: lo is greater than hi");
   text_rejects<psio::json, Range>(R"({"lo":0,"hi":9,"lo":10})", "Range: lo is greater than hi");
   text_rejects<psio::json, Branch>(R"({"left":{"v":2},"right":{"v":1}})", "Branch: unordered");
   text_rejects<psio::json, Branch>(R"({"right":{"v":255},"left":{"v":1}})", "Leaf: ff");
   {
      Guarded g = guarded_sample();
      std::string text = psio::encode(psio::json{}, g);
      const auto  at   = text.find(R"("window":{"lo":3,"hi":9})");
      REQUIRE(at != text.npos);
      text.replace(at, 24, R"("window":{"lo":9,"hi":3})");
      text_rejects<psio::json, Guarded>(text, "Range: lo is greater than hi");
   }
   // Unknown keys are skipped, not checked: they are not values of the type.
   Range r;
   REQUIRE(psio::decode_json_checked(psio::json{},
                                     sp(std::string{R"({"lo":1,"x":{"lo":9,"hi":0},"hi":2})"}), r)
              .ok());
   CHECK(r == Range{1, 2});
   // The status is the invariant's own, unchanged.
   Even e;
   auto status = psio::decode_json_checked(psio::json{}, sp(std::string{R"({"n":7})"}), e);
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().format_name == "Even");
   CHECK(status.error().byte_offset == 3);

   text_refuses(Name{~std::uint64_t{0}}, "Name: value is not a canonical name");
   text_refuses(DerKey{}, "DerKey: not a DER sequence");
   text_refuses(Ref{std::string{}}, "Ref: a name is not empty");
   text_refuses(std::vector<Level>{static_cast<Level>(7)}, "Level: not a listed level");
   text_refuses(std::tuple<Id, std::optional<Span>>{Id{1}, Span{2, 1}}, "Span: reversed");
   {
      Guarded g = guarded_sample();
      g.pair[1] = static_cast<Level>(100);
      text_refuses(g, "Level: not a listed level");
   }
   // A missing member is not an invariant matter.
   CHECK_FALSE(psio::validate<Range>(psio::json{}, sp(std::string{R"({"lo":1})"})).ok());
}

TEST_CASE("invariant: the schema-driven codecs, with and without the handler",
          "[invariant][schema]")
{
   const auto schema = psio::SchemaBuilder{}
                          .insert<Guarded>("Guarded")
                          .insert<Branch>("Branch")
                          .insert<Twig>("Twig")
                          .insert<Span>("Span")
                          .insert<std::vector<Parity>>("Parities")
                          .build();
   ir::CustomHandlers handlers;
   for_each_custom([&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
   handlers.add<Leaf>().add<Branch>().add<Twig>().add<Span>().add<Parity>();
   REQUIRE(handlers.find("Parity"));
   CHECK(handlers.checks_values());
   CHECK_FALSE(ir::CustomHandlers{}.checks_values());

   Guarded g = guarded_sample();
   g.window  = Range{9, 3};
   const auto        bytes = unchecked(g);
   const st::AnyType root  = st::Type{"Guarded"};
   const std::string_view reason = "Range: lo is greater than hi";
   // Without a registry a custom type is its wire type: structure only.
   CHECK(ir::validate_pssz(schema, root, sp(bytes)).ok());
   ir::Value value;
   bool      unknown = false;
   REQUIRE(ir::decode_auto(schema, root, sp(bytes), value, unknown).ok());
   std::vector<char> again;
   REQUIRE(ir::encode_auto(schema, root, value, again).ok());
   CHECK(again == bytes);
   // With it, every entry point checks.
   auto fails = [&](psio::codec_status status) {
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == reason);
      CHECK(status.error().format_name == "Range");
   };
   fails(ir::validate_pssz(schema, root, sp(bytes), nullptr, &handlers));
   fails(ir::validate_auto(schema, root, sp(bytes), nullptr, &handlers));
   ir::Value none;
   fails(ir::decode_auto(schema, root, sp(bytes), none, unknown, &handlers));
   again = {'x'};
   fails(ir::encode_auto(schema, root, value, again, &handlers));
   CHECK(again == std::vector<char>{'x'});
   std::string text = "keep";
   fails(ir::pssz_to_json(schema, root, psio::json{}, sp(bytes), text, nullptr, &handlers));
   fails(ir::value_to_json(schema, root, psio::ijson{}, value, text, &handlers));
   CHECK(text == "keep");
   const ir::JsonNames names(schema, handlers);
   fails(ir::pssz_to_json(schema, root, psio::json{}, sp(bytes), text, nullptr, nullptr, &names));
   // Reading text: the handlers' text for the custom text types.
   std::string typed = psio::encode(psio::json{}, guarded_sample());
   const auto  at    = typed.find(R"("window":{"lo":3,"hi":9})");
   REQUIRE(at != typed.npos);
   typed.replace(at, 24, R"("window":{"lo":9,"hi":3})");
   for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text,
                             psio::json_parser::simdjson, psio::json_parser::simdjson_index})
   {
      again = {'x'};
      fails(ir::json_to_pssz(schema, root, psio::json{}, sp(typed), again, nullptr, parser,
                             &handlers));
      CHECK(again == std::vector<char>{'x'});
      fails(ir::json_to_pssz(schema, root, psio::json{}, sp(typed), again, &names, parser));
      fails(ir::json_to_value(schema, root, psio::json{}, sp(typed), none, nullptr, parser,
                              &handlers));
   }
   // The explicit-width profiles check the same values.
   {
      std::vector<char> wide;
      REQUIRE(pi::encode_ignoring_invariants(psio::pssz32{}, g, wide).ok());
      ir::Value v;
      REQUIRE(ir::decode_compatible<4>(schema, root, sp(wide), v, unknown).ok());
      fails(ir::decode_compatible<4>(schema, root, sp(wide), v, unknown, &handlers));
      fails(ir::decode_exact<4>(schema, root, sp(wide), v, &handlers));
      fails(ir::validate_canonical_for_width<4>(schema, root, sp(wide), &handlers));
      CHECK(ir::validate_canonical_for_width<4>(schema, root, sp(wide)).ok());
      again.clear();
      fails(ir::encode<4>(schema, root, value, again, &handlers));
      CHECK(again.empty());
   }
   // A handler written by hand: only a check.
   {
      ir::CustomHandlers by_hand;
      int                calls = 0;
      by_hand.add("Range", {.check = [&](std::span<const char> pssz) -> psio::codec_status {
                               ++calls;
                               return pssz.size() == 9 ? psio::codec_fail("by hand", 0, "test")
                                                       : psio::codec_ok();
                            }});
      auto status = ir::validate_pssz(schema, root, sp(bytes), nullptr, &by_hand);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "by hand");
      CHECK(calls == 1);
      // Its text stays the wire type's.
      std::string out;
      by_hand.add("Range", {.check = [](std::span<const char>) { return psio::codec_ok(); }});
      REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, sp(bytes), out, nullptr, &by_hand).ok());
      CHECK(out.find(R"("window":{"lo":9,"hi":3})") != out.npos);
   }
   // Strict: a custom type with no handler is an error, on the binary
   // entry points too; the built-in ids and plain aliases are not custom.
   {
      ir::CustomHandlers strict;
      strict.require_all = true;
      CHECK(strict.checks_values());
      const auto good = pssz_of(guarded_sample());
      auto status     = ir::validate_pssz(schema, root, sp(good), nullptr, &strict);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what ==
            std::string_view{"pssz IR: no handler registered for a custom type"});
      status = ir::decode_auto(schema, root, sp(good), none, unknown, &strict);
      REQUIRE_FALSE(status.ok());
      again.clear();
      REQUIRE(ir::decode_auto(schema, root, sp(good), value, unknown).ok());
      status = ir::encode_auto(schema, root, value, again, &strict);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what ==
            std::string_view{"pssz IR: no handler registered for a custom type"});
      // With every handler registered, strict accepts.
      ir::CustomHandlers all = handlers;
      all.require_all        = true;
      CHECK(ir::validate_pssz(schema, root, sp(good), nullptr, &all).ok());
      const auto times = psio::SchemaBuilder{}
                            .insert<std::tuple<std::chrono::seconds, st::Box<Id>, std::string>>("T")
                            .build();
      const auto tbytes =
         pssz_of(std::tuple<std::chrono::seconds, st::Box<Id>, std::string>{
            std::chrono::seconds{5}, st::Box<Id>{Id{1}}, "s"});
      CHECK(ir::validate_pssz(times, st::Type{"T"}, sp(tbytes), nullptr, &strict).ok());
   }
   // Innermost first across a value's custom ids, and nested values first.
   {
      log().clear();
      const auto twig = pssz_of(Twig{Leaf{4}});
      log().clear();
      REQUIRE(ir::validate_pssz(schema, st::Type{"Twig"}, sp(twig), nullptr, &handlers).ok());
      // Leaf's handler (the typed validate of Leaf), then Twig's (the typed
      // validate of Twig, which checks its Leaf first).
      CHECK(log() == std::vector<std::string>{"Leaf", "Leaf", "Twig"});
      auto status = ir::validate_pssz(schema, st::Type{"Twig"}, sp(unchecked(Twig{Leaf{0}})),
                                      nullptr, &handlers);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Twig: zero");
      status = ir::validate_pssz(schema, st::Type{"Twig"}, sp(unchecked(Twig{Leaf{0xff}})),
                                 nullptr, &handlers);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Leaf: ff");
      status = ir::validate_pssz(schema, st::Type{"Branch"},
                                 sp(unchecked(Branch{Leaf{2}, Leaf{1}})), nullptr, &handlers);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Branch: unordered");
   }
   // Types that are not records: a tuple, an enum in a list.
   {
      auto status = ir::validate_pssz(schema, st::Type{"Span"}, sp(unchecked(Span{2, 1})), nullptr,
                                      &handlers);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Span: reversed");
      const std::vector<Parity> odd{static_cast<Parity>(2), static_cast<Parity>(5)};
      status = ir::validate_pssz(schema, st::Type{"Parities"}, sp(unchecked(odd)), nullptr,
                                 &handlers);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Parity: odd");
      CHECK(ir::validate_pssz(schema, st::Type{"Parities"}, sp(unchecked(odd))).ok());
   }
}

TEST_CASE("invariant: text of a custom type through the schema", "[invariant][schema][json]")
{
   const auto schema = psio::SchemaBuilder{}.insert<Ref>("Ref").insert<Guarded>("Guarded").build();
   ir::CustomHandlers handlers;
   for_each_custom([&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
   const st::AnyType ref = st::Type{"Ref"};
   // "@" is this presentation's text for a value the invariant refuses:
   // the handler reads it, the check rejects it, with the invariant's
   // status. Text that is not the presentation at all is the reader's error.
   std::vector<char> out;
   auto status = ir::json_to_pssz(schema, ref, psio::ijson{}, sp(std::string{R"("@")"}), out,
                                  nullptr, psio::json_parser::automatic, &handlers);
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().what == "Ref: a name is not empty");
   CHECK(status.error().format_name == "Ref");
   status = ir::json_to_pssz(schema, ref, psio::ijson{}, sp(std::string{R"("alice")"}), out,
                             nullptr, psio::json_parser::automatic, &handlers);
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().what == "json: text is not a value of the custom type");
   CHECK(out.empty());
   // Writing: the bytes of the empty name have no text with the handler.
   const auto  bytes = unchecked(Ref{std::string{}});
   std::string text;
   status = ir::pssz_to_json(schema, ref, psio::json{}, sp(bytes), text, nullptr, &handlers);
   REQUIRE_FALSE(status.ok());
   CHECK(status.error().what == "Ref: a name is not empty");
   REQUIRE(ir::pssz_to_json(schema, ref, psio::json{}, sp(bytes), text).ok());
   CHECK(text == R"([1,""])");
   // A value nested under a handler with a text form is checked through
   // that handler's bytes: Guarded presented by a handler written by hand.
   {
      psio::Schema wrapped = schema;
      wrapped.insert("G", st::Custom{st::Box<st::AnyType>{st::AnyType{st::Type{"Guarded"}}}, "G"});
      ir::CustomHandlers with_g = handlers;
      with_g.add("G", {[](std::span<const char>, std::string& out) -> psio::codec_status {
                          out += "g";
                          return psio::codec_ok();
                       },
                       [&](std::string_view, std::vector<char>& out) -> psio::codec_status {
                          Guarded g = guarded_sample();
                          g.window  = Range{9, 3};
                          return pi::encode_ignoring_invariants(psio::pssz{}, g, out);
                       }});
      Guarded g = guarded_sample();
      g.window  = Range{9, 3};
      const auto g_bytes = unchecked(g);
      status = ir::pssz_to_json(wrapped, st::Type{"G"}, psio::json{}, sp(g_bytes), text, nullptr,
                                &with_g);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Range: lo is greater than hi");
      ir::Value value;
      bool      unknown = false;
      REQUIRE(ir::decode_auto(wrapped, st::Type{"G"}, sp(g_bytes), value, unknown).ok());
      text   = "keep";
      status = ir::value_to_json(wrapped, st::Type{"G"}, psio::json{}, value, text, &with_g);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Range: lo is greater than hi");
      CHECK(text == "keep");
      out.clear();
      status = ir::json_to_pssz(wrapped, st::Type{"G"}, psio::json{}, sp(std::string{R"("g")"}),
                                out, nullptr, psio::json_parser::automatic, &with_g);
      REQUIRE_FALSE(status.ok());
      CHECK(status.error().what == "Range: lo is greater than hi");
      CHECK(out.empty());
      // A good value is presented by the outer handler.
      text.clear();
      REQUIRE(ir::pssz_to_json(wrapped, st::Type{"G"}, psio::json{},
                               sp(pssz_of(guarded_sample())), text, nullptr, &with_g)
                 .ok());
      CHECK(text == R"("g")");
   }
}

// ── Hostile input ──────────────────────────────────────────────────────────

TEST_CASE("invariant: mutated frames, texts and keys never get past a checked reader",
          "[invariant][fuzz]")
{
   const auto schema = psio::SchemaBuilder{}.insert<Guarded>("Guarded").build();
   ir::CustomHandlers handlers;
   for_each_custom([&]<typename T>(std::type_identity<T>) { handlers.template add<T>(); });
   const st::AnyType root = st::Type{"Guarded"};

   const Guarded     good = guarded_sample();
   const auto        frame = pssz_of(good);
   const std::string text  = psio::encode(psio::ijson{}, good);
   using Key = std::tuple<Name, std::optional<Range>, std::vector<Level>, DerKey, Ref>;
   const auto key = psio::encode(
      psio::key{}, Key{Name{5}, Range{1, 2}, {Level::low, Level::top}, DerKey{{0x30, 9}},
                       Ref{std::string{"a"}}});

   std::mt19937_64 rng(0x6a5d3c2b1a09f8e7ull);
   std::size_t     accepted = 0, by_invariant = 0, rejected = 0;
   auto mutate = [&](auto bytes) {
      const int edits = 1 + static_cast<int>(rng() % 3);
      for (int i = 0; i < edits && !bytes.empty(); ++i)
      {
         const std::size_t at = rng() % bytes.size();
         switch (rng() % 4)
         {
            case 0: bytes[at] = static_cast<char>(rng()); break;
            case 1: bytes[at] = static_cast<char>(bytes[at] ^ (1 << (rng() % 8))); break;
            case 2: bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at)); break;
            default:
               bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                            static_cast<char>(rng()));
         }
      }
      return bytes;
   };
   for (int round = 0; round < 6000; ++round)
   {
      // pssz: whatever validate accepts satisfies every invariant, decodes
      // and re-encodes; the schema-driven validator with the handlers
      // agrees with the typed one.
      {
         const auto bytes  = mutate(frame);
         auto       status = psio::validate<Guarded>(psio::pssz{}, sp(bytes));
         const auto driven = ir::validate_pssz(schema, root, sp(bytes), nullptr, &handlers);
         CHECK(status.ok() == driven.ok());
         bool    unknown = false;
         Guarded value;
         CHECK(psio::decode_pssz_checked(psio::pssz{}, sp(bytes), value, unknown).ok() ==
               status.ok());
         if (status.ok())
         {
            ++accepted;
            std::vector<char> again;
            REQUIRE(psio::encode_pssz_checked(psio::pssz{}, value, again).ok());
            CHECK(again == bytes);
            CHECK(psio::check_invariant(value.owner).ok());
            CHECK(psio::check_invariant(value.key).ok());
            CHECK(psio::check_invariant(value.window).ok());
            CHECK(psio::check_invariant(value.ref).ok());
            for (auto l : value.levels) CHECK(psio::check_invariant(l).ok());
         }
         else if (ir::validate_pssz(schema, root, sp(bytes)).ok())
         {
            // Structurally fine (the schema alone accepts it): only an
            // invariant stands between these bytes and a typed value.
            ++by_invariant;
            const auto& what = status.error().what;
            CHECK((what.starts_with("Name:") || what.starts_with("DerKey:") ||
                   what.starts_with("Range:") || what.starts_with("Level:") ||
                   what.starts_with("Ref:")));
            CHECK(driven.error().what == what);
         }
         else
            ++rejected;
      }
      // ijson text.
      {
         const auto mutated = mutate(text);
         Guarded    value;
         auto status = psio::decode_json_checked(psio::ijson{}, sp(mutated), value);
         CHECK(psio::validate<Guarded>(psio::ijson{}, sp(mutated)).ok() == status.ok());
         std::vector<char> out;
         const auto driven = ir::json_to_pssz(schema, root, psio::ijson{}, sp(mutated), out,
                                              nullptr, psio::json_parser::automatic, &handlers);
         CHECK(driven.ok() == status.ok());
         if (status.ok())
         {
            std::string again;
            REQUIRE(psio::encode_json_checked(psio::ijson{}, value, again).ok());
            CHECK(out == pssz_of(value));
         }
      }
      // key.
      {
         const auto mutated = mutate(key);
         auto       status  = psio::validate<Key>(psio::key{}, sp(mutated));
         if (status.ok())
         {
            const Key value = psio::decode<Key>(psio::key{}, sp(mutated));
            CHECK(psio::encode(psio::key{}, value) == mutated);
         }
         else
            CHECK_THROWS_AS(psio::decode<Key>(psio::key{}, sp(mutated)), psio::codec_exception);
      }
   }
   CHECK(accepted > 0);
   CHECK(by_invariant > 20);
   CHECK(rejected > 1000);
}

// ── GraphQL ────────────────────────────────────────────────────────────────
namespace ct::gq
{
   struct Query
   {
      Id       id{42};
      Endpoint endpoint{V4{{10, 0, 0, 7}, 80}};
      Ratio    ratio{-3, 4};
      Blob     blob{{1, 2, 3}};
      DerKey   key{{0x30, 0x00}};
      Secret   secret{"hush"};
      Ref      ref{std::string{"alice"}};
      Level    level = Level::top;

      Endpoint echo(Endpoint at) const { return at; }
      Id       next(Id after) const { return Id{after.value + 1}; }
      DerKey   der(DerKey key) const { return key; }
      PSIO_REFLECT(Query, id, endpoint, ratio, blob, key, secret, ref, level, method(echo, at),
                   method(next, after), method(der, key))
   };
   using api = psio::graphql::schema<Query>;
}  // namespace ct::gq

TEST_CASE("graphql: custom text types of any shape and transparent leaves are scalars",
          "[graphql][custom][transparent]")
{
   using ct::gq::api;
   const ct::gq::Query q;
   auto run = [&](std::string_view text) {
      std::string out;
      api::run(q, text, out);
      return out;
   };
   CHECK(run("{ id endpoint ratio blob key secret ref level }") ==
         R"({"data":{"id":42,"endpoint":"10.0.0.7:80","ratio":"-3/4","blob":"AQID","key":"3000",)"
         R"("secret":"hush","ref":"@alice","level":"top"}})");
   // Inputs are read by the same typed reader: text form and invariant.
   CHECK(run(R"({ echo(at: "[0000:0000:0000:0000:0000:0000:0000:0001]:9") next(after: 7) })") ==
         R"({"data":{"echo":"[0000:0000:0000:0000:0000:0000:0000:0001]:9","next":8}})");
   CHECK(run(R"({ echo(at: "1.2.3.4") })").find("errors") != std::string::npos);
   CHECK(run(R"({ der(key: "3001") })") == R"({"data":{"der":"3001"}})");
   CHECK(run(R"({ der(key: "0401") })").find("errors") != std::string::npos);
   const std::string sdl = api::sdl();
   CHECK(sdl.find("scalar Endpoint") != std::string::npos);
   CHECK(sdl.find("scalar Ratio") != std::string::npos);
   CHECK(sdl.find("scalar Blob") != std::string::npos);
   CHECK(sdl.find("scalar Id") != std::string::npos);
   CHECK(sdl.find("scalar DerKey") != std::string::npos);
   CHECK(sdl.find("endpoint: Endpoint!") != std::string::npos);
   CHECK(sdl.find("id: Id!") != std::string::npos);
}

// ── schemaId("Id"): a schema id and nothing else ───────────────────────────
//
// A record a schema-driven tool presents its own way (a handler for the id)
// and that is an ordinary record everywhere else: same bytes, width, key,
// text and checks as the record without the keyword, and addressable in
// place.
namespace ct::sid
{
   struct Call
   {
      std::uint64_t     sender  = 0;
      std::uint64_t     service = 0;
      std::uint32_t     method  = 0;
      std::vector<char> data;
      friend bool       operator==(const Call&, const Call&) = default;
      PSIO_REFLECT(Call, schemaId("Action"), sender, service, method, data)
   };
   // The keyword anywhere in the list, with other keywords, at namespace
   // scope.
   struct Fixed
   {
      std::uint32_t a = 0;
      std::uint16_t b = 0;
      friend bool   operator==(const Fixed&, const Fixed&) = default;
   };
   PSIO_REFLECT(Fixed, a, definitionWillNotChange(), b, schemaId("ct.Fixed"))
   // With an invariant declared by keyword: the id is the keyword's.
   struct Even
   {
      std::uint32_t             n = 0;
      static psio::codec_status even(const Even& e) noexcept
      {
         return e.n % 2 == 0 ? psio::codec_ok() : psio::codec_fail("odd", 0, "Even");
      }
      PSIO_REFLECT(Even, n, invariant(even), schemaId("EvenNumber"))
   };
   struct Holder
   {
      std::string       note;
      std::vector<Call> calls;
      Fixed             fixed;
      friend bool       operator==(const Holder&, const Holder&) = default;
      PSIO_REFLECT(Holder, note, calls, fixed)
   };
   struct Query
   {
      Call call;
      PSIO_REFLECT(Query, call)
   };
}  // namespace ct::sid
// The same declarations without the keyword.
namespace ct::plain
{
   struct Call
   {
      std::uint64_t     sender  = 0;
      std::uint64_t     service = 0;
      std::uint32_t     method  = 0;
      std::vector<char> data;
      PSIO_REFLECT(Call, sender, service, method, data)
   };
   struct Fixed
   {
      std::uint32_t a = 0;
      std::uint16_t b = 0;
   };
   PSIO_REFLECT(Fixed, a, definitionWillNotChange(), b)
   struct Holder
   {
      std::string       note;
      std::vector<Call> calls;
      Fixed             fixed;
      PSIO_REFLECT(Holder, note, calls, fixed)
   };
}  // namespace ct::plain

namespace
{
   static_assert(psio::has_schema_id_v<ct::sid::Call>);
   static_assert(psio::reflected_schema_id<ct::sid::Call>() == "Action");
   static_assert(psio::schema_custom_v<ct::sid::Call>);
   static_assert(psio::schema_custom_id<ct::sid::Call>() == "Action");
   static_assert(psio::schema_custom_id<ct::sid::Fixed>() == "ct.Fixed");
   static_assert(psio::schema_custom_id<ct::sid::Even>() == "EvenNumber");
   static_assert(!psio::has_schema_id_v<ct::plain::Call>);
   static_assert(!psio::schema_custom_v<ct::plain::Call>);
   static_assert(!psio::has_schema_id_v<ct::sid::Holder>);
   static_assert(!psio::has_schema_id_v<std::uint32_t>);
   static_assert(!psio::has_schema_id_v<std::vector<ct::sid::Call>>);
   // Not an invariant, not a text form, not an annotation, not blocked.
   static_assert(!psio::has_invariant_v<ct::sid::Call>);
   static_assert(!psio::detail::text_adapter_is_custom<ct::sid::Call>());
   static_assert(!psio::detail::mutation_blocked_v<ct::sid::Call>);
   static_assert(std::tuple_size_v<decltype(psio::reflect<ct::sid::Call>::type_annotations)> == 0);
   static_assert(std::tuple_size_v<decltype(psio::reflect<ct::sid::Fixed>::type_annotations)> ==
                 std::tuple_size_v<decltype(psio::reflect<ct::plain::Fixed>::type_annotations)>);
   static_assert(psio::reflect<ct::sid::Call>::member_count == 4);
   static_assert(psio::reflect<ct::sid::Call>::name == "Call");
   // Width and fixed size are the plain record's.
   static_assert(psio::auto_pssz_width_v<ct::sid::Call> ==
                 psio::auto_pssz_width_v<ct::plain::Call>);
   static_assert(psio::auto_pssz_width_v<ct::sid::Holder> ==
                 psio::auto_pssz_width_v<ct::plain::Holder>);
   static_assert(pi::is_fixed_v<ct::sid::Fixed> && pi::is_fixed_v<ct::plain::Fixed>);
   static_assert(pi::fixed_size_of<ct::sid::Fixed>() == pi::fixed_size_of<ct::plain::Fixed>());
   static_assert(!pi::is_fixed_v<ct::sid::Call>);
   static_assert(psio::pssz_evolves_from<ct::sid::Call, ct::sid::Call>);

   ct::sid::Holder sid_sample()
   {
      return {"n", {{1, 2, 3, {'a', 'b'}}, {4, 5, 6, {}}}, {7, 8}};
   }
   ct::plain::Holder plain_sample()
   {
      return {"n", {{1, 2, 3, {'a', 'b'}}, {4, 5, 6, {}}}, {7, 8}};
   }
}  // namespace

TEST_CASE("schemaId: bytes, key, text and checks are the plain record's", "[custom][schema_id]")
{
   const auto with  = sid_sample();
   const auto plain = plain_sample();
   const auto bytes = pssz_of(with);
   CHECK(bytes == pssz_of(plain));
   CHECK(psio::encode(psio::pssz16{}, with) == psio::encode(psio::pssz16{}, plain));
   CHECK(psio::encode(psio::key{}, with) == psio::encode(psio::key{}, plain));
   CHECK(psio::encode(psio::key{}, with.calls[0]) == psio::encode(psio::key{}, plain.calls[0]));
   CHECK(psio::encode(psio::json{}, with) == psio::encode(psio::json{}, plain));
   CHECK(psio::encode(psio::ijson{}, with) == psio::encode(psio::ijson{}, plain));
   CHECK(psio::encode(psio::json{}, with.calls[0]) ==
         R"({"sender":1,"service":2,"method":3,"data":[97,98]})");
   CHECK(psio::decode<ct::sid::Holder>(psio::pssz{}, sp(bytes)) == with);
   CHECK(psio::decode<ct::sid::Holder>(psio::json{}, sp(psio::encode(psio::json{}, plain))) ==
         with);
   CHECK(psio::validate<ct::sid::Holder>(psio::pssz{}, sp(bytes)).ok());
   CHECK(psio::validate_strict<ct::sid::Holder>(psio::pssz{}, sp(bytes)).ok());
   // Every truncation is refused or accepted exactly as for the plain record.
   for (std::size_t n = 0; n < bytes.size(); ++n)
   {
      const std::span<const char> cut{bytes.data(), n};
      CHECK(psio::validate<ct::sid::Holder>(psio::pssz{}, cut).ok() ==
            psio::validate<ct::plain::Holder>(psio::pssz{}, cut).ok());
   }
}

TEST_CASE("schemaId: the record is addressable in place", "[custom][schema_id][mutable_view]")
{
   ct::sid::Call call{1, 2, 3, {'x', 'y', 'z'}};
   auto          bytes = pssz_of(call);
   const auto    size  = bytes.size();
   psio::mutable_view<ct::sid::Call, psio::pssz> mv{bytes};
   REQUIRE(mv.get<0>().get() == 1);
   REQUIRE(mv.get<0>().set(100));
   REQUIRE(mv.get<1>().set(200));
   REQUIRE(mv.get<2>().set(300));
   call.sender  = 100;
   call.service = 200;
   call.method  = 300;
   CHECK(bytes.size() == size);
   CHECK(bytes == pssz_of(call));
   CHECK(psio::decode<ct::sid::Call>(psio::pssz{}, sp(bytes)) == call);

   // Through a record that holds it, and as a whole fixed-size value.
   auto holder       = sid_sample();
   auto holder_bytes = pssz_of(holder);
   psio::mutable_view<ct::sid::Holder, psio::pssz> hv{holder_bytes};
   REQUIRE(hv.get<2>().get<0>().set(70));
   holder.fixed.a = 70;
   CHECK(holder_bytes == pssz_of(holder));
   REQUIRE(hv.get<2>().set(ct::sid::Fixed{9, 10}));
   holder.fixed = {9, 10};
   CHECK(holder_bytes == pssz_of(holder));
}

TEST_CASE("schemaId: the schema node is Custom{record, id}", "[custom][schema_id][schema]")
{
   const auto schema = psio::SchemaBuilder{}
                          .insert<ct::sid::Holder>("Holder")
                          .insert<ct::sid::Even>("Even")
                          .build();
   const auto plain = psio::SchemaBuilder{}.insert<ct::plain::Holder>("Holder").build();
   auto       custom = [&](std::string_view name) -> const st::Custom& {
      const auto* c = std::get_if<st::Custom>(&resolved(schema, named(schema, name)).value);
      REQUIRE(c);
      return *c;
   };
   CHECK(custom("Call").id == "Action");
   CHECK(custom("Fixed").id == "ct.Fixed");
   CHECK(custom("Even").id == "EvenNumber");
   // The wire type is the record the plain declaration gives.
   const auto* object = std::get_if<st::Object>(&custom("Call").type->value);
   REQUIRE(object);
   const auto* plain_object = std::get_if<st::Object>(&resolved(plain, named(plain, "Call")).value);
   REQUIRE(plain_object);
   REQUIRE(object->members.size() == plain_object->members.size());
   for (std::size_t i = 0; i < object->members.size(); ++i)
      CHECK(object->members[i].name == plain_object->members[i].name);
   CHECK(custom("Fixed").type->value.index() ==
         resolved(plain, named(plain, "Fixed")).value.index());
   // The record that only holds one is not custom.
   CHECK(std::holds_alternative<st::Object>(resolved(schema, named(schema, "Holder")).value));
}

TEST_CASE("schemaId: schema-driven json, without and with a handler for the id",
          "[custom][schema_id][schema][json]")
{
   const auto        schema = psio::SchemaBuilder{}.insert<ct::sid::Holder>("Holder").build();
   const auto        value  = sid_sample();
   const auto        bytes  = pssz_of(value);
   const st::AnyType root   = st::Type{"Holder"};

   // No handler, the type's own (empty) handler, handlers bound once: the
   // record's text, both ways.
   for (auto fmt_ijson : {false, true})
   {
      const std::string typed =
         fmt_ijson ? psio::encode(psio::ijson{}, value) : psio::encode(psio::json{}, value);
      auto to_json = [&](const ir::CustomHandlers* handlers, const ir::JsonNames* names) {
         std::string out;
         auto        status = fmt_ijson ? ir::pssz_to_json(schema, root, psio::ijson{}, sp(bytes),
                                                           out, nullptr, handlers, names)
                                        : ir::pssz_to_json(schema, root, psio::json{}, sp(bytes),
                                                           out, nullptr, handlers, names);
         REQUIRE(status.ok());
         return out;
      };
      ir::CustomHandlers own;
      own.add<ct::sid::Call>();
      CHECK(own.find("Action") != nullptr);
      const ir::JsonNames names(schema, own);
      CHECK(to_json(nullptr, nullptr) == typed);
      CHECK(to_json(&own, nullptr) == typed);
      CHECK(to_json(nullptr, &names) == typed);
      std::vector<char> again;
      auto status = fmt_ijson ? ir::json_to_pssz(schema, root, psio::ijson{}, sp(typed), again)
                              : ir::json_to_pssz(schema, root, psio::json{}, sp(typed), again);
      REQUIRE(status.ok());
      CHECK(again == bytes);
   }

   // A handler that presents the value as any JSON value (to_json /
   // from_json): the call with its data shown as text.
   ir::CustomHandlers shown;
   shown.add("Action",
             {.to_json = [](std::span<const char> pssz, const ir::JsonContext&,
                            std::string& json) -> psio::codec_status {
                 const auto call = psio::decode<ct::sid::Call>(psio::pssz{}, pssz);
                 json += R"({"call":")" + std::to_string(call.service) +
                         "::" + std::to_string(call.method) + R"(","by":)" +
                         std::to_string(call.sender) + R"(,"data":{"text":")" +
                         std::string{call.data.begin(), call.data.end()} + R"("}})";
                 return psio::codec_ok();
              },
              .from_json = [](std::string_view json, const ir::JsonContext&,
                              std::vector<char>& pssz) -> psio::codec_status {
                 psio::json_value v;
                 if (auto status = psio::decode_json_checked(psio::json{}, json, v); !status.ok())
                    return status;
                 const auto* name = v.find("call");
                 const auto* by   = v.find("by");
                 const auto* data = v.find("data");
                 if (!name || !name->as_string() || !by || !by->as_number() || !data ||
                     !data->find("text") || !data->find("text")->as_string())
                    return psio::codec_fail("a call is {call, by, data}", 0, "Action");
                 const std::string& n   = *name->as_string();
                 const auto         sep = n.find("::");
                 if (sep == n.npos)
                    return psio::codec_fail("a call is service::method", 0, "Action");
                 const std::string& text = *data->find("text")->as_string();
                 const ct::sid::Call call{*by->as_number()->as_uint64(),
                                          std::stoull(n.substr(0, sep)),
                                          static_cast<std::uint32_t>(std::stoul(n.substr(sep + 2))),
                                          {text.begin(), text.end()}};
                 const auto bytes = psio::encode(psio::pssz{}, call);
                 pssz.insert(pssz.end(), bytes.begin(), bytes.end());
                 return psio::codec_ok();
              }});
   std::string text;
   REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, sp(bytes), text, nullptr, &shown).ok());
   CHECK(text == R"({"note":"n","calls":[{"call":"2::3","by":1,"data":{"text":"ab"}},)"
                 R"({"call":"5::6","by":4,"data":{"text":""}}],"fixed":{"a":7,"b":8}})");
   std::vector<char> again;
   REQUIRE(ir::json_to_pssz(schema, root, psio::json{}, sp(text), again, nullptr,
                            psio::json_parser::automatic, &shown)
              .ok());
   CHECK(again == bytes);
   // Bound once to the schema's nodes: the same text.
   {
      const ir::JsonNames names(schema, shown);
      std::string         bound;
      REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, sp(bytes), bound, nullptr, nullptr,
                               &names)
                 .ok());
      CHECK(bound == text);
   }
   // The handler's text is not the record's text.
   again.clear();
   CHECK_FALSE(ir::json_to_pssz(schema, root, psio::json{}, sp(psio::encode(psio::json{}, value)),
                                again, nullptr, psio::json_parser::automatic, &shown)
                  .ok());

   // A handler that presents the value as one JSON string (to_text /
   // from_text).
   ir::CustomHandlers as_text;
   as_text.add("Action",
               {.to_text = [](std::span<const char> pssz, std::string& out) -> psio::codec_status {
                   const auto call = psio::decode<ct::sid::Call>(psio::pssz{}, pssz);
                   out += std::to_string(call.sender) + ">" + std::to_string(call.service) + "." +
                          std::to_string(call.method);
                   return psio::codec_ok();
                }});
   text.clear();
   REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, sp(bytes), text, nullptr, &as_text).ok());
   CHECK(text == R"({"note":"n","calls":["1>2.3","4>5.6"],"fixed":{"a":7,"b":8}})");

   // A reserved id is still refused by the registry.
   ir::CustomHandlers reserved;
   CHECK_FALSE(reserved.try_add("string", {}).ok());
}

TEST_CASE("schemaId: in GraphQL the record is the object type it would be anyway",
          "[custom][schema_id][graphql]")
{
   using api = psio::graphql::schema<ct::sid::Query>;
   const ct::sid::Query q{{1, 2, 3, {'a'}}};
   std::string          out;
   api::run(q, "{ call { sender service method } }", out);
   CHECK(out == R"({"data":{"call":{"sender":1,"service":2,"method":3}}})");
   CHECK(api::sdl().find("type Call") != std::string::npos);
   CHECK(api::sdl().find("Action") == std::string::npos);
}
