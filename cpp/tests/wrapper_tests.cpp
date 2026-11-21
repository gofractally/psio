// Phase 2 — rich-wrapper tests. Covers:
//   - psio::bounded<T, N> overflow enforcement
//   - psio::utf8_string<N> / byte_array<N> basic surface
//   - psio::bitvector<N> / bitlist<N>
//   - inherent_annotations exposes wrapper invariants as spec tuples
//   - effective_annotations merges inherent + explicit without conflict
//   - a conflicting spec fires a compile-time static_assert (smoke:
//     we just verify the non-conflicting path compiles)

#include <psio/annotate.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <stdexcept>
#include <tuple>

namespace {

   struct Wrapped
   {
      psio::byte_array<48>  pubkey;
      psio::utf8_string<64> name;
      psio::bounded<int, 8> scores;
   };
   PSIO_REFLECT(Wrapped, pubkey, name, scores)

}  // namespace

// Explicit annotation on `name` that does NOT conflict with the
// wrapper's inherent utf8_spec (adds field_num only).
template <>
inline constexpr auto psio::annotate<&Wrapped::name> = std::tuple{
   psio::field_num_spec{.value = 2},
};

TEST_CASE("bounded<T, N> enforces upper bound on push_back", "[wrapper]")
{
   psio::bounded<int, 3> b;
   b.push_back(1);
   b.push_back(2);
   b.push_back(3);
   REQUIRE(b.size() == 3);
   REQUIRE_THROWS_AS(b.push_back(4), std::length_error);
}

TEST_CASE("bounded<T, N> storage access and iteration", "[wrapper]")
{
   psio::bounded<int, 5> b{1, 2, 3};
   REQUIRE(b.size() == 3);
   REQUIRE(b[1] == 2);

   int sum = 0;
   for (int x : b) sum += x;
   REQUIRE(sum == 6);
}

TEST_CASE("utf8_string<N> enforces bound", "[wrapper]")
{
   REQUIRE_NOTHROW(psio::utf8_string<10>{std::string{"hello"}});
   REQUIRE_THROWS_AS(psio::utf8_string<3>{std::string{"hello"}},
                     std::length_error);
}

TEST_CASE("byte_array<N> has compile-time size", "[wrapper]")
{
   psio::byte_array<32> a;
   STATIC_REQUIRE(decltype(a)::size_value == 32);
   REQUIRE(a.size() == 32);
   a[0] = 0xFF;
   REQUIRE(a[0] == 0xFF);
}

TEST_CASE("bitvector<N> LSB-first bit layout", "[wrapper]")
{
   psio::bitvector<12> bv;
   bv.set(0, true);
   bv.set(11, true);
   REQUIRE(bv.test(0));
   REQUIRE(!bv.test(5));
   REQUIRE(bv.test(11));

   // Underlying byte 0 has bit 0 set; byte 1 has bit 3 (11 & 7) set.
   REQUIRE(bv.data()[0] == 0x01);
   REQUIRE(bv.data()[1] == 0x08);
}

TEST_CASE("bitlist<N> has variable length with compile-time max",
          "[wrapper]")
{
   psio::bitlist<8> bl;
   bl.push_back(true);
   bl.push_back(false);
   bl.push_back(true);
   REQUIRE(bl.size() == 3);
   REQUIRE(bl.test(0));
   REQUIRE(!bl.test(1));
   REQUIRE(bl.test(2));
}

TEST_CASE("inherent_annotations exposes wrapper invariants", "[wrapper]")
{
   // byte_array<48> → length_bound{.exact = 48}
   constexpr auto pk_ann =
       psio::inherent_annotations<psio::byte_array<48>>::value;
   STATIC_REQUIRE(std::tuple_size_v<decltype(pk_ann)> == 1);
   STATIC_REQUIRE(std::get<0>(pk_ann).exact.value() == 48);

   // utf8_string<64> → length_bound{.max = 64} + utf8_spec{.max = 64}
   constexpr auto name_ann =
       psio::inherent_annotations<psio::utf8_string<64>>::value;
   STATIC_REQUIRE(std::tuple_size_v<decltype(name_ann)> == 2);

   // bounded<int, 8> → length_bound{.max = 8}
   constexpr auto scores_ann =
       psio::inherent_annotations<psio::bounded<int, 8>>::value;
   STATIC_REQUIRE(std::get<0>(scores_ann).max.value() == 8);
}

TEST_CASE("effective_annotations_v (2-arg) merges wrapper + member cleanly",
          "[wrapper][eff]")
{
   // `name` has wrapper-inherent length_bound + utf8_spec AND explicit
   // field_num_spec. They don't collide on spec type, so merge succeeds.
   constexpr auto eff =
       psio::effective_annotations_v<psio::utf8_string<64>, &Wrapped::name>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(eff)> == 3);  // member + 2 inherent

   auto utf8 = psio::find_spec<psio::utf8_spec>(eff);
   REQUIRE(utf8.has_value());
   REQUIRE(utf8->max == 64);

   auto fn = psio::find_spec<psio::field_num_spec>(eff);
   REQUIRE(fn.has_value());
   REQUIRE(fn->value == 2);

   auto lb = psio::find_spec<psio::length_bound>(eff);
   REQUIRE(lb.has_value());
   REQUIRE(lb->max.value() == 64);
}

TEST_CASE("pubkey wrapper with no explicit annotations — inherent only",
          "[wrapper][eff]")
{
   constexpr auto eff =
       psio::effective_annotations_v<psio::byte_array<48>, &Wrapped::pubkey>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(eff)> == 1);
   auto lb = psio::find_spec<psio::length_bound>(eff);
   REQUIRE(lb.has_value());
   REQUIRE(lb->exact.value() == 48);
}

// ── Three-way merge: member > wrapper > type ─────────────────────────────

namespace {

   struct Triplet
   {
      psio::byte_array<48> key;
   };
   PSIO_REFLECT(Triplet, key)

}  // namespace

// Type-level field_num set to 1. Member-level overrides to 7.
template <>
inline constexpr auto psio::annotate<psio::type<Triplet>{}> = std::tuple{
   psio::field_num_spec{.value = 1},
};
template <>
inline constexpr auto psio::annotate<&Triplet::key> = std::tuple{
   psio::field_num_spec{.value = 7},
};

TEST_CASE("effective_annotations_for: member beats wrapper beats type",
          "[wrapper][eff][3way]")
{
   constexpr auto eff =
       psio::effective_annotations_for_v<Triplet, psio::byte_array<48>,
                                          &Triplet::key>;

   // Member: field_num=7
   // Wrapper: length_bound{exact=48}
   // Type:    field_num=1 — superseded by member, dropped.
   STATIC_REQUIRE(std::tuple_size_v<decltype(eff)> == 2);

   auto fn = psio::find_spec<psio::field_num_spec>(eff);
   REQUIRE(fn.has_value());
   REQUIRE(fn->value == 7);   // member wins

   auto lb = psio::find_spec<psio::length_bound>(eff);
   REQUIRE(lb.has_value());
   REQUIRE(lb->exact.value() == 48);
}
