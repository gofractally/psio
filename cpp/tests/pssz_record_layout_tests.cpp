#include <psio/detail/pssz_record_layout.hpp>
#include <catch.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace
{
   using Width1 = std::integral_constant<std::size_t, 1>;
   using Width2 = std::integral_constant<std::size_t, 2>;
   using Width4 = std::integral_constant<std::size_t, 4>;
   template <std::size_t W>
   void word(std::vector<char>& bytes, std::size_t value)
   {
      for (std::size_t i = 0; i < W; ++i)
         bytes.push_back(static_cast<char>(value >> (i * 8)));
   }
}

TEMPLATE_TEST_CASE("optional suffix offsets delimit the last known variable field",
                   "[pssz][layout]", Width1, Width2, Width4)
{
   constexpr std::size_t W = TestType::value;
   const std::array fields{psio::detail::pssz_field_layout{W, true, false}};
   std::array<psio::detail::pssz_field_span, 1> spans;
   bool unknown = false;
   std::vector<char> bytes;
   word<W>(bytes, 2 * W);
   word<W>(bytes, 2 * W);
   word<W>(bytes, 2 * W + 1);
   bytes.insert(bytes.end(), {'a', 'b'});
   REQUIRE(psio::detail::pssz_record_spans<W>(bytes, 0, bytes.size(), false,
                                             fields, spans, unknown).ok());
   REQUIRE(unknown);
   REQUIRE(spans[0].present);
   REQUIRE(spans[0].end - spans[0].begin == 1);
   REQUIRE(bytes[spans[0].begin] == 'a');
}

TEST_CASE("optional suffix preserves odd prefix alignment and zero length payloads",
          "[pssz][layout]")
{
   const std::array fields{psio::detail::pssz_field_layout{1, false, false},
                           psio::detail::pssz_field_layout{4, true, false}};
   std::array<psio::detail::pssz_field_span, 2> spans;
   bool unknown = false;
   std::vector<char> bytes;
   word<4>(bytes, 13); // known five bytes plus two optional slots
   bytes.push_back(1);
   word<4>(bytes, 13);
   word<4>(bytes, 14);
   word<4>(bytes, 14); // empty first unknown payload
   bytes.insert(bytes.end(), {'a', 'b'});
   REQUIRE(psio::detail::pssz_record_spans<4>(bytes, 0, bytes.size(), false,
                                             fields, spans, unknown).ok());
   REQUIRE(unknown);
   REQUIRE(spans[0].end - spans[0].begin == 1);
   REQUIRE(spans[1].end - spans[1].begin == 1);
   CHECK(bytes[spans[1].begin] == 'a');
}

TEST_CASE("missing suffix fields must be whole trailing optional slots",
          "[pssz][layout]")
{
   std::array fields{psio::detail::pssz_field_layout{4, true, false},
                      psio::detail::pssz_field_layout{4, true, true}};
   std::array<psio::detail::pssz_field_span, 2> spans;
   bool unknown = true;
   std::vector<char> bytes;
   word<4>(bytes, 4);
   word<4>(bytes, 4);
   bytes.push_back('a');
   REQUIRE(psio::detail::pssz_record_spans<4>(bytes, 0, bytes.size(), false,
                                             fields, spans, unknown).ok());
   CHECK_FALSE(unknown);
   CHECK(spans[0].present);
   CHECK_FALSE(spans[1].present);
   fields[1].optional = false;
   CHECK_FALSE(psio::detail::pssz_record_spans<4>(bytes, 0, bytes.size(), false,
                                                 fields, spans, unknown).ok());
}

TEST_CASE("malformed headers and offset tables reject",
          "[pssz][layout]")
{
   const std::array fields{psio::detail::pssz_field_layout{4, true, false}};
   std::array<psio::detail::pssz_field_span, 1> spans;
   bool unknown = false;
   for (const auto& bytes : std::vector<std::vector<char>>{
           {}, {4, 0, 0}, {6, 0, 0, 0, 6, 0, 0, 0, 0, 0},
           {8, 0, 0, 0, 7, 0, 0, 0, 8, 0, 0, 0},
           {8, 0, 0, 0, 8, 0, 0, 0, 7, 0, 0, 0},
           {8, 0, 0, 0, 8, 0, 0, 0, 99, 0, 0, 0}})
      CHECK_FALSE(psio::detail::pssz_record_spans<4>(bytes, 0, bytes.size(), false,
                                                    fields, spans, unknown).ok());
   const std::vector<char> empty;
   CHECK_FALSE(psio::detail::pssz_record_spans<4>(empty, 1, 0, false,
                                                 fields, spans, unknown).ok());
}

TEMPLATE_TEST_CASE("accepted arbitrary record spans always remain inside their input",
                   "[pssz][layout][malformed]", Width1, Width2, Width4)
{
   constexpr std::size_t W = TestType::value;
   const std::array fields{psio::detail::pssz_field_layout{1, false, false},
                           psio::detail::pssz_field_layout{W, true, false},
                           psio::detail::pssz_field_layout{W, true, true}};
   std::array<psio::detail::pssz_field_span, 3> spans;
   std::uint32_t state = 0x71b81a35;
   auto random = [&]() { return state = state * 1664525u + 1013904223u; };
   std::size_t accepted = 0;
   for (std::size_t attempt = 0; attempt < 10000; ++attempt)
   {
      std::vector<char> bytes(random() % 65);
      for (auto& byte : bytes)
         byte = static_cast<char>(random() >> 24);
      if (attempt % 17 == 0)
      {
         bytes.clear();
         const std::size_t h = 1 + 3 * W;
         word<W>(bytes, h);
         bytes.push_back(1);
         word<W>(bytes, h);
         word<W>(bytes, h + 2);
         word<W>(bytes, h + 2);
         bytes.insert(bytes.end(), {'a', 'b', 5});
      }
      bool unknown = false;
      auto status = psio::detail::pssz_record_spans<W>(
          bytes, 0, bytes.size(), false, fields, spans, unknown);
      if (!status.ok())
         continue;
      ++accepted;
      for (const auto& span : spans)
      {
         if (!span.present)
            continue;
         REQUIRE(span.begin <= span.end);
         REQUIRE(span.end <= bytes.size());
      }
   }
   REQUIRE(accepted > 0);
}
