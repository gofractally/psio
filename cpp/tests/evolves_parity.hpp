#pragma once
// The run-time schema comparison (psio/pssz_ir_compare.hpp) against the
// compile-time psio::pssz_evolves_from<Old, New>: for a pair of C++ types,
// evolves_from over the two Schema IRs SchemaBuilder makes of them gives the
// verdict the compile-time check gives.
//
//    PSIO_RUNTIME_EVOLVES_PARITY(Old, New);
//
#include <catch.hpp>

#include <psio/pssz.hpp>
#include <psio/pssz_ir_compare.hpp>
#include <psio/schema_builder.hpp>

namespace psio_test
{
   template <typename Old, typename New>
   psio::pssz_ir::compatibility runtime_evolves(
      psio::pssz_ir::compare what = psio::pssz_ir::compare::wire)
   {
      psio::SchemaBuilder a, b;
      const psio::AnyType ta = a.insert<Old>(), tb = b.insert<New>();
      const psio::Schema  sa = std::move(a).build(), sb = std::move(b).build();
      return psio::pssz_ir::evolves_from(sa, ta, sb, tb, what);
   }
   template <typename A, typename B>
   psio::pssz_ir::compatibility runtime_equivalent(
      psio::pssz_ir::compare what = psio::pssz_ir::compare::contract)
   {
      psio::SchemaBuilder a, b;
      const psio::AnyType ta = a.insert<A>(), tb = b.insert<B>();
      const psio::Schema  sa = std::move(a).build(), sb = std::move(b).build();
      return psio::pssz_ir::equivalent(sa, ta, sb, tb, what);
   }
}  // namespace psio_test

#define PSIO_RUNTIME_EVOLVES_PARITY(...)                                                  \
   do                                                                                     \
   {                                                                                      \
      const auto psio_parity_ = psio_test::runtime_evolves<__VA_ARGS__>();                \
      INFO(#__VA_ARGS__ << ": " << psio_parity_.code << " at " << psio_parity_.path       \
                        << ": " << psio_parity_.reason);                                  \
      CHECK(psio_parity_.ok == psio::pssz_evolves_from_v<__VA_ARGS__>);                   \
   } while (false)
