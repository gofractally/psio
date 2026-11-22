// Uses of the method surface that must not compile, each with a message
// naming the reason. One case per -D.

#include <psio/reflect.hpp>

#include <cstdint>
#include <string>

namespace fail
{
   struct plain
   {
      int a = 0;
   };
   PSIO_REFLECT(plain, a)

   struct one
   {
      int f(int);
   };
   PSIO_REFLECT(one, method(f, x))

   struct base
   {
      int inherited(int) const;
   };
   PSIO_REFLECT(base, method(inherited, x))
   struct derived : base
   {
      int         own(int);
      static void stat();
   };
   PSIO_REFLECT(derived, method(own, x), method(inherited, x), method(stat))

   struct pointer_dispatch
   {
      template <std::size_t I, auto MemberPtr, typename... Args>
      auto call(Args&&...)
      {
         return psio::reflect<psio::proxied_class<I, MemberPtr>>::template method_name<I>;
      }
   };
}  // namespace fail

// A type without methods has the member templates but no index.
#if defined(METHOD_FAIL_EMPTY_PARAM_NAMES)
auto x = psio::reflect<fail::plain>::method_param_names<0>;
#elif defined(METHOD_FAIL_EMPTY_TYPE)
using x = psio::reflect<fail::plain>::method_type<0>;
#elif defined(METHOD_FAIL_EMPTY_RESULT)
using x = psio::reflect<fail::plain>::method_result<0>;
#elif defined(METHOD_FAIL_EMPTY_NAME)
auto x = psio::reflect<fail::plain>::method_name<0>;
#elif defined(METHOD_FAIL_EMPTY_POINTER)
auto x = psio::reflect<fail::plain>::method_pointer<0>;
#elif defined(METHOD_FAIL_EMPTY_IS_CONST)
bool x = psio::reflect<fail::plain>::method_is_const<0>;
#elif defined(METHOD_FAIL_EMPTY_INVOKE)
void x(fail::plain& p) { psio::reflect<fail::plain>::invoke_method<0>(p, std::tuple{}); }
// An index past the last method of a type that has some.
#elif defined(METHOD_FAIL_PAST_END)
using x = psio::reflect<fail::one>::method_args<1>;
// The walk hands out the index and the name, not the pointer.
#elif defined(METHOD_FAIL_WALK_POINTER)
void x() { psio::reflect<fail::one>::for_each_method([](auto, std::string_view, auto) {}); }
// A dispatcher that recovers the class from the member pointer: an inherited
// method has its base's class, a static method none.
#elif defined(METHOD_FAIL_PROXY_INHERITED)
auto x() { return psio::reflect<fail::derived>::proxy<fail::pointer_dispatch>{}.inherited(1); }
#elif defined(METHOD_FAIL_PROXY_STATIC)
auto x() { return psio::reflect<fail::derived>::proxy<fail::pointer_dispatch>{}.stat(); }
#else
#error "select a case"
#endif
