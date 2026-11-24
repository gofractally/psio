// Where JSON text goes: the capacity of a sink, the exact size of a text,
// and a text written as a member of a packed value.
//
// Proved here, by counting the heap (this binary replaces operator new):
//
//   - a writer appending to a sink uses the capacity the sink has: text
//     that fits what the caller reserved never reallocates, and past the
//     reservation the sink grows by doubling (one buffer at a time);
//   - `json_size` / `ijson_size` are the length of the text, measured
//     without a buffer, for every escape, hex and number form;
//   - `json_sizing::exact` makes one allocation of exactly the text;
//   - `psio::ijson_of(value)` / `psio::json_of(value)` write the text
//     straight into the enclosing packed value: the bytes of the two-step
//     form, in one allocation of the result's size.

#include <catch.hpp>

#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

// ── Allocation counting ──────────────────────────────────────────────────
namespace
{
   std::size_t live = 0, peak = 0, allocations = 0, largest = 0;

   struct heap_window
   {
      std::size_t before;
      heap_window() : before(live)
      {
         peak        = live;
         allocations = 0;
         largest     = 0;
      }
      // The most that was live at once beside what was live at the start.
      [[nodiscard]] std::size_t peak_bytes() const { return peak - before; }
      [[nodiscard]] std::size_t count() const { return allocations; }
   };
}  // namespace

void* operator new(std::size_t n)
{
   auto* p = static_cast<std::size_t*>(std::malloc(n + 16));
   if (!p)
      throw std::bad_alloc{};
   *p = n;
   live += n;
   ++allocations;
   if (n > largest)
      largest = n;
   if (live > peak)
      peak = live;
   return reinterpret_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept
{
   if (!p)
      return;
   auto* q = reinterpret_cast<std::size_t*>(static_cast<char*>(p) - 16);
   live -= *q;
   std::free(q);
}
void operator delete(void* p, std::size_t) noexcept
{
   operator delete(p);
}

namespace
{
   constexpr std::size_t MiB = std::size_t{1} << 20;

   // The shape of a transaction trace: the arguments twice, as bytes.
   struct Trace
   {
      std::vector<char> outer, inner;
      std::uint32_t     time = 0;
   };
   PSIO_REFLECT(Trace, outer, inner, time)

   enum class Color : std::uint8_t { red, green };
   PSIO_REFLECT_ENUM(Color, red, green)

   struct Everything
   {
      std::string                          text;
      std::vector<std::string>             texts;
      std::vector<char>                    bytes;
      std::array<std::uint8_t, 4>          fixed{};
      std::uint64_t                        wide  = 0;
      std::int64_t                         swide = 0;
      std::int8_t                          tiny  = 0;
      double                               real  = 0;
      float                                small = 0;
      bool                                 flag  = false;
      std::optional<std::string>           maybe;
      std::variant<std::uint32_t, std::string> choice;
      std::tuple<std::uint16_t, std::string>   pair;
      std::map<std::string, std::uint32_t>     named;
      std::vector<std::uint32_t>           numbers;
      Color                                color = Color::red;
      psio::uint128                        huge  = 0;
   };
   PSIO_REFLECT(Everything, text, texts, bytes, fixed, wide, swide, tiny, real, small, flag,
                maybe, choice, pair, named, numbers, color, huge)

   // An HTTP reply, and the same holding a source of its body.
   struct Header
   {
      std::string name, value;
   };
   PSIO_REFLECT(Header, name, value)
   struct Reply
   {
      std::uint16_t       status = 200;
      std::string         contentType;
      std::vector<char>   body;
      std::vector<Header> headers;
   };
   PSIO_REFLECT(Reply, status, contentType, body, headers)
   template <typename Body>
   struct ReplyOf
   {
      std::uint16_t       status = 200;
      std::string         contentType;
      Body                body;
      std::vector<Header> headers;
      PSIO_REFLECT(ReplyOf, status, contentType, body, headers)
   };
   // The same, referring to everything it holds.
   template <typename Body>
   struct ReplyRef
   {
      std::uint16_t                                    status = 200;
      std::string_view                                 contentType;
      Body                                             body;
      std::reference_wrapper<const std::vector<Header>> headers;
      PSIO_REFLECT(ReplyRef, status, contentType, body, headers)
   };
   template <typename Body>
   ReplyRef(std::uint16_t, std::string_view, Body,
            std::reference_wrapper<const std::vector<Header>>) -> ReplyRef<Body>;

   struct Action
   {
      std::uint64_t     sender = 0, service = 0, method = 0;
      std::vector<char> rawData;
   };
   PSIO_REFLECT(Action, sender, service, method, rawData)
   template <typename Args>
   struct ActionOf
   {
      std::uint64_t sender = 0, service = 0, method = 0;
      Args          rawData;
      PSIO_REFLECT(ActionOf, sender, service, method, rawData)
   };
   template <typename Args>
   ActionOf(std::uint64_t, std::uint64_t, std::uint64_t, Args) -> ActionOf<Args>;

   std::string random_text(std::mt19937_64& rng, std::size_t max_len)
   {
      // Every class of byte the writer treats differently, at every
      // alignment of the eight-byte scan.
      static constexpr std::string_view pieces[] = {
         "a", "plain ascii text ", "\"", "\\", "\n", "\r", "\t", std::string_view{"\0", 1},
         "\x01", "\x1f", "\b", "\f", "\x7f", "é", "€", "\xF0\x9F\x98\x80", "/", "01234567",
      };
      std::string       out;
      const std::size_t n = rng() % (max_len + 1);
      while (out.size() < n)
         out += pieces[rng() % std::size(pieces)];
      return out;
   }

   Everything random_value(std::mt19937_64& rng)
   {
      Everything v;
      v.text = random_text(rng, 40);
      for (std::size_t i = rng() % 4; i--;)
         v.texts.push_back(random_text(rng, 20));
      v.bytes.resize(rng() % 40);
      for (auto& b : v.bytes)
         b = static_cast<char>(rng());
      for (auto& b : v.fixed)
         b = static_cast<std::uint8_t>(rng());
      // (both sides of 2^53, where an I-JSON integer becomes a string)
      v.wide  = rng() >> (rng() % 64);
      v.swide = static_cast<std::int64_t>(rng()) >> (rng() % 64);
      v.tiny  = static_cast<std::int8_t>(rng());
      v.real  = std::ldexp(static_cast<double>(static_cast<std::int64_t>(rng())),
                           static_cast<int>(rng() % 200) - 100);
      v.small = static_cast<float>(static_cast<std::int32_t>(rng())) / 7.0f;
      v.flag  = rng() & 1;
      if (rng() & 1)
         v.maybe = random_text(rng, 10);
      if (rng() & 1)
         v.choice = static_cast<std::uint32_t>(rng());
      else
         v.choice = random_text(rng, 10);
      v.pair = {static_cast<std::uint16_t>(rng()), random_text(rng, 10)};
      for (std::size_t i = rng() % 4; i--;)
         v.named[random_text(rng, 8)] = static_cast<std::uint32_t>(rng());
      v.numbers.resize(rng() % 6);
      for (auto& n : v.numbers)
         n = static_cast<std::uint32_t>(rng() >> (rng() % 64));
      v.color = rng() & 1 ? Color::green : static_cast<Color>(7);
      v.huge  = (static_cast<psio::uint128>(rng()) << 64 | rng()) >> (rng() % 128);
      return v;
   }

   template <typename Fmt, typename T>
   std::vector<char> text_bytes(Fmt fmt, const T& v)
   {
      std::vector<char> out;
      REQUIRE(psio::encode_json_checked(fmt, v, out).ok());
      return out;
   }
}  // namespace

TEST_CASE("json sink: text that fits the reserved capacity never reallocates",
          "[json][sink][alloc]")
{
   // 4 MiB of hex into 4.5 MiB reserved (gofractally/psio#11: the writer
   // doubled the sink's size and asked for 8 MiB).
   const Trace       value{std::vector<char>(MiB, 'x'), std::vector<char>(MiB, 'y'), 7};
   const std::size_t reserved = 9 * MiB / 2;
   for (auto how : {psio::json_sizing::growth, psio::json_sizing::exact,
                    psio::json_sizing::automatic})
   {
      heap_window       heap;
      std::vector<char> out;
      out.reserve(reserved);
      const char* const data = out.data();
      REQUIRE(psio::encode_json_checked(psio::ijson{}, value, out, how).ok());
      CHECK(out.size() == 4 * MiB + 32);
      CHECK(out.capacity() == reserved);
      CHECK(out.data() == data);
      CHECK(heap.count() == 1);
      CHECK(heap.peak_bytes() == reserved);
   }
   {
      heap_window heap;
      std::string out;
      out.reserve(reserved);
      const std::size_t capacity = out.capacity();
      const char* const data     = out.data();
      psio::to_ijson(value, out);
      CHECK(out.size() == 4 * MiB + 32);
      CHECK(out.capacity() == capacity);
      CHECK(out.data() == data);
      CHECK(heap.count() == 1);
   }
   // Appending after text the sink already holds; and exactly full.
   {
      std::vector<char> out{'x', '='};
      const auto        text = psio::to_ijson(value);
      out.reserve(2 + text.size());
      heap_window heap;
      psio::to_ijson(value, out, psio::json_sizing::growth);
      CHECK(heap.count() == 0);
      CHECK(out.size() == 2 + text.size());
      CHECK(out.capacity() == 2 + text.size());
      CHECK(std::string_view{out.data(), 2} == "x=");
      CHECK(std::string_view{out.data() + 2, text.size()} == text);
   }
}

TEST_CASE("json sink: a small text does not take a large reservation", "[json][sink]")
{
   // The capacity is taken as it is needed (a std::vector<char> gives its
   // new bytes a value: 7 bytes of text must not touch 64 MiB).
   std::vector<char> out;
   out.reserve(64 * MiB);
   const char* const data = out.data();
   psio::to_json(std::uint32_t{1234567}, out);
   CHECK(std::string_view{out.data(), out.size()} == "1234567");
   CHECK(out.data() == data);
   CHECK(out.capacity() == 64 * MiB);
}

TEST_CASE("json sink: past the reservation the sink grows by doubling, one buffer at a time",
          "[json][sink][alloc]")
{
   const Trace value{std::vector<char>(MiB, 'x'), std::vector<char>(MiB, 'y'), 7};
   // Nothing reserved: ~4 MiB of text.
   {
      heap_window       heap;
      std::vector<char> out;
      psio::to_ijson(value, out, psio::json_sizing::growth);
      CHECK(out.size() == 4 * MiB + 32);
      // Amortised: 128 bytes, the first list, the second, and once more.
      CHECK(heap.count() <= 4);
      CHECK(out.capacity() <= 2 * out.size());
      CHECK(heap.peak_bytes() <= 3 * out.size());
   }
   // Reserved for half: one growth, and never two buffers that each hold
   // the text.
   {
      heap_window       heap;
      std::vector<char> out;
      out.reserve(3 * MiB);
      psio::to_ijson(value, out, psio::json_sizing::growth);
      CHECK(heap.count() == 2);
      CHECK(largest <= 6 * MiB);
      CHECK(heap.peak_bytes() <= 9 * MiB);
   }
}

TEST_CASE("json sink: exact sizing is one allocation of the text", "[json][sink][alloc]")
{
   const Trace value{std::vector<char>(MiB, 'x'), std::vector<char>(MiB, 'y'), 7};
   const auto  expected = psio::to_ijson(value, psio::json_sizing::growth);
   {
      heap_window heap;
      const auto  size = psio::ijson_size(value);
      CHECK(heap.count() == 0);
      CHECK(size == expected.size());
   }
   {
      heap_window       heap;
      std::vector<char> out;
      psio::to_ijson(value, out, psio::json_sizing::exact);
      CHECK(heap.count() == 1);
      CHECK(heap.peak_bytes() == expected.size());
      CHECK(out.capacity() == expected.size());
      CHECK(std::string_view{out.data(), out.size()} == expected);
   }
   {
      heap_window heap;
      const auto  out = psio::to_ijson(value, psio::json_sizing::exact);
      CHECK(heap.count() == 1);
      // (a std::string rounds its capacity up)
      CHECK(heap.peak_bytes() <= expected.size() + 32);
      CHECK(out == expected);
   }
   // The array-of-numbers form of bytes (psio::json).
   {
      const auto        numbers = psio::to_json(value, psio::json_sizing::growth);
      heap_window       heap;
      std::vector<char> out;
      psio::to_json(value, out, psio::json_sizing::exact);
      CHECK(heap.count() == 1);
      CHECK(heap.peak_bytes() == numbers.size());
      CHECK(std::string_view{out.data(), out.size()} == numbers);
   }
}

TEST_CASE("json sink: the measured size is the length of the text", "[json][sink][size]")
{
   std::mt19937_64 rng(20261123);
   for (int i = 0; i < 4000; ++i)
   {
      const Everything v = random_value(rng);
      for (int ijson = 0; ijson < 2; ++ijson)
      {
         std::string growth, exact;
         std::size_t size = 0;
         if (ijson)
         {
            growth = psio::to_ijson(v, psio::json_sizing::growth);
            exact  = psio::to_ijson(v, psio::json_sizing::exact);
            size   = psio::ijson_size(v);
         }
         else
         {
            growth = psio::to_json(v, psio::json_sizing::growth);
            exact  = psio::to_json(v, psio::json_sizing::exact);
            size   = psio::json_size(v);
         }
         REQUIRE(size == growth.size());
         REQUIRE(exact == growth);
         // A sink reserved for exactly the text is not reallocated by the
         // one-walk writer either: a worst-case reservation near the end
         // (a number's widest form) does not count against the capacity.
         std::vector<char> sink;
         sink.reserve(size + 3);
         sink.assign({'a', 'b', 'c'});
         const char* const data = sink.data();
         if (ijson)
            psio::to_ijson(v, sink, psio::json_sizing::growth);
         else
            psio::to_json(v, sink, psio::json_sizing::growth);
         REQUIRE(sink.data() == data);
         REQUIRE(sink.capacity() == size + 3);
         REQUIRE(std::string_view{sink.data(), sink.size()} == "abc" + growth);
         // One byte short: it grows, and the text is the same.
         std::string shorter;
         shorter.reserve(size > 40 ? size - 1 : 64);
         if (ijson)
            psio::to_ijson(v, shorter, psio::json_sizing::growth);
         else
            psio::to_json(v, shorter, psio::json_sizing::growth);
         REQUIRE(shorter == growth);
      }
   }
}

TEST_CASE("json sink: every byte, alone and at the end of a string", "[json][sink][size]")
{
   // The last bytes of an exact destination: an escape there has no room
   // to spare.
   for (unsigned c = 0; c < 0x80; ++c)
      for (std::size_t lead = 0; lead < 18; ++lead)
         for (std::size_t tail = 0; tail < 3; ++tail)
         {
            std::string s(lead, 'a');
            s.push_back(static_cast<char>(c));
            s.append(tail, static_cast<char>(c));
            const auto expected = psio::to_json(s, psio::json_sizing::growth);
            REQUIRE(psio::json_size(s) == expected.size());
            REQUIRE(psio::to_json(s, psio::json_sizing::exact) == expected);
            // (a string that ends the text of a record too)
            const std::vector<std::string> list{s, s};
            const auto                     both = psio::to_ijson(list, psio::json_sizing::growth);
            REQUIRE(psio::ijson_size(list) == both.size());
            REQUIRE(psio::to_ijson(list, psio::json_sizing::exact) == both);
         }
   // Numbers at the end: the widest reservation against the fewest bytes.
   for (std::uint64_t v : {std::uint64_t{0}, std::uint64_t{9007199254740991}, ~std::uint64_t{0}})
   {
      CHECK(psio::to_ijson(v, psio::json_sizing::exact) ==
            psio::to_ijson(v, psio::json_sizing::growth));
      CHECK(psio::ijson_size(v) == psio::to_ijson(v, psio::json_sizing::growth).size());
   }
   // Every length of integer, each side of every power of ten and of two,
   // every width and sign: an integer is measured without being written.
   {
      auto check = [](auto v) {
         const auto a = psio::to_json(v, psio::json_sizing::growth);
         const auto b = psio::to_ijson(v, psio::json_sizing::growth);
         REQUIRE(psio::json_size(v) == a.size());
         REQUIRE(psio::ijson_size(v) == b.size());
         REQUIRE(psio::to_json(v, psio::json_sizing::exact) == a);
         REQUIRE(psio::to_ijson(v, psio::json_sizing::exact) == b);
      };
      auto each = [&](std::uint64_t u) {
         for (std::uint64_t v : {u - 1, u, u + 1})
         {
            check(v);
            check(static_cast<std::int64_t>(v));
            check(-static_cast<std::int64_t>(v));
            check(static_cast<std::uint32_t>(v));
            check(static_cast<std::int32_t>(v));
            check(static_cast<std::uint16_t>(v));
            check(static_cast<std::int16_t>(v));
            check(static_cast<std::uint8_t>(v));
            check(static_cast<std::int8_t>(v));
            check(static_cast<psio::uint128>(v) * 1000003u);
            check(-static_cast<psio::int128>(v) * 1000003);
         }
      };
      for (std::uint64_t p = 1; p != 0; p = p > ~std::uint64_t{0} / 10 ? 0 : p * 10)
         each(p);
      for (int bit = 0; bit < 64; ++bit)
         each(std::uint64_t{1} << bit);
      check(std::numeric_limits<std::int64_t>::min());
      check(std::numeric_limits<std::int8_t>::min());
   }
   CHECK(psio::to_json(0.1, psio::json_sizing::exact) == "0.1");
   CHECK(psio::json_size(std::numeric_limits<double>::max()) ==
         psio::to_json(std::numeric_limits<double>::max()).size());
   CHECK(psio::to_json(std::vector<bool>{}, psio::json_sizing::exact) == "[]");
}

TEST_CASE("json sink: a value without a text fails every form alike", "[json][sink]")
{
   const std::vector<std::string> bad{"fine", "not \xff utf-8"};
   std::size_t                    size = 0;
   auto measured = psio::json_size_checked(psio::ijson{}, bad, size);
   REQUIRE(!measured.ok());
   CHECK(measured.error().what == std::string_view{"json: string is not valid UTF-8"});
   for (auto how : {psio::json_sizing::growth, psio::json_sizing::exact})
   {
      std::vector<char> out{'k', 'e', 'p', 't'};
      auto              st = psio::encode_json_checked(psio::ijson{}, bad, out, how);
      REQUIRE(!st.ok());
      CHECK(st.error().what == measured.error().what);
      CHECK(std::string_view{out.data(), out.size()} == "kept");
      CHECK_THROWS_AS(psio::to_ijson(bad, how), psio::codec_exception);
   }
   CHECK_THROWS_AS(psio::ijson_size(bad), psio::codec_exception);
   const double nan = std::nan("");
   CHECK(!psio::json_size_checked(psio::json{}, nan, size).ok());
   // The I-JSON rule the plain form does not have.
   const std::string nonchar = "\xEF\xBF\xBE";
   CHECK(psio::json_size_checked(psio::json{}, nonchar, size).ok());
   CHECK(!psio::json_size_checked(psio::ijson{}, nonchar, size).ok());
}

TEST_CASE("json in place: a text member has the bytes of the two-step encoding",
          "[json][pssz][in_place]")
{
   std::mt19937_64 rng(11);
   static_assert(psio::packs_as<ReplyOf<psio::json_ref<psio::ijson, const Everything&>>, Reply>);
   static_assert(psio::packs_as<ReplyOf<psio::json_ref<psio::json, Everything>>, Reply>);
   for (int i = 0; i < 300; ++i)
   {
      const Everything          v = random_value(rng);
      const std::vector<Header> headers{{"Access-Control-Allow-Origin", "*"}};
      {
         const auto two = psio::to_pssz(Reply{200, "application/json",
                                              text_bytes(psio::ijson{}, v), headers});
         const auto one = psio::to_pssz(
            ReplyOf<decltype(psio::ijson_of(v))>{200, "application/json", psio::ijson_of(v),
                                                 headers});
         REQUIRE(one == two);
         REQUIRE(psio::pssz_size(ReplyOf<decltype(psio::ijson_of(v))>{
                    200, "application/json", psio::ijson_of(v), headers}) == two.size());
         REQUIRE(psio::validate_pssz<Reply>(one).ok());
      }
      {
         const auto two =
            psio::to_pssz(Reply{200, "application/json", text_bytes(psio::json{}, v), headers});
         // An rvalue is held by the source.
         auto source = psio::json_of(Everything{v});
         const auto one = psio::to_pssz(
            ReplyOf<decltype(source)>{200, "application/json", std::move(source), headers});
         REQUIRE(one == two);
      }
      // The text alone, and as the last member of a tuple.
      REQUIRE(psio::to_pssz(psio::ijson_of(v)) == psio::to_pssz(text_bytes(psio::ijson{}, v)));
      REQUIRE(psio::to_pssz(std::tuple{std::uint32_t{7}, psio::ijson_of(v), std::string{"z"}}) ==
              psio::to_pssz(std::tuple{std::uint32_t{7}, text_bytes(psio::ijson{}, v),
                                       std::string{"z"}}));
   }
}

TEST_CASE("json in place: the reply of a large trace inside an action inside a call",
          "[json][pssz][in_place][alloc]")
{
   // psibase's sendReply: the body of an HttpReply is the JSON of a trace;
   // the reply is an argument of an action; the action is packed.
   const Trace               trace{std::vector<char>(MiB, 'x'), std::vector<char>(MiB, 'y'), 7};
   const std::vector<Header> headers{{"Access-Control-Allow-Origin", "*"}};
   const std::size_t         text = psio::ijson_size(trace);
   REQUIRE(text == 4 * MiB + 32);

   std::vector<char> two;
   std::size_t       two_peak = 0, two_count = 0;
   {
      heap_window heap;
      Reply       reply{200, "application/json", {}, headers};
      REQUIRE(psio::encode_json_checked(psio::ijson{}, trace, reply.body,
                                        psio::json_sizing::exact)
                 .ok());
      auto args = psio::to_pssz(std::tuple{std::int32_t{5}, std::move(reply)});
      two       = psio::to_pssz(Action{1, 2, 3, std::move(args)});
      two_peak  = heap.peak_bytes();
      two_count = heap.count();
   }
   std::vector<char> one;
   std::size_t       one_peak = 0, one_count = 0;
   {
      // (the headers are referred to: nothing but the result is allocated)
      heap_window heap;
      one = psio::to_pssz(ActionOf{
         1, 2, 3,
         psio::packed(std::forward_as_tuple(
            std::int32_t{5},
            ReplyRef{200, "application/json", psio::ijson_of(trace), std::cref(headers)}))});
      one_peak  = heap.peak_bytes();
      one_count = heap.count();
   }
   REQUIRE(one == two);
   CHECK(one_count == 1);
   CHECK(one_peak == one.size());
   CHECK(one.size() < text + 256);
   // Two steps: the text, the arguments around it, the action around them.
   CHECK(two_peak >= 2 * text);
   INFO("text " << text << "; in place: " << one_peak << " bytes in " << one_count
                << "; two-step: " << two_peak << " bytes in " << two_count);
   CHECK(two_count > one_count);
   WARN("text " << text << "; in place: peak " << one_peak << " bytes in " << one_count
                << " allocation(s); two-step (exact text): peak " << two_peak << " bytes in "
                << two_count);
}

TEST_CASE("json in place: a view is a source of the text", "[json][pssz][in_place]")
{
   const Trace trace{std::vector<char>(100, 'x'), std::vector<char>(7, 'y'), 7};
   const auto  packed = psio::to_pssz(trace);
   const psio::pssz_view<Trace> view{packed};
   const auto expected = psio::to_pssz(text_bytes(psio::ijson{}, trace));
   CHECK(psio::to_pssz(psio::ijson_of(view)) == expected);
   // A record of references.
   CHECK(psio::to_pssz(psio::ijson_of(std::tie(trace.outer, trace.time))) ==
         psio::to_pssz(text_bytes(psio::ijson{}, std::tuple{trace.outer, trace.time})));
}

namespace
{
   // A trace made of references into bytes held elsewhere (psibase's
   // pruned trace): spans and string views.
   struct ActionRef
   {
      std::uint64_t         sender = 0;
      std::span<const char> rawData;
   };
   PSIO_REFLECT(ActionRef, sender, rawData)
   struct TraceRef
   {
      std::vector<ActionRef>          actions;
      std::optional<std::string_view> error;
   };
   PSIO_REFLECT(TraceRef, actions, error)
}  // namespace

TEST_CASE("json in place: a record of references is written from what it refers to",
          "[json][pssz][in_place][alloc]")
{
   const std::vector<char>   args(MiB, 'x');
   const std::vector<Header> headers{{"Access-Control-Allow-Origin", "*"}};
   TraceRef                  trace;
   trace.actions.push_back(ActionRef{1, args});
   trace.actions.push_back(ActionRef{2, args});
   trace.error = "a \"quoted\" message";
   const auto text = text_bytes(psio::ijson{}, trace);
   const auto two  = psio::to_pssz(
      Action{1, 2, 3, psio::to_pssz(std::tuple{std::int32_t{5},
                                               Reply{200, "application/json", text, headers}})});
   heap_window heap;
   // (as psibase's sendReply would: the text is checked first, so a trace
   // without one is answered with an error instead)
   auto body = psio::ijson_of(trace);
   REQUIRE(body.check().ok());
   CHECK(body.size() == text.size());
   CHECK(heap.count() == 0);
   const auto one = psio::to_pssz(ActionOf{
      1, 2, 3,
      psio::packed(std::forward_as_tuple(
         std::int32_t{5},
         ReplyRef{200, "application/json", std::move(body), std::cref(headers)}))});
   CHECK(heap.count() == 1);
   CHECK(heap.peak_bytes() == one.size());
   CHECK(one == two);
}

TEST_CASE("json in place: a value without a text fails the enclosing encode",
          "[json][pssz][in_place]")
{
   const std::string bad = "not \xff utf-8";
   std::vector<char> out{'k'};
   auto st = psio::encode_pssz_checked(
      psio::pssz{},
      ReplyOf<decltype(psio::ijson_of(bad))>{200, "text/plain", psio::ijson_of(bad), {}}, out);
   REQUIRE(!st.ok());
   CHECK(st.error().what == std::string_view{"json: string is not valid UTF-8"});
   CHECK(out == std::vector<char>{'k'});
   CHECK_THROWS_AS(psio::to_pssz(psio::ijson_of(bad)), psio::codec_exception);
}

TEST_CASE("json in place: a value changed after it was sized is refused, in bounds",
          "[json][pssz][in_place]")
{
   std::string value  = "four";
   auto        source = psio::ijson_of(value);
   CHECK(source.size() == 6);
   std::vector<char> room(64, '#');
   value = "longer than it was";
   CHECK_THROWS_AS(source.write(room.data()), psio::codec_exception);
   // Nothing past the six bytes it was sized as.
   CHECK(std::string_view{room.data() + 6, room.size() - 6} == std::string(58, '#'));
   value = "x";
   CHECK_THROWS_AS(source.write(room.data()), psio::codec_exception);
}
