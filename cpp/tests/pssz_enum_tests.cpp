// Plain C++ enums in pssz: an enum is its underlying integer on the wire
// (sizeof(underlying) little-endian bytes), in every position and under every
// width profile. Any underlying value is accepted; the enumerator set is not
// part of the format.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace enum_fixture
{
   enum class E8 : std::uint8_t { A = 0, B = 1, C = 7 };
   enum E16 : std::uint16_t { E16_A = 0, E16_B = 0x0102 };  // unscoped
   enum class E32 : std::int32_t { Neg = -2, Zero = 0, Pos = 0x01020304 };
   enum class E64 : std::uint64_t { Zero = 0, Big = 0x0102030405060708ull };

   struct Rec
   {
      E8                 a;
      E32                b;
      std::string        s;
      std::optional<E16> o;
      friend bool        operator==(const Rec&, const Rec&) = default;
   };
   PSIO_REFLECT(Rec, a, b, s, o)

   struct Fixed
   {
      E8          a;
      E64         b;
      friend bool operator==(const Fixed&, const Fixed&) = default;
   };
   PSIO_REFLECT(Fixed, a, b)

   // No unbounded member: the bound, and so the automatic width, is inferred.
   struct Narrow
   {
      E8          a;
      E64         b;
      friend bool operator==(const Narrow&, const Narrow&) = default;
   };
   PSIO_REFLECT(Narrow, a, b)

   struct NarrowV2
   {
      E8                a;
      E64               b;
      std::optional<E8> c;
      friend bool       operator==(const NarrowV2&, const NarrowV2&) = default;
   };
   PSIO_REFLECT(NarrowV2, a, b, c)
}  // namespace enum_fixture
PSIO_TYPE_ATTRS(enum_fixture::Fixed, psio::definition_will_not_change{})

// Reflecting an enum must not turn it into a record: it stays a scalar.
enum class ReflectedEnum : std::uint16_t { A = 0, B = 0x0102 };
PSIO_REFLECT(ReflectedEnum)

namespace
{
   using namespace enum_fixture;
   namespace impl = psio::detail::pssz_impl;

   static_assert(impl::is_fixed_v<E8> && impl::is_fixed_v<E16> && impl::is_fixed_v<E32> &&
                 impl::is_fixed_v<E64>);
   static_assert(impl::fixed_size_of<E8>() == 1 && impl::fixed_size_of<E16>() == 2 &&
                 impl::fixed_size_of<E32>() == 4 && impl::fixed_size_of<E64>() == 8);
   static_assert(!impl::Record<E8>);
   static_assert(impl::positive_min_size<E8>());
   static_assert(!impl::optional_uses_selector_v<E8> && !impl::optional_uses_selector_v<E64>);
   static_assert(impl::is_fixed_v<Fixed> && impl::fixed_size_of<Fixed>() == 9);
   static_assert(impl::is_fixed_v<std::array<E32, 2>> &&
                 impl::fixed_size_of<std::array<E32, 2>>() == 8);

   static_assert(psio::max_encoded_size<E8>() == 1 && psio::max_encoded_size<E16>() == 2 &&
                 psio::max_encoded_size<E32>() == 4 && psio::max_encoded_size<E64>() == 8);
   static_assert(psio::auto_pssz_width_v<E8> == 1 && psio::auto_pssz_width_v<E64> == 1);
   // header (counted at W=4) + 1 + 8
   static_assert(psio::max_encoded_size<Narrow>() == 13 && psio::auto_pssz_width_v<Narrow> == 1);
   static_assert(psio::max_encoded_size<Fixed>() == 9 && psio::auto_pssz_width_v<Fixed> == 1);
   // header + 1 + 8 + (slot 4 + payload 1 + conservative selector 1)
   static_assert(psio::max_encoded_size<NarrowV2>() == 19 &&
                 psio::auto_pssz_width_v<NarrowV2> == 1);
   static_assert(!psio::max_encoded_size<Rec>().has_value() && psio::auto_pssz_width_v<Rec> == 4);

   static_assert(psio::pssz_evolves_from<Narrow, NarrowV2>);
   static_assert(psio::pssz_evolves_from<E8, E8>);
   // Scalars evolve only as the identical type: an enum is not interchangeable
   // with another enum or with its underlying integer in a compatible lineage.
   static_assert(!psio::pssz_evolves_from<E8, std::uint8_t>);

   template <typename F>
   constexpr std::size_t explicit_width()
   {
      if constexpr (requires { F::offset_width; }) return F::offset_width;
      else return 0;
   }
   template <typename F, typename T>
   constexpr std::size_t width_for()
   {
      return explicit_width<F>() ? explicit_width<F>() : psio::auto_pssz_width_v<T>;
   }

   std::vector<char> bytes(std::initializer_list<unsigned> list)
   {
      std::vector<char> out;
      for (auto b : list) out.push_back(static_cast<char>(b));
      return out;
   }
   void put(std::vector<char>& out, std::size_t W, std::size_t value)
   {
      for (std::size_t i = 0; i < W; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
   }
   void append(std::vector<char>& out, const std::vector<char>& more)
   {
      out.insert(out.end(), more.begin(), more.end());
   }

   template <typename F, typename T>
   void canonical(const T& value, const std::vector<char>& wire)
   {
      REQUIRE(psio::encode(F{}, value) == wire);
      REQUIRE(psio::size_of(F{}, value) == wire.size());
      REQUIRE(psio::validate<T>(F{}, wire).ok());
      REQUIRE_NOTHROW(psio::validate_or_throw<T>(F{}, wire));
      REQUIRE(psio::validate_strict<T>(F{}, wire).ok());
      REQUIRE(psio::decode<T>(F{}, wire) == value);
      bool unknown = true;
      REQUIRE(psio::validate_pssz_compatible<T>(F{}, wire, unknown).ok());
      REQUIRE_FALSE(unknown);
      T out{};
      unknown = true;
      REQUIRE(psio::decode_pssz_checked(F{}, wire, out, unknown).ok());
      REQUIRE_FALSE(unknown);
      REQUIRE(out == value);
   }

   template <typename F, typename T>
   void rejected(const std::vector<char>& wire)
   {
      REQUIRE_FALSE(psio::validate<T>(F{}, wire).ok());
      REQUIRE_FALSE(psio::validate_strict<T>(F{}, wire).ok());
      REQUIRE_THROWS_AS(psio::validate_or_throw<T>(F{}, wire), psio::codec_exception);
      REQUIRE_THROWS_AS(psio::decode<T>(F{}, wire), psio::codec_exception);
      bool unknown = false;
      REQUIRE_FALSE(psio::validate_pssz_compatible<T>(F{}, wire, unknown).ok());
      T out{};
      REQUIRE_FALSE(psio::decode_pssz_checked(F{}, wire, out, unknown).ok());
   }
}  // namespace

#define ENUM_FORMATS psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz

TEMPLATE_TEST_CASE("pssz encodes a plain enum as its underlying integer", "[pssz][enum]",
                   ENUM_FORMATS)
{
   using F = TestType;
   canonical<F, E8>(E8::A, bytes({0}));
   canonical<F, E8>(E8::C, bytes({7}));
   canonical<F, E16>(E16_B, bytes({0x02, 0x01}));
   canonical<F, E32>(E32::Neg, bytes({0xfe, 0xff, 0xff, 0xff}));
   canonical<F, E32>(E32::Pos, bytes({0x04, 0x03, 0x02, 0x01}));
   canonical<F, E64>(E64::Big, bytes({8, 7, 6, 5, 4, 3, 2, 1}));

   // The span must be exactly the underlying integer.
   rejected<F, E8>(bytes({}));
   rejected<F, E8>(bytes({0, 0}));
   rejected<F, E16>(bytes({1}));
   rejected<F, E32>(bytes({1, 2, 3}));
   rejected<F, E32>(bytes({1, 2, 3, 4, 5}));
   rejected<F, E64>(bytes({1, 2, 3, 4, 5, 6, 7}));
}

TEMPLATE_TEST_CASE("pssz accepts any underlying value for an enum", "[pssz][enum]", ENUM_FORMATS)
{
   using F = TestType;
   // 0x55 names no enumerator of E8; it is carried through unchanged.
   canonical<F, E8>(static_cast<E8>(0x55), bytes({0x55}));
   canonical<F, E32>(static_cast<E32>(-1), bytes({0xff, 0xff, 0xff, 0xff}));
   canonical<F, std::vector<E8>>({static_cast<E8>(0xff), E8::B}, bytes({0xff, 1}));
}

TEMPLATE_TEST_CASE("pssz enum containers: flat vectors, arrays, optionals, variants",
                   "[pssz][enum]", ENUM_FORMATS)
{
   using F = TestType;
   // A vector of enums is flat: no offset table, count = span / size.
   canonical<F, std::vector<E16>>({E16_A, E16_B, E16_B}, bytes({0, 0, 2, 1, 2, 1}));
   canonical<F, std::vector<E16>>({}, bytes({}));
   canonical<F, std::vector<E8>>({E8::A, E8::B, E8::C}, bytes({0, 1, 7}));
   canonical<F, std::array<E32, 2>>({E32::Zero, E32::Pos}, bytes({0, 0, 0, 0, 4, 3, 2, 1}));
   rejected<F, std::vector<E16>>(bytes({0, 0, 2}));
   rejected<F, std::array<E32, 2>>(bytes({0, 0, 0, 0, 4, 3, 2}));

   // optional<enum>: the payload has a positive minimum size, so no selector.
   canonical<F, std::optional<E8>>(std::nullopt, bytes({}));
   canonical<F, std::optional<E8>>(E8::A, bytes({0}));
   canonical<F, std::optional<E64>>(E64::Zero, bytes({0, 0, 0, 0, 0, 0, 0, 0}));
   canonical<F, std::optional<E32>>(E32::Neg, bytes({0xfe, 0xff, 0xff, 0xff}));
   rejected<F, std::optional<E8>>(bytes({1, 0}));
   rejected<F, std::optional<E32>>(bytes({1, 0xfe, 0xff, 0xff, 0xff}));

   // variant: one index byte, then the alternative.
   using V = std::variant<E8, std::string, E32>;
   canonical<F, V>(V{std::in_place_index<0>, E8::C}, bytes({0, 7}));
   canonical<F, V>(V{std::in_place_index<1>, "hi"}, bytes({1, 'h', 'i'}));
   canonical<F, V>(V{std::in_place_index<2>, E32::Pos}, bytes({2, 4, 3, 2, 1}));
   REQUIRE_FALSE(psio::validate<V>(F{}, bytes({0})).ok());
   REQUIRE_FALSE(psio::validate<V>(F{}, bytes({2, 4, 3, 2})).ok());

   // optional<vector<enum>> can be empty when present, so it has a selector.
   canonical<F, std::optional<std::vector<E8>>>(std::vector<E8>{}, bytes({1}));
   canonical<F, std::optional<std::vector<E8>>>(std::vector<E8>{E8::B}, bytes({1, 1}));
}

TEMPLATE_TEST_CASE("pssz enum record fields", "[pssz][enum]", ENUM_FORMATS)
{
   using F = TestType;
   {
      const std::size_t W = width_for<F, Rec>();
      // header | a | b | slot(s) | [slot(o)] | "hi" | [o]
      std::vector<char> pruned;
      put(pruned, W, 5 + W);
      append(pruned, bytes({7, 0x04, 0x03, 0x02, 0x01}));
      put(pruned, W, 5 + W);
      append(pruned, bytes({'h', 'i'}));
      canonical<F, Rec>(Rec{E8::C, E32::Pos, "hi", std::nullopt}, pruned);

      std::vector<char> full;
      put(full, W, 5 + 2 * W);
      append(full, bytes({7, 0x04, 0x03, 0x02, 0x01}));
      put(full, W, 5 + 2 * W);
      put(full, W, 5 + 2 * W + 2);
      append(full, bytes({'h', 'i', 0x02, 0x01}));
      canonical<F, Rec>(Rec{E8::C, E32::Pos, "hi", E16_B}, full);

      // A stored trailing None must have been pruned.
      std::vector<char> unpruned;
      put(unpruned, W, 5 + 2 * W);
      append(unpruned, bytes({7, 0x04, 0x03, 0x02, 0x01}));
      put(unpruned, W, 5 + 2 * W);
      put(unpruned, W, 5 + 2 * W + 2);
      append(unpruned, bytes({'h', 'i'}));
      rejected<F, Rec>(unpruned);

      auto view = psio::view<Rec, F, psio::storage::const_borrow>{full};
      REQUIRE(view.template get<0>().get() == E8::C);
      REQUIRE(view.template get<1>().get() == E32::Pos);
      REQUIRE(view.template get<2>().view_() == "hi");
      REQUIRE(view.template get<3>().has_value());
      REQUIRE(view.template get<3>().get().get() == E16_B);
      auto none = psio::view<Rec, F, psio::storage::const_borrow>{pruned};
      REQUIRE_FALSE(none.template get<3>().has_value());
   }
   {
      // DWNC all-fixed: no header, the enum bytes inline.
      canonical<F, Fixed>(Fixed{E8::B, E64::Big}, bytes({1, 8, 7, 6, 5, 4, 3, 2, 1}));
      rejected<F, Fixed>(bytes({1, 8, 7, 6, 5, 4, 3, 2}));
      canonical<F, std::vector<Fixed>>(
         {Fixed{E8::A, E64::Zero}, Fixed{E8::B, E64::Big}},
         bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 8, 7, 6, 5, 4, 3, 2, 1}));
      auto wire = psio::encode(F{}, Fixed{E8::B, E64::Big});
      auto view = psio::view<Fixed, F, psio::storage::const_borrow>{wire};
      REQUIRE(view.template get<1>().get() == E64::Big);
   }
   {
      // Extensible all-fixed record: its header uses the record's own width.
      const std::size_t W = width_for<F, Narrow>();
      std::vector<char> wire;
      put(wire, W, 9);
      append(wire, bytes({7, 8, 7, 6, 5, 4, 3, 2, 1}));
      canonical<F, Narrow>(Narrow{E8::C, E64::Big}, wire);
      // Old data read by the next revision, and new data by the old one.
      REQUIRE(psio::decode<NarrowV2>(F{}, wire) == NarrowV2{E8::C, E64::Big, std::nullopt});
      std::vector<char> v2;
      put(v2, W, 9 + W);
      append(v2, bytes({7, 8, 7, 6, 5, 4, 3, 2, 1}));
      put(v2, W, 9 + W);
      append(v2, bytes({1}));
      canonical<F, NarrowV2>(NarrowV2{E8::C, E64::Big, E8::B}, v2);
      bool unknown = false;
      Narrow old{};
      REQUIRE(psio::decode_pssz_checked(F{}, v2, old, unknown).ok());
      REQUIRE(unknown);
      REQUIRE(old == Narrow{E8::C, E64::Big});
      REQUIRE_FALSE(psio::validate_strict<Narrow>(F{}, v2).ok());
   }
   {
      // Tuples are positional records.
      using T = std::tuple<E8, std::optional<E32>>;
      const std::size_t W = width_for<F, T>();
      std::vector<char> wire;
      put(wire, W, 1 + W);
      append(wire, bytes({1}));
      put(wire, W, 1 + W);
      append(wire, bytes({0xfe, 0xff, 0xff, 0xff}));
      canonical<F, T>(T{E8::B, E32::Neg}, wire);
      auto view = psio::view<T, F, psio::storage::const_borrow>{wire};
      REQUIRE(view.template get<0>().get() == E8::B);
      REQUIRE(view.template get<1>().get().get() == E32::Neg);
   }
}

static_assert(psio::Reflected<ReflectedEnum>);
static_assert(!psio::detail::pssz_impl::Record<ReflectedEnum>);
static_assert(psio::detail::pssz_impl::is_fixed_v<ReflectedEnum> &&
              psio::detail::pssz_impl::fixed_size_of<ReflectedEnum>() == 2);

TEMPLATE_TEST_CASE("pssz treats a reflected enum as a scalar", "[pssz][enum]", ENUM_FORMATS)
{
   using F = TestType;
   canonical<F, ReflectedEnum>(ReflectedEnum::B, bytes({0x02, 0x01}));
   rejected<F, ReflectedEnum>(bytes({0x02}));
   canonical<F, std::optional<ReflectedEnum>>(ReflectedEnum::A, bytes({0, 0}));
   const auto wire = psio::encode(F{}, ReflectedEnum::B);
   REQUIRE(psio::view<ReflectedEnum, F, psio::storage::const_borrow>{wire}.get() ==
           ReflectedEnum::B);
}

TEST_CASE("pssz automatic width of a record holding enums", "[pssz][enum][width]")
{
   // Narrow has bound 13, so the automatic profile uses one-byte offsets and
   // is byte-identical to pssz8.
   const Narrow value{E8::C, E64::Big};
   REQUIRE(psio::encode(psio::pssz{}, value) == psio::encode(psio::pssz8{}, value));
   REQUIRE(psio::encode(psio::pssz{}, value).size() == 10);
   REQUIRE(psio::encode(psio::pssz32{}, value).size() == 13);
}

TEST_CASE("SchemaBuilder describes an enum as its underlying integer", "[pssz][enum][schema]")
{
   namespace ir = psio::schema_types;
   psio::SchemaBuilder builder;
   auto e8      = builder.insert<E8>();
   auto e16     = builder.insert<E16>();
   auto e32     = builder.insert<E32>();
   auto e64     = builder.insert<E64>();
   auto rec     = builder.insert<Narrow>();
   auto schema  = std::move(builder).build();
   REQUIRE(std::get<ir::Int>(e8.resolve(schema)->value) == ir::Int{8, false});
   REQUIRE(std::get<ir::Int>(e16.resolve(schema)->value) == ir::Int{16, false});
   REQUIRE(std::get<ir::Int>(e32.resolve(schema)->value) == ir::Int{32, true});
   REQUIRE(std::get<ir::Int>(e64.resolve(schema)->value) == ir::Int{64, false});
   const auto& object = std::get<ir::Object>(rec.resolve(schema)->value);
   REQUIRE(std::get<ir::Int>(object.members[0].type->resolve(schema)->value) == ir::Int{8, false});
   REQUIRE(std::get<ir::Int>(object.members[1].type->resolve(schema)->value) == ir::Int{64, false});
}
