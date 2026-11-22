#pragma once
// Record lineages for the PSSZ cross-version (field evolution) matrix.
//
// The owner's rule: any field added after a type is first published must be
// optional<T>. It therefore owns an offset slot, and old and new readers
// discern what was written from the record's fixed-region header.
//
// Every record below carries an unbounded string, so the automatic profile
// selects four-byte offsets for each revision. A compatible lineage must keep
// its width: appending a field can never be allowed to change it.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace evo
{
   // ── Base lineage: one, two and three appended optional fields ─────────
   struct R0
   {
      std::uint32_t a;
      std::string   s;
      friend bool   operator==(const R0&, const R0&) = default;
   };
   PSIO_REFLECT(R0, a, s)

   struct R1  // + optional of a fixed type
   {
      std::uint32_t                a;
      std::string                  s;
      std::optional<std::uint32_t> x;
      friend bool                  operator==(const R1&, const R1&) = default;
   };
   PSIO_REFLECT(R1, a, s, x)

   struct R2  // + optional of a variable type (string)
   {
      std::uint32_t                a;
      std::string                  s;
      std::optional<std::uint32_t> x;
      std::optional<std::string>   y;
      friend bool                  operator==(const R2&, const R2&) = default;
   };
   PSIO_REFLECT(R2, a, s, x, y)

   struct R3  // + optional of a variable type (vector)
   {
      std::uint32_t                             a;
      std::string                               s;
      std::optional<std::uint32_t>              x;
      std::optional<std::string>                y;
      std::optional<std::vector<std::uint16_t>> z;
      friend bool                               operator==(const R3&, const R3&) = default;
   };
   PSIO_REFLECT(R3, a, s, x, y, z)

   // ── Appended optional of a nested record ──────────────────────────────
   struct Inner
   {
      std::uint8_t k;
      std::string  t;
      friend bool  operator==(const Inner&, const Inner&) = default;
   };
   PSIO_REFLECT(Inner, k, t)

   struct N1
   {
      std::uint32_t        a;
      std::string          s;
      std::optional<Inner> in;
      friend bool          operator==(const N1&, const N1&) = default;
   };
   PSIO_REFLECT(N1, a, s, in)

   // ── Appended optional<optional<T>> ────────────────────────────────────
   struct P1
   {
      std::uint32_t                               a;
      std::string                                 s;
      std::optional<std::optional<std::uint32_t>> oo;
      friend bool                                 operator==(const P1&, const P1&) = default;
   };
   PSIO_REFLECT(P1, a, s, oo)

   // ── A non-optional appended field: NOT a compatible revision ──────────
   struct Bad1
   {
      std::uint32_t a;
      std::string   s;
      std::uint32_t b;
      friend bool   operator==(const Bad1&, const Bad1&) = default;
   };
   PSIO_REFLECT(Bad1, a, s, b)

   // ── The evolving record as a non-last field of a parent ───────────────
   // The parent must delimit `child` by the next offset (`tail`), not by the
   // end of its own frame.
   struct Parent0
   {
      R0          child;
      std::string tail;
      friend bool operator==(const Parent0&, const Parent0&) = default;
   };
   struct Parent2
   {
      R2          child;
      std::string tail;
      friend bool operator==(const Parent2&, const Parent2&) = default;
   };
   PSIO_REFLECT(Parent0, child, tail)
   PSIO_REFLECT(Parent2, child, tail)

   // Projections between revisions: what an older reader keeps, and how a
   // newer reader sees older data (every appended field absent).
   inline R0 old_of(const R1& v) { return {v.a, v.s}; }
   inline R0 old_of(const R2& v) { return {v.a, v.s}; }
   inline R0 old_of(const R3& v) { return {v.a, v.s}; }
   inline R0 old_of(const N1& v) { return {v.a, v.s}; }
   inline R0 old_of(const P1& v) { return {v.a, v.s}; }
   inline R1 mid_of(const R2& v) { return {v.a, v.s, v.x}; }
   inline R1 mid_of(const R3& v) { return {v.a, v.s, v.x}; }
   inline R2 mid2_of(const R3& v) { return {v.a, v.s, v.x, v.y}; }

   using OO = std::optional<std::optional<std::uint32_t>>;
   using VecNew = std::vector<R2>;
   using VecOld = std::vector<R0>;
   using OptNew = std::optional<R2>;
   using OptOld = std::optional<R0>;
   using VarNew = std::variant<std::uint32_t, R2>;
   using VarOld = std::variant<std::uint32_t, R0>;

   // The named corpus. Each entry's canonical bytes are pinned per width in
   // pssz_evolution_goldens.hpp; the visitor receives (name, value).
   template <typename Visit>
   void for_each_vector(Visit&& visit)
   {
      visit("r0", R0{7, "hi"});
      visit("r1_none", R1{7, "hi", std::nullopt});
      visit("r1_some", R1{7, "hi", 9});
      visit("r2_none_none", R2{7, "hi", std::nullopt, std::nullopt});
      visit("r2_some_none", R2{7, "hi", 9, std::nullopt});
      visit("r2_none_some", R2{7, "hi", std::nullopt, std::string("yo")});
      visit("r2_none_empty", R2{7, "hi", std::nullopt, std::string()});
      visit("r3_none", R3{7, "hi", std::nullopt, std::nullopt, std::nullopt});
      visit("r3_all", R3{7, "hi", 9, std::string("yo"), std::vector<std::uint16_t>{1, 2}});
      visit("r3_mid", R3{7, "hi", std::nullopt, std::string("yo"), std::nullopt});
      visit("r3_empty_vec",
            R3{7, "hi", std::nullopt, std::nullopt, std::vector<std::uint16_t>{}});
      visit("n1_none", N1{7, "hi", std::nullopt});
      visit("n1_some", N1{7, "hi", Inner{3, "t"}});
      visit("p1_none", P1{7, "hi", std::nullopt});
      visit("p1_some_none", P1{7, "hi", OO{std::in_place, std::nullopt}});
      visit("p1_some_some", P1{7, "hi", OO{std::in_place, 5}});
      visit("vec_new", VecNew{{1, "a", 9, std::string("b")}, {2, "c", std::nullopt, std::nullopt}});
      visit("opt_new", OptNew{R2{1, "a", 9, std::string("b")}});
      visit("var_new", VarNew{R2{1, "a", 9, std::string("b")}});
      visit("parent_new", Parent2{{1, "a", 9, std::string("b")}, "tail"});
   }
}  // namespace evo
