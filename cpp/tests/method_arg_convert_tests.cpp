// psio::method_args_to_pssz<T, I>(args...) takes its arguments as an ordinary
// C++ call of the method does: an argument that is already a source of the
// parameter is written as it stands (never copied), anything else is
// converted IMPLICITLY to the parameter's type, and arguments are forwarded,
// so conversion operators that are not const (or are &&-qualified) work.

#include <catch.hpp>

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
   // An implicit constructor and an explicit one from another arithmetic
   // type: `Quantity q = 5;` picks the implicit one, a static_cast<Quantity>(5)
   // is ambiguous.
   struct Quantity
   {
      std::uint64_t value = 0;
      Quantity() = default;
      Quantity(std::uint64_t v) : value(v) {}
      explicit Quantity(double d) : value(static_cast<std::uint64_t>(d) + 1000) {}
      friend bool operator==(const Quantity&, const Quantity&) = default;
   };
   PSIO_REFLECT(Quantity, value)

   struct Name
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Name&, const Name&) = default;
   };
   PSIO_REFLECT(Name, definitionWillNotChange(), value)

   // Converts through an operator that is not const.
   struct UserContext
   {
      std::uint64_t id    = 0;
      int           calls = 0;
      operator Name()
      {
         ++calls;
         return Name{id};
      }
   };
   // Converts only as an rvalue.
   struct Ticket
   {
      std::uint64_t id = 0;
      operator Name() && { return Name{id}; }
   };
   // Converts only explicitly.
   struct Sealed
   {
      std::uint64_t id = 0;
      explicit operator Name() const { return Name{id}; }
   };

   int copies = 0;
   int moves  = 0;
   struct Counted
   {
      std::uint32_t v = 0;
      std::string   s;
      Counted() = default;
      Counted(std::uint32_t v_, std::string s_) : v(v_), s(std::move(s_)) {}
      Counted(const Counted& o) : v(o.v), s(o.s) { ++copies; }
      Counted(Counted&& o) noexcept : v(o.v), s(std::move(o.s)) { ++moves; }
      Counted& operator=(const Counted&) = default;
      Counted& operator=(Counted&&)      = default;
   };
   PSIO_REFLECT(Counted, v, s)

   struct Svc
   {
      void pay(Quantity amount) {}
      void greet(Name who, std::string text, std::uint32_t n) {}
      void keep(const Counted& c, std::vector<char> raw) {}
      void none() {}
      PSIO_REFLECT(Svc,
                   method(pay, amount),
                   method(greet, who, text, n),
                   method(keep, c, raw),
                   method(none))
   };

   template <std::size_t I>
   using Args = psio::reflect<Svc>::method_arg_tuple<I>;

   // What the proxy accepts, as a concept.
   static_assert(psio::method_arguments<Svc, 0, int>);
   static_assert(psio::method_arguments<Svc, 0, Quantity>);
   static_assert(psio::method_arguments<Svc, 0, const Quantity&>);
   static_assert(psio::method_arguments<Svc, 0, psio::pssz_view<Quantity>>);
   static_assert(!psio::method_arguments<Svc, 0, std::string>);
   static_assert(!psio::method_arguments<Svc, 0>);
   static_assert(!psio::method_arguments<Svc, 0, int, int>);
   static_assert(psio::method_arguments<Svc, 1, UserContext&, const char*, int>);
   // A const UserContext has no usable conversion, exactly as in a call.
   static_assert(!psio::method_arguments<Svc, 1, const UserContext&, const char*, int>);
   static_assert(psio::method_arguments<Svc, 1, Ticket, std::string_view, int>);
   static_assert(!psio::method_arguments<Svc, 1, Ticket&, std::string_view, int>);
   static_assert(!psio::method_arguments<Svc, 1, Sealed, std::string_view, int>);
   static_assert(psio::method_arguments<Svc, 3>);
}  // namespace

TEST_CASE("method arguments convert implicitly, as in a call", "[pssz][method][convert]")
{
   // int -> Quantity through the implicit std::uint64_t constructor; the
   // explicit double constructor is not a candidate.
   Quantity q = 5;
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(5) == psio::to_pssz(Args<0>{q})));
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(std::uint64_t{5}) == psio::to_pssz(Args<0>{q})));
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(q) == psio::to_pssz(Args<0>{q})));
   // A view of the parameter's type is a source: written as the value.
   auto qb = psio::to_pssz(q);
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(psio::pssz_view<Quantity>{qb}) ==
            psio::to_pssz(Args<0>{q})));
}

TEST_CASE("method arguments are forwarded: non-const and rvalue conversion operators",
          "[pssz][method][convert]")
{
   const auto  want = psio::to_pssz(Args<1>{Name{7}, std::string("hi"), 3u});
   UserContext user{7};
   REQUIRE((psio::method_args_to_pssz<Svc, 1>(user, "hi", 3) == want));
   REQUIRE(user.calls == 1);  // converted once
   REQUIRE((psio::method_args_to_pssz<Svc, 1>(UserContext{7}, std::string_view{"hi"}, 3) ==
            want));
   REQUIRE((psio::method_args_to_pssz<Svc, 1>(Ticket{7}, std::string("hi"), 3u) == want));
   REQUIRE((psio::method_args_to_pssz<Svc, 1>(Name{7}, "hi", std::uint8_t{3}) == want));
   std::vector<char> out{'x'};
   psio::method_args_append_pssz<Svc, 1>(out, user, "hi", 3);
   REQUIRE(out.size() == 1 + want.size());
   REQUIRE(user.calls == 2);
   REQUIRE(psio::method_args_to_pssz<Svc, 3>().empty() ==
           psio::to_pssz(Args<3>{}).empty());
}

TEST_CASE("an argument that is a source of its parameter is not copied",
          "[pssz][method][convert]")
{
   Counted           c{4, "abc"};
   std::vector<char> raw{1, 2, 3};
   const auto        want = psio::to_pssz(Args<2>{c, raw});
   copies = moves = 0;
   REQUIRE((psio::method_args_to_pssz<Svc, 2>(c, raw) == want));
   REQUIRE((psio::method_args_to_pssz<Svc, 2>(std::as_const(c), std::span<const char>{raw}) ==
            want));
   REQUIRE((psio::method_args_to_pssz<Svc, 2>(Counted{4, "abc"}, std::vector<char>{1, 2, 3}) ==
            want));
   REQUIRE((psio::method_args_to_pssz<Svc, 2>(std::cref(c), raw) == want));
   REQUIRE(copies == 0);
   REQUIRE(moves == 0);
}
