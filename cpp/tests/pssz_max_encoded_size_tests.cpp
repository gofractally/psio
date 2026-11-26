// The two numbers of a type (docs/pssz-spec.md 3.2 and 3.7):
//
//   psio::inferred_bound<T>()     what an offset width is chosen from. It
//                                 never reads T's own maxDynamicData.
//   psio::max_encoded_size<T>()   an upper bound of every psio::pssz
//                                 encoding of T: every header, slot and
//                                 table at its real width, and min(cap, sum)
//                                 for a capped type.
//
// The cases here sit on the boundary: a value that is exactly the reported
// maximum, and the next larger value, which the encoder refuses.

#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace max_tests
{
   using u32 = std::uint32_t;
   using u64 = std::uint64_t;
   using std::optional;

   template <typename T>
   constexpr optional<std::size_t> inferred = psio::inferred_bound<T>();
   template <typename T>
   constexpr optional<std::size_t> largest = psio::max_encoded_size<T>();
   template <typename T>
   constexpr std::size_t width = psio::auto_pssz_width_v<T>;

   template <typename T>
   std::size_t size(const T& value)
   {
      const auto bytes = psio::encode(psio::pssz{}, value);
      // What was written is a value of T, and never above the maximum.
      REQUIRE(psio::validate_pssz<T>(std::span<const char>{bytes.data(), bytes.size()}).ok());
      if (largest<T>) REQUIRE(bytes.size() <= *largest<T>);
      return bytes.size();
   }
   template <typename T>
   bool refused(const T& value)
   {
      try
      {
         (void)psio::to_pssz_checked(value);   // the validated write
         return false;
      }
      catch (const psio::codec_exception&)
      {
         return true;
      }
   }
   std::string s(std::size_t n) { return std::string(n, 'x'); }

   // No cap: the two numbers are one.
   struct Open
   {
      u64         id = 0;
      std::string memo;
      std::string note;
      PSIO_REFLECT(Open, id, attr(memo, max<80>), attr(note, max<16>))
   };
   static_assert(inferred<Open> == 3 + 8 + 80 + 16 && width<Open> == 1);
   static_assert(largest<Open> == inferred<Open>);

   // A cap in a wider class than the inferred bound: the header and the two
   // slots are 2 bytes each, which the inferred bound (counted 1 wide)
   // does not hold.
   struct Wider
   {
      u64         id = 0;
      std::string memo;
      std::string note;
      PSIO_REFLECT(Wider, id, attr(memo, max<80>), attr(note, max<16>), maxDynamicData(1024))
   };
   static_assert(inferred<Wider> == 107 && width<Wider> == 2);
   static_assert(largest<Wider> == 2 * 3 + 8 + 80 + 16);   // 110 > 107
   static_assert(psio::effective_max_dynamic_v<Wider> == 1024);

   // A cap below what the members could add up to: the cap is the maximum.
   struct Tight
   {
      u64         id = 0;
      std::string memo;
      std::string note;
      PSIO_REFLECT(Tight, id, attr(memo, max<80>), attr(note, max<16>), maxDynamicData(100))
   };
   static_assert(inferred<Tight> == 107 && width<Tight> == 1);
   static_assert(largest<Tight> == 100);

   // An unbounded member: no inferred bound, and the cap is the maximum.
   struct Loose
   {
      std::string text;
      PSIO_REFLECT(Loose, text, maxDynamicData(300))
   };
   static_assert(!inferred<Loose>.has_value() && width<Loose> == 2);
   static_assert(largest<Loose> == 300);
   // Unbounded and no cap: there is no maximum.
   struct Free
   {
      std::string text;
      PSIO_REFLECT(Free, text)
   };
   static_assert(!inferred<Free>.has_value() && !largest<Free>.has_value());

   // A capped member. What holds it reserves the member's cap in full when
   // it chooses its width (the inferred bound), and counts the member's own
   // maximum in its maximum encoded size.
   struct Child
   {
      std::string name;
      PSIO_REFLECT(Child, attr(name, max<8>), maxDynamicData(300))
   };
   static_assert(inferred<Child> == 1 + 1 + 8 && width<Child> == 2);
   static_assert(largest<Child> == 2 + 2 + 8);
   struct Holder
   {
      Child child;
      u32   n = 0;
      PSIO_REFLECT(Holder, child, n)
   };
   static_assert(inferred<Holder> == 2 * 2 + 300 + 4 && width<Holder> == 2);
   static_assert(largest<Holder> == 2 * 2 + 12 + 4);
   // Through an optional, a list, a variant and a tuple.
   static_assert(largest<optional<Child>> == 1 + 12);
   static_assert(largest<psio::bounded<Child, 3>> == 3 * 2 + 3 * 12);   // table as wide as 906 needs
   static_assert(inferred<psio::bounded<Child, 3>> == 3 * 2 + 3 * 300);
   static_assert(largest<std::variant<u32, Child>> == 1 + 12);
   static_assert(inferred<std::tuple<u32, Child>> == 2 * 2 + 4 + 300);
   static_assert(largest<std::tuple<u32, Child>> == 2 * 2 + 4 + 12);

   // The position cap (docs/pssz-spec.md 3.6). `kids` alone is 2 bytes wide
   // (its inferred bound reserves three caps of 100: 306); beneath a record
   // capped at 255 its table is 1 byte wide, and that is what is counted.
   struct Small
   {
      std::string name;
      PSIO_REFLECT(Small, attr(name, max<8>), maxDynamicData(100))
   };
   static_assert(largest<Small> == 1 + 1 + 8 && width<Small> == 1);
   struct Narrowed
   {
      std::vector<Small> kids;
      PSIO_REFLECT(Narrowed, attr(kids, max<3>), maxDynamicData(255))
   };
   static_assert(width<psio::bounded<Small, 3>> == 2);
   static_assert(inferred<Narrowed> == 2 * 2 + 3 * 2 + 3 * 100);
   static_assert(largest<Narrowed> == 2 + (3 * 1 + 3 * 10));

   // A method's argument list is a record of its own, with its own cap.
   struct Api
   {
      void transfer(u64 from, u64 to, u64 amount, std::string memo);
      void note(u64 id, std::string text);
      PSIO_REFLECT(Api,
                   method(transfer, from, to, amount, attr(memo, utf8 | max<80>),
                          maxDynamicData(1024)),
                   method(note, id, attr(text, utf8 | max<80>), maxDynamicData(64)))
   };
   using TransferArgs = psio::reflect<Api>::method_arg_tuple<0>;
   using NoteArgs     = psio::reflect<Api>::method_arg_tuple<1>;
   static_assert(inferred<TransferArgs> == 106 && width<TransferArgs> == 2);
   static_assert(largest<TransferArgs> == 2 * 2 + 24 + 80);   // 108
   static_assert(inferred<NoteArgs> == 90 && width<NoteArgs> == 1);
   static_assert(largest<NoteArgs> == 64);
}  // namespace max_tests

using namespace max_tests;

TEST_CASE("max_encoded_size: a cap in a wider class counts the wider offsets",
          "[pssz][max_encoded_size]")
{
   // The largest value is exactly the maximum, and above the inferred bound.
   CHECK(size(Wider{1, s(80), s(16)}) == 110);
   CHECK(size(Wider{1, s(80), s(16)}) > *inferred<Wider>);
   CHECK(refused(Wider{1, s(81), s(16)}));
   CHECK(refused(Wider{1, s(80), s(17)}));
   // Without the cap the same value is the inferred bound.
   CHECK(size(Open{1, s(80), s(16)}) == 107);
   CHECK(size(Open{}) <= 107);
}

TEST_CASE("max_encoded_size: a cap below the members' sum is the maximum",
          "[pssz][max_encoded_size]")
{
   CHECK(size(Tight{1, s(80), s(9)}) == 100);
   CHECK(refused(Tight{1, s(80), s(10)}));
   CHECK(size(Tight{1, s(73), s(16)}) == 100);
   CHECK(refused(Tight{1, s(74), s(16)}));
   CHECK(refused(Tight{1, s(80), s(16)}));

   CHECK(size(Loose{s(296)}) == 300);
   CHECK(refused(Loose{s(297)}));
}

TEST_CASE("max_encoded_size: a capped member counts as its own maximum",
          "[pssz][max_encoded_size]")
{
   CHECK(size(Child{s(8)}) == 12);
   CHECK(size(Holder{Child{s(8)}, 7}) == 20);
   CHECK(size(optional<Child>{Child{s(8)}}) <= 13);
   CHECK(size(psio::bounded<Child, 3>{std::vector<Child>(3, Child{s(8)})}) == 42);
   CHECK(size(std::variant<u32, Child>{Child{s(8)}}) == 13);
   CHECK(size(std::tuple<u32, Child>{1, Child{s(8)}}) == 20);
}

TEST_CASE("max_encoded_size: a list beneath a capped record is counted at its narrowed width",
          "[pssz][max_encoded_size]")
{
   CHECK(size(Narrowed{std::vector<Small>(3, Small{s(8)})}) == 35);
   CHECK(refused(Narrowed{std::vector<Small>(4, Small{s(8)})}));
}

TEST_CASE("max_encoded_size: a method's argument list", "[pssz][max_encoded_size]")
{
   CHECK(size(TransferArgs{1, 2, 3, s(80)}) == 108);
   CHECK(refused(TransferArgs{1, 2, 3, s(81)}));
   // header + slot + id + text = 10 + text; the cap of 64 ends at 54.
   CHECK(size(NoteArgs{1, s(54)}) == 64);
   CHECK(refused(NoteArgs{1, s(55)}));
}
