// The method surface of psio::reflect<T> is the same for every reflected
// type (a type that lists no method included), and the proxy's call
// convention: forwarding references, and where the reflected type comes from.

#include <psio/reflect.hpp>
#include <psio/structural.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace surface
{
   // No method(...): fields only.
   struct plain
   {
      std::uint32_t a = 0;
      std::string   b;
   };
   PSIO_REFLECT(plain, a, b)

   // Neither fields nor methods.
   struct empty_service
   {
   };
   PSIO_REFLECT(empty_service)

   // Reflected in the class body, no methods.
   class sealed
   {
      int hidden_ = 0;
      PSIO_REFLECT(sealed, hidden_)
   };

   struct service
   {
      std::uint64_t        add(std::uint32_t x, const std::string& tag) const { return x + tag.size(); }
      void                 reset() noexcept {}
      static std::uint16_t version() { return 7; }
   };
   PSIO_REFLECT(service, method(add, x, tag), method(reset), method(version))

   // A generic walker that names EVERY method member of reflect<T> inside
   // the per-method callback. It must compile for any reflected T.
   struct method_row
   {
      std::size_t              index = 0;
      std::string              name;
      std::vector<std::string> params;
      std::size_t              arity     = 0;
      bool                     is_static = false;
      bool                     is_const  = false;
      bool                     returns   = false;
      bool                     pointer_matches_type = false;

      friend bool operator==(const method_row&, const method_row&) = default;
   };

   template <typename T>
   std::vector<method_row> describe()
   {
      using R = psio::reflect<T>;
      std::vector<method_row> rows;
      R::for_each_method([&](auto index, std::string_view name) {
         constexpr std::size_t I = decltype(index)::value;
         method_row            row;
         row.index = I;
         row.name  = std::string{name};
         for (std::string_view p : R::template method_param_names<I>)
            row.params.emplace_back(p);
         using traits = typename R::template method_traits<I>;
         using result = typename R::template method_result<I>;
         using args   = typename R::template method_args<I>;
         using params = typename R::template method_params<I>;
         using type   = typename R::template method_type<I>;
         static_assert(std::tuple_size_v<args> == traits::arity);
         static_assert(std::tuple_size_v<params> == traits::arity);
         static_assert(std::is_same_v<result, typename traits::result>);
         row.arity                = traits::arity;
         row.is_static            = R::template method_is_static<I>;
         row.is_const             = R::template method_is_const<I>;
         row.returns              = !std::is_void_v<result>;
         row.pointer_matches_type = std::is_same_v<decltype(R::template method_pointer<I>), const type>;
         CHECK(R::template method_name<I> == name);
         CHECK(R::index_of_method(name) == std::optional<std::size_t>{I});
         CHECK(R::find_method_index(name) == I);
         // invoke_method is named, too: callable with the declared arguments.
         static_assert(requires(T& object, params& values) {
            R::template invoke_method<I>(object, values);
         });
         rows.push_back(std::move(row));
      });
      CHECK(rows.size() == R::method_count);
      CHECK(R::method_names.size() == R::method_count);
      CHECK(R::find_method_index("no_such_method") == R::method_count);
      CHECK(!R::index_of_method("no_such_method"));
      return rows;
   }

   struct fnv
   {
      constexpr std::uint64_t operator()(std::string_view s) const
      {
         std::uint64_t h = 0xcbf29ce484222325ull;
         for (char c : s)
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
         return h;
      }
   };
}  // namespace surface

// A type without methods has the members; nothing is instantiated with an
// index.
static_assert(psio::reflect<surface::plain>::method_count == 0);
static_assert(psio::reflect<surface::plain>::method_names.empty());
static_assert(psio::reflect<surface::plain>::method_param_name_lists.empty());
static_assert(psio::reflect<surface::plain>::find_method_index("a") == 0);
static_assert(!psio::reflect<surface::plain>::index_of_method("a"));
static_assert(!psio::reflect<surface::plain>::index_of_method_hash<surface::fnv>(surface::fnv{}("a")));
static_assert(psio::reflect<surface::empty_service>::method_count == 0);
static_assert(psio::reflect<surface::empty_service>::member_count == 0);

TEST_CASE("reflect methods: one surface for types with and without methods", "[reflect][methods]")
{
   CHECK(surface::describe<surface::plain>().empty());
   CHECK(surface::describe<surface::empty_service>().empty());
   CHECK(surface::describe<surface::sealed>().empty());

   using row = surface::method_row;
   CHECK(surface::describe<surface::service>() ==
         std::vector<row>{row{0, "add", {"x", "tag"}, 2, false, true, true, true},
                          row{1, "reset", {}, 0, false, false, false, true},
                          row{2, "version", {}, 0, true, false, true, true}});

   // A walk over a method-less type calls nothing, at compile time too.
   static constexpr std::size_t calls = [] {
      std::size_t n = 0;
      psio::reflect<surface::plain>::for_each_method([&](auto, std::string_view) { ++n; });
      return n;
   }();
   STATIC_REQUIRE(calls == 0);
   static constexpr std::size_t name_chars = [] {
      std::size_t n = 0;
      psio::reflect<surface::service>::for_each_method(
         [&](auto, std::string_view name) { n += name.size(); });
      return n;
   }();
   STATIC_REQUIRE(name_chars == 3 + 5 + 7);
}

// ── The proxy forwards references ──────────────────────────────────────────

namespace surface
{
   // Records the value category each argument arrives with.
   struct category_recorder
   {
      std::vector<std::string>* log;
      template <std::size_t I, auto MemberPtr, typename... Args>
      void call(Args&&...)
      {
         std::string row = std::to_string(I);
         auto        add = [&]<typename A>() {
            row += std::is_lvalue_reference_v<A>
                      ? (std::is_const_v<std::remove_reference_t<A>> ? " const&" : " &")
                      : " &&";
         };
         (add.template operator()<Args>(), ...);
         log->push_back(row);
      }
   };

   // Counts copies and moves of an argument on its way through the proxy.
   struct counted
   {
      static inline int copies = 0;
      static inline int moves  = 0;
      counted()                = default;
      counted(const counted&) { ++copies; }
      counted(counted&&) noexcept { ++moves; }
   };
   struct sink
   {
      void take(counted value, const counted& ref);
   };
   PSIO_REFLECT(sink, method(take, value, ref))

   // A dispatcher that wants owned arguments decays them itself.
   struct owning_dispatch
   {
      template <std::size_t I, auto MemberPtr, typename... Args>
      auto call(Args&&... args)
      {
         return std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...);
      }
   };
}  // namespace surface

TEST_CASE("reflect proxy: arguments are forwarded as references", "[reflect][proxy]")
{
   std::vector<std::string>                                    log;
   psio::reflect<surface::service>::proxy<surface::category_recorder> api{&log};
   std::string       tag = "t";
   const std::string ctag = "c";
   std::uint32_t     x   = 1;
   api.add(x, tag);
   api.add(2u, ctag);
   api.add(std::move(x), std::string{"temp"});
   CHECK(log == std::vector<std::string>{"0 & &", "0 && const&", "0 && &&"});

   // Nothing is copied or moved on the way to call().
   psio::reflect<surface::sink>::proxy<surface::category_recorder> sink_api{&log};
   surface::counted                                                 value;
   surface::counted::copies = surface::counted::moves = 0;
   sink_api.take(value, value);
   sink_api.take(surface::counted{}, std::as_const(value));
   CHECK(surface::counted::copies == 0);
   CHECK(surface::counted::moves == 0);
   CHECK(log.back() == "0 && const&");

   // The dispatcher decides about ownership: here it decays.
   psio::reflect<surface::sink>::proxy<surface::owning_dispatch> owning;
   auto owned = owning.take(value, std::as_const(value));
   STATIC_REQUIRE(std::is_same_v<decltype(owned), std::tuple<surface::counted, surface::counted>>);
   CHECK(surface::counted::copies == 2);
}

// ── The reflected type of a proxied call ───────────────────────────────────

namespace surface
{
   struct base_service
   {
      std::uint32_t inherited(std::uint32_t x) const { return x; }
   };
   PSIO_REFLECT(base_service, method(inherited, x))

   struct derived_service : base_service
   {
      std::uint32_t own(std::uint32_t x) { return x + 1; }
      static void   stat() {}
   };
   // Index 1 is `inherited` here and nothing in base_service; index 0 is
   // `own` here and `inherited` in base_service.
   PSIO_REFLECT(derived_service, method(own, x), method(inherited, x), method(stat))

   // A dispatcher that is told the reflected type: right for every method.
   template <typename T>
   struct typed_dispatch
   {
      template <std::size_t I, auto MemberPtr, typename... Args>
      std::string call(Args&&...)
      {
         static_assert(std::is_same_v<decltype(MemberPtr),
                                      typename psio::reflect<T>::template method_type<I>>);
         return std::string{psio::reflect<T>::name} + "." +
                std::string{psio::reflect<T>::template method_name<I>};
      }
   };

   // A dispatcher that recovers the class from the member pointer, checked.
   struct pointer_dispatch
   {
      template <std::size_t I, auto MemberPtr, typename... Args>
      std::string call(Args&&...)
      {
         using C = psio::proxied_class<I, MemberPtr>;
         return std::string{psio::reflect<C>::name} + "." +
                std::string{psio::reflect<C>::template method_name<I>};
      }
   };

   template <std::size_t I, auto MemberPtr>
   concept has_proxied_class = requires { typename psio::detail::proxied_class_of<I, MemberPtr>; } &&
                               psio::detail::method_of_reflection<
                                  typename psio::method_traits<decltype(MemberPtr)>::class_type, I,
                                  MemberPtr>;
}  // namespace surface

// The language's type of &derived_service::inherited names the base.
static_assert(std::is_same_v<psio::reflect<surface::derived_service>::method_type<1>,
                             std::uint32_t (surface::base_service::*)(std::uint32_t) const>);
static_assert(std::is_same_v<
              psio::reflect<surface::derived_service>::method_traits<1>::class_type,
              surface::base_service>);
static_assert(std::is_void_v<psio::reflect<surface::derived_service>::method_traits<2>::class_type>);
static_assert(std::is_same_v<psio::proxied_class<0, &surface::derived_service::own>,
                             surface::derived_service>);
static_assert(std::is_same_v<psio::proxied_class<0, &surface::base_service::inherited>,
                             surface::base_service>);
static_assert(surface::has_proxied_class<0, &surface::derived_service::own>);
// `inherited` is method 1 of derived_service; its pointer's class is the base,
// which has no method 1 (and whose method 0 would be the wrong answer for a
// different index): proxied_class refuses it.
static_assert(!surface::has_proxied_class<1, &surface::derived_service::inherited>);

TEST_CASE("reflect proxy: the reflected type of an inherited or static method",
          "[reflect][proxy]")
{
   using DR = psio::reflect<surface::derived_service>;
   DR::proxy<surface::typed_dispatch<surface::derived_service>> typed;
   CHECK(typed.own(1) == "derived_service.own");
   CHECK(typed.inherited(1) == "derived_service.inherited");
   CHECK(typed.stat() == "derived_service.stat");

   // The interface proxy of a reflected type is the same proxy.
   DR::proxy<surface::pointer_dispatch> by_pointer;
   CHECK(by_pointer.own(1) == "derived_service.own");
   psio::reflect<surface::base_service>::proxy<surface::pointer_dispatch> base;
   CHECK(base.inherited(1) == "base_service.inherited");
   // by_pointer.inherited(1) and by_pointer.stat() do not compile:
   // reflect_method_surface_fail.cpp.

   // Metadata and invocation of an inherited method work as for any other.
   surface::derived_service object;
   CHECK(DR::invoke_method<1>(object, std::tuple{std::uint32_t{5}}) == 5);
   CHECK(DR::method_param_names<1>[0] == "x");
}
