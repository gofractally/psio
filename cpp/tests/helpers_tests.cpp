// The per-format helper names of psio/helpers.hpp: for every format tag,
// to_F / from_F / validate_F / F_size / F_view / F_buffer / shared_F.
// One HELPERS_CASE line per format runs the whole set that format has.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/bson.hpp>
#include <psio/capnp.hpp>
#include <psio/flatbuf.hpp>
#include <psio/helpers.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/msgpack.hpp>
#include <psio/mutable_view.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

   struct Rec
   {
      std::uint32_t              id = 0;
      std::string                name;
      std::vector<std::uint32_t> xs;

      friend bool operator==(const Rec&, const Rec&) = default;
   };
   PSIO_REFLECT(Rec, id, name, xs)

   // All members fixed-size and the layout frozen: every member is
   // writable through a mutable view.
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;

      friend bool operator==(const Point&, const Point&) = default;
   };
   PSIO_REFLECT(Point, x, y, definitionWillNotChange())

   const Rec sample{7, "alice", {1, 2, 3}};

   template <typename Buffer>
   std::span<const char> span_of(const Buffer& b)
   {
      return std::span<const char>{b.data(), b.size()};
   }

   // A toy format whose tag is not named like its helpers: one int, four
   // bytes. Declared in psio so its hidden friends are found the same way
   // a real format's are.
}  // namespace

namespace psio {
   struct helpers_test_tag : format_tag_base<helpers_test_tag>
   {
      friend std::vector<char> tag_invoke(decltype(::psio::encode), helpers_test_tag,
                                          const int& v)
      {
         std::vector<char> out(sizeof(int));
         std::memcpy(out.data(), &v, sizeof(int));
         return out;
      }
      friend int tag_invoke(decltype(::psio::decode<int>), helpers_test_tag, int*,
                            std::span<const char> b)
      {
         int v = 0;
         std::memcpy(&v, b.data(), sizeof(int));
         return v;
      }
      friend codec_status tag_invoke(decltype(::psio::validate<int>), helpers_test_tag, int*,
                                     std::span<const char> b) noexcept
      {
         return b.size() == sizeof(int) ? codec_ok() : codec_fail("toy: size", 0, "toy");
      }
   };
   PSIO_FORMAT_HELPERS_AS(toy, ::psio::helpers_test_tag)
}  // namespace psio

// Everything a format has, on one record: fresh and appending encode, size,
// status validation, checked and trusted decode, rejection of a truncated
// input (the first half) by both the status and the checked form, the typed
// buffers.
#define HELPERS_CASE(F)                                                                    \
   TEST_CASE("helpers: " #F, "[helpers]")                                                  \
   {                                                                                       \
      const auto bytes = psio::to_##F(sample);                                             \
      const auto in    = span_of(bytes);                                                   \
      CHECK(bytes == psio::encode(psio::F{}, sample));                                     \
      CHECK(psio::F##_size(sample) == psio::size_of(psio::F{}, sample));                   \
      CHECK(psio::validate_##F<Rec>(in).ok());                                             \
      CHECK(psio::from_##F<Rec>(in) == sample);                                            \
      CHECK(psio::from_##F<Rec>(bytes) == sample);                                         \
      CHECK(psio::from_##F<Rec>(psio::trusted{in}) == sample);                             \
      CHECK(psio::from_##F<Rec>(psio::trusted{bytes}) == sample);                          \
                                                                                           \
      const auto cut = in.first(in.size() / 2);                                            \
      auto       st  = psio::validate_##F<Rec>(cut);                                       \
      REQUIRE_FALSE(st.ok());                                                              \
      CHECK_FALSE(st.error().what.empty());                                                \
      CHECK_THROWS_AS(psio::from_##F<Rec>(cut), psio::codec_exception);                    \
                                                                                           \
      [&](const auto& value) {                                                             \
         if constexpr (requires(std::remove_cvref_t<decltype(bytes)>& sink) {              \
                          psio::to_##F(value, sink);                                       \
                       })                                                                  \
         {                                                                                 \
            std::remove_cvref_t<decltype(bytes)> sink;                                     \
            psio::to_##F(value, sink);                                                     \
            CHECK(sink == bytes);                                                          \
         }                                                                                 \
      }(sample);                                                                           \
                                                                                           \
      psio::F##_buffer<Rec> owned{std::vector<char>(in.begin(), in.end())};                \
      CHECK(psio::from_##F<Rec>(psio::bytes(owned)) == sample);                            \
      psio::shared_##F<Rec> first{in};                                                     \
      psio::shared_##F<Rec> second = first;                                                \
      CHECK(psio::bytes(first).data() == psio::bytes(second).data());                      \
      CHECK(psio::bytes(first).data() != in.data());                                       \
      CHECK(psio::from_##F<Rec>(psio::bytes(second)) == sample);                           \
   }

HELPERS_CASE(pssz)
HELPERS_CASE(ssz)
HELPERS_CASE(bin)
HELPERS_CASE(borsh)
HELPERS_CASE(bincode)
HELPERS_CASE(key)
HELPERS_CASE(json)
HELPERS_CASE(ijson)
HELPERS_CASE(msgpack)
HELPERS_CASE(avro)
HELPERS_CASE(bson)
HELPERS_CASE(protobuf)
HELPERS_CASE(capnp)
HELPERS_CASE(flatbuf)
HELPERS_CASE(wit)

// to_F(v, sink) adds to what the sink holds. capnp and flatbuf are the
// exception: a message of those formats is addressed from its first byte,
// and their sink form reuses the vector's capacity for one message.
#define APPENDS(F)                                    \
   [] {                                               \
      auto sink = psio::to_##F(sample);               \
      const auto one = sink.size();                   \
      psio::to_##F(sample, sink);                     \
      return sink.size() == 2 * one;                  \
   }()
TEST_CASE("helpers: encoding into an existing buffer", "[helpers]")
{
   CHECK(APPENDS(pssz));
   CHECK(APPENDS(ssz));
   CHECK(APPENDS(bin));
   CHECK(APPENDS(borsh));
   CHECK(APPENDS(bincode));
   CHECK(APPENDS(key));
   CHECK(APPENDS(json));
   CHECK(APPENDS(ijson));
   CHECK(APPENDS(msgpack));
   CHECK(APPENDS(avro));
   CHECK(APPENDS(bson));
   CHECK(APPENDS(protobuf));
   CHECK_FALSE(APPENDS(capnp));
   CHECK_FALSE(APPENDS(flatbuf));
}

// ── What each format does not have is absent, not a hard error ──────────

template <typename T>
concept has_wit_sink = requires(const T& v, std::vector<char>& s) { psio::to_wit(v, s); };
template <typename T>
concept has_pssz_sink = requires(const T& v, std::vector<char>& s) { psio::to_pssz(v, s); };
template <typename T>
concept has_json_string_sink = requires(const T& v, std::string& s) { psio::to_json(v, s); };
template <typename T>
concept has_json_vector_sink = requires(const T& v, std::vector<char>& s) { psio::to_json(v, s); };
static_assert(has_pssz_sink<Rec> && has_json_string_sink<Rec>);
static_assert(!has_wit_sink<Rec>, "wit has no appending encode");
static_assert(!has_json_vector_sink<Rec>, "JSON text is appended to a std::string");

template <typename T>
concept has_json_literal = requires { psio::from_json<T>("{}"); psio::validate_ijson<T>("{}"); };
template <typename T>
concept has_toy_size = requires(const T& v) { psio::toy_size(v); };
static_assert(has_json_literal<Rec>);
static_assert(!has_toy_size<int>, "the toy format has no size_of");

#define VIEW_CONCEPTS(F)                                                 \
   template <typename T>                                                 \
   concept F##_reads = requires { typename psio::F##_view<T>; };         \
   template <typename T>                                                 \
   concept F##_writes = requires { typename psio::F##_mutable_view<T>; };
VIEW_CONCEPTS(pssz)
VIEW_CONCEPTS(ssz)
VIEW_CONCEPTS(wit)
VIEW_CONCEPTS(bin)
VIEW_CONCEPTS(json)
VIEW_CONCEPTS(key)
VIEW_CONCEPTS(protobuf)
static_assert(pssz_reads<Rec> && pssz_writes<Rec>);
static_assert(ssz_reads<Rec> && !ssz_writes<Rec>);
static_assert(wit_reads<Rec> && !wit_writes<Rec>);
static_assert(!bin_reads<Rec> && !bin_writes<Rec>);
static_assert(!json_reads<Rec> && !json_writes<Rec>);
static_assert(!key_reads<Rec> && !key_writes<Rec>);
static_assert(!protobuf_reads<Rec> && !protobuf_writes<Rec>);

// F_view<T> is the read-only view, the same type as the generic
// view<T, F>; spelling T const names that same type. Writing has its own
// name.
static_assert(std::is_same_v<psio::pssz_view<Rec>, psio::view<Rec, psio::pssz>>);
static_assert(std::is_same_v<psio::pssz_view<Rec>,
                             psio::view<Rec, psio::pssz, psio::storage::const_borrow>>);
static_assert(std::is_same_v<psio::pssz_view<const Rec>, psio::pssz_view<Rec>>);
static_assert(std::is_same_v<psio::ssz_view<const Rec>, psio::view<Rec, psio::ssz>>);
static_assert(std::is_same_v<psio::pssz_mutable_view<Rec>, psio::mutable_view<Rec, psio::pssz>>);

// The generic spelling view<const T, F> is a distinct class that is a
// view<T, F> and converts implicitly both ways.
static_assert(std::is_base_of_v<psio::view<Rec, psio::pssz>, psio::view<const Rec, psio::pssz>>);
static_assert(std::is_convertible_v<psio::view<const Rec, psio::pssz>, psio::view<Rec, psio::pssz>>);
static_assert(std::is_convertible_v<psio::view<Rec, psio::pssz>, psio::view<const Rec, psio::pssz>>);
static_assert(sizeof(psio::view<const Rec, psio::pssz>) == sizeof(psio::view<Rec, psio::pssz>));

// ── Const safety ────────────────────────────────────────────────────────
//
// No read-only view has a write, under any spelling.
template <typename V>
concept has_set = requires(V v) { v.set(std::uint32_t{1}); } ||
                  requires(V v) { v.set(Point{}); } ||
                  requires(V v) { v.template get<0>().set(std::int32_t{1}); };
static_assert(has_set<psio::pssz_mutable_view<Point>>);
static_assert(has_set<psio::pssz_mutable_view<std::uint32_t>>);
static_assert(!has_set<psio::pssz_view<Point>>);
static_assert(!has_set<psio::pssz_view<const Point>>);
static_assert(!has_set<psio::view<Point, psio::pssz>>);
static_assert(!has_set<psio::view<const Point, psio::pssz>>);
static_assert(!has_set<psio::pssz_view<std::uint32_t>>);
static_assert(!has_set<psio::view<const std::uint32_t, psio::pssz>>);
static_assert(!has_set<psio::ssz_view<Point>>);

// A mutable view is made from mutable bytes only: not from const bytes, a
// read-only view, a const buffer or a shared buffer, and no read-only view
// converts to one.
using MutPoint = psio::pssz_mutable_view<Point>;
static_assert(std::is_constructible_v<MutPoint, std::vector<char>&>);
static_assert(std::is_constructible_v<MutPoint, std::span<char>>);
static_assert(!std::is_constructible_v<MutPoint, const std::vector<char>&>);
static_assert(!std::is_constructible_v<MutPoint, std::span<const char>>);
static_assert(!std::is_constructible_v<MutPoint, std::string_view>);
static_assert(!std::is_constructible_v<MutPoint, psio::pssz_view<Point>>);
static_assert(!std::is_constructible_v<MutPoint, psio::view<const Point, psio::pssz>>);
static_assert(!std::is_constructible_v<MutPoint, psio::pssz_buffer<Point>&>);
static_assert(!std::is_constructible_v<MutPoint, psio::shared_pssz<Point>&>);
template <typename B>
concept gives_mutable_view = requires(B b) { psio::as_mutable_view(b); };
template <typename B>
concept gives_mutable_bytes = requires(B b) { psio::mutable_bytes(b); };
static_assert(gives_mutable_view<psio::pssz_buffer<Point>&>);
static_assert(!gives_mutable_view<const psio::pssz_buffer<Point>&>);
static_assert(!gives_mutable_view<psio::shared_pssz<Point>&>);
static_assert(!gives_mutable_view<psio::buffer<Point, psio::pssz, psio::storage::const_borrow>&>);
static_assert(!gives_mutable_bytes<const psio::pssz_buffer<Point>&>);
static_assert(!gives_mutable_bytes<psio::shared_pssz<Point>&>);

static_assert(std::is_same_v<psio::pssz_buffer<Rec>, psio::buffer<Rec, psio::pssz>>);
static_assert(std::is_same_v<psio::shared_pssz<Rec>,
                             psio::buffer<Rec, psio::pssz, psio::storage::shared>>);
static_assert(std::is_same_v<decltype(psio::to_pssz(sample)), std::vector<char>>);
static_assert(std::is_same_v<decltype(psio::to_json(sample)), std::string>);
static_assert(std::is_same_v<decltype(psio::to_key(std::uint32_t{})), std::array<char, 4>>);
static_assert(std::is_same_v<decltype(psio::to_key(std::string{})), std::vector<char>>);

// ── Views ───────────────────────────────────────────────────────────────

TEST_CASE("helpers: views read", "[helpers]")
{
   const auto pssz = psio::to_pssz(sample);
   psio::pssz_view<Rec> pv{pssz};
   CHECK(pv.get<0>() == 7u);
   CHECK(pv.get<1>().view_() == "alice");
   CHECK(pv.get<2>().size() == 3);
   CHECK(pv.get<2>()[2].get() == 3u);

   const auto ssz = psio::to_ssz(sample);
   psio::ssz_view<Rec> sv{ssz};
   CHECK(sv.get<0>() == 7u);
   CHECK(sv.get<1>().view_() == "alice");

   const auto wit = psio::to_wit(sample);
   psio::wit_view<Rec> wv{wit};
   CHECK(wv.get<0>() == 7u);
   CHECK(wv.get<1>().view_() == "alice");
}

TEST_CASE("helpers: view<const T, F> and view<T, F> are one read-only view", "[helpers]")
{
   const auto bytes = psio::to_pssz(sample);  // const bytes: a view needs no more

   psio::view<const Rec, psio::pssz> c{bytes};
   psio::view<Rec, psio::pssz>       v{bytes};
   CHECK(c.get<0>() == 7u);
   CHECK(c.get<1>().view_() == v.get<1>().view_());
   CHECK(psio::bytes(c).data() == psio::bytes(v).data());
   CHECK(psio::size(c) == bytes.size());

   // Each converts to the other, so either can be passed for either.
   psio::view<Rec, psio::pssz>       from_const = c;
   psio::view<const Rec, psio::pssz> from_plain = v;
   CHECK(from_const.get<2>()[2].get() == 3u);
   CHECK(from_plain.get<2>()[2].get() == 3u);
   const auto takes_plain = [](psio::pssz_view<Rec> x) { return x.get<0>(); };
   const auto takes_const = [](psio::view<const Rec, psio::pssz> x) { return x.get<0>(); };
   CHECK(takes_plain(c) == 7u);
   CHECK(takes_const(v) == 7u);

   // The named alias with const is the plain type itself.
   psio::pssz_view<const Rec> named{bytes};
   CHECK(takes_plain(named) == 7u);

   const auto five = psio::to_pssz(std::uint32_t{5});
   psio::view<const std::uint32_t, psio::pssz> scalar{five};
   CHECK(scalar.get() == 5u);
}

TEST_CASE("helpers: a mutable view writes into the borrowed bytes", "[helpers]")
{
   auto bytes = psio::to_pssz(Point{1, 2});
   psio::pssz_mutable_view<Point> mv{bytes};
   CHECK(mv.get<0>().get() == 1);
   REQUIRE(mv.get<1>().set(40));
   CHECK(bytes == psio::to_pssz(Point{1, 40}));
   CHECK(psio::from_pssz<Point>(bytes) == Point{1, 40});

   // The read view over the same bytes sees the write.
   psio::pssz_view<Point> cv{bytes};
   CHECK(cv.get<1>() == 40);

   // A member of a record with variable-size members is writable too.
   auto rec = psio::to_pssz(sample);
   psio::pssz_mutable_view<Rec> rv{rec};
   REQUIRE(rv.get<0>().set(9u));
   REQUIRE(rv.get<2>()[1].set(20u));
   CHECK(psio::from_pssz<Rec>(rec) == Rec{9, "alice", {1, 20, 3}});
}

// ── Typed buffers ───────────────────────────────────────────────────────

TEST_CASE("helpers: shared bytes are owned by every copy", "[helpers]")
{
   auto              bytes   = psio::to_pssz(sample);
   const char* const address = bytes.data();

   // Adopting a fresh buffer copies no byte.
   psio::shared_pssz<Rec> tx{std::move(bytes)};
   CHECK(psio::bytes(tx).data() == address);
   CHECK(psio::size(tx) == psio::pssz_size(sample));

   psio::shared_pssz<Rec> held;
   CHECK(psio::size(held) == 0);
   {
      psio::shared_pssz<Rec> copy = tx;
      CHECK(psio::bytes(copy).data() == address);
      held = copy;
   }
   tx = {};
   // The last owner still reads the same bytes.
   REQUIRE(psio::bytes(held).data() == address);
   CHECK(psio::as_view(held).get<1>().view_() == "alice");
   CHECK(psio::from_pssz<Rec>(psio::bytes(held)) == sample);

   // to_shared: an owning rvalue gives up its bytes, a borrow is copied,
   // a shared buffer is shared again.
   psio::pssz_buffer<Rec> owned{psio::to_pssz(sample)};
   const char* const      owned_address = psio::bytes(owned).data();
   auto                   from_borrow   = psio::to_shared(owned);
   CHECK(psio::bytes(from_borrow).data() != owned_address);
   auto from_owned = psio::to_shared(std::move(owned));
   CHECK(psio::bytes(from_owned).data() == owned_address);
   auto again = psio::to_shared(from_owned);
   CHECK(psio::bytes(again).data() == owned_address);
   CHECK(psio::bytes(psio::to_buffer(again)).data() != owned_address);
}

TEST_CASE("helpers: an owning buffer gives both views", "[helpers]")
{
   psio::pssz_buffer<Point> buf{psio::to_pssz(Point{3, 4})};
   REQUIRE(psio::as_mutable_view(buf).get<0>().set(30));
   CHECK(psio::as_view(buf).get<0>() == 30);
   CHECK(psio::from_pssz<Point>(psio::bytes(buf)) == Point{30, 4});
}

// ── Decoding ────────────────────────────────────────────────────────────

TEST_CASE("helpers: the checked decode accepts what validate accepts", "[helpers]")
{
   // JSON: psio::decode fills a missing member with its default; validate
   // rejects the text, and so does the checked decode.
   const std::string_view partial = R"({"id":7,"name":"alice"})";
   const auto             text    = span_of(partial);
   CHECK_FALSE(psio::validate_json<Rec>(text).ok());
   CHECK_THROWS_AS(psio::from_json<Rec>(text), psio::codec_exception);
   CHECK(psio::from_json<Rec>(psio::trusted{text}) == Rec{7, "alice", {}});

   // A string literal is its characters, without the terminator.
   CHECK(psio::from_json<Rec>(R"({"id":7,"name":"alice","xs":[1,2,3]})") == sample);
   CHECK(psio::from_ijson<Rec>(R"({"id":7,"name":"alice","xs":[1,2,3]})") == sample);
   CHECK(psio::validate_json<Rec>(R"({"id":7,"name":"alice","xs":[1,2,3]})").ok());
   CHECK_FALSE(psio::validate_json<Rec>(R"({"id":7)").ok());

   // The exception carries the validator's error.
   const auto pssz = psio::to_pssz(sample);
   const auto cut  = span_of(pssz).first(3);
   auto       st   = psio::validate_pssz<Rec>(cut);
   REQUIRE_FALSE(st.ok());
   try
   {
      (void)psio::from_pssz<Rec>(cut);
      FAIL("from_pssz accepted a truncated frame");
   }
   catch (const psio::codec_exception& e)
   {
      CHECK(e.error().what == st.error().what);
      CHECK(e.error().format_name == st.error().format_name);
   }
}

TEST_CASE("helpers: a key of fixed size is an array", "[helpers]")
{
   // A fixed-size key is a std::array on the stack; psio::encode gives the
   // same bytes in a vector.
   const auto fixed = psio::to_key(std::uint32_t{0x01020304});
   CHECK(std::vector<char>(fixed.begin(), fixed.end()) ==
         psio::encode(psio::key{}, std::uint32_t{0x01020304}));
   CHECK(psio::from_key<std::uint32_t>(fixed) == 0x01020304u);
   CHECK(psio::key_size(std::uint32_t{}) == 4);
   CHECK_THROWS_AS(psio::from_key<std::uint32_t>(span_of(fixed).first(3)), psio::codec_exception);
   CHECK(psio::to_key(std::uint32_t{1}) < psio::to_key(std::uint32_t{2}));
}

TEST_CASE("helpers: named apart from the tag", "[helpers]")
{
   const auto bytes = psio::to_toy(42);
   CHECK(psio::from_toy<int>(bytes) == 42);
   CHECK(psio::from_toy<int>(psio::trusted{bytes}) == 42);
   CHECK(psio::validate_toy<int>(bytes).ok());
   CHECK_FALSE(psio::validate_toy<int>(span_of(bytes).first(2)).ok());
   CHECK_THROWS_AS(psio::from_toy<int>(span_of(bytes).first(2)), psio::codec_exception);
   psio::shared_toy<int> shared{span_of(bytes)};
   CHECK(psio::from_toy<int>(psio::bytes(shared)) == 42);
}
