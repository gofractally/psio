// Borrowing decode (pssz_borrow_cases.hpp); pssz_borrow_noexceptions.cpp
// runs the same cases with exceptions and RTTI disabled.

#include <catch.hpp>

#include "pssz_borrow_cases.hpp"

TEST_CASE("from_pssz into a record that borrows from the bytes", "[pssz][borrow]")
{
   REQUIRE(borrow_cases::run() == 0);
}

TEST_CASE("a borrowing decode of invalid bytes fails like any checked decode", "[pssz][borrow]")
{
   const std::vector<char> junk{char(0xff), char(0xff)};
   REQUIRE_THROWS_AS(
      psio::from_pssz<borrow_cases::BorrowedRequest>(std::span<const char>{junk}),
      psio::codec_exception);
}
