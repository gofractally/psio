// A byte list has one Schema IR node whichever C++ type spells the byte:
// std::vector<char>, <unsigned char>, <std::byte> and <std::uint8_t> all
// build Custom{List<u8>, "hex"} — the node Rust's Vec<u8> and the JS /
// Python byte lists build. The same for psio::bounded and std::array, and
// for a member bounded by annotation. A lone `char` member is still the
// "char" custom. Bytes, JSON and keys are untouched: only the schema node
// of a char-spelled byte sequence changes (its elements were
// Custom{u8, "char"}).

#include <psio/emit_wit.hpp>
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One record per spelling, under the same name so that the schemas can be
// compared as text.
#define PSIO_BYTE_LIST_CASE(NS, BYTE)                                                    \
   namespace NS                                                                          \
   {                                                                                     \
      struct Rec                                                                         \
      {                                                                                  \
         std::vector<BYTE>                list;                                          \
         std::array<BYTE, 4>              array{};                                       \
         psio::bounded<BYTE, 8>           bounded;                                       \
         std::vector<BYTE>                capped;                                        \
         std::optional<std::vector<BYTE>> maybe;                                         \
         std::vector<std::vector<BYTE>>   chunks;                                        \
         PSIO_REFLECT(Rec, list, array, bounded, attr(capped, max<16>), maybe,           \
                      attr(chunks, max<4> | element(max<8>)))                            \
      };                                                                                 \
   }
PSIO_BYTE_LIST_CASE(by_char, char)
PSIO_BYTE_LIST_CASE(by_uchar, unsigned char)
PSIO_BYTE_LIST_CASE(by_byte, std::byte)
PSIO_BYTE_LIST_CASE(by_u8, std::uint8_t)

namespace
{
   namespace ir = psio::schema_types;

   struct Lone
   {
      char              c = 0;
      std::vector<char> data;
      PSIO_REFLECT(Lone, c, data)
   };

   template <typename T>
   psio::Schema schema_of(const char* name)
   {
      psio::SchemaBuilder builder;
      builder.insert<T>(name);
      return std::move(builder).build();
   }
   template <typename T>
   std::string schema_text()
   {
      const auto text = psio::encode(psio::json{}, schema_of<T>("Rec"));
      return std::string{text.begin(), text.end()};
   }
   // Follow named references ("@1") to the node.
   const ir::AnyType& resolve(const psio::Schema& schema, const ir::AnyType& type)
   {
      const ir::AnyType* at = &type;
      while (auto ref = std::get_if<ir::Type>(&at->value))
      {
         at = schema.get(ref->type);
         REQUIRE(at != nullptr);
      }
      return *at;
   }
   const ir::AnyType& member(const psio::Schema& schema, const char* record, std::size_t i)
   {
      const auto& node = resolve(schema, ir::AnyType{ir::Type{record}});
      const auto* object = std::get_if<ir::Object>(&node.value);
      REQUIRE(object != nullptr);
      return resolve(schema, *object->members.at(i).type);
   }
   bool is_u8(const psio::Schema& schema, const ir::AnyType& type)
   {
      const auto* integer = std::get_if<ir::Int>(&resolve(schema, type).value);
      return integer && integer->bits == 8 && !integer->isSigned;
   }
}  // namespace

TEST_CASE("a byte list is one schema node whichever C++ type spells the byte",
          "[schema_builder][bytes]")
{
   const auto reference = schema_text<by_u8::Rec>();
   CHECK(schema_text<by_char::Rec>() == reference);
   CHECK(schema_text<by_uchar::Rec>() == reference);
   CHECK(schema_text<by_byte::Rec>() == reference);
   // No "char" node anywhere in a schema that only holds byte sequences.
   CHECK(reference.find("\"char\"") == std::string::npos);
   CHECK(reference.find("\"hex\"") != std::string::npos);
}

TEST_CASE("std::vector<char> is Custom{List<u8>, hex}; a lone char is still the char custom",
          "[schema_builder][bytes]")
{
   const auto schema = schema_of<Lone>("Lone");
   // c: Custom{u8, "char"}.
   const auto* lone = std::get_if<ir::Custom>(&member(schema, "Lone", 0).value);
   REQUIRE(lone != nullptr);
   CHECK(lone->id == "char");
   CHECK(is_u8(schema, *lone->type));
   // data: Custom{List<u8>, "hex"}, with plain u8 elements.
   const auto* hex = std::get_if<ir::Custom>(&member(schema, "Lone", 1).value);
   REQUIRE(hex != nullptr);
   CHECK(hex->id == "hex");
   const auto* list = std::get_if<ir::List>(&resolve(schema, *hex->type).value);
   REQUIRE(list != nullptr);
   CHECK(is_u8(schema, *list->type));
   CHECK(std::get_if<ir::Custom>(&resolve(schema, *list->type).value) == nullptr);

   // WIT: bytes are list<u8>; `char` is WIT's character.
   const auto wit = psio::emit_wit(schema);
   CHECK(wit.find("c: char") != std::string::npos);
   CHECK(wit.find("data: list<u8>") != std::string::npos);
   CHECK(wit.find("list<char>") == std::string::npos);
}

TEST_CASE("the bytes and text of a char-spelled byte list are those of the u8 one",
          "[schema_builder][bytes]")
{
   by_char::Rec a;
   a.list    = {'h', 'i', char(0xff)};
   a.array   = {'a', 'b', 'c', 'd'};
   a.bounded = psio::bounded<char, 8>{std::vector<char>{1, 2}};
   a.capped  = {9};
   a.maybe   = std::vector<char>{7};
   a.chunks  = {{1}, {2, 3}};
   by_u8::Rec b;
   b.list    = {'h', 'i', 0xff};
   b.array   = {'a', 'b', 'c', 'd'};
   b.bounded = psio::bounded<std::uint8_t, 8>{std::vector<std::uint8_t>{1, 2}};
   b.capped  = {9};
   b.maybe   = std::vector<std::uint8_t>{7};
   b.chunks  = {{1}, {2, 3}};
   const auto bytes = psio::encode(psio::pssz{}, a);
   REQUIRE(bytes == psio::encode(psio::pssz{}, b));

   // The schema-driven reader gives the same text from either schema, in
   // both JSON forms.
   const ir::AnyType root{ir::Type{"Rec"}};
   const auto text = [&](const psio::Schema& schema, auto format) {
      std::string out;
      REQUIRE(psio::pssz_ir::pssz_to_json(schema, root, format, bytes, out).ok());
      return out;
   };
   const auto from_char = schema_of<by_char::Rec>("Rec");
   const auto from_u8   = schema_of<by_u8::Rec>("Rec");
   CHECK(text(from_char, psio::json{}) == text(from_u8, psio::json{}));
   CHECK(text(from_char, psio::ijson{}) == text(from_u8, psio::ijson{}));
   // ... and back to the same bytes.
   std::vector<char> again;
   const auto        json = text(from_char, psio::ijson{});
   REQUIRE(psio::pssz_ir::json_to_pssz(from_char, root, psio::ijson{},
                                       std::span<const char>{json.data(), json.size()}, again)
              .ok());
   CHECK(again == bytes);
}

TEST_CASE("a schema written with the former char-element byte list is still read",
          "[schema_builder][bytes]")
{
   // Custom{List<Custom{u8, "char"}>, "hex"}: what std::vector<char> built
   // before. A published schema may hold it; the reader takes it as bytes.
   const ir::AnyType char_node{ir::Custom{ir::AnyType{ir::Int{8, false}}, "char"}};
   const ir::AnyType former{ir::Custom{ir::AnyType{ir::List{char_node}}, "hex"}};
   const ir::AnyType now{
      ir::Custom{ir::AnyType{ir::List{ir::AnyType{ir::Int{8, false}}}}, "hex"}};
   const psio::Schema      schema;
   const std::vector<char> bytes{0, char(128), char(255)};
   std::string             a, b;
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, former, psio::ijson{}, bytes, a).ok());
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, now, psio::ijson{}, bytes, b).ok());
   CHECK(a == b);
}
