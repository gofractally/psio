// A member's declared length_bound is enforced by the pssz status validator,
// and therefore by validate, validate_strict and the checked readers built on
// it (decode_pssz_checked, validated_pssz_view), under every width. The encoder and the unchecked
// decode do not check it.

#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lb_test
{
   struct LbItem
   {
      std::uint32_t id;
      std::string   name;
   };
   struct LbRec
   {
      std::string                             s;      // max 4 bytes
      std::vector<std::uint16_t>              v;      // max 2 elements
      std::vector<LbItem>                     items;  // max 2 variable-size elements
      std::optional<std::string>              os;     // max 3 bytes when present
      std::optional<std::vector<std::string>> ov;     // max 1 element when present
      std::string                             exact;  // exactly 2 bytes
      std::vector<std::uint8_t>               atleast;  // at least 1 element
   };
   PSIO_REFLECT(LbItem, id, name)
   PSIO_REFLECT(LbRec, s, v, items, os, ov, exact, atleast)
}  // namespace lb_test
using lb_test::LbRec;
PSIO_FIELD_ATTRS(lb_test::LbRec, s, psio::length_bound{.max = 4})
PSIO_FIELD_ATTRS(lb_test::LbRec, v, psio::length_bound{.max = 2})
PSIO_FIELD_ATTRS(lb_test::LbRec, items, psio::length_bound{.max = 2})
PSIO_FIELD_ATTRS(lb_test::LbRec, os, psio::length_bound{.max = 3})
PSIO_FIELD_ATTRS(lb_test::LbRec, ov, psio::length_bound{.max = 1})
PSIO_FIELD_ATTRS(lb_test::LbRec, exact, psio::length_bound{.exact = 2})
PSIO_FIELD_ATTRS(lb_test::LbRec, atleast, psio::length_bound{.min = 1})

namespace
{
   LbRec good()
   {
      return LbRec{"1234", {1, 2}, {{1, "a"}, {2, "bb"}}, std::string("abc"),
                   std::vector<std::string>{"x"}, "ok", {9}};
   }

   template <typename Fmt>
   bool accepted(const LbRec& value)
   {
      const auto bytes = psio::encode(Fmt{}, value);
      const bool ok    = psio::validate<LbRec>(Fmt{}, bytes).ok();
      // Every checked entry point agrees with validate.
      CHECK(psio::validate_strict<LbRec>(Fmt{}, bytes).ok() == ok);
      LbRec out;
      bool  unknown = false;
      CHECK(psio::decode_pssz_checked(Fmt{}, bytes, out, unknown).ok() == ok);
      return ok;
   }

   template <typename Fmt>
   void check_format()
   {
      CHECK(accepted<Fmt>(good()));
      {
         auto r = good();
         r.os.reset();
         r.ov.reset();
         r.s.clear();
         r.v.clear();
         r.items.clear();
         CHECK(accepted<Fmt>(r));
      }
      auto bad = [](auto&& edit) {
         auto r = good();
         edit(r);
         return !accepted<Fmt>(r);
      };
      CHECK(bad([](LbRec& r) { r.s = "12345"; }));
      CHECK(bad([](LbRec& r) { r.v = {1, 2, 3}; }));
      CHECK(bad([](LbRec& r) { r.items.push_back({3, "c"}); }));
      CHECK(bad([](LbRec& r) { r.os = "abcd"; }));
      CHECK(bad([](LbRec& r) { r.ov = std::vector<std::string>{"x", "y"}; }));
      CHECK(bad([](LbRec& r) { r.exact = "o"; }));
      CHECK(bad([](LbRec& r) { r.exact = "okk"; }));
      CHECK(bad([](LbRec& r) { r.atleast.clear(); }));
   }
}  // namespace

TEST_CASE("pssz validate enforces member length_bound", "[pssz][length_bound]")
{
   check_format<psio::pssz8>();
   check_format<psio::pssz16>();
   check_format<psio::pssz32>();
   check_format<psio::pssz>();
}

TEST_CASE("pssz length_bound violation reports its own error", "[pssz][length_bound]")
{
   auto r = good();
   r.s    = "12345";
   const auto bytes = psio::encode(psio::pssz32{}, r);
   const auto st    = psio::validate<LbRec>(psio::pssz32{}, bytes);
   REQUIRE(!st.ok());
   CHECK(st.error().what == "pssz: member length_bound violated");
   // The unchecked decode is the fail-fast path and does not check bounds.
   CHECK(psio::decode<LbRec>(psio::pssz32{}, bytes).s == "12345");
}
