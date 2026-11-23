// Method reflection in PSIO_REFLECT, the one proxy, and interfaces derived
// from a reflected type (reflect.hpp, structural.hpp).

#include <psio/compress_name.hpp>
#include <psio/emit_wit.hpp>
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/structural.hpp>
#include <psio/wit_abi.hpp>
#include <psio/wit_constexpr.hpp>
#include <psio/wit_gen.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// ── A service-like class in a nested namespace, reflected after the class ──

namespace chain::svc
{
   struct account
   {
      std::string   owner;
      std::uint64_t balance = 0;
   };
   PSIO_REFLECT(account, owner, balance)

   class token
   {
     public:
      std::uint64_t supply = 0;
      std::string   symbol;

      std::uint64_t balance_of(const std::string& owner) const { return owner.size() + supply; }
      void          transfer(std::string from, std::string to, std::uint64_t amount)
      {
         supply += amount + from.size() + to.size();
      }
      static std::uint32_t   version() { return 3; }
      std::optional<account> find(std::uint64_t id) noexcept
      {
         return id ? std::optional<account>{account{"x", id}} : std::nullopt;
      }
      void          ping() {}
      std::uint64_t unnamed(std::uint32_t a, std::uint32_t b) const noexcept { return a + b; }
   };
   // Fields and methods in one macro. Parameter names are optional.
   PSIO_REFLECT(token,
                supply,
                symbol,
                method(balance_of, owner),
                method(transfer, from, to, amount),
                method(version),
                method(find, id),
                method(ping),
                method(unnamed))

   // No PSIO_PACKAGE in scope: the package defaults to the namespace.
   PSIO_INTERFACE(token, types(account))

   // ── The same, written inside the class body, with private members ──────
   class vault
   {
      std::uint64_t locked_ = 5;
      std::string   owner_  = "root";

      std::uint64_t unlock(std::uint64_t amount)
      {
         locked_ -= amount;
         return locked_;
      }

      PSIO_REFLECT(vault, locked_, owner_, method(unlock, amount), method(peek), method(reset, to))

      // Declared after the macro: the metadata is looked up when first used.
      std::uint64_t        peek() const { return locked_; }
      static std::uint64_t reset(std::uint64_t to) { return to; }

     public:
      std::uint64_t locked() const { return locked_; }
   };

   struct unreflected
   {
   };
}  // namespace chain::svc

using chain::svc::token;
using chain::svc::vault;
using TR = psio::reflect<token>;
using VR = psio::reflect<vault>;

// ── Compile-time surface ───────────────────────────────────────────────────

static_assert(TR::is_reflected);
static_assert(TR::member_count == 2);
static_assert(TR::method_count == 6);
static_assert(!psio::reflect<chain::svc::unreflected>::is_reflected);
static_assert(psio::reflect<chain::svc::account>::method_count == 0);
static_assert(!psio::reflect<chain::svc::account>::index_of_method("owner"));

static_assert(TR::method_pointer<0> == &token::balance_of);
static_assert(TR::method_pointer<1> == &token::transfer);
static_assert(TR::method_pointer<2> == &token::version);
static_assert(std::is_same_v<TR::method_type<0>,
                             std::uint64_t (token::*)(const std::string&) const>);
static_assert(std::is_same_v<TR::method_type<2>, std::uint32_t (*)()>);
static_assert(std::is_same_v<TR::method_type<3>,
                             std::optional<chain::svc::account> (token::*)(std::uint64_t) noexcept>);

static_assert(TR::method_name<0> == "balance_of");
static_assert(TR::method_names.size() == 6);
static_assert(TR::method_names[1] == "transfer" && TR::method_names[5] == "unnamed");

static_assert(TR::method_param_names<0>.size() == 1 && TR::method_param_names<0>[0] == "owner");
static_assert(TR::method_param_names<1>.size() == 3 && TR::method_param_names<1>[2] == "amount");
static_assert(TR::method_param_names<2>.empty());
static_assert(TR::method_param_names<5>.empty());  // names are optional

static_assert(std::is_same_v<TR::method_result<0>, std::uint64_t>);
static_assert(std::is_same_v<TR::method_result<1>, void>);
static_assert(std::is_same_v<TR::method_args<0>, std::tuple<const std::string&>>);
static_assert(std::is_same_v<TR::method_params<0>, std::tuple<std::string>>);
static_assert(
   std::is_same_v<TR::method_params<1>, std::tuple<std::string, std::string, std::uint64_t>>);
static_assert(std::is_same_v<TR::method_params<2>, std::tuple<>>);
static_assert(TR::method_is_const<0> && !TR::method_is_static<0>);
static_assert(!TR::method_is_const<1> && !TR::method_is_static<1>);
static_assert(TR::method_is_static<2>);
static_assert(TR::method_is_const<5>);
static_assert(TR::method_traits<1>::arity == 3);

static_assert(TR::index_of_method("transfer") == std::size_t{1});
static_assert(TR::index_of_method("unnamed") == std::size_t{5});
static_assert(!TR::index_of_method("supply"));  // a field is not a method
static_assert(!TR::index_of_method("nope"));
static_assert(TR::find_method_index("nope") == TR::method_count);
static_assert(TR::index_of("supply") == std::size_t{0});
static_assert(!TR::index_of("transfer"));

// In-class reflection of private members.
static_assert(VR::is_reflected);
static_assert(VR::member_count == 2 && VR::method_count == 3);
static_assert(VR::member_name<0> == "locked_");
static_assert(std::is_same_v<VR::member_type<1>, std::string>);
static_assert(std::is_same_v<VR::method_type<1>, std::uint64_t (vault::*)() const>);
static_assert(VR::method_is_static<2>);
static_assert(VR::index_of_method("peek") == std::size_t{1});

// wit_abi's MemberPtrType reads the same pointers.
static_assert(psio::MemberPtrType<TR::method_type<1>>::numArgs == 3);
static_assert(std::is_same_v<psio::MemberPtrType<TR::method_type<0>>::ReturnType, std::uint64_t>);
static_assert(psio::MemberPtrType<TR::method_type<2>>::numArgs == 0);

namespace
{
   // The caller's own 64-bit hash of a method name.
   struct method_hash
   {
      constexpr std::uint64_t operator()(std::string_view s) const noexcept
      {
         std::uint64_t h = 0xcbf29ce484222325ull;
         for (char c : s)
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
         return h;
      }
   };
}  // namespace

static_assert(TR::index_of_method_hash<method_hash>(method_hash{}("find")) == std::size_t{3});
static_assert(!TR::index_of_method_hash<method_hash>(method_hash{}("supply")));
static_assert(!TR::index_of_method_hash<method_hash>(12345));

TEST_CASE("reflect methods: for_each_method and invoke_method", "[reflect][methods]")
{
   std::vector<std::string> seen;
   std::size_t              statics = 0;
   TR::for_each_method([&](auto index, std::string_view name) {
      seen.push_back(std::to_string(index.value) + ":" + std::string{name});
      if (!std::is_member_function_pointer_v<TR::method_type<decltype(index)::value>>)
         ++statics;
   });
   CHECK(seen == std::vector<std::string>{"0:balance_of", "1:transfer", "2:version", "3:find",
                                          "4:ping", "5:unnamed"});
   CHECK(statics == 1);

   token t;
   t.supply = 10;
   CHECK(TR::invoke_method<0>(t, std::tuple{std::string{"abc"}}) == 13);
   TR::invoke_method<1>(t, std::tuple{std::string{"a"}, std::string{"bc"}, std::uint64_t{7}});
   CHECK(t.supply == 20);
   CHECK(TR::invoke_method<2>(t, std::tuple{}) == 3);
   CHECK(TR::invoke_method<3>(t, std::tuple{std::uint64_t{9}})->balance == 9);
   const token& ct = t;
   CHECK(TR::invoke_method<5>(ct, std::tuple{std::uint32_t{2}, std::uint32_t{3}}) == 5);
   static_assert(std::is_void_v<decltype(TR::invoke_method<4>(t, std::tuple{}))>);

   vault v;
   CHECK(VR::invoke_method<0>(v, std::tuple{std::uint64_t{2}}) == 3);
   CHECK(VR::invoke_method<1>(v, std::tuple{}) == 3);
   CHECK(VR::invoke_method<2>(v, std::tuple{std::uint64_t{11}}) == 11);
   std::vector<std::string> fields;
   VR::for_each_field(v, [&](auto, std::string_view name, auto&) { fields.emplace_back(name); });
   CHECK(fields == std::vector<std::string>{"locked_", "owner_"});
}

TEST_CASE("reflect methods: dispatch by hashed name", "[reflect][methods]")
{
   // An action arrives as (hash of the method name, argument tuple bytes).
   token t;
   t.supply = 1;
   auto dispatch = [&](std::uint64_t key) -> std::string {
      const auto index = TR::index_of_method_hash<method_hash>(key);
      if (!index)
         return "unknown";
      std::string out;
      [&]<std::size_t... I>(std::index_sequence<I...>) {
         (void)((*index == I ? (out = std::string{TR::method_name<I>} + "/" +
                                      std::to_string(TR::method_traits<I>::arity),
                                true)
                             : false) ||
                ...);
      }(std::make_index_sequence<TR::method_count>{});
      return out;
   };
   CHECK(dispatch(method_hash{}("transfer")) == "transfer/3");
   CHECK(dispatch(method_hash{}("version")) == "version/0");
   CHECK(dispatch(method_hash{}("symbol")) == "unknown");

   // psio's own compressed method names work as the caller-supplied hash.
   struct compressed
   {
      constexpr std::uint64_t operator()(std::string_view s) const
      {
         return psio::detail::method_to_number(s);
      }
   };
   CHECK(TR::index_of_method_hash<compressed>(psio::detail::method_to_number("ping")) ==
         std::size_t{4});
}

// ── The one proxy ──────────────────────────────────────────────────────────

namespace
{
   struct call_record
   {
      std::size_t index = 0;
      bool        is_transfer   = false;
      bool        is_balance_of = false;
      bool        is_version    = false;
      std::string args;
   };

   // "call" style dispatcher: records (I, member pointer, args). It has
   // constructor arguments of its own.
   struct recorder
   {
      std::vector<call_record>* log;
      std::string               sender;
      recorder(std::vector<call_record>* log_, std::string sender_)
          : log(log_), sender(std::move(sender_))
      {
      }

      template <std::size_t I, auto MemberPtr, typename... Args>
      auto call(Args&&... args)
      {
         call_record r;
         r.index = I;
         if constexpr (std::is_same_v<decltype(MemberPtr), decltype(&token::transfer)>)
            r.is_transfer = MemberPtr == &token::transfer;
         if constexpr (std::is_same_v<decltype(MemberPtr), decltype(&token::balance_of)>)
            r.is_balance_of = MemberPtr == &token::balance_of;
         if constexpr (std::is_same_v<decltype(MemberPtr), decltype(&token::version)>)
            r.is_version = MemberPtr == &token::version;
         r.args = sender;
         auto add = [&](const auto& a) {
            r.args += '|';
            if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(a)>>)
               r.args += std::to_string(a);
            else
               r.args += a;
         };
         (add(args), ...);
         log->push_back(r);
         return sizeof...(Args);
      }
   };

   // The same shape as an aggregate (no constructors).
   struct aggregate_recorder
   {
      int         a = 0;
      std::string b;
      template <std::size_t I, auto MemberPtr, typename... Args>
      auto call(Args&&...)
      {
         return std::to_string(I) + b + std::to_string(a + static_cast<int>(sizeof...(Args)));
      }
   };

   // Field projection: get<I, &T::field>() over an object.
   struct field_projection
   {
      token* object;
      explicit field_projection(token* o) : object(o) {}
      template <std::size_t I, auto MemberPtr>
      decltype(auto) get()
      {
         visited.push_back(I);
         return (object->*MemberPtr);
      }
      template <std::size_t I, auto MemberPtr>
      decltype(auto) get() const
      {
         return (std::as_const(*object).*MemberPtr);
      }
      std::vector<std::size_t> visited;
   };
}  // namespace

TEST_CASE("reflect proxy: methods forward to call<I, &T::method>(args...)", "[reflect][proxy]")
{
   std::vector<call_record> log;
   using proxy_t = TR::proxy<recorder>;
   proxy_t api{&log, "alice"};  // the dispatcher's own constructor arguments

   CHECK(api.transfer("alice", std::string{"bob"}, 25) == 3);
   CHECK(api.balance_of("bob") == 1);
   CHECK(api.version() == 0);
   CHECK(api.sender == "alice");

   REQUIRE(log.size() == 3);
   CHECK(log[0].index == 1);
   CHECK(log[0].is_transfer);
   CHECK(log[0].args == "alice|alice|bob|25");
   CHECK(log[1].index == 0);
   CHECK(log[1].is_balance_of);
   CHECK(log[1].args == "alice|bob");
   CHECK(log[2].index == 2);
   CHECK(log[2].is_version);

   // Copy / from-dispatcher construction still work.
   proxy_t copy{api};
   CHECK(copy.ping() == 0);
   proxy_t from{recorder{&log, "carol"}};
   CHECK(from.sender == "carol");
   CHECK(log.back().index == 4);
}

TEST_CASE("reflect proxy: an aggregate dispatcher is built from its members",
          "[reflect][proxy]")
{
   using proxy_t = TR::proxy<aggregate_recorder>;
   proxy_t api{4, "x"};
   CHECK(api.find(1) == "3x5");
   CHECK(api.unnamed(1, 2) == "5x6");
   proxy_t empty;
   CHECK(empty.ping() == "40");
}

TEST_CASE("reflect proxy: fields forward to get<I, &T::field>()", "[reflect][proxy]")
{
   token t;
   t.supply = 77;
   t.symbol = "PSI";
   TR::proxy<field_projection> view{&t};
   CHECK(view.supply() == 77);
   view.symbol() = "TOK";
   CHECK(t.symbol == "TOK");
   CHECK(view.visited == std::vector<std::size_t>{0, 1});
   const auto& cview = view;
   CHECK(cview.symbol() == "TOK");
   static_assert(std::is_same_v<decltype(cview.supply()), const std::uint64_t&>);
}

// ── An interface taken from the reflected type ─────────────────────────────

template <typename T>
concept has_interface_of = requires { typename psio::interface_of<T>::type; };
template <typename T>
concept has_interface = requires { psio::detail::interface_info<T>::name; };

TEST_CASE("interface from a reflected type: metadata and default package", "[structural]")
{
   using info = psio::detail::interface_info<token>;
   STATIC_REQUIRE(info::name == std::string_view{"token"});
   STATIC_REQUIRE(info::package::name == std::string_view{"chain::svc"});
   STATIC_REQUIRE(info::package::version.empty());
   STATIC_REQUIRE(std::is_same_v<info::types, std::tuple<chain::svc::account>>);
   STATIC_REQUIRE(std::tuple_size_v<info::func_types> == 6);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<1, info::func_types>,
                                 decltype(&token::transfer)>);
   STATIC_REQUIRE(info::func_names.size() == 6);
   CHECK(info::func_names[3] == "find");
   CHECK(info::param_names[1].size() == 3);
   CHECK(std::string_view{*info::param_names[0].begin()} == "owner");
   STATIC_REQUIRE(info::method<0>() == &token::balance_of);
   STATIC_REQUIRE(
      std::is_same_v<typename psio::interface_of<chain::svc::account>::type, token>);
   STATIC_REQUIRE(!has_interface_of<chain::svc::unreflected>);
   STATIC_REQUIRE(!has_interface<vault>);
   STATIC_REQUIRE(has_interface<token>);

   // The interface proxy is the reflect proxy: one call convention.
   std::vector<call_record>    log;
   info::proxy<recorder>       api{&log, "dan"};
   CHECK(api.transfer("a", "b", 1) == 3);
   CHECK(log.at(0).is_transfer);
}

TEST_CASE("interface from a reflected type: WIT text", "[structural][wit]")
{
   const std::string expected =
      "package chain:svc;\n"
      "\n"
      "interface token {\n"
      "  record account {\n"
      "    owner: string,\n"
      "    balance: u64,\n"
      "  }\n"
      "\n"
      "  balance-of: func(owner: string) -> u64;\n"
      "  transfer: func(from: string, to: string, amount: u64);\n"
      "  version: func() -> u32;\n"
      "  find: func(id: u64) -> option<account>;\n"
      "  ping: func();\n"
      "  unnamed: func(: u32, : u32) -> u64;\n"
      "}\n"
      "\n"
      "world token {\n"
      "  export token;\n"
      "}\n";
   CHECK(psio::generate_wit_text<token>() == expected);

   // The consteval generator emits the same interface block.
   constexpr auto block = psio::constexpr_wit::interface_text<token>();
   const std::string_view text{block.first.data(), block.second};
   CHECK(expected.find(text) != std::string::npos);
   CHECK(text.starts_with("interface token {\n"));

   // And the Schema IR carries the same functions, object implicit.
   auto schema = psio::SchemaBuilder{}.insert_interface<token>().build();
   REQUIRE(schema.interfaces.size() == 1);
   const auto& iface = schema.interfaces[0];
   CHECK(schema.package.name == "chain::svc");
   REQUIRE(iface.funcs.size() == 6);
   CHECK(iface.funcs[1].name == "transfer");
   REQUIRE(iface.funcs[1].params.size() == 3);
   CHECK(iface.funcs[1].params[2].name == "amount");
   CHECK(!iface.funcs[1].result);
   CHECK(iface.funcs[0].result.has_value());
   const auto wit = psio::emit_wit(schema);
   CHECK(wit.find("transfer: func(from: string, to: string, amount: u64);") != std::string::npos);
}

// ── Packages declared in a namespace; name override ────────────────────────

namespace chain::db
{
   PSIO_PACKAGE(chain_db, "1.2.0");

   struct store
   {
      std::optional<std::string> get(std::string_view key) const;
      void                       put(std::string key, std::string value);
   };
   PSIO_REFLECT(store, method(get, key), method(put, key, value))
   PSIO_INTERFACE(store, name("psi:db/store"))

   namespace nested
   {
      struct cursor
      {
         static bool next();
      };
      // funcs(...) given; the enclosing namespace's package applies.
      PSIO_INTERFACE(cursor, funcs(func(next)), types())
   }  // namespace nested
}  // namespace chain::db

namespace chain::two
{
   // Two packages in one namespace: each interface names its own.
   PSIO_PACKAGE(two_a, "1");
   PSIO_PACKAGE(two_b, "2");
   struct picked
   {
      static void f();
   };
   PSIO_INTERFACE(picked, package(two_b), funcs(f))
}  // namespace chain::two

// The older pattern at global scope, left defined for the rest of the file.
PSIO_PACKAGE(stale_global, "9");
#undef PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(stale_global)
PSIO_PACKAGE(stale_other, "8");
struct global_staled
{
   static void f();
};
PSIO_INTERFACE(global_staled, types(), funcs(f))

namespace chain::bare
{
   // PSIO_CURRENT_PACKAGE_ still names a global package here; a global
   // package does not apply to an interface registered in a namespace.
   struct lone
   {
      static void f();
   };
   PSIO_INTERFACE(lone, funcs(f))
}  // namespace chain::bare


TEST_CASE("packages: namespace scope, lookup order, name override", "[structural]")
{
   using store = psio::detail::interface_info<chain::db::store>;
   STATIC_REQUIRE(store::name == std::string_view{"psi:db/store"});
   STATIC_REQUIRE(store::package::name == std::string_view{"chain_db"});
   STATIC_REQUIRE(store::package::version == std::string_view{"1.2.0"});
   STATIC_REQUIRE(
      std::is_same_v<store::package, psio::detail::package_info<chain::db::chain_db_pkg_tag>>);

   using cursor = psio::detail::interface_info<chain::db::nested::cursor>;
   STATIC_REQUIRE(cursor::package::name == std::string_view{"chain_db"});
   STATIC_REQUIRE(cursor::func_names.size() == 1);

   using picked = psio::detail::interface_info<chain::two::picked>;
   STATIC_REQUIRE(picked::package::name == std::string_view{"two_b"});
   STATIC_REQUIRE(picked::package::version == std::string_view{"2"});

   using lone = psio::detail::interface_info<chain::bare::lone>;
   STATIC_REQUIRE(lone::package::name == std::string_view{"chain::bare"});
   STATIC_REQUIRE(lone::package::version.empty());

   using global = psio::detail::interface_info<global_staled>;
   STATIC_REQUIRE(global::package::name == std::string_view{"stale_global"});

   CHECK(psio::registered_package_id<chain::db::store>() == "chain-db@1.2.0");
   CHECK(psio::registered_package_id<chain::bare::lone>() == "chain:bare");
   const auto text = psio::generate_wit_text<chain::db::store>();
   CHECK(text.find("interface psi:db/store {\n"
                   "  get: func(key: string) -> option<string>;\n"
                   "  put: func(key: string, value: string);\n"
                   "}\n") != std::string::npos);
}

// ── A resource with instance methods ───────────────────────────────────────

namespace chain::res
{
   struct counter : psio::wit_resource
   {
      std::uint32_t increment(std::uint32_t by);
      std::uint32_t value() const;
      // The older pattern: a static function with an explicit self.
      static bool legacy(psio::borrow<counter> self);
   };
   PSIO_REFLECT(counter, method(increment, by), method(value), method(legacy, self))
   PSIO_INTERFACE(counter)

   struct counters
   {
      static psio::own<counter> make(std::uint32_t start);
   };
   PSIO_INTERFACE(counters, types(counter), funcs(func(make, start)))
}  // namespace chain::res

TEST_CASE("resource: instance methods have an implicit self", "[structural][wit]")
{
   const std::string expected =
      "package chain:res;\n"
      "\n"
      "interface counters {\n"
      "  resource counter {\n"
      "    increment: func(by: u32) -> u32;\n"
      "    value: func() -> u32;\n"
      "    legacy: func(self: borrow<counter>) -> bool;\n"
      "  }\n"
      "\n"
      "  make: func(start: u32) -> own<counter>;\n"
      "}\n"
      "\n"
      "world counters {\n"
      "  export counters;\n"
      "}\n";
   CHECK(psio::generate_wit_text<chain::res::counters>() == expected);

   auto schema = psio::SchemaBuilder{}.insert_interface<chain::res::counters>().build();
   const auto wit = psio::emit_wit(schema);
   CHECK(wit.find("increment: func(by: u32) -> u32;") != std::string::npos);
   // (The Schema IR models a handle as its u32 representation.)
   CHECK(wit.find("legacy: func(self: u32) -> bool;") != std::string::npos);
}

// ── Interfaces built from a reflected type: the corner cases ─────────────
namespace iface_corner
{
   // A reflected type that lists no method: a service without actions.
   struct History
   {
   };
   PSIO_REFLECT(History)
   PSIO_INTERFACE(History, name("svc.history"))

   struct Svc
   {
      void named(std::uint32_t amount, std::optional<std::string> memo);
      void unnamed(int a, int b);
      void partly(int a, int b);
      void none();
   };
   PSIO_REFLECT(Svc, method(named, amount, memo), method(unnamed), method(partly, a), method(none))
   PSIO_INTERFACE(Svc, name("svc"))
}  // namespace iface_corner

TEST_CASE("an interface with no functions", "[reflect][interface][schema]")
{
   auto s = psio::SchemaBuilder{}.insert_interface<iface_corner::History>().build();
   REQUIRE(s.interfaces.size() == 1);
   CHECK(s.interfaces[0].name == "svc.history");
   CHECK(s.interfaces[0].funcs.empty());
   CHECK(s.interfaces[0].type_names.empty());
   CHECK(psio::pssz_ir::validate_schema(s).ok());
   // The schema is a value like any other.
   const auto bytes = psio::encode(psio::pssz{}, s);
   CHECK(psio::decode<psio::Schema>(psio::pssz{}, std::span<const char>{bytes}) == s);
}

TEST_CASE("unnamed parameters of an interface function are positional: 0, 1, ...",
          "[reflect][interface][schema]")
{
   namespace st = psio::schema_types;
   auto s = psio::SchemaBuilder{}.insert_interface<iface_corner::Svc>().build();
   REQUIRE(psio::pssz_ir::validate_schema(s).ok());
   const auto& funcs = s.interfaces[0].funcs;
   REQUIRE(funcs.size() == 4);
   auto names = [](const st::Func& f) {
      std::vector<std::string> out;
      for (const auto& p : f.params)
         out.push_back(p.name);
      return out;
   };
   CHECK(names(funcs[0]) == std::vector<std::string>{"amount", "memo"});
   CHECK(names(funcs[1]) == std::vector<std::string>{"0", "1"});
   // Not every parameter named: positional, as its argument tuple is.
   CHECK(names(funcs[2]) == std::vector<std::string>{"0", "1"});
   CHECK(names(funcs[3]).empty());

   // The argument list of each as a type: a call packed from the schema.
   auto tuple_form = [](const st::AnyType& args) {
      return *psio::pssz_ir::detail::find_attribute(std::get<st::Object>(args.value).attributes,
                                                    "tuple")
                 ->value;
   };
   const auto named = psio::pssz_ir::argument_list(funcs[0]);
   CHECK(tuple_form(named) == "named");
   std::vector<char> bytes;
   REQUIRE(psio::pssz_ir::json_to_pssz(s, named, psio::ijson{}, R"({"amount":5})", bytes).ok());
   CHECK(bytes == psio::encode(psio::pssz{}, std::tuple{std::uint32_t{5},
                                                        std::optional<std::string>{}}));
   const auto positional = psio::pssz_ir::argument_list(funcs[1]);
   CHECK(tuple_form(positional) == "positional");
   bytes.clear();
   REQUIRE(psio::pssz_ir::json_to_pssz(s, positional, psio::ijson{}, "[1,2]", bytes).ok());
   CHECK(bytes == psio::encode(psio::pssz{}, std::tuple{1, 2}));
   std::string text;
   REQUIRE(psio::pssz_ir::pssz_to_json(s, positional, psio::ijson{}, std::span<const char>{bytes},
                                       text)
              .ok());
   CHECK(text == "[1,2]");
   // (what a consumer wrote before argument_list existed: the parameters as
   // the members of a record — no duplicate "" member any more)
   bytes.clear();
   CHECK(psio::pssz_ir::json_to_pssz(s, st::AnyType{st::Object{funcs[1].params, {{"tuple", "named"}}}},
                                     psio::ijson{}, "[1,2]", bytes)
            .ok());
   CHECK(tuple_form(psio::pssz_ir::argument_list(funcs[3])) == "named");
}
