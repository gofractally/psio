// Standalone native/WASI consumer. Compile with PSIO_EXCEPTIONS_ENABLED=0
// and -fno-exceptions; selectors 1..4 intentionally exercise stream aborts.
#include <psio/stream.hpp>

#include <array>
#include <cstdio>
#include <limits>

int main(int argc, char** argv)
{
   std::array<char, 4> buffer{};
   psio::fixed_buf_stream stream(buffer.data(), buffer.size());
   if (argc > 1)
   {
      switch (argv[1][0])
      {
         case '1': stream.skip(4); stream.write('x'); break;
         case '2': stream.write("x", std::numeric_limits<std::size_t>::max()); break;
         case '3': stream.skip(std::numeric_limits<std::int32_t>::max()); break;
         case '4': stream.skip(std::numeric_limits<std::int32_t>::min()); break;
         default: return 2;
      }
      return 3; // Invalid unchecked writes must not return.
   }
   stream.write('a'); stream.write("bcd", 3); stream.skip(-2); stream.write("ef", 2);
   if (buffer != std::array<char, 4>{'a', 'b', 'e', 'f'}) return 4;

   if (!psio::codec_ok().ok()) return 5;
   auto status = psio::codec_fail("expected status", 7, "stream-check");
   if (status.ok() || status.error().format_name != "stream-check" ||
       status.error().byte_offset != 7 || status.error().what != "expected status") return 6;
   for (const unsigned char byte : buffer) std::printf("%02x", byte);
   std::puts("\nstream-and-status=PASS");
}
