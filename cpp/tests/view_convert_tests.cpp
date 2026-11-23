// From a view to what it designates, implicitly — one rule:
//   - to the VALUE, exactly when the value is trivially copyable and of
//     fixed encoded size (scalar, enum, time point, uint256, std::array of
//     such, definitionWillNotChange record of such);
//   - to a borrowed std::string_view / std::span for a byte sequence;
//   - everything else is unpack().
// view_convert_fail.cpp holds the conversions that must not compile.

#include <catch.hpp>

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/time.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
   // psibase::AccountNumber: a definitionWillNotChange record of one integer
   // with a text adapter (its JSON form); its bytes are the integer's.
   struct Name
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Name&, const Name&) = default;
   };
   PSIO_REFLECT(Name, definitionWillNotChange(), value)
   struct NameText
   {
      static constexpr std::string_view schema_id = "Name";
      static std::size_t packsize(const Name& n) { return std::to_string(n.value).size(); }
      static void        encode(const Name& n, std::string& out) { out += std::to_string(n.value); }
      static Name        decode(std::span<const char> t)
      {
         return Name{std::stoull(std::string(t.data(), t.size()))};
      }
      static psio::codec_status validate(std::span<const char>) noexcept { return psio::codec_ok(); }
      static psio::codec_status validate_strict(std::span<const char> t) noexcept
      {
         return validate(t);
      }
   };
}  // namespace
PSIO_ADAPTER(Name, psio::text_category, NameText)

namespace
{
   using Digest = std::array<std::uint8_t, 32>;  // psibase::Checksum256
   enum class Color : std::uint16_t { red = 1, green = 2 };

   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      friend bool  operator==(const Point&, const Point&) = default;
   };
   PSIO_REFLECT(Point, definitionWillNotChange(), x, y)
   // Fixed records nest, and padding in memory does not matter.
   struct Box
   {
      std::uint8_t tag = 0;
      Point        low;
      Point        high;
      Name         owner;
      friend bool  operator==(const Box&, const Box&) = default;
   };
   PSIO_REFLECT(Box, definitionWillNotChange(), tag, low, high, owner)
   // A transparent type over a fixed value.
   struct Id
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Id&, const Id&) = default;
   };
   PSIO_REFLECT(Id, transparent(), value)

   struct Row
   {
      Name                              owner;
      Digest                            id;
      std::vector<unsigned char>        data;
      std::vector<char>                 chars;
      std::vector<std::byte>            raw;
      std::vector<std::int8_t>          small;
      std::array<char, 4>               tag{};
      std::array<std::uint32_t, 3>      triple{};
      std::array<Point, 2>              corners{};
      Point                             point;
      Box                               box;
      Id                                ident;
      Color                             color = Color::red;
      psio::uint256                     big;
      std::chrono::seconds              elapsed{0};
      std::string                       text;
      psio::utf8_string<16>             label;
      std::uint32_t                     n = 0;
      bool                              flag = false;
   };
   PSIO_REFLECT(Row, owner, id, data, chars, raw, small, tag, triple, corners, point, box, ident,
                color, big, elapsed, text, label, n, flag)

   Row sample()
   {
      Row r;
      r.owner = Name{7};
      for (std::size_t i = 0; i < r.id.size(); ++i) r.id[i] = static_cast<std::uint8_t>(i + 1);
      r.data    = {1, 2, 3};
      r.chars   = {'a', 'b'};
      r.raw     = {std::byte{4}, std::byte{5}};
      r.small   = {-1, 2};
      r.tag     = {'t', 'a', 'g', '!'};
      r.triple  = {10, 20, 30};
      r.corners = {Point{1, 2}, Point{3, 4}};
      r.point   = Point{5, 6};
      r.box     = Box{9, Point{1, 1}, Point{2, 2}, Name{8}};
      r.ident   = Id{77};
      r.color   = Color::green;
      r.big.limb[0] = 42;
      r.elapsed = std::chrono::seconds{90};
      r.text    = "text";
      r.label   = psio::utf8_string<16>{"label"};
      r.n       = 12;
      r.flag    = true;
      return r;
   }

   // Functions that take the value / the borrowed form: the view is passed
   // as it is.
   Name                 takesName(Name n) { return n; }
   Digest               takesDigest(const Digest& d) { return d; }
   std::size_t          takesOctets(std::span<const unsigned char> s) { return s.size(); }
   std::size_t          takesChars(std::span<const char> s) { return s.size(); }
   std::size_t          takesBytes(std::span<const std::byte> s) { return s.size(); }
   std::size_t          takesDigestSpan(std::span<const std::uint8_t, 32> s) { return s.size(); }
   Point                takesPoint(Point p) { return p; }
   Box                  takesBox(const Box& b) { return b; }
   std::string_view     takesText(std::string_view s) { return s; }

   template <typename M>
   using view_of = decltype(std::declval<psio::pssz_view<Row>>().template get<
      *psio::reflect<Row>::index_of(M::name)>());

   // The rule, type by type: implicit conversion to the value.
   template <typename T>
   inline constexpr bool to_value = std::is_convertible_v<psio::pssz_view<T>, T>;
   static_assert(to_value<Name>);
   static_assert(to_value<Digest>);
   static_assert(to_value<std::array<std::uint32_t, 3>>);
   static_assert(to_value<std::array<Point, 2>>);
   static_assert(to_value<Point>);
   static_assert(to_value<Box>);
   static_assert(to_value<Id>);
   static_assert(to_value<Color>);
   static_assert(to_value<psio::uint256>);
   static_assert(to_value<std::chrono::seconds>);
   static_assert(to_value<std::uint64_t>);
   // Not for a value that allocates or is walked.
   static_assert(!to_value<std::string>);
   static_assert(!to_value<std::vector<unsigned char>>);
   static_assert(!to_value<std::vector<Point>>);
   static_assert(!to_value<std::optional<std::uint32_t>>);
   static_assert(!to_value<Row>);
   static_assert(!to_value<psio::utf8_string<16>>);
   // Borrowed forms of byte sequences.
   static_assert(std::is_convertible_v<psio::pssz_view<std::vector<unsigned char>>,
                                       std::span<const unsigned char>>);
   static_assert(std::is_convertible_v<psio::pssz_view<std::vector<char>>, std::span<const char>>);
   static_assert(std::is_convertible_v<psio::pssz_view<std::vector<std::byte>>,
                                       std::span<const std::byte>>);
   static_assert(std::is_convertible_v<psio::pssz_view<Digest>, std::span<const std::uint8_t, 32>>);
   static_assert(std::is_convertible_v<psio::pssz_view<Digest>, std::span<const std::uint8_t>>);
   static_assert(std::is_convertible_v<psio::pssz_view<std::string>, std::string_view>);
   // Not a span over elements wider than a byte, nor of another byte type.
   static_assert(!std::is_convertible_v<psio::pssz_view<std::vector<std::uint32_t>>,
                                        std::span<const std::uint32_t>>);
   static_assert(!std::is_convertible_v<psio::pssz_view<std::array<std::uint32_t, 3>>,
                                        std::span<const std::uint32_t>>);
   static_assert(!std::is_convertible_v<psio::pssz_view<std::vector<unsigned char>>,
                                        std::span<const char>>);
   static_assert(!std::is_convertible_v<psio::pssz_view<std::vector<bool>>, std::span<const bool>>);
}  // namespace

TEST_CASE("a view converts to a trivially copyable fixed-size value", "[view][convert]")
{
   const Row  row   = sample();
   const auto bytes = psio::to_pssz(row);
   psio::pssz_view<Row> v{bytes};

   // The three of the gap report.
   REQUIRE(takesName(v.owner()) == Name{7});
   REQUIRE(takesDigest(v.id()) == row.id);
   REQUIRE(takesOctets(v.data()) == 3);

   // Declarations and assignments.
   Name   owner = v.owner();
   Digest id    = v.id();
   REQUIRE(owner == row.owner);
   REQUIRE(id == row.id);
   std::array<std::uint32_t, 3> triple = v.triple();
   REQUIRE(triple == row.triple);
   std::array<Point, 2> corners = v.corners();
   REQUIRE(corners == row.corners);
   REQUIRE(takesPoint(v.point()) == row.point);
   REQUIRE(takesBox(v.box()) == row.box);
   REQUIRE(takesName(v.box().owner()) == Name{8});
   Id ident = v.ident();
   REQUIRE(ident == row.ident);
   Color color = v.color();
   REQUIRE(color == Color::green);
   psio::uint256 big = v.big();
   REQUIRE(big.limb[0] == 42);
   std::chrono::seconds elapsed = v.elapsed();
   REQUIRE(elapsed.count() == 90);
   std::uint32_t n = v.n();
   REQUIRE(n == 12);
   // Elements of a list of fixed records.
   const auto                        pb = psio::to_pssz(std::vector<Point>{{1, 2}, {3, 4}});
   psio::pssz_view<std::vector<Point>> points{pb};
   Point second = points[1];
   REQUIRE(second == Point{3, 4});
   // Through an optional view.
   const auto ob = psio::to_pssz(std::optional<Name>{Name{5}});
   psio::pssz_view<std::optional<Name>> maybe{ob};
   Name inner = *maybe;
   REQUIRE(inner == Name{5});
   // A view of const T is the same view.
   psio::pssz_view<const Row> cv{bytes};
   Name cowner = cv.owner();
   REQUIRE(cowner == row.owner);
   // unpack() still returns the same values.
   REQUIRE(v.owner().unpack() == owner);
   REQUIRE(v.id().unpack() == id);
   // Comparison is unchanged.
   REQUIRE(v.owner() == Name{7});
   REQUIRE(v.point() == row.point);
}

TEST_CASE("a view of a byte sequence converts to what borrows it", "[view][convert]")
{
   const Row  row   = sample();
   const auto bytes = psio::to_pssz(row);
   psio::pssz_view<Row> v{bytes};
   const auto inside = [&](const void* p) {
      const char* c = static_cast<const char*>(p);
      return c >= bytes.data() && c < bytes.data() + bytes.size();
   };

   std::span<const unsigned char> data = v.data();
   REQUIRE(data.size() == 3);
   REQUIRE(data[2] == 3);
   REQUIRE(inside(data.data()));
   REQUIRE(v.data().data() == data.data());
   REQUIRE(takesChars(v.chars()) == 2);
   std::span<const std::byte> raw = v.raw();
   REQUIRE(raw.size() == 2);
   REQUIRE(raw[1] == std::byte{5});
   REQUIRE(takesBytes(v.raw()) == 2);
   std::span<const std::int8_t> small = v.small();
   REQUIRE(small[0] == -1);

   // A fixed array of bytes: the value, and both spans.
   std::span<const std::uint8_t, 32> fixed = v.id();
   REQUIRE(fixed[31] == 32);
   REQUIRE(inside(fixed.data()));
   std::span<const std::uint8_t> dynamic = v.id();
   REQUIRE(dynamic.size() == 32);
   REQUIRE(dynamic.data() == fixed.data());
   REQUIRE(takesDigestSpan(v.id()) == 32);
   std::span<const char, 4> tag = v.tag();
   REQUIRE(std::string_view(tag.data(), tag.size()) == "tag!");
   REQUIRE(v.id().data() == fixed.data());

   // Strings.
   REQUIRE(takesText(v.text()) == "text");
   REQUIRE(takesText(v.label()) == "label");
   REQUIRE(inside(takesText(v.text()).data()));

   // Other profiles and positions answer alike.
   const auto                           wide = psio::encode(psio::pssz_<4>{}, row);
   psio::view<Row, psio::pssz_<4>>      wv{wide};
   Name                                 wowner = wv.owner();
   std::span<const unsigned char>       wdata  = wv.data();
   REQUIRE(wowner == row.owner);
   REQUIRE(wdata.size() == 3);
}
