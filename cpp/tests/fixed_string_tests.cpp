// psio::fixed_string: a compile-time string usable as a template argument,
// with constexpr concatenation and comparison, and the reflected name in
// that form (reflect<T>::fixed_name, reflect_enum<E>::fixed_name).

#include <psio/fixed_string.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace fixed_string_tests
{
   struct User
   {
      std::uint32_t id;
      PSIO_REFLECT(User, id)
   };
   enum class Mode { on, off };
   PSIO_REFLECT_ENUM(Mode, on, off)

   // The two ways of taking one as a template argument.
   template <psio::fixed_string Name>
   struct Edge
   {
      static constexpr std::string_view name = Name;
   };
   template <auto Name>
   struct Any
   {
      static constexpr std::string_view name = Name;
   };
   // psibase's shape: a connection named from the node type.
   template <typename T, psio::fixed_string ConnectionName, psio::fixed_string EdgeName>
   struct Connection
   {
      static constexpr std::string_view connection = ConnectionName;
      static constexpr std::string_view edge       = EdgeName;
   };
   template <typename T>
   using ConnectionOf = Connection<T,
                                   psio::reflect<T>::fixed_name + "Connection",
                                   psio::reflect<T>::fixed_name + "Edge">;

   // Deduction, size, contents.
   static_assert(std::is_same_v<decltype(psio::fixed_string{"abc"}), psio::fixed_string<3>>);
   static_assert(psio::fixed_string{"abc"}.size() == 3);
   static_assert(psio::fixed_string{""}.empty());
   static_assert(psio::fixed_string{"abc"}.view() == "abc");
   static_assert(psio::fixed_string{"abc"}[1] == 'b');
   static_assert(psio::fixed_string{"abc"}.c_str()[3] == '\0');
   static_assert(psio::fixed_string<3>{std::string_view{"abc"}} == psio::fixed_string{"abc"});
   static_assert(std::is_convertible_v<psio::fixed_string<3>, std::string_view>);

   // Concatenation, in every combination.
   static_assert(psio::fixed_string{"User"} + psio::fixed_string{"Edge"} == "UserEdge");
   static_assert(psio::fixed_string{"User"} + "Edge" == "UserEdge");
   static_assert("User" + psio::fixed_string{"Edge"} == "UserEdge");
   static_assert("a" + psio::fixed_string{"b"} + "c" + psio::fixed_string{""} == "abc");
   static_assert(std::is_same_v<decltype(psio::fixed_string{"User"} + "Edge"),
                                psio::fixed_string<8>>);

   // Comparison: by characters, across lengths and against views.
   static_assert(psio::fixed_string{"abc"} == psio::fixed_string{"abc"});
   static_assert(psio::fixed_string{"abc"} != psio::fixed_string{"abd"});
   static_assert(psio::fixed_string{"abc"} != psio::fixed_string{"abcd"});
   static_assert(psio::fixed_string{"abc"} < psio::fixed_string{"abd"});
   static_assert(psio::fixed_string{"ab"} < psio::fixed_string{"abc"});
   static_assert(psio::fixed_string{"b"} > psio::fixed_string{"abc"});
   static_assert(psio::fixed_string{"abc"} == std::string_view{"abc"});
   static_assert(psio::fixed_string{"abc"} < std::string_view{"b"});
   static_assert("abc" == psio::fixed_string{"abc"});

   // A template argument: equal strings name one type, different ones do not.
   static_assert(std::is_same_v<Edge<"UserEdge">, Edge<psio::fixed_string{"User"} + "Edge">>);
   static_assert(!std::is_same_v<Edge<"UserEdge">, Edge<"UserEdgf">>);
   static_assert(!std::is_same_v<Edge<"User">, Edge<"UserEdge">>);
   static_assert(Edge<"UserEdge">::name == "UserEdge");
   static_assert(Any<psio::fixed_string{"x"} + "y">::name == "xy");

   // The reflected name in this form.
   static_assert(psio::reflect<User>::fixed_name == "User");
   static_assert(psio::reflect<User>::fixed_name == psio::reflect<User>::name);
   static_assert(std::is_same_v<decltype(psio::reflect<User>::fixed_name),
                                const psio::fixed_string<4>>);
   static_assert(psio::reflect_enum<Mode>::fixed_name == "Mode");
   static_assert(std::is_same_v<Edge<psio::reflect<User>::fixed_name + "Edge">, Edge<"UserEdge">>);
   static_assert(Any<psio::reflect<User>::fixed_name>::name == "User");
   static_assert(ConnectionOf<User>::connection == "UserConnection");
   static_assert(ConnectionOf<User>::edge == "UserEdge");
   static_assert(std::is_same_v<ConnectionOf<User>,
                                Connection<User, "UserConnection", "UserEdge">>);
}  // namespace fixed_string_tests

TEST_CASE("fixed_string at run time", "[fixed_string]")
{
   const auto name = psio::reflect<fixed_string_tests::User>::fixed_name + "Edge";
   CHECK(std::string{name.c_str()} == "UserEdge");
   CHECK(std::string{name.view()} == "UserEdge");
   CHECK(std::string(name.begin(), name.end()) == "UserEdge");
   std::string_view view = name;
   CHECK(view == "UserEdge");
   CHECK(name == std::string{"UserEdge"});
}
