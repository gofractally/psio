// Annotations declared inside PSIO_REFLECT — attr(F, spec),
// definitionWillNotChange(), maxFields(N), maxDynamicData(N) — for a type in
// a nested namespace, in the in-class form and in the namespace-scope form,
// and PSIO_TYPE_ATTRS / PSIO_FIELD_ATTRS written in the type's own
// namespace. Every consumer must see exactly what it sees for the same type
// declared at global scope with the separate macros: widths, bytes, JSON
// text, the Schema IR, views, validation, keys and pssz_evolves_from.

#include <psio/annotate.hpp>
#include <psio/format.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

// The members every form of the record has.
#define ANS_ACCOUNT_MEMBERS                                              \
   std::uint64_t              id = 0;                                    \
   std::string                owner;                                     \
   std::vector<std::uint16_t> tags;                                      \
   std::optional<std::string> memo;                                      \
   std::uint32_t              weight = 0;                                \
   std::uint64_t scaled(std::uint32_t by) const { return weight * std::uint64_t{by}; } \
   void          credit(std::uint32_t amount) { weight += amount; }      \
   static std::uint32_t version() { return 2; }

// ── Reference: global scope, separate macros ────────────────────────────
struct RefAccount
{
   ANS_ACCOUNT_MEMBERS
};
PSIO_REFLECT(RefAccount, id, owner, tags, memo, weight, method(scaled, by), method(credit, amount),
             method(version))
PSIO_TYPE_ATTRS(RefAccount,
                psio::definition_will_not_change{} | psio::max_dynamic_data_spec{200})
PSIO_FIELD_ATTRS(RefAccount, id, psio::json_string)
PSIO_FIELD_ATTRS(RefAccount, owner, psio::length_bound{.max = 32})
PSIO_FIELD_ATTRS(RefAccount, tags, psio::length_bound{.max = 4})
PSIO_FIELD_ATTRS(RefAccount, memo, psio::length_bound{.max = 16})

// An extensible pair (cap, bounds, no DWNC) and its evolution.
struct RefOpen
{
   std::uint64_t id = 0;
   std::string   owner;
};
PSIO_REFLECT(RefOpen, id, owner)
PSIO_TYPE_ATTRS(RefOpen, psio::max_dynamic_data_spec{200})
PSIO_FIELD_ATTRS(RefOpen, id, psio::json_string)
PSIO_FIELD_ATTRS(RefOpen, owner, psio::length_bound{.max = 32})
struct RefOpenGrown
{
   std::uint64_t                id = 0;
   std::string                  owner;
   std::optional<std::uint32_t> note;
};
PSIO_REFLECT(RefOpenGrown, id, owner, note)
PSIO_TYPE_ATTRS(RefOpenGrown, psio::max_dynamic_data_spec{200})
PSIO_FIELD_ATTRS(RefOpenGrown, id, psio::json_string)
PSIO_FIELD_ATTRS(RefOpenGrown, owner, psio::length_bound{.max = 32})

// ── Nested namespace, everything in one PSIO_REFLECT ────────────────────
namespace chain::state::v1
{
   // In the class body.
   struct InClass
   {
      ANS_ACCOUNT_MEMBERS
      PSIO_REFLECT(InClass,
                   attr(id, json_string),
                   attr(owner, max<32>),
                   attr(tags, max<4>),
                   attr(memo, max<16>),
                   weight,
                   method(scaled, by),
                   method(credit, amount),
                   method(version),
                   definitionWillNotChange(),
                   maxDynamicData(200))
   };

   // At namespace scope, after the type.
   struct AtScope
   {
      ANS_ACCOUNT_MEMBERS
   };
   PSIO_REFLECT(AtScope,
                attr(id, json_string),
                attr(owner, max<32>),
                attr(tags, max<4>),
                attr(memo, max<16>),
                weight,
                method(scaled, by),
                method(credit, amount),
                method(version),
                definitionWillNotChange(),
                maxDynamicData(200))

   // In-class, before the members it names, some of them private.
   class Private
   {
      PSIO_REFLECT(Private,
                   attr(id, json_string),
                   attr(owner, max<32>),
                   attr(tags, max<4>),
                   attr(memo, max<16>),
                   weight,
                   method(scaled, by),
                   method(credit, amount),
                   method(version),
                   definitionWillNotChange(),
                   maxDynamicData(200))
      std::uint64_t              id = 0;
      std::string                owner;
      std::vector<std::uint16_t> tags;
      std::optional<std::string> memo;
      std::uint32_t              weight = 0;

     public:
      Private() = default;
      Private(const RefAccount& r)
          : id(r.id), owner(r.owner), tags(r.tags), memo(r.memo), weight(r.weight)
      {
      }
      std::uint64_t        scaled(std::uint32_t by) const { return weight * std::uint64_t{by}; }
      void                 credit(std::uint32_t amount) { weight += amount; }
      static std::uint32_t version() { return 2; }
   };

   // The separate statements, written in the type's own namespace.
   struct Separate
   {
      ANS_ACCOUNT_MEMBERS
   };
   PSIO_REFLECT(Separate, id, owner, tags, memo, weight, method(scaled, by),
                method(credit, amount), method(version))
   PSIO_TYPE_ATTRS(Separate,
                   psio::definition_will_not_change{} | psio::max_dynamic_data_spec{200})
   PSIO_FIELD_ATTRS(Separate, id, psio::json_string)
   PSIO_ATTRS(Separate, (owner, (psio::length_bound{.max = 32})),
              (tags, (psio::length_bound{.max = 4})), (memo, (psio::length_bound{.max = 16})))

   // Both forms on one type, different spec types: they compose.
   struct Mixed
   {
      ANS_ACCOUNT_MEMBERS
      PSIO_REFLECT(Mixed, attr(id, json_string), attr(owner, max<32>), tags, memo, weight,
                   method(scaled, by), method(credit, amount), method(version),
                   definitionWillNotChange())
   };
   PSIO_TYPE_ATTRS(Mixed, psio::max_dynamic_data_spec{200})
   PSIO_FIELD_ATTRS(Mixed, tags, psio::length_bound{.max = 4})

   // The extensible pair, in-class and at namespace scope.
   struct Open
   {
      std::uint64_t id = 0;
      std::string   owner;
      PSIO_REFLECT(Open, attr(id, json_string), attr(owner, max<32>), maxDynamicData(200))
   };
   struct OpenGrown
   {
      std::uint64_t                id = 0;
      std::string                  owner;
      std::optional<std::uint32_t> note;
   };
   PSIO_REFLECT(OpenGrown, attr(id, json_string), attr(owner, max<32>), note, maxDynamicData(200))
   // Without the cap the appended optional moves the inferred width.
   struct Uncapped
   {
      std::uint64_t id = 0;
      std::string   owner;
      PSIO_REFLECT(Uncapped, id, attr(owner, max<240>))
   };
   struct UncappedGrown
   {
      std::uint64_t                id = 0;
      std::string                  owner;
      std::optional<std::uint64_t> note;
      PSIO_REFLECT(UncappedGrown, id, attr(owner, max<240>), note)
   };

   namespace inner
   {
      // One more level, a nested class, and maxFields.
      struct Outer
      {
         struct Row
         {
            std::uint8_t  kind = 0;
            std::string   text;
            std::uint64_t big = 0;
            PSIO_REFLECT(Row, kind, attr(text, max<8>), attr(big, json_string | ijson_number),
                         maxFields(8), maxDynamicData(64))
         };
         std::vector<Row> rows;
         PSIO_REFLECT(Outer, attr(rows, max<3>))
      };
   }  // namespace inner
}  // namespace chain::state::v1

// Mixed gets its memo bound from the global namespace: a statement for a
// namespaced type may still be written there.
PSIO_FIELD_ATTRS(chain::state::v1::Mixed, memo, psio::length_bound{.max = 16})

namespace
{
   namespace cs = chain::state::v1;
   namespace ir = psio::pssz_ir;
   namespace st = psio::schema_types;
   using Bytes = std::vector<char>;

   RefAccount sample()
   {
      RefAccount r;
      r.id     = 0x1122334455667788ull;
      r.owner  = "alice";
      r.tags   = {7, 65535};
      r.memo   = "rent";
      r.weight = 42;
      return r;
   }
   template <typename T>
   T as(const RefAccount& r)
   {
      if constexpr (std::is_constructible_v<T, const RefAccount&>)
         return T{r};
      else
      {
         T t;
         t.id = r.id, t.owner = r.owner, t.tags = r.tags, t.memo = r.memo, t.weight = r.weight;
         return t;
      }
   }
   // The schema of T with T's own name replaced by a fixed one, so that two
   // declarations of one record under different C++ names compare equal
   // exactly when everything else — members, types, attributes — is equal.
   template <typename T>
   Bytes schema_bytes()
   {
      psio::SchemaBuilder builder;
      builder.insert<T>("Account");
      psio::Schema           schema = std::move(builder).build();
      const std::string_view own    = psio::reflect<T>::name;
      std::size_t            renamed = 0;
      for (auto& member : schema.types)
      {
         if (member.name == own)
            member.name = "Rec", ++renamed;
         if (auto* ref = std::get_if<st::Type>(&member.type->value); ref && ref->type == own)
            ref->type = "Rec", ++renamed;
      }
      REQUIRE(renamed == 2);
      return psio::encode(psio::pssz{}, schema);
   }
   template <typename T>
   psio::Schema schema_of()
   {
      psio::SchemaBuilder builder;
      builder.insert<T>("Account");
      return std::move(builder).build();
   }

   // Everything a consumer derives from the annotations of T.
   template <typename T>
   void same_as_reference(const char* form)
   {
      INFO(form);
      using R = RefAccount;

      // The annotations themselves.
      STATIC_REQUIRE(psio::is_dwnc_v<T>);
      STATIC_REQUIRE(psio::is_dwnc_v<T> == psio::is_dwnc_v<R>);
      STATIC_REQUIRE(psio::max_dynamic_data_v<T> == std::optional<std::size_t>{200});
      STATIC_REQUIRE(psio::max_dynamic_data_v<T> == psio::max_dynamic_data_v<R>);
      STATIC_REQUIRE(psio::max_fields_v<T> == psio::max_fields_v<R>);
      STATIC_REQUIRE(psio::effective_max_dynamic_v<T> == psio::effective_max_dynamic_v<R>);
      STATIC_REQUIRE(psio::inferred_bound<T>() == psio::inferred_bound<R>());
      using Refl = psio::reflect<T>;
      STATIC_REQUIRE(Refl::member_count == 5);
      STATIC_REQUIRE(Refl::method_count == 3);
      constexpr auto id_ann = psio::effective_annotations_for_v<
         T, std::uint64_t, Refl::template member_pointer<0>>;
      STATIC_REQUIRE(psio::find_spec<psio::json_int_spec>(id_ann) ==
                     std::optional{psio::json_string});
      constexpr auto owner_ann =
         psio::effective_annotations_for_v<T, std::string, Refl::template member_pointer<1>>;
      STATIC_REQUIRE(psio::find_spec<psio::length_bound>(owner_ann) ==
                     std::optional{psio::length_bound{.max = 32}});
      constexpr auto tags_ann = psio::annotate<Refl::template member_pointer<2>>;
      STATIC_REQUIRE(psio::find_spec<psio::length_bound>(tags_ann) ==
                     std::optional{psio::length_bound{.max = 4}});
      constexpr auto weight_ann = psio::annotate<Refl::template member_pointer<4>>;
      STATIC_REQUIRE(std::tuple_size_v<decltype(weight_ann)> == 0);

      // Widths and sizes.
      STATIC_REQUIRE(psio::auto_pssz_width<T>() == psio::auto_pssz_width<R>());
      STATIC_REQUIRE(psio::auto_pssz_width<T>() == 1);

      // Bytes.
      const R    ref   = sample();
      const T    value = as<T>(ref);
      const auto bytes = psio::encode(psio::pssz{}, value);
      CHECK(bytes == psio::encode(psio::pssz{}, ref));
      CHECK(psio::size_of(psio::pssz{}, value) == bytes.size());
      CHECK(psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok());
      CHECK(psio::encode(psio::pssz{}, psio::decode<T>(psio::pssz{}, std::span<const char>{bytes})) ==
            bytes);
      for (std::size_t w : {1u, 2u, 4u})
      {
         const auto explicit_bytes = w == 1   ? psio::encode(psio::pssz_<1>{}, value)
                                     : w == 2 ? psio::encode(psio::pssz_<2>{}, value)
                                              : psio::encode(psio::pssz_<4>{}, value);
         const auto explicit_ref = w == 1   ? psio::encode(psio::pssz_<1>{}, ref)
                                   : w == 2 ? psio::encode(psio::pssz_<2>{}, ref)
                                            : psio::encode(psio::pssz_<4>{}, ref);
         CHECK(explicit_bytes == explicit_ref);
      }
      CHECK(psio::encode(psio::key{}, value) == psio::encode(psio::key{}, ref));

      // JSON text: the json_string override applies to json, not to ijson.
      const std::string json  = psio::encode(psio::json{}, value);
      const std::string ijson = psio::encode(psio::ijson{}, value);
      CHECK(json == psio::encode(psio::json{}, ref));
      CHECK(ijson == psio::encode(psio::ijson{}, ref));
      CHECK(json == R"({"id":"1234605616436508552","owner":"alice","tags":[7,65535],)"
                    R"("memo":"rent","weight":42})");
      T from_json{};
      REQUIRE(psio::decode_json_checked(psio::json{}, std::span<const char>{json}, from_json).ok());
      CHECK(psio::encode(psio::pssz{}, from_json) == bytes);

      // Bounds: on checked encode, validate, JSON and key, as the reference.
      auto over_owner = ref;
      over_owner.owner.assign(33, 'x');
      auto over_tags = ref;
      over_tags.tags.assign(5, 1);
      auto over_memo = ref;
      over_memo.memo = std::string(17, 'm');
      for (const R* bad_ref : {&over_owner, &over_tags, &over_memo})
      {
         const T bad = as<T>(*bad_ref);
         Bytes   out;
         CHECK_FALSE(psio::encode_pssz_checked(psio::pssz{}, bad, out).ok());
         CHECK_FALSE(psio::encode_pssz_checked(psio::pssz{}, *bad_ref, out).ok());
         const auto frame = psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, bad);
         CHECK(frame ==
               psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, *bad_ref));
         CHECK_FALSE(psio::validate<T>(psio::pssz{}, std::span<const char>{frame}).ok());
         CHECK_FALSE(psio::validate<R>(psio::pssz{}, std::span<const char>{frame}).ok());
         std::string text;
         CHECK_FALSE(psio::encode_json_checked(psio::json{}, bad, text).ok());
         // No key writer takes the value; the key an unchecked writer would
         // produce is rejected by the readers.
         CHECK_THROWS_AS(psio::encode(psio::key{}, bad), psio::codec_exception);
         Bytes refused;
         CHECK_FALSE(psio::encode_key_checked(bad, refused).ok());
         CHECK_FALSE(psio::encode_key_checked(*bad_ref, refused).ok());
         Bytes key;
         {
            psio::detail::order_key_stream unchecked{key};
            psio::detail::key_impl::encode_value(bad, unchecked);
         }
         CHECK_FALSE(psio::validate<T>(psio::key{}, std::span<const char>{key}).ok());
         CHECK_FALSE(psio::validate<R>(psio::key{}, std::span<const char>{key}).ok());
      }
      // The cap: a value within every member bound but over maxDynamicData.
      {
         auto over_cap = ref;
         over_cap.owner.assign(32, 'o');
         over_cap.memo = std::string(16, 'm');
         over_cap.tags.assign(4, 9);
         Bytes out_t, out_r;
         CHECK(psio::encode_pssz_checked(psio::pssz{}, as<T>(over_cap), out_t).ok() ==
               psio::encode_pssz_checked(psio::pssz{}, over_cap, out_r).ok());
         CHECK(out_t == out_r);
      }

      // Schema IR: the same node, attributes included; and the schema-driven
      // codec derives the same width and accepts the same bytes.
      CHECK(schema_bytes<T>() == schema_bytes<R>());
      const auto        schema = schema_of<T>();
      const st::AnyType root{st::Type{"Account"}};
      CHECK(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());
      std::string ir_json;
      REQUIRE(ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, ir_json).ok());
      CHECK(ir_json == json);
      const auto frame = psio::detail::pssz_impl::encode_ignoring_length_bounds(
         psio::pssz{}, as<T>(over_owner));
      CHECK_FALSE(ir::validate_pssz(schema, root, std::span<const char>{frame}).ok());

      // Views read the fixed layout.
      psio::view<T, psio::pssz, psio::storage::const_borrow> view{std::span<const char>{bytes}};
      psio::view<R, psio::pssz, psio::storage::const_borrow> ref_view{
         std::span<const char>{bytes}};
      CHECK(view.template get<0>() == ref.id);
      CHECK(view.template get<1>().view_() == "alice");
      CHECK(view.template get<1>().view_() == ref_view.template get<1>().view_());
      CHECK(view.template get<4>() == 42u);
      CHECK(view.template get<4>() == ref_view.template get<4>());

      // Evolution: a fixed-layout record evolves from itself and from the
      // reference (same layout), and gains no fields.
      STATIC_REQUIRE(psio::pssz_evolves_from<T, T>);
      STATIC_REQUIRE(psio::pssz_evolves_from<R, T>);
      STATIC_REQUIRE(psio::pssz_evolves_from<T, R>);
      STATIC_REQUIRE(psio::pssz_evolves_from<R, R>);
      STATIC_REQUIRE_FALSE(psio::pssz_evolves_from<T, RefOpen>);
      STATIC_REQUIRE_FALSE(psio::pssz_evolves_from<R, RefOpen>);
      STATIC_REQUIRE_FALSE(psio::pssz_evolves_from<T, cs::Open>);

      // The methods are there and are not data.
      T mutated = value;
      mutated.credit(8);
      CHECK(mutated.scaled(2) == 100u);
      STATIC_REQUIRE(Refl::method_names[0] == "scaled");
   }
}  // namespace

TEST_CASE("PSIO_REFLECT annotations: nested namespace, in-class", "[annotate][namespace]")
{
   same_as_reference<cs::InClass>("in-class");
}
TEST_CASE("PSIO_REFLECT annotations: nested namespace, namespace scope", "[annotate][namespace]")
{
   same_as_reference<cs::AtScope>("namespace scope");
}
TEST_CASE("PSIO_REFLECT annotations: in-class before private members", "[annotate][namespace]")
{
   same_as_reference<cs::Private>("in-class, private, macro first");
}
TEST_CASE("PSIO_TYPE_ATTRS / PSIO_FIELD_ATTRS / PSIO_ATTRS in the type's namespace",
          "[annotate][namespace]")
{
   same_as_reference<cs::Separate>("separate statements in the namespace");
}
TEST_CASE("PSIO_REFLECT annotations and separate statements compose", "[annotate][namespace]")
{
   same_as_reference<cs::Mixed>("mixed");
   // Each source contributed its part.
   STATIC_REQUIRE(std::tuple_size_v<std::remove_cvref_t<
                     decltype(psio::annotate<psio::type<cs::Mixed>{}>)>> == 2);
   STATIC_REQUIRE(psio::reflect<cs::Mixed>::has_member_annotations);
   STATIC_REQUIRE_FALSE(psio::reflect<cs::Separate>::has_member_annotations);
   STATIC_REQUIRE(std::tuple_size_v<std::remove_cvref_t<
                     decltype(psio::reflect<cs::Separate>::type_annotations)>> == 0);
   STATIC_REQUIRE(std::tuple_size_v<std::remove_cvref_t<
                     decltype(psio::reflect<cs::InClass>::type_annotations)>> == 2);
}

TEST_CASE("pssz_evolves_from sees namespaced caps and bounds", "[annotate][namespace][evolves]")
{
   // The capped lineage evolves, exactly as the global one.
   STATIC_REQUIRE(psio::pssz_evolves_from<RefOpen, RefOpenGrown>);
   STATIC_REQUIRE(psio::pssz_evolves_from<cs::Open, cs::OpenGrown>);
   STATIC_REQUIRE(psio::pssz_evolves_from<RefOpen, cs::OpenGrown>);
   STATIC_REQUIRE(psio::pssz_evolves_from<cs::Open, RefOpenGrown>);
   STATIC_REQUIRE_FALSE(psio::pssz_evolves_from<cs::OpenGrown, cs::Open>);
   STATIC_REQUIRE_FALSE(psio::pssz_evolves_from<RefOpenGrown, RefOpen>);
   STATIC_REQUIRE(psio::auto_pssz_width<cs::Open>() == psio::auto_pssz_width<RefOpen>());
   STATIC_REQUIRE(psio::auto_pssz_width<cs::OpenGrown>() == psio::auto_pssz_width<RefOpenGrown>());
   STATIC_REQUIRE(psio::auto_pssz_width<cs::Open>() == 1);
   // Without a cap the bound decides the width, and the appended field
   // moves it: the base type fits one-byte offsets, the grown one does not.
   STATIC_REQUIRE(psio::auto_pssz_width<cs::Uncapped>() !=
                  psio::auto_pssz_width<cs::UncappedGrown>());
   STATIC_REQUIRE_FALSE(psio::pssz_evolves_from<cs::Uncapped, cs::UncappedGrown>);

   const cs::Open      open{5, "bob"};
   const RefOpen       ref{5, "bob"};
   const cs::OpenGrown grown{5, "bob", 9};
   CHECK(psio::encode(psio::pssz{}, open) == psio::encode(psio::pssz{}, ref));
   CHECK(psio::encode(psio::json{}, open) == R"({"id":"5","owner":"bob"})");
   // Old bytes read as the grown type, and back.
   const auto old_bytes = psio::encode(psio::pssz{}, open);
   const auto as_grown  = psio::decode<cs::OpenGrown>(psio::pssz{}, std::span<const char>{old_bytes});
   CHECK(as_grown.owner == "bob");
   CHECK_FALSE(as_grown.note.has_value());
   const auto new_bytes = psio::encode(psio::pssz{}, grown);
   bool       unknown   = false;
   cs::Open   as_old;
   REQUIRE(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{new_bytes}, as_old, unknown)
              .ok());
   CHECK(unknown);
   CHECK(as_old.id == 5);
}

TEST_CASE("annotations of a nested class two namespaces down", "[annotate][namespace]")
{
   using Row   = cs::inner::Outer::Row;
   using Outer = cs::inner::Outer;
   STATIC_REQUIRE(psio::max_fields_v<Row> == std::optional<std::size_t>{8});
   STATIC_REQUIRE(psio::max_dynamic_data_v<Row> == std::optional<std::size_t>{64});
   STATIC_REQUIRE_FALSE(psio::is_dwnc_v<Row>);
   STATIC_REQUIRE(psio::auto_pssz_width<Row>() == 1);
   // rows: at most 3 rows of at most 64 bytes.
   STATIC_REQUIRE(psio::auto_pssz_width<Outer>() == 1);

   const Outer value{{Row{1, "ab", 1ull << 60}, Row{2, "", 7}}};
   CHECK(psio::encode(psio::json{}, value) ==
         R"({"rows":[{"kind":1,"text":"ab","big":"1152921504606846976"},)"
         R"({"kind":2,"text":"","big":"7"}]})");
   CHECK(psio::encode(psio::ijson{}, value) ==
         R"({"rows":[{"kind":1,"text":"ab","big":1152921504606846976},)"
         R"({"kind":2,"text":"","big":7}]})");
   Bytes out;
   CHECK(psio::encode_pssz_checked(psio::pssz{}, value, out).ok());
   CHECK(psio::validate<Outer>(psio::pssz{}, std::span<const char>{out}).ok());
   Outer too_many{{Row{}, Row{}, Row{}, Row{}}};
   out.clear();
   CHECK_FALSE(psio::encode_pssz_checked(psio::pssz{}, too_many, out).ok());
   Outer too_long{{Row{1, "123456789", 0}}};
   out.clear();
   CHECK_FALSE(psio::encode_pssz_checked(psio::pssz{}, too_long, out).ok());

   // The schema carries the bounds and caps.
   psio::SchemaBuilder builder;
   builder.insert<Outer>("Outer");
   const psio::Schema schema = std::move(builder).build();
   const st::AnyType  root{st::Type{"Outer"}};
   out.clear();
   REQUIRE(psio::encode_pssz_checked(psio::pssz{}, value, out).ok());
   CHECK(ir::validate_pssz(schema, root, std::span<const char>{out}).ok());
   std::string text;
   REQUIRE(ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{out}, text).ok());
   CHECK(text == psio::encode(psio::ijson{}, value));
   const auto frame =
      psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, too_many);
   CHECK_FALSE(ir::validate_pssz(schema, root, std::span<const char>{frame}).ok());
}
