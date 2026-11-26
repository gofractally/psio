// A message computed at run time (codec_fail_owned): see owned_error_cases.hpp
// for the status-returning entry points; here, the fail-fast ones.
#include "owned_error_cases.hpp"

#include <catch.hpp>

#include <optional>

TEST_CASE("owned error message: every checked entry point", "[error][invariant]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = owned_error_cases::run());
   CHECK(failed == 0);
}

#if defined(__cpp_exceptions) && PSIO_EXCEPTIONS_ENABLED
TEST_CASE("owned error message: the exception carries it", "[error][invariant]")
{
   using namespace owned_error_cases;
   const Token bad{1, {9}, {}};
   std::optional<psio::codec_exception> kept;
   try
   {
      (void)psio::to_pssz_checked(bad);
      FAIL("the validated write accepted a value that violates its invariant");
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(std::string_view{e.what()} == "Value 9 outside allowed Precision range");
      CHECK(e.error().what == "Value 9 outside allowed Precision range");
      CHECK(e.error().format_name == "Precision");
      kept.emplace(e);   // a copy that outlives the original
   }
   REQUIRE(kept);
   CHECK(kept->error().what == "Value 9 outside allowed Precision range");
   CHECK(std::string_view{kept->what()} == "Value 9 outside allowed Precision range");

   // or_throw of a status.
   auto status = psio::check_invariant(Precision{77});
   try
   {
      status.or_throw();
      FAIL("or_throw did not throw");
   }
   catch (const psio::codec_exception& e)
   {
      status = psio::codec_ok();   // the status is gone; the exception owns the text
      CHECK(e.error().what == "Value 77 outside allowed Precision range");
   }
}
#endif
