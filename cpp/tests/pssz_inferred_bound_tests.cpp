// The inferred size bound of variants and tuples (docs/pssz-spec.md 3.2):
//   Variant(Ts...)  1 + max(max_encoded_size(T_i))
//   Tuple(Ts...)    the extensible record of its elements: header + per
//                   element its payload (+ an offset slot when variable)
//   Pair(A, B)      Tuple(A, B)
// with every header and slot counted as 4 bytes. A bounded tuple / variant,
// and a record that holds one, therefore gets one- or two-byte offsets
// instead of four; the typed codec and the schema-driven one agree.

#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/schema_builder.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace bound_tests
{
   using u8  = std::uint8_t;
   using u16 = std::uint16_t;
   using u32 = std::uint32_t;
   using u64 = std::uint64_t;
   using Name = psio::utf8_string<32>;

   template <typename T>
   constexpr std::optional<std::size_t> bound = psio::max_encoded_size<T>();
   template <typename T>
   constexpr std::size_t width = psio::auto_pssz_width_v<T>;

   struct Point
   {
      u32 x;
      u32 y;
      friend bool operator==(const Point&, const Point&) = default;
      PSIO_REFLECT(Point, x, y, definitionWillNotChange())
   };
   struct Capped
   {
      std::string text;
      friend bool operator==(const Capped&, const Capped&) = default;
      PSIO_REFLECT(Capped, text, maxDynamicData(300))
   };
   struct Free
   {
      std::string text;
      friend bool operator==(const Free&, const Free&) = default;
      PSIO_REFLECT(Free, text)
   };
   // A record with a variant member, and one with a tuple member.
   struct Event
   {
      u64                          at;
      std::variant<u8, Point, u64> what;
      friend bool operator==(const Event&, const Event&) = default;
      PSIO_REFLECT(Event, at, what)
   };
   struct Call
   {
      u32                       method;
      std::tuple<u64, Name, u8> args;
      friend bool operator==(const Call&, const Call&) = default;
      PSIO_REFLECT(Call, method, args)
   };
   struct Open
   {
      u32                                 id;
      std::variant<u8, std::vector<u8>>   what;
      PSIO_REFLECT(Open, id, what)
   };

   // ── Variant: 1 + the widest alternative ───────────────────────────────
   static_assert(bound<std::variant<u8>> == 2);
   static_assert(bound<std::variant<u8, u32>> == 5);
   static_assert(bound<std::variant<u64, u8, u16>> == 9);
   static_assert(bound<std::variant<u8, Point>> == 9);
   static_assert(bound<std::variant<u8, Name>> == 33);
   static_assert(bound<std::variant<u8, std::optional<u32>>> == 6);
   static_assert(bound<std::variant<u8, std::variant<u16, u64>>> == 10);
   // An alternative with an explicit cap contributes the cap.
   static_assert(bound<std::variant<u8, Capped>> == 301);
   // Unbounded if any alternative is.
   static_assert(!bound<std::variant<u8, std::string>>);
   static_assert(!bound<std::variant<std::vector<u8>, u8>>);
   static_assert(!bound<std::variant<u8, Free>>);

   // ── Tuple: header 4, each element its payload, + 4 when variable ──────
   static_assert(bound<std::tuple<>> == 4);
   static_assert(bound<std::tuple<u32>> == 8);
   static_assert(bound<std::tuple<u8, u16, u32, u64>> == 19);
   static_assert(bound<std::tuple<Point, bool>> == 13);
   static_assert(bound<std::tuple<Name>> == 4 + 32 + 4);
   static_assert(bound<std::tuple<std::optional<u32>>> == 4 + 5 + 4);
   static_assert(bound<std::tuple<std::tuple<u8>, u8>> == 4 + (5 + 4) + 1);
   static_assert(bound<std::tuple<std::variant<u8, u32>>> == 4 + 5 + 4);
   static_assert(bound<std::tuple<Capped, u8>> == 4 + 300 + 4 + 1);
   static_assert(bound<std::tuple<std::array<u16, 3>>> == 4 + 6);
   // An element has no member annotation: a string or list is unbounded.
   static_assert(!bound<std::tuple<u32, std::string>>);
   static_assert(!bound<std::tuple<std::vector<u8>>>);
   static_assert(!bound<std::tuple<Free>>);
   // A pair is the 2-tuple (a map's value_type included).
   static_assert(bound<std::pair<u32, u8>> == bound<std::tuple<u32, u8>> &&
                 bound<std::pair<u32, u8>> == 9);
   static_assert(bound<std::pair<const u32, Name>> == bound<std::tuple<u32, Name>>);
   static_assert(!bound<std::pair<std::string, u8>>);

   // ── Widths: the next power of two that holds the bound ────────────────
   static_assert(width<std::tuple<>> == 1 && width<std::tuple<u32>> == 1);
   static_assert(width<std::tuple<u64, Name, u8>> == 1);
   static_assert(width<std::pair<u32, u8>> == 1);
   static_assert(width<std::tuple<Capped, u8>> == 2);                  // 309
   static_assert(width<std::tuple<std::array<u64, 40>>> == 2);         // 324
   static_assert(width<std::tuple<std::array<u64, 9000>>> == 4);       // 72004
   static_assert(width<std::tuple<u32, std::string>> == 4);
   // A record holding a bounded variant / tuple is bounded itself.
   static_assert(bound<Event> == 4 + 8 + (9 + 4) && width<Event> == 1);
   static_assert(bound<Call> == 4 + 4 + (4 + 8 + 36 + 1 + 4) && width<Call> == 1);
   static_assert(!bound<Open> && width<Open> == 4);
   // Lists keep four-byte offset tables whatever their elements.
   static_assert(width<std::vector<std::tuple<u32>>> == 4 &&
                 width<std::map<u32, u8>> == 4 && width<std::set<std::tuple<u8>>> == 4);

   using Bytes = std::vector<char>;
   inline Bytes hex(std::string_view text)
   {
      Bytes out;
      auto  nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
      for (std::size_t i = 0; i + 1 < text.size(); i += 2)
         out.push_back(static_cast<char>(nibble(text[i]) * 16 + nibble(text[i + 1])));
      return out;
   }

   // The typed and the schema-driven codec, on one value.
   template <typename T>
   void agree(const T& value, const Bytes& wire)
   {
      const Bytes bytes = psio::encode(psio::pssz{}, value);
      CHECK(bytes == wire);
      CHECK(psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok());
      CHECK(psio::validate_strict<T>(psio::pssz{}, std::span<const char>{bytes}).ok());
      CHECK(psio::decode<T>(psio::pssz{}, std::span<const char>{bytes}) == value);
      T    out{};
      bool unknown = true;
      CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bytes}, out, unknown)
               .ok());
      CHECK((out == value && !unknown));

      namespace rt = psio::pssz_ir;
      const auto        schema = psio::SchemaBuilder{}.insert<T>("Root").build();
      std::size_t       w      = 0;
      REQUIRE(rt::auto_width(schema, "Root", w).ok());
      CHECK(w == psio::auto_pssz_width_v<T>);
      CHECK(rt::validate_auto(schema, psio::schema_types::AnyType{psio::schema_types::Type{"Root"}}, bytes).ok());
      rt::Value dynamic;
      REQUIRE(rt::decode_auto(schema, "Root", bytes, dynamic, unknown).ok());
      Bytes again;
      REQUIRE(rt::encode_auto(schema, "Root", dynamic, again).ok());
      CHECK(again == bytes);
   }
}  // namespace bound_tests

TEST_CASE("pssz inferred bound: tuples take the width their bound allows",
          "[pssz][width]")
{
   using namespace bound_tests;
   // One-byte header and no slots: all elements fixed.
   agree(std::tuple<>{}, hex("00"));
   agree(std::tuple<u32>{9}, hex("0409000000"));
   agree(std::pair<u32, u8>{9, 7}, hex("050900000007"));
   // A variable element: one-byte header, one-byte offset slot.
   agree(std::tuple<u64, Name, u8>{1, Name{"bob"}, 2},
         hex("0a" "0100000000000000" "0a" "02" "626f62"));
   // Trailing optionals prune as in any record.
   agree(std::tuple<u8, std::optional<u32>>{1, std::nullopt}, hex("0101"));
   agree(std::tuple<u8, std::optional<u32>>{1, 5}, hex("02" "01" "02" "05000000"));
   // Two-byte offsets.
   agree(std::tuple<Capped, u8>{Capped{"hi"}, 3},
         hex("0300" "0300" "03" "0200" "0200" "6869"));
   // An unbounded element keeps the tuple at four bytes.
   agree(std::tuple<u32, std::string>{9, "hi"},
         hex("08000000" "09000000" "08000000" "6869"));
}

TEST_CASE("pssz inferred bound: a variant bounds the record that holds it", "[pssz][width]")
{
   using namespace bound_tests;
   // The variant itself has no offsets: selector, payload.
   agree(std::variant<u8, u32>{u32{9}}, hex("0109000000"));
   agree(std::variant<u8, Point>{Point{1, 2}}, hex("01" "0100000002000000"));
   // Event: one-byte header (9) and slot.
   agree(Event{7, u8{5}}, hex("09" "0700000000000000" "09" "0005"));
   agree(Event{7, Point{1, 2}}, hex("09" "0700000000000000" "09" "01" "0100000002000000"));
   // Call: the argument tuple inside a record, both at one byte.
   agree(Call{3, {1, Name{"bob"}, 2}},
         hex("05" "03000000" "05" "0a" "0100000000000000" "0a" "02" "626f62"));
}

TEST_CASE("pssz inferred bound: map entries are bounded tuples", "[pssz][width]")
{
   using namespace bound_tests;
   // The list's offset table stays four bytes wide; each entry is the
   // 2-tuple at its own width (one byte: header 5, u32, u8).
   agree(std::map<u32, u8>{{1, 2}, {3, 4}},
         hex("08000000" "0e000000" "05" "01000000" "02" "05" "03000000" "04"));
   agree(std::vector<std::tuple<u32, u8>>{{1, 2}}, hex("04000000" "05" "01000000" "02"));
}
