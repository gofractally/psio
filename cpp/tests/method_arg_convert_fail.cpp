// MUST NOT COMPILE (except with no MAC_FAIL_* macro, the control).
//
// psio::method_args_to_pssz<T, I>(args...) accepts what an ordinary call of
// the method accepts: explicit constructors and explicit conversion
// operators are not used, a const object does not convert through a
// non-const operator, and the number of arguments is the method's.

#include <psio/pssz.hpp>

#include <cstdint>
#include <string>

struct Amount
{
   std::uint64_t value = 0;
   Amount()            = default;
   explicit Amount(std::uint64_t v) : value(v) {}
};
PSIO_REFLECT(Amount, value)

struct Name
{
   std::uint64_t value = 0;
};
PSIO_REFLECT(Name, definitionWillNotChange(), value)

struct Sealed
{
   explicit operator Name() const { return Name{1}; }
};
struct User
{
   operator Name() { return Name{2}; }
};

struct Svc
{
   void pay(Amount amount) {}
   void greet(Name who, std::string text) {}
   PSIO_REFLECT(Svc, method(pay, amount), method(greet, who, text))
};

int main()
{
   User user;
   auto ok = psio::method_args_to_pssz<Svc, 0>(Amount{5});
   auto ok2 = psio::method_args_to_pssz<Svc, 1>(user, "hi");
#if defined(MAC_FAIL_EXPLICIT_CONSTRUCTOR)
   auto bad = psio::method_args_to_pssz<Svc, 0>(5);
#elif defined(MAC_FAIL_EXPLICIT_OPERATOR)
   auto bad = psio::method_args_to_pssz<Svc, 1>(Sealed{}, "hi");
#elif defined(MAC_FAIL_CONST_OPERATOR)
   const User cuser;
   auto       bad = psio::method_args_to_pssz<Svc, 1>(cuser, "hi");
#elif defined(MAC_FAIL_UNRELATED)
   auto bad = psio::method_args_to_pssz<Svc, 1>(std::string("who"), "hi");
#elif defined(MAC_FAIL_TOO_FEW)
   auto bad = psio::method_args_to_pssz<Svc, 1>(user);
#elif defined(MAC_FAIL_TOO_MANY)
   auto bad = psio::method_args_to_pssz<Svc, 1>(user, "hi", 3);
#endif
   return int(ok.size() + ok2.size());
}
