// Describing a type does not need its implementation: the methods below are
// DECLARED AND NEVER DEFINED, and this program walks all of their metadata,
// builds the interface schema and its WIT text, a GraphQL schema's SDL and a
// call proxy — and links. Compiled without optimisation, so that no reference to a
// method is removed by the optimiser.

#include <psio/emit_wit.hpp>
#include <psio/graphql.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/structural.hpp>
#include <psio/wit_constexpr.hpp>
#include <psio/wit_gen.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace remote
{
   struct account
   {
      std::string   owner;
      std::uint64_t balance = 0;
   };
   PSIO_REFLECT(account, owner, balance)

   // Lives in another binary.
   class ledger
   {
     public:
      std::uint64_t          balance_of(const std::string& owner) const;
      void                   transfer(std::string from, std::string to, std::uint64_t amount);
      std::optional<account> find(std::uint64_t id) noexcept;
      static std::uint32_t   version();
      virtual void           audit(std::uint32_t depth);
      std::uint64_t          total() const noexcept;
   };
   PSIO_REFLECT(ledger,
                method(balance_of, owner),
                method(transfer, from, to, amount),
                method(find, id),
                method(version),
                method(audit, depth),
                method(total))
   PSIO_INTERFACE(ledger, types(account))

   struct base
   {
      int inherited(int x) const;
   };
   struct derived : base
   {
      int own(int x);
   };
   PSIO_REFLECT(derived, method(own, x), method(inherited, x))

   // A GraphQL root whose resolvers live elsewhere: its schema text is
   // metadata.
   struct catalog
   {
      std::uint32_t id = 0;
      std::string   greet(std::string who) const;
      std::uint32_t count() const;
   };
   PSIO_REFLECT(catalog, id, method(greet, who), method(count))
}  // namespace remote

namespace
{
   int failures = 0;
   void expect(bool ok, const char* what)
   {
      if (!ok)
      {
         std::printf("FAILED: %s\n", what);
         ++failures;
      }
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

   template <typename T>
   std::string type_text()
   {
      if constexpr (std::is_void_v<T>)
         return "void";
      else if constexpr (std::is_same_v<T, std::string>)
         return "string";
      else if constexpr (std::is_integral_v<T>)
         return "u" + std::to_string(sizeof(T) * 8);
      else
         return "other";
   }

   // "name(param: type, ...) -> result [static] [const]" per method.
   template <typename T>
   std::vector<std::string> describe()
   {
      using R = psio::reflect<T>;
      std::vector<std::string> out;
      R::for_each_method([&](auto index, std::string_view name) {
         constexpr std::size_t I = decltype(index)::value;
         using params            = typename R::template method_params<I>;
         using args              = typename R::template method_args<I>;
         using traits            = typename R::template method_traits<I>;
         using pointer_type      = typename R::template method_type<I>;
         static_assert(std::tuple_size_v<args> == traits::arity);
         static_assert(std::is_member_function_pointer_v<pointer_type> != traits::is_static);
         std::string row{name};
         row += '(';
         const auto& names = R::template method_param_names<I>;
         [&]<std::size_t... K>(std::index_sequence<K...>) {
            ((row += (K ? ", " : "") + std::string{K < names.size() ? names[K] : "_"} + ": " +
                     type_text<std::tuple_element_t<K, params>>()),
             ...);
         }(std::make_index_sequence<std::tuple_size_v<params>>{});
         row += ") -> " + type_text<typename R::template method_result<I>>();
         if (R::template method_is_static<I>)
            row += " static";
         if (R::template method_is_const<I>)
            row += " const";
         expect(R::template method_name<I> == name, "method_name<I>");
         expect(R::index_of_method(name) == std::optional<std::size_t>{I}, "index_of_method");
         expect(R::find_method_index(name) == I, "find_method_index");
         expect(R::template index_of_method_hash<fnv>(fnv{}(name)) == std::optional<std::size_t>{I},
                "index_of_method_hash");
         out.push_back(std::move(row));
      });
      return out;
   }

   // A call proxy for the remote type: it encodes, it does not call.
   struct encoder
   {
      std::vector<std::string>* log;
      template <std::size_t I, auto MemberPtr, typename... Args>
      void call(Args&&...)
      {
         log->push_back(std::string{psio::reflect<remote::ledger>::method_name<I>} + "/" +
                        std::to_string(sizeof...(Args)) + "/" +
                        std::to_string(psio::method_traits<decltype(MemberPtr)>::arity));
      }
   };
}  // namespace

int main()
{
   using R = psio::reflect<remote::ledger>;
   static_assert(R::method_count == 6);
   static_assert(R::method_names[4] == "audit");
   static_assert(R::method_param_names<1>.size() == 3);

   const auto rows = describe<remote::ledger>();
   expect(rows == std::vector<std::string>{"balance_of(owner: string) -> u64 const",
                                           "transfer(from: string, to: string, amount: u64) -> void",
                                           "find(id: u64) -> other",
                                           "version() -> u32 static",
                                           "audit(depth: u32) -> void",
                                           "total() -> u64 const"},
          "method rows of ledger");
   expect(!R::index_of_method("missing"), "index_of_method(missing)");
   expect(R::find_method_index("missing") == R::method_count, "find_method_index(missing)");
   expect(!R::index_of_method_hash<fnv>(1), "index_of_method_hash(missing)");

   expect(describe<remote::derived>() ==
             std::vector<std::string>{"own(x: u32) -> u32", "inherited(x: u32) -> u32 const"},
          "method rows of derived");

   // The interface of the reflected type: registry, schema IR, WIT text.
   using info = psio::detail::interface_info<remote::ledger>;
   static_assert(std::tuple_size_v<info::func_types> == 6);
   static_assert(std::is_same_v<std::tuple_element_t<3, info::func_types>, std::uint32_t (*)()>);
   expect(info::func_names[1] == "transfer", "interface func_names");
   expect(info::param_names[1].size() == 3, "interface param_names");

   const auto schema = psio::SchemaBuilder{}.insert_interface<remote::ledger>().build();
   expect(schema.interfaces.size() == 1 && schema.interfaces[0].funcs.size() == 6,
          "schema interface");
   expect(schema.interfaces[0].funcs[1].params.size() == 3 &&
             schema.interfaces[0].funcs[1].params[2].name == "amount",
          "schema params");
   const auto wit = psio::emit_wit(schema);
   expect(wit.find("transfer: func(from: string, to: string, amount: u64);") != std::string::npos,
          "wit from schema");
   const auto text = psio::generate_wit_text<remote::ledger>();
   expect(text.find("find: func(id: u64) -> option<account>;") != std::string::npos,
          "generated wit");
   constexpr auto block = psio::constexpr_wit::interface_text<remote::ledger>();
   expect(std::string_view{block.first.data(), block.second}.starts_with("interface ledger {\n"),
          "constexpr wit");

   // A proxy names every method as a template argument and calls none.
   std::vector<std::string> log;
   R::proxy<encoder>        api{&log};
   api.transfer("a", "b", 3);
   api.version();
   api.audit(1u);
   info::proxy<encoder> iface{&log};
   iface.total();
   expect(log == std::vector<std::string>{"transfer/3/3", "version/0/0", "audit/1/1", "total/0/0"},
          "proxy calls");

   expect(psio::graphql::schema<remote::catalog>::sdl().find(
             "type catalog {\n  id: Int!\n  greet(who: String!): String!\n  count: Int!\n}") !=
             std::string::npos,
          "graphql sdl");

   if (failures == 0)
      std::printf("reflect metadata-only: ok (%zu methods described)\n", rows.size());
   return failures == 0 ? 0 : 1;
}
