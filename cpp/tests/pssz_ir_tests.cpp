#include <array>
#include <catch.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/schema_builder.hpp>

namespace ir = psio::schema_types;
namespace runtime = psio::pssz_ir;
#include "fixtures/pssz_bits_shared.hpp"
#include "fixtures/pssz_ir_shared.hpp"

namespace
{
ir::AnyType integer(std::uint32_t bits)
{
   return ir::Int{bits, false};
}
// A raw string: any bytes.
ir::AnyType text()
{
   return ir::Custom{ir::AnyType{ir::List{integer(8)}}, "string"};
}
// A UTF-8 string: validated.
ir::AnyType utf8_text()
{
   return ir::Custom{ir::AnyType{ir::List{integer(8)}}, "utf8"};
}
ir::AnyType optional(ir::AnyType type)
{
   return ir::Option{std::move(type)};
}
ir::Member member(std::string name, ir::AnyType type)
{
   return {std::move(name), std::move(type), {}};
}
ir::AnyType record(bool extended)
{
   ir::Object object{{member("name", text())}, {}};
   if (extended)
      object.members.push_back(member("extra", optional(text())));
   return object;
}
runtime::Value record_value(bool some)
{
   return runtime::Record{
       {"name", std::string("a")},
       {"extra", some ? runtime::Value::some(std::string("b")) : runtime::Value::none()}};
}
template <std::size_t W> void profile()
{
   ir::Schema schema;
   auto old = record(false), current = record(true);
   std::vector<char> bytes;
   REQUIRE(runtime::encode<W>(schema, current, record_value(true), bytes).ok());
   REQUIRE(bytes.size() == W * 3 + 3);
   runtime::Value value = runtime::Unsigned{99};
   bool unknown = false;
   REQUIRE(runtime::decode_compatible<W>(schema, old, bytes, value, unknown).ok());
   REQUIRE(unknown);
   REQUIRE(value == runtime::Value(runtime::Record{{"name", std::string("a")}}));
   REQUIRE_FALSE(runtime::decode_exact<W>(schema, old, bytes, value).ok());
   REQUIRE(value == runtime::Value(runtime::Record{{"name", std::string("a")}}));
   std::vector<char> none, base;
   REQUIRE(runtime::encode<W>(schema, current, record_value(false), none).ok());
   REQUIRE(runtime::encode<W>(schema, old, runtime::Record{{"name", std::string("a")}}, base).ok());
   REQUIRE(none == base);
   REQUIRE(runtime::decode_exact<W>(schema, current, base, value).ok());
   REQUIRE(value == record_value(false));
   REQUIRE(runtime::validate_canonical_for_width<W>(schema, current, base).ok());
   std::vector<char> destination{9, 8};
   REQUIRE_FALSE(runtime::encode<W>(schema, integer(8), runtime::Unsigned{256}, destination).ok());
   REQUIRE(destination == std::vector<char>{9, 8});
   auto bad = bytes;
   bad[W] = 0;
   unknown = false;
   value = runtime::Unsigned{99};
   REQUIRE_FALSE(runtime::decode_compatible<W>(schema, old, bad, value, unknown).ok());
   REQUIRE_FALSE(unknown);
   REQUIRE(value == runtime::Value(runtime::Unsigned{99}));
   // Minimum-size optional selectors preserve zero-sized Some(unit).
   ir::AnyType unit = ir::Struct{};
   std::vector<char> unit_some, unit_none;
   REQUIRE(runtime::encode<W>(schema, optional(unit), runtime::Value::some(runtime::Record{}),
                              unit_some)
               .ok());
   REQUIRE(runtime::encode<W>(schema, optional(unit), runtime::Value::none(), unit_none).ok());
   REQUIRE(unit_some == std::vector<char>{1});
   REQUIRE(unit_none == std::vector<char>{0});
   REQUIRE(runtime::decode_exact<W>(schema, optional(unit), unit_some, value).ok());
   REQUIRE(value == runtime::Value::some(runtime::Record{}));
   REQUIRE_FALSE(runtime::decode_exact<W>(schema, integer(32), std::vector<char>(5), value).ok());
   ir::AnyType bool_type = ir::Custom{integer(1), "bool"};
   REQUIRE_FALSE(runtime::decode_exact<W>(schema, bool_type, std::vector<char>{2}, value).ok());
   // "utf8" is validated on read and on write; a raw "string" is any bytes.
   REQUIRE_FALSE(
       runtime::decode_exact<W>(schema, utf8_text(), std::vector<char>{char(0xff)}, value).ok());
   std::vector<char> invalid_string;
   REQUIRE_FALSE(
       runtime::encode<W>(schema, utf8_text(), std::string(1, char(0xff)), invalid_string).ok());
   REQUIRE(invalid_string.empty());
   REQUIRE(runtime::decode_exact<W>(schema, text(), std::vector<char>{char(0xff)}, value).ok());
   REQUIRE(value == runtime::Value(std::string(1, char(0xff))));
   REQUIRE(runtime::encode<W>(schema, text(), std::string(1, char(0xff)), invalid_string).ok());
   REQUIRE(invalid_string == std::vector<char>{char(0xff)});
   invalid_string.clear();
   // Nested{u8}: a byte list holding the encoding of the inner value, whose
   // dynamic value is the inner value itself.
   {
      const ir::AnyType nested = ir::Nested{integer(8)};
      std::vector<char> nested_bytes;
      REQUIRE(runtime::encode<W>(schema, nested, runtime::Unsigned{7}, nested_bytes).ok());
      REQUIRE(nested_bytes == std::vector<char>{7});
      runtime::Value inner;
      REQUIRE(runtime::decode_exact<W>(schema, nested, nested_bytes, inner).ok());
      REQUIRE(inner == runtime::Value(runtime::Unsigned{7}));
      REQUIRE_FALSE(
          runtime::decode_exact<W>(schema, nested, std::vector<char>{7, 7}, inner).ok());
      REQUIRE_FALSE(
          runtime::encode<W>(schema, nested, runtime::Unsigned{256}, nested_bytes).ok());
      REQUIRE(nested_bytes == std::vector<char>{7});
   }
   REQUIRE_FALSE(
       runtime::decode_exact<W>(schema, ir::Resource{}, std::span<const char>{}, value).ok());
   // Fixed-count arrays may contain zero-sized values; list count cannot
   // be recovered from a nonempty list of those elements.
   ir::AnyType array = ir::Array{unit, 2};
   REQUIRE(runtime::encode<W>(schema, array,
                              runtime::Sequence{runtime::Record{}, runtime::Record{}}, unit_some)
               .ok());
   REQUIRE(runtime::decode_exact<W>(schema, array, std::span<const char>{}, value).ok());
   REQUIRE(value == runtime::Value(runtime::Sequence{runtime::Record{}, runtime::Record{}}));
   REQUIRE_FALSE(
       runtime::encode<W>(schema, ir::List{unit}, runtime::Sequence{runtime::Record{}}, destination)
           .ok());
}
} // namespace

TEST_CASE("actual IR checked runtime applies optional suffix contract at every width", "[pssz][ir]")
{
   profile<1>();
   profile<2>();
   profile<4>();
}

TEST_CASE("actual IR checked runtime rejects recursive aliases without caching pointers",
          "[pssz][ir]")
{
   ir::Schema schema;
   schema.insert("self", ir::Type{"self"});
   std::vector<char> bytes;
   runtime::Value value;
   REQUIRE_FALSE(runtime::encode<4>(schema, ir::Type{"self"}, runtime::Value::none(), bytes).ok());
   schema.insert("self", ir::Option{ir::AnyType{ir::Type{"self"}}});
   REQUIRE_FALSE(runtime::encode<4>(schema, ir::Type{"self"}, runtime::Value::none(), bytes).ok());
   REQUIRE_FALSE(runtime::decode_exact<4>(schema, ir::Type{"self"}, bytes, value).ok());
}

TEST_CASE("actual IR runtime consumes exact shared Rust schema and value fixtures",
          "[pssz][ir][shared]")
{
   auto run = []<std::size_t W>(const auto &fixture)
   {
      INFO(fixture.name);
      INFO(W);
      std::vector<char> bytes;
      auto encoded = runtime::encode<W>(fixture.schema, fixture.root, fixture.value, bytes);
      REQUIRE(encoded.ok() == fixture.accept);
      runtime::Value decoded = runtime::Unsigned{99};
      auto status = runtime::decode_exact<W>(fixture.schema, fixture.root, fixture.bytes, decoded);
      REQUIRE(status.ok() == fixture.accept);
      if (fixture.accept)
      {
         REQUIRE(bytes == fixture.bytes);
         REQUIRE(decoded == fixture.value);
         REQUIRE(
             runtime::validate_canonical_for_width<W>(fixture.schema, fixture.root, fixture.bytes)
                 .ok());
      }
      else
      {
         REQUIRE(bytes.empty());
         REQUIRE(decoded == runtime::Value(runtime::Unsigned{99}));
      }
   };
   auto fixtures = shared_pssz_ir_fixtures::cases();
   REQUIRE(fixtures.size() == 63);
   for (const auto &fixture : fixtures)
   {
      if (fixture.width == 1)
         run.operator()<1>(fixture);
      else if (fixture.width == 2)
         run.operator()<2>(fixture);
      else if (fixture.width == 4)
         run.operator()<4>(fixture);
      else
         FAIL("unsupported fixture width");
   }
}

TEST_CASE("actual IR primitive adapters bounds variants and canonicality are explicit",
          "[pssz][ir]")
{
   ir::Schema schema;
   runtime::Value decoded;
   std::vector<char> bytes;
   ir::AnyType char_type = ir::Custom{integer(8), "char"};
   REQUIRE(runtime::encode<4>(schema, char_type, runtime::Unsigned{255}, bytes).ok());
   REQUIRE(bytes == std::vector<char>{char(255)});
   REQUIRE(runtime::decode_exact<4>(schema, char_type, bytes, decoded).ok());
   REQUIRE(decoded == runtime::Value(runtime::Unsigned{255}));
   REQUIRE_FALSE(runtime::encode<4>(schema, char_type, runtime::Unsigned{256}, bytes).ok());
   ir::AnyType hex = ir::Custom{ir::AnyType{ir::List{char_type}}, "hex"};
   bytes.clear();
   runtime::Value raw = runtime::Bytes{0, 128, 255};
   REQUIRE(runtime::encode<4>(schema, hex, raw, bytes).ok());
   REQUIRE(runtime::decode_exact<4>(schema, hex, bytes, decoded).ok());
   REQUIRE(decoded == raw);
   ir::AnyType bounded = ir::Custom{ir::AnyType{ir::BoundedList{integer(8), 2}}, "string"};
   REQUIRE_FALSE(runtime::encode<4>(schema, bounded, std::string("abc"), bytes).ok());
   REQUIRE_FALSE(
       runtime::decode_exact<4>(schema, bounded, std::vector<char>{'a', 'b', 'c'}, decoded).ok());
   ir::AnyType choice = ir::Variant{
       {member("unit", ir::Struct{}), member("pair", ir::Tuple{{integer(16), text()}})}, {}};
   auto pair =
       runtime::Value::variant(1, runtime::Sequence{runtime::Unsigned{9}, std::string("hello")});
   bytes.clear();
   REQUIRE(runtime::encode<4>(schema, choice, pair, bytes).ok());
   REQUIRE(runtime::decode_exact<4>(schema, choice, bytes, decoded).ok());
   REQUIRE(decoded == pair);
   REQUIRE_FALSE(runtime::decode_exact<4>(schema, choice, std::vector<char>{0, 1}, decoded).ok());
   REQUIRE_FALSE(runtime::decode_exact<4>(schema, choice, std::vector<char>{2}, decoded).ok());
   // Pruning is mandatory (owner decision at the trunk merge): a stored
   // trailing None is non-canonical and every reader rejects it. This line
   // previously asserted that exact-schema structural reads accepted it.
   std::vector<char> unpruned{8, 0, 0, 0, 8, 0, 0, 0, 9, 0, 0, 0, 'a', 0};
   REQUIRE_FALSE(runtime::decode_exact<4>(schema, record(true), unpruned, decoded).ok());
   bool unpruned_unknown = false;
   REQUIRE_FALSE(runtime::decode_compatible<4>(schema, record(true), unpruned, decoded,
                                               unpruned_unknown)
                     .ok());
   REQUIRE_FALSE(runtime::validate_canonical_for_width<4>(schema, record(true), unpruned).ok());
   auto max = runtime::Value(runtime::Unsigned{~static_cast<unsigned __int128>(0)});
   bytes.clear();
   REQUIRE(runtime::encode<4>(schema, integer(128), max, bytes).ok());
   REQUIRE(runtime::decode_exact<4>(schema, integer(128), bytes, decoded).ok());
   REQUIRE(decoded == max);
   auto signed_min = runtime::Value(
       runtime::Signed{std::bit_cast<__int128>(static_cast<unsigned __int128>(1) << 127)});
   ir::AnyType signed_type = ir::Int{128, true};
   bytes.clear();
   REQUIRE(runtime::encode<4>(schema, signed_type, signed_min, bytes).ok());
   REQUIRE(runtime::decode_exact<4>(schema, signed_type, bytes, decoded).ok());
   REQUIRE(decoded == signed_min);
}

TEST_CASE("actual IR wire node depth excludes omitted defaults and treats strings atomically",
          "[pssz][ir][depth]")
{
   auto nest = [](runtime::Value payload)
   {
      auto result = runtime::Value::variant(1, std::move(payload));
      for (std::size_t i = 0; i < 63; ++i)
         result = runtime::Value::variant(0, std::move(result));
      return result;
   };
   auto schema_for = [](ir::AnyType payload)
   {
      ir::Schema schema;
      schema.insert(
          "nest",
          ir::Variant{{member("next", ir::Type{"nest"}), member("leaf", std::move(payload))}, {}});
      return schema;
   };
   ir::AnyType root = ir::Type{"nest"};
   auto bytes_for = [](std::vector<char> payload)
   {
      std::vector<char> bytes(63, 0);
      bytes.push_back(1);
      bytes.insert(bytes.end(), payload.begin(), payload.end());
      return bytes;
   };
   ir::AnyType at_limit = ir::Object{{member("suffix", optional(integer(8)))}, {}};
   auto schema = schema_for(at_limit);
   runtime::Value decoded;
   std::vector<char> bytes;
   auto omitted = nest(runtime::Record{{"suffix", runtime::Value::none()}});
   REQUIRE(runtime::encode<4>(schema, root, omitted, bytes).ok());
   REQUIRE(runtime::decode_exact<4>(schema, root, bytes, decoded).ok());
   REQUIRE(decoded == omitted);
   REQUIRE_FALSE(runtime::encode<4>(
                     schema, root,
                     nest(runtime::Record{{"suffix", runtime::Value::some(runtime::Unsigned{7})}}),
                     bytes)
                     .ok());
   REQUIRE_FALSE(
       runtime::decode_exact<4>(schema, root, bytes_for({4, 0, 0, 0, 4, 0, 0, 0}), decoded).ok());
   schema = schema_for(ir::Custom{ir::AnyType{ir::List{integer(8)}}, "hex"});
   bytes.clear();
   REQUIRE(runtime::encode<4>(schema, root, nest(runtime::Bytes{}), bytes).ok());
   REQUIRE_FALSE(runtime::encode<4>(schema, root, nest(runtime::Bytes{7}), bytes).ok());
   REQUIRE_FALSE(runtime::decode_exact<4>(schema, root, bytes_for({7}), decoded).ok());
   schema = schema_for(text());
   bytes.clear();
   REQUIRE(runtime::encode<4>(schema, root, nest(std::string("a")), bytes).ok());
   REQUIRE(runtime::decode_exact<4>(schema, root, bytes, decoded).ok());
}

TEST_CASE("actual C++ SchemaBuilder char containers preserve every byte", "[pssz][ir][char]")
{
   psio::SchemaBuilder builder;
   auto vector_type = builder.insert<std::vector<char>>();
   auto array_type = builder.insert<std::array<char, 256>>();
   auto empty_array = builder.insert<std::array<char, 0>>();
   auto optional_char = builder.insert<std::optional<char>>();
   auto schema = std::move(builder).build();
   runtime::Bytes all;
   std::vector<char> native;
   std::array<char, 256> native_array{};
   for (unsigned int byte = 0; byte < 256; ++byte)
   {
      all.push_back(static_cast<unsigned char>(byte));
      native.push_back(std::bit_cast<char>(static_cast<unsigned char>(byte)));
      native_array[byte] = native.back();
   }
   auto run = [&]<std::size_t W>()
   {
      for (const auto *type : {&vector_type, &array_type})
      {
         std::vector<char> bytes;
         runtime::Value decoded;
         REQUIRE(runtime::encode<W>(schema, *type, all, bytes).ok());
         REQUIRE(bytes == native);
         REQUIRE(psio::encode(psio::pssz_<W>{}, native) == bytes);
         REQUIRE(psio::encode(psio::pssz_<W>{}, native_array) == bytes);
         REQUIRE(psio::validate<std::vector<char>>(psio::pssz_<W>{}, bytes).ok());
         REQUIRE(psio::validate<std::array<char, 256>>(psio::pssz_<W>{}, bytes).ok());
         REQUIRE(runtime::decode_exact<W>(schema, *type, bytes, decoded).ok());
         REQUIRE(decoded == runtime::Value(all));
      }
      std::vector<char> bytes;
      runtime::Value decoded;
      REQUIRE(runtime::encode<W>(schema, vector_type, runtime::Bytes{}, bytes).ok());
      REQUIRE(bytes.empty());
      REQUIRE(runtime::encode<W>(schema, empty_array, runtime::Bytes{}, bytes).ok());
      REQUIRE(bytes.empty());
      REQUIRE(runtime::decode_exact<W>(schema, empty_array, bytes, decoded).ok());
      REQUIRE(decoded == runtime::Value(runtime::Bytes{}));
      for (unsigned int byte = 0; byte < 256; ++byte)
      {
         bytes.clear();
         auto value = runtime::Value::some(runtime::Unsigned{byte});
         REQUIRE(runtime::encode<W>(schema, optional_char, value, bytes).ok());
         REQUIRE(bytes == std::vector<char>{std::bit_cast<char>(static_cast<unsigned char>(byte))});
         REQUIRE(psio::encode(psio::pssz_<W>{}, std::optional<char>{native[byte]}) == bytes);
         REQUIRE(runtime::decode_exact<W>(schema, optional_char, bytes, decoded).ok());
         REQUIRE(decoded == value);
      }
   };
   run.operator()<1>();
   run.operator()<2>();
   run.operator()<4>();

   // Use the actual interned char schemas at the wire depth boundary too.
   psio::SchemaBuilder nested_builder;
   auto vec = nested_builder.insert<std::vector<char>>();
   auto zero = nested_builder.insert<std::array<char, 0>>();
   auto one = nested_builder.insert<std::array<char, 1>>();
   auto nested_schema = std::move(nested_builder).build();
   auto nest = [](runtime::Value value)
   {
      value = runtime::Value::variant(1, std::move(value));
      for (int i = 0; i < 63; ++i)
         value = runtime::Value::variant(0, std::move(value));
      return value;
   };
   auto check = [&](ir::AnyType type, runtime::Bytes value, bool accept)
   {
      nested_schema.insert(
          "nest",
          ir::Variant{{member("next", ir::Type{"nest"}), member("leaf", std::move(type))}, {}});
      std::vector<char> encoded;
      runtime::Value decoded;
      auto status = runtime::encode<4>(nested_schema, ir::Type{"nest"}, nest(value), encoded);
      REQUIRE(status.ok() == accept);
      if (accept)
      {
         REQUIRE(runtime::decode_exact<4>(nested_schema, ir::Type{"nest"}, encoded, decoded).ok());
      }
      else
      {
         std::vector<char> raw(63, 0);
         raw.push_back(1);
         raw.insert(raw.end(), value.begin(), value.end());
         REQUIRE_FALSE(
             runtime::decode_exact<4>(nested_schema, ir::Type{"nest"}, raw, decoded).ok());
      }
   };
   check(vec, {}, true);
   check(vec, {255}, false);
   check(zero, {}, true);
   check(one, {255}, false);
}

TEST_CASE("actual IR rejects unsupported constraint annotations before traversing values",
          "[pssz][ir][metadata]")
{
   ir::Schema schema;
   for (auto name : {"maxFields", "unknownConstraint"})
   {
      ir::AnyType root = ir::Object{{}, {ir::Attribute{name, "0"}}};
      std::vector<char> bytes{9};
      runtime::Value value = runtime::Unsigned{99};
      bool unknown = true;
      REQUIRE_FALSE(runtime::encode<4>(schema, root, runtime::Record{}, bytes).ok());
      REQUIRE(bytes == std::vector<char>{9});
      REQUIRE_FALSE(runtime::decode_exact<4>(schema, root, std::vector<char>(4, 0), value).ok());
      REQUIRE(value == runtime::Value(runtime::Unsigned{99}));
      REQUIRE_FALSE(
          runtime::decode_compatible<4>(schema, root, std::vector<char>(4, 0), value, unknown)
              .ok());
      REQUIRE(unknown);
      REQUIRE_FALSE(runtime::encode<4>(schema, optional(root), runtime::Value::none(), bytes).ok());
   }
   ir::AnyType descriptive = ir::Object{{}, {ir::Attribute{"doc", "description"}}};
   std::vector<char> bytes;
   REQUIRE(runtime::encode<4>(schema, descriptive, runtime::Record{}, bytes).ok());
   REQUIRE(bytes == std::vector<char>(4, 0));

   // maxDynamicData under an explicit width is what psio::pssz_<W> does with
   // it: the cap of the top-level type bounds the whole encoding; a nested
   // record's cap is not looked up. A malformed value is still an error.
   {
      ir::AnyType capped4 = ir::Object{{}, {ir::Attribute{"maxDynamicData", "4"}}};
      ir::AnyType capped0 = ir::Object{{}, {ir::Attribute{"maxDynamicData", "0"}}};
      ir::AnyType broken  = ir::Object{{}, {ir::Attribute{"maxDynamicData", "x"}}};
      std::vector<char> out{9};
      runtime::Value    value = runtime::Unsigned{99};
      bool              unknown = false;
      REQUIRE(runtime::encode<4>(schema, capped4, runtime::Record{}, out).ok());
      REQUIRE(out == std::vector<char>{9, 0, 0, 0, 0});
      REQUIRE(runtime::decode_exact<4>(schema, capped4, std::vector<char>(4, 0), value).ok());
      REQUIRE(value == runtime::Value(runtime::Record{}));
      // Top-level type over its cap: refused on write and on read.
      out = {9};
      value = runtime::Unsigned{99};
      REQUIRE_FALSE(runtime::encode<4>(schema, capped0, runtime::Record{}, out).ok());
      REQUIRE(out == std::vector<char>{9});
      REQUIRE_FALSE(runtime::decode_exact<4>(schema, capped0, std::vector<char>(4, 0), value).ok());
      REQUIRE(value == runtime::Value(runtime::Unsigned{99}));
      REQUIRE_FALSE(
          runtime::decode_compatible<4>(schema, capped0, std::vector<char>(4, 0), value, unknown)
              .ok());
      // Nested (here under an Option): not looked up.
      out.clear();
      REQUIRE(runtime::encode<4>(schema, optional(capped0),
                                 runtime::Value::some(runtime::Record{}), out)
                 .ok());
      REQUIRE(runtime::decode_exact<4>(schema, optional(capped0), out, value).ok());
      REQUIRE_FALSE(runtime::encode<4>(schema, broken, runtime::Record{}, out).ok());
   }
}

TEST_CASE("actual IR packed Bits match all shared static Rust frames", "[pssz][ir][bits][shared]")
{
   auto run = []<std::size_t W>(const auto &row)
   {
      INFO(row.name);
      INFO(W);
      std::vector<char> bytes;
      runtime::Value decoded;
      auto status = runtime::decode_exact<W>(row.schema, row.root, row.bytes, decoded);
      REQUIRE(status.ok() == row.accept);
      if (row.encode_check)
      {
         REQUIRE(runtime::encode<W>(row.schema, row.root, row.value, bytes).ok());
         REQUIRE(bytes == row.bytes);
         REQUIRE(decoded == row.value);
      }
   };
   auto rows = shared_pssz_bits_fixtures::cases();
   REQUIRE(rows.size() == 69);
   for (const auto &row : rows)
   {
      if (row.width == 1)
         run.operator()<1>(row);
      else if (row.width == 2)
         run.operator()<2>(row);
      else if (row.width == 4)
         run.operator()<4>(row);
      else
         FAIL("bad bit profile");
   }
}

namespace
{
template <typename T> struct bit_option : std::false_type
{
};
template <typename T> struct bit_option<std::optional<T>> : std::true_type
{
   using inner = T;
};
template <typename T> T native_bit_value(const runtime::Value &value)
{
   if constexpr (bit_option<T>::value)
   {
      const auto &optional = std::get<runtime::Optional>(value.value);
      if (!optional.some)
         return std::nullopt;
      return native_bit_value<typename bit_option<T>::inner>(*optional.some);
   }
   else
   {
      const auto &bits = std::get<runtime::Bits>(value.value);
      T result;
      for (std::size_t i = 0; i < bits.bit_count; ++i)
      {
         const bool bit = (bits.bytes[i / 8] >> (i % 8)) & 1;
         if constexpr (requires { T::bytes_value; })
            result.set(i, bit);
         else
            result.push_back(bit);
      }
      return result;
   }
}
template <std::size_t W, typename T> void actual_bit_schema(const auto &row)
{
   INFO(row.name);
   INFO(W);
   psio::SchemaBuilder builder;
   auto root = builder.insert<T>();
   auto schema = std::move(builder).build();
   REQUIRE(root == row.root);
   REQUIRE(schema == row.schema);
   REQUIRE(psio::validate<T>(psio::pssz_<W>{}, row.bytes).ok() == row.accept);
   if (row.encode_check)
   {
      auto value = native_bit_value<T>(row.value);
      REQUIRE(psio::encode(psio::pssz_<W>{}, value) == row.bytes);
   }
}
template <std::size_t W> void actual_bit_case(const auto &row)
{
   const std::string_view name = row.name;
   const bool optional = name.starts_with("option_");
   if (name.find("vector_zero") != name.npos)
   {
      if (optional)
         actual_bit_schema<W, std::optional<psio::bitvector<0>>>(row);
      else
         actual_bit_schema<W, psio::bitvector<0>>(row);
   }
   else if (name.find("vector_one") != name.npos)
      actual_bit_schema<W, psio::bitvector<1>>(row);
   else if (name.find("vector") != name.npos)
   {
      if (optional)
         actual_bit_schema<W, std::optional<psio::bitvector<9>>>(row);
      else
         actual_bit_schema<W, psio::bitvector<9>>(row);
   }
   else if (name == "list_zero_bound")
      actual_bit_schema<W, psio::bitlist<0>>(row);
   else
   {
      if (optional)
         actual_bit_schema<W, std::optional<psio::bitlist<10>>>(row);
      else
         actual_bit_schema<W, psio::bitlist<10>>(row);
   }
}
} // namespace

TEST_CASE("actual C++ builder schemas and typed bit bytes match Rust literally",
          "[pssz][ir][bits][builder]")
{
   for (const auto &row : shared_pssz_bits_fixtures::cases())
   {
      if (row.width == 1)
         actual_bit_case<1>(row);
      else if (row.width == 2)
         actual_bit_case<2>(row);
      else if (row.width == 4)
         actual_bit_case<4>(row);
      else
         FAIL("bad bit width");
   }
}

namespace
{
template <std::size_t N, typename T> struct bit_depth_type
{
   using type = std::array<typename bit_depth_type<N - 1, T>::type, 1>;
};
template <typename T> struct bit_depth_type<0, T>
{
   using type = T;
};
template <typename T> auto &bit_leaf(T &value)
{
   if constexpr (requires {
                    typename T::value_type;
                    value[0];
                 })
      return bit_leaf(value[0]);
   else
      return value;
}
} // namespace

TEST_CASE("packed Bits adapters are atomic at node64", "[pssz][ir][bits][depth]")
{
   auto check = []<typename T>(const T &value)
   {
      psio::SchemaBuilder builder;
      auto root = builder.insert<T>();
      auto schema = std::move(builder).build();
      auto bytes = psio::encode(psio::pssz32{}, value);
      runtime::Value decoded;
      REQUIRE(psio::validate<T>(psio::pssz32{}, bytes).ok());
      REQUIRE(runtime::decode_exact<4>(schema, root, bytes, decoded).ok());
      std::vector<char> encoded;
      REQUIRE(runtime::encode<4>(schema, root, decoded, encoded).ok());
      REQUIRE(encoded == bytes);
   };
   bit_depth_type<64, psio::bitlist<10>>::type list;
   bit_leaf(list).push_back(true);
   bit_leaf(list).push_back(false);
   bit_leaf(list).push_back(false);
   check(list);
   bit_depth_type<64, psio::bitvector<9>>::type vector;
   bit_leaf(vector).set(8, true);
   check(vector);
   bit_depth_type<64, psio::bitvector<0>>::type empty;
   check(empty);
}

TEST_CASE("actual IR rejects a tuple of more than 255 elements", "[pssz][ir][metadata]")
{
   // docs/pssz-spec.md 8.4: a tuple has at most 255 elements.
   ir::Schema schema;
   const auto tuple_of = [](std::size_t count) {
      ir::Tuple tuple;
      for (std::size_t i = 0; i < count; ++i)
         tuple.members.push_back(ir::Box<ir::AnyType>{ir::AnyType{ir::Int{8, false}}});
      return ir::AnyType{std::move(tuple)};
   };
   const auto values_of = [](std::size_t count) {
      runtime::Sequence values;
      for (std::size_t i = 0; i < count; ++i)
         values.push_back(runtime::Unsigned{i & 0xff});
      return runtime::Value{std::move(values)};
   };
   const ir::AnyType at_limit = tuple_of(255), over = tuple_of(256);
   std::vector<char> bytes;
   REQUIRE(runtime::encode_auto(schema, at_limit, values_of(255), bytes).ok());
   // header 2 + 255 one-byte members (1 + 255 does not fit one-byte offsets)
   REQUIRE(bytes.size() == 257);
   std::size_t width = 0;
   REQUIRE(runtime::auto_width(schema, at_limit, width).ok());
   REQUIRE(width == 2);
   runtime::Value decoded;
   bool           unknown = false;
   REQUIRE(runtime::decode_auto(schema, at_limit, bytes, decoded, unknown).ok());
   REQUIRE(decoded == values_of(255));

   std::vector<char> refused{9};
   auto status = runtime::encode_auto(schema, over, values_of(256), refused);
   REQUIRE_FALSE(status.ok());
   REQUIRE(std::string(status.error().what) == "pssz IR: a tuple has more than 255 elements");
   REQUIRE(refused == std::vector<char>{9});
   REQUIRE_FALSE(runtime::auto_width(schema, over, width).ok());
   REQUIRE_FALSE(
       runtime::decode_auto(schema, over, std::vector<char>(260, 0), decoded, unknown).ok());
   REQUIRE_FALSE(runtime::encode<4>(schema, over, values_of(256), refused).ok());
   // Nested anywhere: the schema is checked before any value is.
   REQUIRE_FALSE(
       runtime::encode_auto(schema, optional(over), runtime::Value::none(), refused).ok());
}
