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
