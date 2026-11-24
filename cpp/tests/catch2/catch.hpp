// What PSIO's tests include as <catch.hpp>: the vendored Catch2 single
// header (cpp/external/catch2/catch.hpp, unmodified but for the one change
// noted there), and one accommodation for a compiler / library pair.
#ifndef PSIO_TESTS_CATCH_HPP_INCLUDED
#define PSIO_TESTS_CATCH_HPP_INCLUDED

#include "../../external/catch2/catch.hpp"

// clang up to 20 with libstdc++ 15: `REQUIRE(a == b)` with a std::tuple a
// (or `REQUIRE(a < t)` with a std::tuple t) does not compile.
//
// Catch decomposes the assertion as `Catch::Decomposer() <= a`, and the
// right-hand side of the comparison as `Catch::ExprLhs<A>{a} < t`. For each
// the compiler also considers the rewritten candidate `tuple <=> X`, which
// finds libstdc++'s tuple-like operator<=>; its return type names
// std::tuple_size<X> in a default template argument, and clang before 21
// substitutes that before it checks the constraint that would have
// discarded the candidate (CWG 2369). X is no tuple, std::tuple_size<X> is
// incomplete, and the result is a hard error in <bits/utility.h>, not a
// discarded candidate. GCC, and clang from 22 on, compile it.
//
// Giving the two Catch types a (zero) tuple size lets the default argument
// be formed; the constraint then discards the candidate as intended.
// Neither type is tuple-like to the library (std::__tuple_like names the
// standard's own types only), so nothing else changes.
#if defined(__GLIBCXX__) && defined(__clang__) && __clang_major__ < 22
#include <cstddef>
#include <tuple>
#include <type_traits>
template <>
struct std::tuple_size<Catch::Decomposer> : std::integral_constant<std::size_t, 0>
{
};
template <typename T>
struct std::tuple_size<Catch::ExprLhs<T>> : std::integral_constant<std::size_t, 0>
{
};
#endif

#endif  // PSIO_TESTS_CATCH_HPP_INCLUDED
