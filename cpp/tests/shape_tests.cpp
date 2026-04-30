// Phase 2 — shape concept tests.

#include <psio/shapes.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

   enum class Color { red, green, blue };

   struct Rec
   {
      int a;
      int b;
   };
   PSIO_REFLECT(Rec, a, b)

   struct NotRecord
   {
      int x;
   };

}  // namespace

TEST_CASE("Primitive concept matches arithmetic + bool", "[shapes]")
{
   STATIC_REQUIRE((psio::Primitive<std::uint32_t>));
   STATIC_REQUIRE((psio::Primitive<std::int64_t>));
   STATIC_REQUIRE((psio::Primitive<float>));
   STATIC_REQUIRE((psio::Primitive<double>));
   STATIC_REQUIRE((psio::Primitive<bool>));

   STATIC_REQUIRE((!psio::Primitive<Color>));           // enum — separate concept
   STATIC_REQUIRE((!psio::Primitive<std::string>));     // string — separate
   STATIC_REQUIRE((!psio::Primitive<std::vector<int>>));
}

TEST_CASE("Enum concept matches std::is_enum", "[shapes]")
{
   STATIC_REQUIRE((psio::Enum<Color>));
   STATIC_REQUIRE((!psio::Enum<int>));
   STATIC_REQUIRE((!psio::Enum<Rec>));
}

TEST_CASE("FixedSequence matches std::array and C arrays", "[shapes]")
{
   using A = std::array<int, 4>;

   STATIC_REQUIRE((psio::FixedSequence<A>));
   STATIC_REQUIRE((psio::fixed_size_of_v<A> == 4));
   STATIC_REQUIRE((std::is_same_v<psio::element_of_t<A>, int>));

   STATIC_REQUIRE((psio::FixedSequence<int[7]>));
   STATIC_REQUIRE((psio::fixed_size_of_v<int[7]> == 7));

   STATIC_REQUIRE((!psio::FixedSequence<std::vector<int>>));
}

TEST_CASE("VariableSequence matches std::vector, not std::array/std::string",
          "[shapes]")
{
   STATIC_REQUIRE((psio::VariableSequence<std::vector<int>>));
   STATIC_REQUIRE((std::is_same_v<psio::element_of_t<std::vector<int>>, int>));

   STATIC_REQUIRE((!psio::VariableSequence<std::array<int, 4>>));
   STATIC_REQUIRE((!psio::VariableSequence<std::string>));
   STATIC_REQUIRE((!psio::VariableSequence<int>));
}

TEST_CASE("Optional / Variant concepts", "[shapes]")
{
   STATIC_REQUIRE((psio::Optional<std::optional<int>>));
   STATIC_REQUIRE((!psio::Optional<int>));

   STATIC_REQUIRE((psio::Variant<std::variant<int, double>>));
   STATIC_REQUIRE((!psio::Variant<int>));
}

TEST_CASE("Bitfield concept matches psio bitvector/bitlist", "[shapes]")
{
   STATIC_REQUIRE((psio::Bitfield<psio::bitvector<16>>));
   STATIC_REQUIRE((psio::Bitfield<psio::bitlist<32>>));
   STATIC_REQUIRE((!psio::Bitfield<std::vector<bool>>));
}

TEST_CASE("Record matches reflected types", "[shapes]")
{
   STATIC_REQUIRE((psio::Record<Rec>));
   STATIC_REQUIRE((!psio::Record<NotRecord>));
}

TEST_CASE("Shape umbrella concept", "[shapes]")
{
   STATIC_REQUIRE((psio::Shape<int>));
   STATIC_REQUIRE((psio::Shape<std::string>));
   STATIC_REQUIRE((psio::Shape<std::array<int, 3>>));
   STATIC_REQUIRE((psio::Shape<std::vector<int>>));
   STATIC_REQUIRE((psio::Shape<std::optional<int>>));
   STATIC_REQUIRE((psio::Shape<std::variant<int, double>>));
   STATIC_REQUIRE((psio::Shape<Color>));
   STATIC_REQUIRE((psio::Shape<Rec>));
   STATIC_REQUIRE((!psio::Shape<NotRecord>));  // no reflection, no shape match
}
