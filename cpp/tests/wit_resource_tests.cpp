// libraries/psio/cpp/tests/wit_resource_tests.cpp
//
// Vocabulary checks for psio::wit_resource / own<T> / borrow<T>.

#include <psio/wit_resource.hpp>

#include <catch.hpp>

#include <cstdint>
#include <type_traits>

namespace {

   struct my_resource : psio::wit_resource
   {
   };

   struct not_a_resource
   {
   };

   // Track drop calls so the test can verify RAII fires.
   inline int drop_count = 0;

}  // namespace

namespace psio {
   template <>
   inline void wit_resource_drop<my_resource>(std::uint32_t /*handle*/)
   {
      ++drop_count;
   }
}

TEST_CASE("is_wit_resource_v classifies correctly", "[wit_resource][trait]")
{
   STATIC_REQUIRE(psio::is_wit_resource_v<my_resource>);
   STATIC_REQUIRE_FALSE(psio::is_wit_resource_v<not_a_resource>);
   STATIC_REQUIRE_FALSE(psio::is_wit_resource_v<int>);
}

TEST_CASE("own<T> is move-only RAII", "[wit_resource][own]")
{
   drop_count = 0;
   {
      psio::own<my_resource> a{42};
      REQUIRE(a.handle == 42);
   }
   REQUIRE(drop_count == 1);

   // Move transfers ownership; only the surviving handle drops.
   drop_count = 0;
   {
      psio::own<my_resource> a{1};
      psio::own<my_resource> b{std::move(a)};
      REQUIRE(b.handle == 1);
      REQUIRE(a.handle == psio::own<my_resource>::null_handle);
   }
   REQUIRE(drop_count == 1);

   // release() suppresses the drop.
   drop_count = 0;
   {
      psio::own<my_resource> a{7};
      auto                    raw = a.release();
      REQUIRE(raw == 7);
      REQUIRE(a.handle == psio::own<my_resource>::null_handle);
   }
   REQUIRE(drop_count == 0);
}

TEST_CASE("borrow<T> is a non-owning u32 wrapper", "[wit_resource][borrow]")
{
   drop_count = 0;
   {
      psio::borrow<my_resource> b{99};
      REQUIRE(b.handle == 99);
   }
   REQUIRE(drop_count == 0);
}

TEST_CASE("own<T> implicitly converts to borrow<T>",
          "[wit_resource][borrow]")
{
   psio::own<my_resource> o{5};
   psio::borrow<my_resource> b = o;  // implicit conversion
   REQUIRE(b.handle == 5);
   (void)o.release();  // suppress drop in test
}

TEST_CASE("non-resource T fails the static_assert at instantiation",
          "[wit_resource][trait]")
{
   // We can name own<int> as a type at the trait level (no
   // instantiation), but actually instantiating own<int> would fire
   // the static_assert.  Just confirm the detection trait.
   STATIC_REQUIRE_FALSE(
      psio::is_wit_resource_v<not_a_resource>);
}
