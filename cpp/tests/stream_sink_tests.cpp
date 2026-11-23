// Sinks and sources (probes g11_sink, g_bin_cursor, g_e_unhex):
// psio::to_pssz(v, sink) into any psio stream and into std::string;
// psio::input_stream, a bounds-checked read cursor with typed and LEB128
// readers; psio::from_hex into a fixed destination.

#include <psio/bin.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/stream.hpp>
#include <psio/to_hex.hpp>

#include <catch.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sinkt
{
   struct A
   {
      std::uint32_t            x = 0;
      std::string              s;
      std::vector<std::string> v;
   };
   PSIO_REFLECT(A, x, s, v)

   struct Small
   {
      std::uint8_t  id = 0;
      std::uint16_t n  = 0;
   };
   PSIO_REFLECT(Small, id, n, definitionWillNotChange())

   struct Capped
   {
      std::string text;
   };
   PSIO_REFLECT(Capped, text, maxDynamicData(8))
}  // namespace sinkt

using namespace sinkt;

TEST_CASE("to_pssz into psio streams (probe g11_sink)", "[pssz][sink]")
{
   const A    a{1, "ab", {"c", "dd"}};
   const auto want = psio::to_pssz(a);

   // A caller's buffer, bounds-checked. A second value is appended: the
   // offsets inside it are relative to where it starts.
   char                   buf[96];
   psio::fixed_buf_stream fixed{buf, sizeof buf};
   psio::to_pssz(a, fixed);
   REQUIRE(fixed.written() == want.size());
   REQUIRE(std::equal(want.begin(), want.end(), buf));
   psio::to_pssz(a, fixed);
   REQUIRE(fixed.written() == 2 * want.size());
   REQUIRE(std::equal(want.begin(), want.end(), buf + want.size()));
   REQUIRE(psio::from_pssz<A>(std::span<const char>{buf + want.size(), want.size()}).v[1] == "dd");

   // Counting only.
   psio::size_stream ss;
   psio::to_pssz(a, ss);
   REQUIRE(ss.size == want.size());
   REQUIRE(ss.size == psio::pssz_size(a));

   // Growing a vector that already holds bytes.
   std::vector<char>   vec{char(9)};
   psio::vector_stream vs{vec};
   psio::to_pssz(a, vs);
   REQUIRE(vec.size() == 1 + want.size());
   REQUIRE(vec[0] == 9);
   REQUIRE(std::equal(want.begin(), want.end(), vec.begin() + 1));

   // A std::string, directly and through string_stream.
   std::string str = "#";
   psio::to_pssz(a, str);
   REQUIRE(str.size() == 1 + want.size());
   REQUIRE(std::equal(want.begin(), want.end(), str.begin() + 1));
   std::string         str2 = "#";
   psio::string_stream sts{str2};
   psio::to_pssz(a, sts);
   REQUIRE(str2 == str);

   // A buffer the caller sized.
   std::vector<char>      exact(psio::pssz_size(a));
   psio::fast_buf_stream fb{exact.data(), exact.size()};
   psio::to_pssz(a, fb);
   REQUIRE(exact == want);

   // The existing vector form still appends.
   std::vector<char> msg{char(7)};
   psio::to_pssz(a, msg);
   REQUIRE(msg.size() == 1 + want.size());

   // The generic layer.
   char                   buf2[96];
   psio::fixed_buf_stream f2{buf2, sizeof buf2};
   psio::encode(psio::pssz{}, a, f2);
   REQUIRE(f2.written() == want.size());

   // Scalars, fixed records, strings.
   char                   sbuf[16];
   psio::fixed_buf_stream sf{sbuf, sizeof sbuf};
   psio::to_pssz(std::uint32_t{0x01020304}, sf);
   psio::to_pssz(Small{5, 6}, sf);
   psio::to_pssz(std::string("xy"), sf);
   REQUIRE(sf.written() == 4 + 3 + 2);
   REQUIRE(sbuf[0] == 4);
   REQUIRE(sbuf[4] == 5);
   REQUIRE(sbuf[8] == 'y');
}

#if defined(__cpp_exceptions)
TEST_CASE("to_pssz into a stream: failures write nothing or stop at the bound", "[pssz][sink]")
{
   const A a{1, "ab", {"c", "dd"}};
   // Too small: the bounds-checked stream refuses.
   char                   small[4];
   psio::fixed_buf_stream tiny{small, sizeof small};
   REQUIRE_THROWS_AS(psio::to_pssz(a, tiny), psio::codec_exception);
   // A value with no encoding is rejected before anything is written.
   std::vector<char>   out{char(1)};
   psio::vector_stream vs{out};
   REQUIRE_THROWS_AS(psio::to_pssz(Capped{"far too long for the cap"}, vs),
                     psio::codec_exception);
   REQUIRE(out.size() == 1);
}
#endif

TEST_CASE("input_stream: a bounds-checked read cursor (probe g_bin_cursor)", "[stream][cursor]")
{
   // magic, version, a varuint, a 3-byte varuint, sleb -1, a bin string.
   const std::vector<char> wasm{0, 'a', 's', 'm', 1, 0, 0, 0, 5,
                                char(0xe5), char(0x8e), 0x26, 0x7f, 2, 'h', 'i'};
   psio::input_stream in{wasm};
   REQUIRE(in.position() == 0);
   REQUIRE(in.remaining() == wasm.size());
   const auto magic   = psio::from_bin<std::uint32_t>(in);   // reads AND advances
   const auto version = psio::from_bin<std::uint32_t>(in);
   REQUIRE(magic == 0x6d736100);
   REQUIRE(version == 1);
   REQUIRE(in.position() == 8);
   REQUIRE(psio::varuint32_from_bin(in) == 5);
   REQUIRE(psio::varuint32_from_bin(in) == 624485);
   REQUIRE(psio::sleb64_from_bin(in) == -1);
   REQUIRE(psio::from_bin<std::string>(in) == "hi");
   REQUIRE(in.empty());
   REQUIRE(in.remaining() == 0);
   REQUIRE(in.position() == wasm.size());

   // Raw reads, borrowed bytes, skipping.
   psio::input_stream text{std::string_view{"abcdefgh"}};
   const auto         two = text.read_bytes(2);
   REQUIRE(two.size() == 2);
   REQUIRE(two[1] == 'b');
   text.skip(1);
   REQUIRE(text.get<char>() == 'd');
   char pair[2];
   text.read(pair, 2);
   REQUIRE(pair[1] == 'f');
   std::uint16_t raw = 0;
   text.read_raw(raw);
   REQUIRE(raw == static_cast<std::uint16_t>('g' | ('h' << 8)));
   REQUIRE(text.rest().empty());
   std::span<const char> rest = psio::input_stream{std::string_view{"xyz"}};
   REQUIRE(rest.size() == 3);

   // A record, and what follows it.
   std::vector<char> two_values = psio::to_bin(Small{1, 2});
   psio::to_bin(std::uint8_t{9}, two_values);
   psio::input_stream rs{two_values};
   const Small        s = psio::from_bin<Small>(rs);
   REQUIRE(s.n == 2);
   REQUIRE(psio::from_bin<std::uint8_t>(rs) == 9);
   REQUIRE(rs.empty());

   // Writers and readers agree.
   std::vector<char>   o;
   psio::vector_stream os{o};
   psio::varuint32_to_bin(624485, os);
   psio::sleb64_to_bin(-123456789012, os);
   psio::varuint64_to_bin(1ull << 40, os);
   psio::sleb32_to_bin(-2, os);
   psio::varuint32_to_bin(0, os);
   psio::input_stream back{o};
   REQUIRE(psio::varuint32_from_bin(back) == 624485);
   REQUIRE(psio::sleb64_from_bin(back) == -123456789012);
   REQUIRE(psio::varuint64_from_bin(back) == (1ull << 40));
   REQUIRE(psio::sleb32_from_bin(back) == -2);
   REQUIRE(psio::varuint32_from_bin(back) == 0);
   REQUIRE(back.empty());
}

#if defined(__cpp_exceptions)
TEST_CASE("input_stream: reading past the end or a malformed varint fails", "[stream][cursor]")
{
   const std::vector<char> one{1};
   psio::input_stream      in{one};
   REQUIRE_THROWS_AS(in.get<std::uint32_t>(), psio::codec_exception);
   REQUIRE(in.position() == 0);   // a failed read consumes nothing
   REQUIRE_THROWS_AS(in.skip(2), psio::codec_exception);
   REQUIRE_THROWS_AS(in.read_bytes(2), psio::codec_exception);
   REQUIRE_THROWS_AS(psio::from_bin<std::uint32_t>(in), psio::codec_exception);
   REQUIRE(in.get<std::uint8_t>() == 1);
   REQUIRE_THROWS_AS(psio::varuint32_from_bin(in), psio::codec_exception);
   // A varuint32 that never terminates within five bytes.
   const std::vector<char> endless(6, char(0x80));
   psio::input_stream      bad{endless};
   REQUIRE_THROWS_AS(psio::varuint32_from_bin(bad), psio::codec_exception);
   REQUIRE(bad.position() == 0);
   // A truncated bin string.
   const std::vector<char> cut{5, 'a', 'b'};
   psio::input_stream      c{cut};
   REQUIRE_THROWS_AS(psio::from_bin<std::string>(c), psio::codec_exception);
}
#endif

TEST_CASE("from_hex into a fixed destination (probe g_e_unhex)", "[util][hex]")
{
   std::array<unsigned char, 4> digest{};
   REQUIRE(psio::from_hex("deadBEEF", digest));
   REQUIRE(digest == std::array<unsigned char, 4>{0xde, 0xad, 0xbe, 0xef});
   // Exactly the destination's size: no more, no fewer.
   REQUIRE_FALSE(psio::from_hex("deadbe", digest));
   REQUIRE_FALSE(psio::from_hex("deadbeef00", digest));
   REQUIRE_FALSE(psio::from_hex("deadbeeg", digest));
   REQUIRE_FALSE(psio::from_hex("deadbee", digest));
   char raw[2];
   REQUIRE(psio::from_hex("0aff", raw));
   REQUIRE(static_cast<unsigned char>(raw[1]) == 0xff);
   std::vector<char> store(3);
   REQUIRE(psio::from_hex("010203", std::span<char>{store}));
   REQUIRE(store == std::vector<char>{1, 2, 3});
   std::array<char, 0> none{};
   REQUIRE(psio::from_hex("", none));
   // The growing form is unchanged, and no longer reads past an odd digit.
   std::vector<char> out;
   REQUIRE(psio::from_hex("0aFF", out));
   REQUIRE(out.size() == 2);
   REQUIRE_FALSE(psio::from_hex("0aF", out));
}
