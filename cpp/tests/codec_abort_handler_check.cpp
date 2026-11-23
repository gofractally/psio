// The abort handler (psio/error.hpp), with exceptions and RTTI disabled —
// how a WebAssembly guest is built. A fail-fast operation that cannot throw
// hands the error to the embedder's handler before std::abort().
//
// Built twice: with the run-time handler (psio::set_codec_abort_handler) and
// with the build-time one (-DPSIO_CODEC_ABORT_HANDLER=psio_test_abort).
// Usage: <program> <case> [nohandler]; the handler prints
// "psio-abort: <what> [<format>]" and exits with status 7. With "nohandler"
// (run-time build only) nothing is installed and the program must abort.

#include <psio/bin.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/stream.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

struct Rec
{
   std::uint32_t id = 0;
   std::string   name;
};
PSIO_REFLECT(Rec, id, name)

struct Capped
{
   std::string text;
};
PSIO_REFLECT(Capped, text, maxDynamicData(8))

namespace
{
   void report(std::string_view what, std::string_view format)
   {
      std::printf("psio-abort: %.*s [%.*s]", static_cast<int>(what.size()), what.data(),
                  static_cast<int>(format.size()), format.data());
      std::fflush(stdout);
      std::_Exit(7);
   }
}  // namespace

#if defined(PSIO_CODEC_ABORT_HANDLER)
extern "C" void psio_test_abort(const char* what, std::size_t what_size, std::uint32_t,
                                const char* format, std::size_t format_size)
{
   report({what, what_size}, {format, format_size});
}
#else
static void on_abort(const psio::codec_error& error) noexcept
{
   report(error.what, error.format_name);
}
// A handler may be installed from a static initializer: the slot is
// constant-initialized, so there is no order to get wrong.
static const bool installed_early = [] {
   psio::set_codec_abort_handler(on_abort);
   return true;
}();
#endif

int main(int argc, char** argv)
{
   if (argc < 2)
      return 2;
   const std::string_view which = argv[1];
#if !defined(PSIO_CODEC_ABORT_HANDLER)
   if (argc > 2 && std::strcmp(argv[2], "nohandler") == 0)
   {
      if (psio::set_codec_abort_handler(nullptr) != on_abort)
         return 3;
   }
   else if (!installed_early || psio::set_codec_abort_handler(on_abort) != on_abort)
      return 3;
#endif
   const std::vector<char> junk(3, 'x');
   if (which == "pssz")   // the checked decode of bytes the validator rejects
      return static_cast<int>(psio::from_pssz<Rec>(junk).id);
   if (which == "json")
      return static_cast<int>(psio::from_json<Rec>(R"({"id":"no"})").id);
   if (which == "key")
      return static_cast<int>(psio::from_key<std::uint32_t>(junk));
   if (which == "encode")   // a value with no encoding
      return static_cast<int>(psio::to_pssz(Capped{"far too long for the cap"}).size());
   if (which == "overrun")   // a bounds-checked sink that is too small
   {
      char                   small[2];
      psio::fixed_buf_stream sink{small, sizeof small};
      psio::to_pssz(Rec{1, "abc"}, sink);
      return 0;
   }
   if (which == "cursor")   // reading past the end of an input_stream
   {
      psio::input_stream in{junk};
      return static_cast<int>(in.get<std::uint64_t>());
   }
   if (which == "absent")   // value() of an absent optional view
      return static_cast<int>(psio::pssz_view<std::optional<std::uint32_t>>{}.value().get());
   return 2;
}
