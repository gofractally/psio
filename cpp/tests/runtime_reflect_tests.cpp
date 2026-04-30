// Phase 13 — runtime-reflection patterns (§5.2.5.2).
//
// The design doc calls these "make_proxy" patterns — they're all
// realized by the existing reflect<T> API:
//
//   - visit_field_by_name(obj, name, fn)     → RPC server dispatch
//   - visit_field_by_number(obj, n, fn)      → wire-ordinal dispatch
//   - for_each_field(obj, fn)                → debug printers, schema walk
//
// No additional machinery is needed; these tests document the patterns
// and serve as ready-to-copy examples for consumers building RPC
// stubs, dynamic viewers, or debug tooling on top of psio.

#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ── A sample "service" type with methods modelled as callable members ────

struct PaymentService
{
   // In a real RPC stub each field would be a descriptor carrying a
   // remote method; here we just model the "invoke name" pattern with
   // plain-old-function members.
   int  (*transfer)(int from, int to, int amount) = [](int, int, int) {
      return 1;
   };
   int  (*balance)(int acct)                         = [](int) {
      return 100;
   };
   void (*freeze)(int acct)                          = [](int) {
   };
};
PSIO_REFLECT(PaymentService, transfer, balance, freeze)

// ── Pattern 1: RPC server dispatch ───────────────────────────────────────
//
// Incoming call name → dispatch to the right method on the service impl.
// visit_field_by_name is the core; the handler decides how to marshal
// args / reply.

TEST_CASE("runtime reflection: RPC-style dispatch by method name",
          "[reflect][make_proxy][rpc]")
{
   PaymentService svc{};
   bool           found = false;
   int            result = 0;

   found = psio::reflect<PaymentService>::visit_field_by_name(
      svc, "balance",
      [&](auto& field)
      {
         // The visitor body is instantiated for every field type in
         // the macro expansion; use if constexpr to gate on "this
         // callable matches the signature I want." In real RPC code
         // this is where wire args get marshaled in and back out.
         using F = std::remove_cvref_t<decltype(field)>;
         if constexpr (std::is_invocable_r_v<int, F, int>)
            result = field(7);
      });

   REQUIRE(found);
   REQUIRE(result == 100);
}

TEST_CASE("runtime reflection: unknown method name returns false",
          "[reflect][make_proxy][rpc]")
{
   PaymentService svc{};
   bool found = psio::reflect<PaymentService>::visit_field_by_name(
      svc, "doesnotexist", [](auto&) {});
   REQUIRE(!found);
}

TEST_CASE("runtime reflection: dispatch by wire ordinal (field_number)",
          "[reflect][make_proxy][wire]")
{
   PaymentService svc{};
   // Field numbers are 1-based by default (source order). transfer=1,
   // balance=2, freeze=3.
   int invoked = 0;
   psio::reflect<PaymentService>::visit_field_by_number(
      svc, 2,
      [&](auto& field)
      {
         // The lambda is instantiated for every field type during
         // compilation; use if constexpr to gate type-specific work.
         using F = std::remove_cvref_t<decltype(field)>;
         if constexpr (std::is_invocable_r_v<int, F, int>)
            invoked = field(0);
      });
   REQUIRE(invoked == 100);
}

// ── Pattern 2: debug printer / introspection ─────────────────────────────
//
// for_each_field walks fields with (index, name, value_ref). This is
// the same machinery schemas, diff tools, and test reporters use.

struct Address
{
   std::string street;
   std::string city;
   std::int32_t zip;
};
PSIO_REFLECT(Address, street, city, zip)

TEST_CASE("runtime reflection: for_each_field visits every field in order",
          "[reflect][make_proxy][debug]")
{
   Address a{"1 Main", "Anywhere", 12345};
   std::vector<std::string> names;

   psio::reflect<Address>::for_each_field(
      a,
      [&]<typename I>(I, std::string_view name, auto&)
      {
         names.emplace_back(name);
      });

   REQUIRE(names.size() == 3);
   REQUIRE(names[0] == "street");
   REQUIRE(names[1] == "city");
   REQUIRE(names[2] == "zip");
}

TEST_CASE("runtime reflection: streamed debug dump",
          "[reflect][make_proxy][debug]")
{
   Address           a{"42 Elm", "Somewhere", 99999};
   std::stringstream out;

   psio::reflect<Address>::for_each_field(
      a,
      [&]<typename I>(I, std::string_view name, const auto& value)
      {
         out << name << '=' << value << '\n';
      });

   const auto dump = out.str();
   REQUIRE(dump.find("street=42 Elm")  != std::string::npos);
   REQUIRE(dump.find("city=Somewhere") != std::string::npos);
   REQUIRE(dump.find("zip=99999")      != std::string::npos);
}

// ── Pattern 3: field map (dynamic accessor) ──────────────────────────────
//
// Build a runtime `map<name, variant<...>>` from a reflected type. The
// pattern lets tooling work with heterogeneous types through a uniform
// handle without needing a schema at build time.

TEST_CASE("runtime reflection: build a field-map from a reflected type",
          "[reflect][make_proxy][dynamic]")
{
   Address a{"Broadway", "NYC", 10001};

   using any_field = std::variant<std::string, std::int32_t>;
   std::map<std::string, any_field> m;

   psio::reflect<Address>::for_each_field(
      a,
      [&]<typename I>(I, std::string_view name, const auto& value)
      {
         m.emplace(std::string(name), any_field{value});
      });

   REQUIRE(m.size() == 3);
   REQUIRE(std::get<std::string>(m["street"]) == "Broadway");
   REQUIRE(std::get<std::int32_t>(m["zip"])   == 10001);
}

// ── Pattern 4: update-by-name (mutating visitor) ─────────────────────────
//
// Overwrite a named field at runtime. The same visit_field_by_name
// machinery works with mutable refs when the object isn't const.

TEST_CASE("runtime reflection: update a field by name",
          "[reflect][make_proxy][dynamic]")
{
   Address a{"old", "oldcity", 1};

   bool ok = psio::reflect<Address>::visit_field_by_name(
      a, "city",
      [](auto& field)
      {
         if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>,
                                      std::string>)
            field = "updated";
      });

   REQUIRE(ok);
   REQUIRE(a.city == "updated");
}

TEST_CASE("runtime reflection: name-to-index lookup round-trip",
          "[reflect][make_proxy][dynamic]")
{
   auto i_street = psio::reflect<Address>::index_of("street");
   auto i_city   = psio::reflect<Address>::index_of("city");
   auto i_zip    = psio::reflect<Address>::index_of("zip");
   auto i_miss   = psio::reflect<Address>::index_of("zzzzz");

   REQUIRE(i_street.has_value());
   REQUIRE(*i_street == 0);
   REQUIRE(i_city.has_value());
   REQUIRE(*i_city == 1);
   REQUIRE(i_zip.has_value());
   REQUIRE(*i_zip == 2);
   REQUIRE(!i_miss.has_value());
}
