#include <psio/stream.hpp>

#include <catch.hpp>

#include <array>
#include <limits>

TEST_CASE("fixed stream writes and backward skips preserve bytes", "[stream]")
{
   std::array<char, 4> bytes{};
   psio::fixed_buf_stream stream(bytes.data(), bytes.size());
   stream.write('a');
   stream.write("bcd", 3);
   REQUIRE(bytes == std::array<char, 4>{'a', 'b', 'c', 'd'});
   REQUIRE(stream.remaining() == 0);
   stream.skip(-2);
   REQUIRE(stream.consumed() == 2);
   stream.write("ef", 2);
   REQUIRE(bytes == std::array<char, 4>{'a', 'b', 'e', 'f'});
   stream.skip(-4);
   REQUIRE(stream.written() == 0);
   stream.skip(4);
   REQUIRE(stream.written() == 4);
}

#if defined(__cpp_exceptions) && defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
TEST_CASE("fixed stream retains native overrun exceptions without mutation", "[stream][exceptions]")
{
   std::array<char, 4> bytes{'a', 'b', 'c', 'd'};
   const auto original = bytes;
   psio::fixed_buf_stream stream(bytes.data(), bytes.size());
   const auto check_error = [&](auto operation)
   {
      const auto consumed = stream.consumed();
      try
      {
         operation();
         FAIL("overrun must throw");
      }
      catch (const psio::codec_exception& e)
      {
         REQUIRE(e.error().what == "stream overrun");
         REQUIRE(e.error().byte_offset == 0);
         REQUIRE(e.error().format_name == "fixed_buf_stream");
      }
      REQUIRE(stream.consumed() == consumed);
      REQUIRE(bytes == original);
   };
   check_error([&] { stream.write("x", std::numeric_limits<std::size_t>::max()); });
   check_error([&] { stream.skip(std::numeric_limits<std::int32_t>::min()); });
   check_error([&] { stream.skip(std::numeric_limits<std::int32_t>::max()); });
   stream.skip(4);
   check_error([&] { stream.write('x'); });
   check_error([&] { stream.write("x", 1); });
   check_error([&] { stream.skip(1); });
   stream.skip(-4);
   check_error([&] { stream.skip(-1); });
}
#endif
