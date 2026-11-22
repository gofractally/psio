// psio::mutable_view — in-place overwrite of fixed-size values in a PSSZ
// frame (docs/pssz-spec.md §17).
//
// Every mutation is followed by the same check: the bytes still pass
// validate and validate_strict, they are byte-identical to a fresh encoding
// of the expected value, and decoding and re-encoding them reproduces them.
// The rejected operations are covered by the psio_mutable_view_*_compile_fail
// tests (mutable_view_fail.cpp).

#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace {

   enum class MvColor : std::uint16_t { red = 1, green = 2, blue = 0x1234 };

   // DWNC, layout equal to memory: the encoder's memcpy path.
   struct MvPoint
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
   };
   PSIO_REFLECT(MvPoint, x, y, definitionWillNotChange())

   // DWNC with padding in memory: the encoder's member walk.
   struct MvMixed
   {
      bool          flag = false;
      std::uint64_t big  = 0;
      MvColor       color{MvColor::red};
      MvPoint       at;
   };
   PSIO_REFLECT(MvMixed, flag, big, color, at, definitionWillNotChange())

   // Extensible, with a variable member between fixed ones.
   struct MvInner
   {
      std::uint32_t id = 0;
      std::string   name;
      MvPoint       pos;
      bool          live = false;
   };
   PSIO_REFLECT(MvInner, id, name, pos, live)

   struct MvScalars
   {
      std::uint8_t                                tag = 0;
      std::string                                 title;
      bool                                        active = false;
      MvColor                                     color{MvColor::red};
      std::chrono::seconds                        ttl{};
      std::chrono::sys_time<std::chrono::seconds> created{};
      psio::uint128                               wide = 0;
      psio::int128                                swide = 0;
      psio::uint256                               huge{};
      double                                      ratio = 0;
      std::int64_t                                balance = 0;
   };
   PSIO_REFLECT(MvScalars, tag, title, active, color, ttl, created, wide, swide, huge, ratio,
                balance)

   struct MvContainers
   {
      std::array<std::uint16_t, 3>             triple{};
      std::vector<std::uint32_t>               numbers;
      std::vector<MvInner>                     children;
      MvInner                                  inner;
      MvMixed                                  mixed;
      std::array<MvPoint, 2>                   corners{};
      std::variant<std::uint32_t, std::string> choice;
      psio::bitvector<9>                       bits;
      std::optional<std::uint32_t>             maybe;
      std::optional<MvPoint>                   maybe_point;
      std::optional<std::string>               note;
   };
   PSIO_REFLECT(MvContainers, triple, numbers, children, inner, mixed, corners, choice,
                bits, maybe, maybe_point, note)

   // Automatic width 1 and 2 through a declared budget.
   struct MvNarrow
   {
      std::uint16_t count = 0;
      std::string   label;
      std::uint32_t total = 0;
   };
   PSIO_REFLECT(MvNarrow, count, label, total, maxDynamicData(64))
   struct MvMedium
   {
      std::uint16_t count = 0;
      std::string   label;
      std::uint32_t total = 0;
   };
   PSIO_REFLECT(MvMedium, count, label, total, maxDynamicData(1000))

   // One schema revision apart.
   struct MvOld
   {
      std::uint32_t id = 0;
      std::string   name;
   };
   PSIO_REFLECT(MvOld, id, name)
   struct MvNew
   {
      std::uint32_t              id = 0;
      std::string                name;
      std::optional<std::string> extra;
   };
   PSIO_REFLECT(MvNew, id, name, extra)

   // The bytes are valid, are exactly the encoding of `expected`, and
   // survive decode + encode unchanged.
   template <typename F, typename T>
   void require_canonical(const std::vector<char>& bytes, const T& expected)
   {
      REQUIRE(psio::validate<T>(F{}, bytes).ok());
      REQUIRE(psio::validate_strict<T>(F{}, bytes).ok());
      REQUIRE(bytes == psio::encode(F{}, expected));
      REQUIRE(psio::encode(F{}, psio::decode<T>(F{}, bytes)) == bytes);
   }

   MvScalars sample_scalars()
   {
      MvScalars v;
      v.tag     = 7;
      v.title   = "title";
      v.active  = false;
      v.color   = MvColor::green;
      v.ttl     = std::chrono::seconds{30};
      v.created = std::chrono::sys_time<std::chrono::seconds>{std::chrono::seconds{1795342530}};
      v.wide    = 1;
      v.swide   = -1;
      v.huge.limb[0] = 5;
      v.ratio   = 0.5;
      v.balance = -9;
      return v;
   }

   MvContainers sample_containers()
   {
      MvContainers v;
      v.triple   = {1, 2, 3};
      v.numbers  = {10, 20, 30, 40};
      v.children = {MvInner{1, "a", {1, 2}, false}, MvInner{2, "bc", {3, 4}, true}};
      v.inner    = MvInner{9, "in", {5, 6}, true};
      v.mixed    = MvMixed{true, 77, MvColor::blue, {7, 8}};
      v.corners  = {MvPoint{1, 1}, MvPoint{2, 2}};
      v.choice   = std::uint32_t{5};
      v.bits.set(0, true);
      v.bits.set(8, true);
      v.maybe       = 11;
      v.maybe_point = MvPoint{12, 13};
      v.note        = "n";
      return v;
   }

}  // namespace

TEMPLATE_TEST_CASE("mutable_view overwrites scalar members of an extensible record",
                   "[mutable_view]", psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz)
{
   using F = TestType;
   auto  expected = sample_scalars();
   auto  bytes    = psio::encode(F{}, expected);
   const auto size = bytes.size();
   psio::mutable_view<MvScalars, F> mv{bytes};

   REQUIRE(mv.template get<0>().get() == 7);
   REQUIRE(mv.template get<0>().set(200));
   expected.tag = 200;
   require_canonical<F>(bytes, expected);

   // bool: written as 0 / 1 whatever the argument converts from.
   REQUIRE(mv.template get<2>().set(true));
   expected.active = true;
   require_canonical<F>(bytes, expected);
   REQUIRE(mv.template get<2>().get() == true);
   REQUIRE(mv.template get<2>().set(false));
   expected.active = false;
   require_canonical<F>(bytes, expected);

   REQUIRE(mv.template get<3>().set(MvColor::blue));
   expected.color = MvColor::blue;
   require_canonical<F>(bytes, expected);
   REQUIRE(mv.template get<3>().get() == MvColor::blue);

   REQUIRE(mv.template get<4>().set(std::chrono::seconds{-5}));
   expected.ttl = std::chrono::seconds{-5};
   require_canonical<F>(bytes, expected);
   REQUIRE(mv.template get<4>().get() == std::chrono::seconds{-5});

   const std::chrono::sys_time<std::chrono::seconds> later{std::chrono::seconds{1900000000}};
   REQUIRE(mv.template get<5>().set(later));
   expected.created = later;
   require_canonical<F>(bytes, expected);

   const psio::uint128 wide = (psio::uint128{0x0102030405060708ull} << 64) | 0x1112131415161718ull;
   REQUIRE(mv.template get<6>().set(wide));
   expected.wide = wide;
   require_canonical<F>(bytes, expected);

   const psio::int128 swide = -(psio::int128{1} << 100);
   REQUIRE(mv.template get<7>().set(swide));
   expected.swide = swide;
   require_canonical<F>(bytes, expected);

   psio::uint256 huge;
   huge.limb[0] = 1;
   huge.limb[3] = 0xfedcba9876543210ull;
   REQUIRE(mv.template get<8>().set(huge));
   expected.huge = huge;
   require_canonical<F>(bytes, expected);
   REQUIRE(mv.template get<8>().get() == huge);

   REQUIRE(mv.template get<9>().set(-2.25));
   expected.ratio = -2.25;
   require_canonical<F>(bytes, expected);

   REQUIRE(mv.template get<10>().set(std::int64_t{1} << 40));
   expected.balance = std::int64_t{1} << 40;
   require_canonical<F>(bytes, expected);

   // Nothing moved: the variable member and the frame size are untouched.
   REQUIRE(bytes.size() == size);
   REQUIRE(psio::as_view(mv).template get<1>().view_() == "title");
}

TEMPLATE_TEST_CASE("mutable_view overwrites through arrays, vectors and nested records",
                   "[mutable_view]", psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz)
{
   using F = TestType;
   auto  expected = sample_containers();
   auto  bytes    = psio::encode(F{}, expected);
   psio::mutable_view<MvContainers, F> mv{bytes};

   SECTION("array element and whole array")
   {
      auto triple = mv.template get<0>();
      REQUIRE(triple.size() == 3);
      REQUIRE(triple[1].set(0xbeef));
      expected.triple[1] = 0xbeef;
      require_canonical<F>(bytes, expected);
      REQUIRE(triple.at(2).get() == 3);
      REQUIRE_FALSE(triple.at(3).set(1));
      require_canonical<F>(bytes, expected);

      REQUIRE(triple.set({7, 8, 9}));
      expected.triple = {7, 8, 9};
      require_canonical<F>(bytes, expected);
   }
   SECTION("vector of fixed elements")
   {
      auto numbers = mv.template get<1>();
      REQUIRE(numbers.size() == 4);
      REQUIRE_FALSE(numbers.empty());
      REQUIRE(numbers[0].set(0xffffffffu));
      REQUIRE(numbers[3].set(0));
      expected.numbers[0] = 0xffffffffu;
      expected.numbers[3] = 0;
      require_canonical<F>(bytes, expected);

      std::uint32_t next = 100;
      for (auto element : numbers)
         REQUIRE(element.set(next++));
      expected.numbers = {100, 101, 102, 103};
      require_canonical<F>(bytes, expected);

      // Out of range: nothing is written.
      REQUIRE_FALSE(numbers.at(4).set(1));
      require_canonical<F>(bytes, expected);
   }
   SECTION("vector of variable elements: fixed members of each element")
   {
      auto children = mv.template get<2>();
      REQUIRE(children.size() == 2);
      REQUIRE(children[1].template get<0>().set(4242));
      REQUIRE(children[1].template get<2>().template get<1>().set(-4));
      REQUIRE(children[0].template get<3>().set(true));
      REQUIRE(children[0].template get<2>().set(MvPoint{100, 200}));
      expected.children[1].id    = 4242;
      expected.children[1].pos.y = -4;
      expected.children[0].live  = true;
      expected.children[0].pos   = MvPoint{100, 200};
      require_canonical<F>(bytes, expected);
      REQUIRE(psio::as_view(children[1]).template get<1>().view_() == "bc");
   }
   SECTION("nested extensible record and DWNC records")
   {
      auto inner = mv.template get<3>();
      REQUIRE(inner.template get<0>().set(1000));
      REQUIRE(inner.template get<2>().template get<0>().set(-1));
      REQUIRE(inner.template get<3>().set(false));
      expected.inner.id    = 1000;
      expected.inner.pos.x = -1;
      expected.inner.live  = false;
      require_canonical<F>(bytes, expected);

      // A DWNC record member by member, then as a whole.
      auto mixed = mv.template get<4>();
      REQUIRE(mixed.template get<0>().set(false));
      REQUIRE(mixed.template get<1>().set(0x0123456789abcdefull));
      REQUIRE(mixed.template get<2>().set(MvColor::red));
      REQUIRE(mixed.template get<3>().template get<1>().set(55));
      expected.mixed = MvMixed{false, 0x0123456789abcdefull, MvColor::red, {7, 55}};
      require_canonical<F>(bytes, expected);

      const MvMixed whole{true, 1, MvColor::green, {-7, -8}};
      REQUIRE(mixed.set(whole));
      expected.mixed = whole;
      require_canonical<F>(bytes, expected);

      auto corners = mv.template get<5>();
      REQUIRE(corners[1].set(MvPoint{21, 22}));
      REQUIRE(corners[0].template get<0>().set(-11));
      expected.corners[1]   = MvPoint{21, 22};
      expected.corners[0].x = -11;
      require_canonical<F>(bytes, expected);
   }
   SECTION("active fixed-size variant alternative")
   {
      auto choice = mv.template get<6>();
      REQUIRE(choice.index() == 0);
      const bool written = choice.visit([](auto alternative) {
         if constexpr (std::is_same_v<typename decltype(alternative)::element_type,
                                      std::uint32_t>)
            return alternative.set(99);
         else
            return false;
      });
      REQUIRE(written);
      expected.choice = std::uint32_t{99};
      require_canonical<F>(bytes, expected);
   }
   SECTION("bitvector: a value with unused bits set has no encoding")
   {
      auto bits = mv.template get<7>();
      psio::bitvector<9> next;
      next.set(3, true);
      REQUIRE(bits.set(next));
      expected.bits = next;
      require_canonical<F>(bytes, expected);

      psio::bitvector<9> stray;
      stray.data()[1] = 0x02;  // bit 9 does not exist
      REQUIRE_FALSE(bits.set(stray));
      require_canonical<F>(bytes, expected);
   }
   SECTION("present optional payloads")
   {
      auto maybe = mv.template get<8>();
      REQUIRE(maybe.has_value());
      REQUIRE(bool(maybe));
      REQUIRE((*maybe).get() == 11);
      REQUIRE((*maybe).set(12));
      expected.maybe = 12;
      require_canonical<F>(bytes, expected);
      // Some(0) is a value, not a default: it stays present.
      REQUIRE(maybe.get().set(0));
      expected.maybe = 0;
      require_canonical<F>(bytes, expected);
      REQUIRE(maybe.has_value());

      auto point = mv.template get<9>();
      REQUIRE(point.has_value());
      REQUIRE((*point).template get<1>().set(-13));
      expected.maybe_point->y = -13;
      require_canonical<F>(bytes, expected);
      REQUIRE((*point).set(MvPoint{0, 0}));
      expected.maybe_point = MvPoint{0, 0};
      require_canonical<F>(bytes, expected);
   }
}

TEMPLATE_TEST_CASE("mutable_view never writes through an absent optional", "[mutable_view]",
                   psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz)
{
   using F = TestType;
   auto  expected = sample_containers();

   SECTION("None followed by a present member")
   {
      expected.maybe.reset();
      expected.maybe_point.reset();
      auto bytes = psio::encode(F{}, expected);
      psio::mutable_view<MvContainers, F> mv{bytes};
      REQUIRE_FALSE(mv.template get<8>().has_value());
      REQUIRE_FALSE((*mv.template get<8>()).set(1));
      REQUIRE_FALSE((*mv.template get<9>()).set(MvPoint{1, 2}));
      REQUIRE_FALSE((*mv.template get<9>()).template get<0>().set(1));
      require_canonical<F>(bytes, expected);
   }
   SECTION("pruned trailing optionals")
   {
      expected.maybe.reset();
      expected.maybe_point.reset();
      expected.note.reset();
      auto bytes = psio::encode(F{}, expected);
      psio::mutable_view<MvContainers, F> mv{bytes};
      REQUIRE_FALSE(mv.template get<8>().has_value());
      REQUIRE_FALSE(mv.template get<9>().has_value());
      REQUIRE_FALSE((*mv.template get<8>()).set(1));
      REQUIRE_FALSE((*mv.template get<9>()).set(MvPoint{1, 2}));
      require_canonical<F>(bytes, expected);
      // Members before the pruned suffix are still writable.
      REQUIRE(mv.template get<0>()[0].set(31));
      expected.triple[0] = 31;
      require_canonical<F>(bytes, expected);
   }
}

TEST_CASE("mutable_view follows the automatic width of each type", "[mutable_view]")
{
   STATIC_REQUIRE(psio::auto_pssz_width_v<MvNarrow> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<MvMedium> == 2);
   STATIC_REQUIRE(psio::auto_pssz_width_v<MvOld> == 4);

   MvNarrow narrow{1, "abc", 2};
   auto     narrow_bytes = psio::encode(psio::pssz{}, narrow);
   psio::mutable_view<MvNarrow, psio::pssz> narrow_view{narrow_bytes};
   REQUIRE(narrow_view.get<0>().set(0xa1b2));
   REQUIRE(narrow_view.get<2>().set(0xc1c2c3c4));
   narrow.count = 0xa1b2;
   narrow.total = 0xc1c2c3c4;
   require_canonical<psio::pssz>(narrow_bytes, narrow);

   MvMedium medium{1, "abc", 2};
   auto     medium_bytes = psio::encode(psio::pssz{}, medium);
   psio::mutable_view<MvMedium, psio::pssz> medium_view{medium_bytes};
   REQUIRE(medium_view.get<0>().set(0xa1b2));
   REQUIRE(medium_view.get<2>().set(0xc1c2c3c4));
   medium.count = 0xa1b2;
   medium.total = 0xc1c2c3c4;
   require_canonical<psio::pssz>(medium_bytes, medium);

   // A vector of narrow records inside a wide one: each level uses its own.
   std::vector<MvNarrow> list{{1, "x", 2}, {3, "yz", 4}};
   auto                  list_bytes = psio::encode(psio::pssz{}, list);
   psio::mutable_view<std::vector<MvNarrow>, psio::pssz> list_view{list_bytes};
   REQUIRE(list_view.size() == 2);
   REQUIRE(list_view[1].get<2>().set(400));
   list[1].total = 400;
   require_canonical<psio::pssz>(list_bytes, list);
}

TEMPLATE_TEST_CASE("mutable_view at the top level: scalar, containers, tuple", "[mutable_view]",
                   psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz)
{
   using F = TestType;

   auto scalar = psio::encode(F{}, std::uint32_t{1});
   REQUIRE(psio::mutable_view<std::uint32_t, F>{scalar}.set(0xdeadbeef));
   require_canonical<F>(scalar, std::uint32_t{0xdeadbeef});

   auto point = psio::encode(F{}, MvPoint{1, 2});
   REQUIRE(psio::mutable_view<MvPoint, F>{point}.set(MvPoint{3, 4}));
   require_canonical<F>(point, MvPoint{3, 4});

   std::vector<std::uint16_t> values{1, 2, 3};
   auto                       vector = psio::encode(F{}, values);
   REQUIRE(psio::mutable_view<std::vector<std::uint16_t>, F>{vector}[2].set(300));
   values[2] = 300;
   require_canonical<F>(vector, values);

   std::optional<std::uint32_t> some{5};
   auto                         optional = psio::encode(F{}, some);
   psio::mutable_view<std::optional<std::uint32_t>, F> optional_view{optional};
   REQUIRE((*optional_view).set(6));
   some = 6;
   require_canonical<F>(optional, some);

   auto none = psio::encode(F{}, std::optional<std::uint32_t>{});
   REQUIRE_FALSE((*psio::mutable_view<std::optional<std::uint32_t>, F>{none}).set(6));
   REQUIRE(none.empty());

   // A selector-carrying optional (the payload can encode to zero bytes).
   std::optional<std::vector<std::uint32_t>> list{std::vector<std::uint32_t>{1, 2}};
   auto                                      list_bytes = psio::encode(F{}, list);
   psio::mutable_view<std::optional<std::vector<std::uint32_t>>, F> list_view{list_bytes};
   REQUIRE(list_view.has_value());
   REQUIRE((*list_view)[1].set(22));
   (*list)[1] = 22;
   require_canonical<F>(list_bytes, list);

   using Arguments = std::tuple<std::uint32_t, std::string, bool>;
   Arguments arguments{7, "hi", false};
   auto      tuple = psio::encode(F{}, arguments);
   psio::mutable_view<Arguments, F> tuple_view{tuple};
   REQUIRE(tuple_view.template get<0>().set(8));
   REQUIRE(tuple_view.template get<2>().set(true));
   arguments = Arguments{8, "hi", true};
   require_canonical<F>(tuple, arguments);
}

TEMPLATE_TEST_CASE("mutable_view of a frame written by a newer schema", "[mutable_view]",
                   psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz)
{
   using F = TestType;
   MvNew newer{1, "name", "extra"};
   auto  bytes = psio::encode(F{}, newer);

   std::optional<psio::mutable_view<MvOld, F>> view;
   bool                                        unknown = false;
   REQUIRE(psio::validated_pssz_mutable_view<MvOld>(F{}, std::span<char>{bytes}, view, unknown)
              .ok());
   REQUIRE(unknown);
   REQUIRE(view->template get<0>().set(2));

   // The writer's frame, unknown member included, is still canonical.
   newer.id = 2;
   require_canonical<F>(bytes, newer);

   // A frame that fails validation yields no view.
   std::vector<char> broken(bytes.begin(), bytes.begin() + 3);
   view.reset();
   REQUIRE_FALSE(
      psio::validated_pssz_mutable_view<MvOld>(F{}, std::span<char>{broken}, view, unknown).ok());
   REQUIRE_FALSE(view.has_value());
}

TEST_CASE("mutable_view works over every kind of caller-owned mutable storage",
          "[mutable_view]")
{
   using F = psio::pssz;
   MvInner expected{1, "name", {2, 3}, false};

   SECTION("std::vector<char>&")
   {
      auto bytes = psio::encode(F{}, expected);
      psio::mutable_view<MvInner, F> mv{bytes};
      REQUIRE(psio::bytes(mv).data() == bytes.data());
      REQUIRE(psio::size(mv) == bytes.size());
      STATIC_REQUIRE(std::is_same_v<decltype(psio::format_of(mv)), F>);
      REQUIRE(mv.get<0>().set(5));
      expected.id = 5;
      require_canonical<F>(bytes, expected);
   }
   SECTION("std::span<char> over foreign memory")
   {
      const auto encoded = psio::encode(F{}, expected);
      char       storage[64];
      REQUIRE(encoded.size() <= sizeof(storage));
      std::copy(encoded.begin(), encoded.end(), storage);
      psio::mutable_view<MvInner, F> mv{std::span<char>{storage, encoded.size()}};
      REQUIRE(mv.get<3>().set(true));
      expected.live = true;
      require_canonical<F>(std::vector<char>(storage, storage + encoded.size()), expected);
   }
   SECTION("psio::buffer, owning and mutably borrowing")
   {
      psio::buffer<MvInner, F> owned{psio::encode(F{}, expected)};
      auto                     mv = psio::as_mutable_view(owned);
      REQUIRE(mv.get<2>().get<0>().set(-2));
      expected.pos.x = -2;
      const auto owned_bytes = psio::bytes(owned);
      require_canonical<F>(std::vector<char>(owned_bytes.begin(), owned_bytes.end()), expected);

      auto bytes = psio::encode(F{}, expected);
      psio::buffer<MvInner, F, psio::storage::mut_borrow> borrowed{std::span<char>{bytes}};
      REQUIRE(psio::as_mutable_view(borrowed).get<0>().set(77));
      expected.id = 77;
      require_canonical<F>(bytes, expected);
   }
   SECTION("the read view sees the write")
   {
      auto bytes = psio::encode(F{}, expected);
      psio::mutable_view<MvInner, F> mv{bytes};
      REQUIRE(mv.get<0>().set(123456));
      REQUIRE(psio::as_view(mv).get<0>() == 123456u);
      REQUIRE(psio::as_view(mv).get<1>().view_() == "name");
   }
}

TEST_CASE("mutable_view::assignable is the compile-time rule for set", "[mutable_view]")
{
   using F = psio::pssz;
   STATIC_REQUIRE(psio::mutable_view<std::uint32_t, F>::assignable);
   STATIC_REQUIRE(psio::mutable_view<bool, F>::assignable);
   STATIC_REQUIRE(psio::mutable_view<MvColor, F>::assignable);
   STATIC_REQUIRE(psio::mutable_view<std::chrono::seconds, F>::assignable);
   STATIC_REQUIRE(psio::mutable_view<psio::uint256, F>::assignable);
   STATIC_REQUIRE(psio::mutable_view<psio::bitvector<9>, F>::assignable);
   STATIC_REQUIRE((psio::mutable_view<std::array<MvPoint, 2>, F>::assignable));
   STATIC_REQUIRE(psio::mutable_view<MvPoint, F>::assignable);
   STATIC_REQUIRE(psio::mutable_view<MvMixed, F>::assignable);

   STATIC_REQUIRE_FALSE(psio::mutable_view<std::string, F>::assignable);
   STATIC_REQUIRE_FALSE(psio::mutable_view<std::vector<std::uint32_t>, F>::assignable);
   STATIC_REQUIRE_FALSE(psio::mutable_view<std::optional<std::uint32_t>, F>::assignable);
   STATIC_REQUIRE_FALSE((psio::mutable_view<std::variant<std::uint32_t, bool>, F>::assignable));
   STATIC_REQUIRE_FALSE(psio::mutable_view<MvInner, F>::assignable);
   STATIC_REQUIRE_FALSE(psio::mutable_view<MvScalars, F>::assignable);
   STATIC_REQUIRE_FALSE(psio::mutable_view<psio::bitlist<8>, F>::assignable);
   STATIC_REQUIRE_FALSE((psio::mutable_view<std::array<std::string, 2>, F>::assignable));
}
