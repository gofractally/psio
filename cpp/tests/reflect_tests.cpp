// Phase 1 tests for psio::reflect<T> — the reflection core.
// Covers every surface the design doc § 5.2.5 enumerates.

#include <psio/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

   struct Point {
      int x;
      int y;
   };
   PSIO_REFLECT(Point, x, y)

   struct Validator {
      std::array<unsigned char, 48> pubkey;
      std::array<unsigned char, 32> withdrawal_credentials;
      std::uint64_t                 effective_balance;
      bool                          slashed;
      std::uint64_t                 activation_epoch;
      std::uint64_t                 exit_epoch;
   };
   PSIO_REFLECT(Validator,
                 pubkey,
                 withdrawal_credentials,
                 effective_balance,
                 slashed,
                 activation_epoch,
                 exit_epoch)

   struct NotReflected {
      int a;
   };

}  // namespace

TEST_CASE("reflect<T>::is_reflected gates unreflected types", "[reflect]")
{
   // STATIC_REQUIRE is macro-like; `,` inside < > confuses its
   // preprocessor argument split, so wrap template expressions in
   // parens.
   STATIC_REQUIRE((psio::reflect<Point>::is_reflected));
   STATIC_REQUIRE((psio::reflect<Validator>::is_reflected));
   STATIC_REQUIRE((!psio::reflect<NotReflected>::is_reflected));
   STATIC_REQUIRE((psio::Reflected<Point>));
   STATIC_REQUIRE((!psio::Reflected<NotReflected>));
}

TEST_CASE("reflect<T> basic compile-time metadata", "[reflect]")
{
   using R = psio::reflect<Point>;

   STATIC_REQUIRE(R::name == "Point");
   STATIC_REQUIRE(R::member_count == 2);
   STATIC_REQUIRE(R::member_name<0> == "x");
   STATIC_REQUIRE(R::member_name<1> == "y");
   STATIC_REQUIRE(R::field_number<0> == 1);
   STATIC_REQUIRE(R::field_number<1> == 2);

   STATIC_REQUIRE(std::is_same_v<R::member_type<0>, int>);
   STATIC_REQUIRE(std::is_same_v<R::member_type<1>, int>);

   // Member pointer round-trips.
   Point p{.x = 10, .y = 20};
   REQUIRE(p.*R::member_pointer<0> == 10);
   REQUIRE(p.*R::member_pointer<1> == 20);
}

TEST_CASE("reflect<T> metadata on a larger struct", "[reflect]")
{
   using R = psio::reflect<Validator>;

   STATIC_REQUIRE(R::name == "Validator");
   STATIC_REQUIRE(R::member_count == 6);
   STATIC_REQUIRE(R::member_name<0> == "pubkey");
   STATIC_REQUIRE(R::member_name<2> == "effective_balance");
   STATIC_REQUIRE(R::member_name<5> == "exit_epoch");

   STATIC_REQUIRE(std::is_same_v<R::member_type<2>, std::uint64_t>);
   STATIC_REQUIRE(std::is_same_v<R::member_type<3>, bool>);
}

TEST_CASE("reflect<T>::index_of name lookup (runtime + constexpr)", "[reflect]")
{
   using R = psio::reflect<Validator>;

   // Runtime calls.
   REQUIRE(R::index_of("pubkey") == 0);
   REQUIRE(R::index_of("effective_balance") == 2);
   REQUIRE(R::index_of("exit_epoch") == 5);
   REQUIRE(R::index_of("does_not_exist") == std::nullopt);

   // Constexpr call.
   constexpr auto hit = R::index_of("effective_balance");
   STATIC_REQUIRE(hit.has_value());
   STATIC_REQUIRE(*hit == 2);
}

TEST_CASE("reflect<T>::index_of_field_number (source order)", "[reflect]")
{
   using R = psio::reflect<Validator>;

   // 1-based source order by default.
   REQUIRE(R::index_of_field_number(1) == 0);
   REQUIRE(R::index_of_field_number(6) == 5);
   REQUIRE(R::index_of_field_number(0) == std::nullopt);
   REQUIRE(R::index_of_field_number(99) == std::nullopt);
}

TEST_CASE("reflect<T>::visit_field_by_name typed dispatch", "[reflect]")
{
   using R = psio::reflect<Validator>;

   Validator v{};
   v.effective_balance = 42;
   v.slashed           = true;

   std::uint64_t eb_seen = 0;
   bool          hit     = R::visit_field_by_name(
      v, "effective_balance",
      [&](auto& field) {
         if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>,
                                      std::uint64_t>)
            eb_seen = field;
      });
   REQUIRE(hit);
   REQUIRE(eb_seen == 42);

   REQUIRE(!R::visit_field_by_name(v, "no_such_field", [](auto&) {}));
}

TEST_CASE("reflect<T>::visit_field_by_number typed dispatch", "[reflect]")
{
   using R = psio::reflect<Validator>;

   Validator v{};
   v.activation_epoch = 100;

   std::uint64_t epoch_seen = 0;
   bool hit = R::visit_field_by_number(v, 5, [&](auto& field) {
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>,
                                   std::uint64_t>)
         epoch_seen = field;
   });
   REQUIRE(hit);
   REQUIRE(epoch_seen == 100);

   REQUIRE(!R::visit_field_by_number(v, 99, [](auto&) {}));
}

TEST_CASE("reflect<T>::for_each_field iterates all fields", "[reflect]")
{
   using R = psio::reflect<Validator>;

   Validator v{};
   v.effective_balance = 1'000;

   std::size_t count        = 0;
   std::size_t seen_eb_idx  = 0;
   std::string seen_eb_name;

   R::for_each_field(v, [&](auto Idx, std::string_view name, auto& field) {
      count += 1;
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>,
                                   std::uint64_t>)
         if (name == "effective_balance")
         {
            seen_eb_idx  = Idx;
            seen_eb_name = std::string{name};
         }
   });
   REQUIRE(count == 6);
   REQUIRE(seen_eb_idx == 2);
   REQUIRE(seen_eb_name == "effective_balance");
}

TEST_CASE("reflect<T> mutating visitor writes through to the object",
          "[reflect]")
{
   using R = psio::reflect<Point>;

   Point p{.x = 0, .y = 0};

   R::visit_field_by_name(p, "x", [](auto& field) {
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, int>)
         field = 7;
   });
   R::visit_field_by_number(p, 2, [](auto& field) {
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, int>)
         field = 13;
   });

   REQUIRE(p.x == 7);
   REQUIRE(p.y == 13);
}

namespace {
   // PSIO_REFLECT with no fields — used for resource markers like
   // wasi:io/poll's `pollable` (an empty struct deriving wit_resource).
   struct EmptyMarker {};
   PSIO_REFLECT(EmptyMarker)
}

TEST_CASE("reflect<T> with zero fields", "[reflect]")
{
   using R = psio::reflect<EmptyMarker>;

   STATIC_REQUIRE(R::is_reflected);
   STATIC_REQUIRE(R::name == std::string_view{"EmptyMarker"});
   STATIC_REQUIRE(R::member_count == 0);

   CHECK_FALSE(R::index_of("anything").has_value());
   CHECK_FALSE(R::index_of_field_number(1).has_value());

   EmptyMarker m;
   CHECK_FALSE(R::visit_field_by_name(m, "x", [](auto&) {}));
   CHECK_FALSE(R::visit_field_by_number(m, 1, [](auto&) {}));

   int touches = 0;
   R::for_each_field(m, [&](auto&&...) { ++touches; });
   CHECK(touches == 0);
}

// ── Traits are well-formed for every type category (G22) ────────────────
//
// A constraint on generic code is tried with references, cv-qualified
// types, void, function types and arrays (partial-specialization matching,
// overload resolution): asking must be `false`, never a hard error.
#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/invariant.hpp>
#include <psio/shapes.hpp>
#include <psio/transparent.hpp>

namespace reflect_sfinae
{
   struct Rec
   {
      int x;
   };
   PSIO_REFLECT(Rec, x)
   enum class Kind { a };
   PSIO_REFLECT_ENUM(Kind, a)

   template <typename T>
   constexpr int pick = 0;
   template <typename T>
      requires psio::Reflected<T>
   constexpr int pick<T> = 1;

#define PSIO_TEST_NOT_REFLECTED(X)                                        \
   static_assert(!psio::Reflected<X>);                                    \
   static_assert(!psio::reflect<X>::is_reflected);                        \
   static_assert(!psio::ReflectedEnum<X>);                                \
   static_assert(!psio::Record<X>);                                       \
   static_assert(!psio::is_transparent_v<X>);                             \
   static_assert(!psio::has_invariant_v<X>);                              \
   static_assert(!psio::is_dwnc_v<X>);                                    \
   static_assert(!psio::max_dynamic_data_v<X>.has_value());               \
   static_assert(!psio::max_fields_v<X>.has_value());                     \
   static_assert(!psio::has_adapter_v<X, psio::binary_category>);         \
   static_assert(pick<X> == 0);

   PSIO_TEST_NOT_REFLECTED(Rec&)
   PSIO_TEST_NOT_REFLECTED(const Rec&)
   PSIO_TEST_NOT_REFLECTED(Rec&&)
   PSIO_TEST_NOT_REFLECTED(int&)
   PSIO_TEST_NOT_REFLECTED(Kind&)
   PSIO_TEST_NOT_REFLECTED(void)
   PSIO_TEST_NOT_REFLECTED(const void)
   PSIO_TEST_NOT_REFLECTED(int(int))
   PSIO_TEST_NOT_REFLECTED(int(int) const)
   PSIO_TEST_NOT_REFLECTED(int (&)(int))
   PSIO_TEST_NOT_REFLECTED(Rec[3])
   PSIO_TEST_NOT_REFLECTED(Rec[])
   PSIO_TEST_NOT_REFLECTED(Rec*)
   PSIO_TEST_NOT_REFLECTED(int Rec::*)
#undef PSIO_TEST_NOT_REFLECTED

   // cv-qualified: well-formed, and not the registered type.
   static_assert(!psio::Reflected<const Rec>);
   static_assert(!psio::Reflected<volatile Rec>);
   static_assert(!psio::Reflected<const volatile Rec>);
   static_assert(pick<const Rec> == 0);
   // The type itself.
   static_assert(psio::Reflected<Rec>);
   static_assert(psio::Reflected<std::remove_cvref_t<const Rec&>>);
   static_assert(pick<Rec> == 1);
   static_assert(psio::ReflectedEnum<Kind>);
   static_assert(!psio::ReflectedEnum<const Kind&>);
}  // namespace reflect_sfinae

// ── member_types, index_of_member and the method equivalents (G20) ──────
namespace reflect_index
{
   struct Row
   {
      std::uint32_t id;
      std::string   name;
      std::uint32_t age;
      double        score;
      int           unlisted;
      void          rename(std::string to);
      int           total() const;
      static int    version();
      int           hidden() const;
      PSIO_REFLECT(Row, id, name, age, score, method(rename, to), method(total), method(version))
   };
   struct Empty
   {
      PSIO_REFLECT(Empty)
   };
   struct Other
   {
      std::uint32_t id;
      PSIO_REFLECT(Other, id)
   };
   using R = psio::reflect<Row>;

   static_assert(std::is_same_v<R::member_types,
                                std::tuple<std::uint32_t, std::string, std::uint32_t, double>>);
   static_assert(std::is_same_v<psio::reflect<Empty>::member_types, std::tuple<>>);
   static_assert(std::is_same_v<psio::reflect<Empty>::method_types, std::tuple<>>);
   static_assert(std::is_same_v<R::method_types,
                                std::tuple<void (Row::*)(std::string), int (Row::*)() const,
                                           int (*)()>>);

   // Two members of one type are told apart; the result is a constant.
   static_assert(R::find_member_index(&Row::id) == 0);
   static_assert(R::find_member_index(&Row::name) == 1);
   static_assert(R::find_member_index(&Row::age) == 2);
   static_assert(R::find_member_index(&Row::score) == 3);
   static_assert(R::index_of_member(&Row::age) == std::optional<std::size_t>{2});
   static_assert(std::is_same_v<R::member_type<R::find_member_index(&Row::name)>, std::string>);
   static_assert(R::member_name<R::find_member_index(&Row::score)> == "score");
   // Not a listed member; a member of another type; not a member pointer.
   static_assert(R::find_member_index(&Row::unlisted) == R::member_count);
   static_assert(!R::index_of_member(&Row::unlisted));
   static_assert(!R::index_of_member(&Other::id));
   static_assert(!R::index_of_member(42));
   static_assert(!psio::reflect<Empty>::index_of_member(&Row::id));

   static_assert(R::find_method_pointer_index(&Row::rename) == 0);
   static_assert(R::find_method_pointer_index(&Row::total) == 1);
   static_assert(R::find_method_pointer_index(&Row::version) == 2);
   static_assert(R::index_of_method_pointer(&Row::total) == std::optional<std::size_t>{1});
   static_assert(!R::index_of_method_pointer(&Row::hidden));
   static_assert(R::method_names[R::find_method_pointer_index(&Row::total)] == "total");

   // An unreflected type has none of these (the facade stays empty).
   template <typename T>
   concept has_member_types = requires { typename psio::reflect<T>::member_types; };
   static_assert(has_member_types<Row> && !has_member_types<int> && !has_member_types<Row&>);
}  // namespace reflect_index

TEST_CASE("reflect: index_of_member at run time", "[reflect]")
{
   using R = psio::reflect<reflect_index::Row>;
   std::uint32_t reflect_index::Row::* which = &reflect_index::Row::age;
   CHECK(R::find_member_index(which) == 2);
   which = &reflect_index::Row::id;
   CHECK(R::find_member_index(which) == 0);
}
