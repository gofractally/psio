#pragma once
// The declaration behind conformance/interface-vectors.json: one interface
// whose methods cover what a declaration says about a function — its name,
// parameter names and types, result, parameter bounds, returns(...), the
// argument cap and the parameter-count cap — and the text forms of its
// argument lists (docs/json.md "Method arguments").
//
// Every other language declares the same interface in its own syntax and
// must produce the same Interface / Func IR and the same rows.
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_compare.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>
#include <psio/structural.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// The text of the declaration below, recorded in the fixture for readers
// (it is documentation: nothing parses it).
#define PSIO_INTERFACE_CONF_DECLARATION                                                        \
   "struct Receipt { u64 id; string note; };",                                                 \
   "PSIO_REFLECT(Receipt, id, attr(note, utf8 | max<32>))",                                    \
   "struct token {",                                                                           \
   "   Receipt transfer(string to, u64 amount, optional<string> memo);",                       \
   "   vector<string> names(u32 limit) const;",                                                \
   "   void note(string text);",                                                               \
   "   u64 add(u32 a, u32 b);",                                                                \
   "   void ping();",                                                                          \
   "   bool batch(vector<string> names, u8 tip);",                                             \
   "   optional<Receipt> find(u64 id) const;",                                                 \
   "   optional<string> label(u64 id) const;",                                                 \
   "   PSIO_REFLECT(token,",                                                                   \
   "      method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>),",    \
   "             maxDynamicData(256), maxFields(8)),",                                         \
   "      method(names, limit, returns(max<4> | element(utf8 | max<16>))),",                   \
   "      method(note, attr(text, max<62>), maxDynamicData(64)),",                             \
   "      method(add),",                                                                       \
   "      method(ping),",                                                                      \
   "      method(batch, attr(names, max<4> | element(max<8>)), tip),",                         \
   "      method(find, id),",                                                                  \
   "      method(label, id, returns(utf8 | max<8>)))",                                         \
   "};",                                                                                       \
   "PSIO_INTERFACE(token, types())"

namespace iface_conf
{
   using u8  = std::uint8_t;
   using u32 = std::uint32_t;
   using u64 = std::uint64_t;

   struct Receipt
   {
      u64         id = 0;
      std::string note;
      friend bool operator==(const Receipt&, const Receipt&) = default;
   };
   PSIO_REFLECT(Receipt, id, attr(note, utf8 | max<32>))

   struct token
   {
      Receipt                  transfer(std::string to, u64 amount, std::optional<std::string> memo);
      std::vector<std::string> names(u32 limit) const;
      void                     note(std::string text);
      u64                      add(u32 a, u32 b);
      void                     ping();
      bool                     batch(std::vector<std::string> names, u8 tip);
      std::optional<Receipt>   find(u64 id) const;
      std::optional<std::string> label(u64 id) const;
      PSIO_REFLECT(token,
                   method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>),
                          maxDynamicData(256), maxFields(8)),
                   method(names, limit, returns(max<4> | element(utf8 | max<16>))),
                   method(note, attr(text, max<62>), maxDynamicData(64)),
                   method(add),
                   method(ping),
                   method(batch, attr(names, max<4> | element(max<8>)), tip),
                   method(find, id),
                   method(label, id, returns(utf8 | max<8>)))
   };
   PSIO_INTERFACE(token, types())

   using R = psio::reflect<token>;
   template <std::size_t I>
   using Args = R::method_arg_tuple<I>;
   inline constexpr std::size_t method_count = R::method_count;

   // The schema: the interface, and every argument list under the name
   // "<interface>.<method>" so that a schema-only reader has a root for it.
   inline const psio::Schema& fixture_schema()
   {
      static const psio::Schema schema = [] {
         psio::SchemaBuilder builder;
         builder.insert_interface<token>();
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (builder.insert<Args<Is>>("token." + std::string{R::method_names[Is]}), ...);
         }(std::make_index_sequence<method_count>{});
         return std::move(builder).build();
      }();
      return schema;
   }

   // The same methods as a schema that lists functions its own way, without
   // the Interface envelope: one FunctionType per method — the argument list
   // and the result as returns(...) types it — from
   // SchemaBuilder::insert_method<T, I>(), in declaration order on a fresh
   // builder.
   struct Functions
   {
      psio::Schema                                  schema;
      std::vector<psio::schema_types::FunctionType> types;
   };
   inline const Functions& fixture_functions()
   {
      static const Functions functions = [] {
         psio::SchemaBuilder builder;
         Functions           out;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (out.types.push_back(builder.insert_method<token, Is>()), ...);
         }(std::make_index_sequence<method_count>{});
         out.schema = std::move(builder).build();
         return out;
      }();
      return functions;
   }
}  // namespace iface_conf
