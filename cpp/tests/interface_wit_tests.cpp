// What psio::emit_wit writes for the schema of the shared interface fixture
// (conformance/interface-vectors.json, interface_conformance_types.hpp):
// the reference text for the WIT emitters of the other implementations
// (js/src/wit.ts emitWit).
//
// The whole text is pinned, including two places where it is NOT valid
// WIT today. They are asserted separately below so that a fix has to change
// this file knowingly:
//
//   1. (fixed) method(add) lists no parameter names: its parameters are
//      positional, named "0" and "1" in the Func, and WIT needs an
//      identifier before the colon. The emitter writes `arg0`, `arg1`
//      (psio::schema_types::parameter_identifier).
//   2. `type token.transfer = /* TODO: inline record not supported */;`,
//      one per method. The fixture registers every argument list as a named
//      type `token.<method>` whose node is a reference to an interned Object
//      (attributes `tuple`, caps). The emitter resolves the reference,
//      finds an Object in type position and has no spelling for it; and
//      `token.transfer` is not a WIT identifier either.
//   3. Bounds have no WIT form: a bounded list is `list<T>/* @psio:max=N */`,
//      and the bound of a string (`to` <= 16, `memo` <= 80, `text` <= 62,
//      the elements of `names`) is dropped. This one is by design (the
//      header comment of emit_wit.hpp); the text stays valid WIT.

#include "interface_conformance_types.hpp"

#include <psio/emit_wit.hpp>

#include <catch.hpp>

#include <string>

namespace
{
   const std::string& wit()
   {
      static const std::string text = psio::emit_wit(iface_conf::fixture_schema());
      return text;
   }

   // The interface itself: every method, in declaration order.
   constexpr const char* interface_block = R"(package iface_conf;

interface token {
  @maxFields(8)
  @maxDynamicData(256)
  transfer: func(to: string, amount: u64, memo: option<string>) -> receipt;
  names: func(limit: u32) -> list<string>/* @psio:max=4 */;
  @maxDynamicData(64)
  note: func(text: string);
  add: func(arg0: u32, arg1: u32) -> u64;
  ping: func();
  batch: func(names: list<string>/* @psio:max=4 */, tip: u8) -> bool;
  find: func(id: u64) -> option<receipt>;
  label: func(id: u64) -> option<string>;
}
)";
   // The named types: Receipt, then the eight argument lists.
   constexpr const char* types_block = R"(
record receipt {
  id: u64,
  note: string
}
type token.transfer = /* TODO: inline record not supported */;
type token.names = /* TODO: inline record not supported */;
type token.note = /* TODO: inline record not supported */;
type token.add = /* TODO: inline record not supported */;
type token.ping = /* TODO: inline record not supported */;
type token.batch = /* TODO: inline record not supported */;
type token.find = /* TODO: inline record not supported */;
type token.label = /* TODO: inline record not supported */;
)";
}  // namespace

TEST_CASE("emit_wit of the interface fixture's schema: the pinned text", "[emit_wit][interface]")
{
   CHECK(wit() == std::string{interface_block} + types_block);
}

TEST_CASE("emit_wit of the interface fixture's schema: what is right", "[emit_wit][interface]")
{
   const auto has = [](std::string_view part) { return wit().find(part) != std::string::npos; };
   // Names and types of named parameters; the result; an optional; a void
   // method; a method with no parameters.
   CHECK(has("transfer: func(to: string, amount: u64, memo: option<string>) -> receipt;"));
   CHECK(has("note: func(text: string);"));
   CHECK(has("ping: func();"));
   CHECK(has("batch: func(names: list<string>/* @psio:max=4 */, tip: u8) -> bool;"));
   // returns(max<4> | element(utf8 | max<16>)): the list bound as a comment.
   CHECK(has("names: func(limit: u32) -> list<string>/* @psio:max=4 */;"));
   // The two caps of a method are attribute lines above it.
   CHECK(has("  @maxFields(8)\n  @maxDynamicData(256)\n  transfer:"));
   CHECK(has("  @maxDynamicData(64)\n  note:"));
   // A record result is declared once, by name.
   CHECK(has("record receipt {\n  id: u64,\n  note: string\n}"));
}

TEST_CASE("emit_wit of the interface fixture's schema: known defects", "[emit_wit][interface]")
{
   const auto count = [](std::string_view part) {
      std::size_t n = 0;
      for (auto at = wit().find(part); at != std::string::npos; at = wit().find(part, at + 1))
         ++n;
      return n;
   };
   // 1. Positional parameters are arg0, arg1, ...
   CHECK(count("add: func(arg0: u32, arg1: u32) -> u64;") == 1);
   CHECK(count("func(:") == 0);
   // 2. One unusable alias per argument list.
   CHECK(count("/* TODO: inline record not supported */") == iface_conf::method_count);
   CHECK(count("type token.") == iface_conf::method_count);
}
