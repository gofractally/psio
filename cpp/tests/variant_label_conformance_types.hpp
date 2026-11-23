#pragma once
// The declarations behind conformance/variant-label-vectors.json: variants
// whose alternatives get their labels by every rule of
// psio/variant_label.hpp (docs/annotations.md "Variant alternative labels").
//
// Every other language declares the same variants in its own syntax and must
// produce the same Schema IR and the same rows.
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>
#include <psio/variant_label.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

// The text of the declarations below, recorded in the fixture for readers
// (documentation: nothing parses it).
#define PSIO_VARIANT_LABEL_CONF_DECLARATION                                                     \
   "struct Circle { u32 radius; };   PSIO_REFLECT(Circle, radius)",                             \
   "struct Square { u32 side; };     PSIO_REFLECT(Square, side)",                               \
   "using Records  = variant<Circle, Square>;",                                                 \
   "using Scalars  = variant<bool, u8, i16, u32, i64, u64, float, double, string>;",            \
   "using Generic  = variant<vector<u32>, optional<u32>, tuple<u32, string>, array<u16, 2>,",   \
   "                         vector<Circle>, vector<u8>>;",                                     \
   "using Repeated = variant<u32, string, string, Circle>;",                                    \
   "using Stated   = variant<Circle, Square, u32>;",                                            \
   "template <> struct psio::variant_labels<Stated>",                                           \
   "{ static constexpr std::array<std::string_view, 3> names{\"circle\", \"square\", \"side\"}; };"

namespace label_conf
{
   using u8  = std::uint8_t;
   using u16 = std::uint16_t;
   using i16 = std::int16_t;
   using u32 = std::uint32_t;
   using i64 = std::int64_t;
   using u64 = std::uint64_t;

   struct Circle
   {
      u32         radius = 0;
      friend bool operator==(const Circle&, const Circle&) = default;
   };
   PSIO_REFLECT(Circle, radius)
   struct Square
   {
      u32         side = 0;
      friend bool operator==(const Square&, const Square&) = default;
   };
   PSIO_REFLECT(Square, side)

   // Rule 2, reflected types: the reflected name.
   using Records = std::variant<Circle, Square>;
   // Rule 2, types without a declared name: the table.
   using Scalars = std::variant<bool, u8, i16, u32, i64, u64, float, double, std::string>;
   // Rule 2, generic types: built from the names of what they hold.
   using Generic = std::variant<std::vector<u32>, std::optional<u32>, std::tuple<u32, std::string>,
                                std::array<u16, 2>, std::vector<Circle>, std::vector<u8>>;
   // Rule 3: a type name two alternatives share is no label; they are
   // labelled by position.
   using Repeated = std::variant<u32, std::string, std::string, Circle>;
   // Stated labels.
   using Stated = std::variant<Circle, Square, u32>;
}  // namespace label_conf

template <>
struct psio::variant_labels<label_conf::Stated>
{
   static constexpr std::array<std::string_view, 3> names{"circle", "square", "side"};
};

namespace label_conf
{
   // The schema: every variant under its name, in this order.
   inline const psio::Schema& fixture_schema()
   {
      static const psio::Schema schema = [] {
         psio::SchemaBuilder builder;
         builder.insert<Records>("Records");
         builder.insert<Scalars>("Scalars");
         builder.insert<Generic>("Generic");
         builder.insert<Repeated>("Repeated");
         builder.insert<Stated>("Stated");
         return std::move(builder).build();
      }();
      return schema;
   }
}  // namespace label_conf
