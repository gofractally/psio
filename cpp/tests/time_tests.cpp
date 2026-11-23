// Time as a first-class value (docs/time.md): std::chrono durations and
// system_clock time points in pssz / bin / ssz / borsh / bincode / key / json /
// ijson / schema.

#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>
#include <psio/ssz.hpp>
#include <psio/time.hpp>

#include <catch.hpp>

#include <chrono>
#include <random>

using namespace std::chrono;

// (global scope: PSIO_REFLECT with attributes specializes psio::annotate)
   using sys_millis = sys_time<milliseconds>;
   using sys_micros = sys_time<microseconds>;
   using sys_nanos  = sys_time<nanoseconds>;
   using sys_sec32  = sys_time<duration<std::uint32_t>>;  // a 32-bit unsigned seconds stamp
   using sys_deci   = sys_time<duration<std::int64_t, std::deci>>;
   using minutes32  = duration<std::int32_t, std::ratio<60>>;

   template <typename T>
   std::string text(const T& v)
   {
      return psio::to_rfc3339(v);
   }

   // A fixed-layout record of time values, and the same layout in integers.
   struct Stamps
   {
      sys_seconds  created;
      sys_micros   updated;
      milliseconds ttl{};
      sys_sec32    legacy;
      friend bool  operator==(const Stamps&, const Stamps&) = default;
   };
   PSIO_REFLECT(Stamps, created, updated, ttl, legacy, definitionWillNotChange())
   struct StampInts
   {
      std::int64_t  created = 0;
      std::int64_t  updated = 0;
      std::int64_t  ttl     = 0;
      std::uint32_t legacy  = 0;
      friend bool   operator==(const StampInts&, const StampInts&) = default;
   };
   PSIO_REFLECT(StampInts, created, updated, ttl, legacy, definitionWillNotChange())

   // An extensible record mixing time with variable members.
   struct Event
   {
      std::string               name;
      sys_millis                at;
      std::optional<sys_nanos>  precise;
      std::vector<sys_seconds>  history;
      nanoseconds               elapsed{};
      minutes32                 timeout{};
      friend bool operator==(const Event&, const Event&) = default;
   };
   PSIO_REFLECT(Event, name, at, precise, history, elapsed, timeout)
   struct EventInts
   {
      std::string                 name;
      std::int64_t                at = 0;
      std::optional<std::int64_t> precise;
      std::vector<std::int64_t>   history;
      std::int64_t                elapsed = 0;
      std::int32_t                timeout = 0;
   };
   PSIO_REFLECT(EventInts, name, at, precise, history, elapsed, timeout)

   struct Forced
   {
      seconds small{};
      seconds forced{};
      friend bool operator==(const Forced&, const Forced&) = default;
   };
   PSIO_REFLECT(Forced, small, forced)
   PSIO_FIELD_ATTRS(Forced, forced, psio::json_string | psio::ijson_string)

   const Stamps stamps{sys_seconds{seconds{1795342530}}, sys_micros{microseconds{-1}},
                       milliseconds{1500}, sys_sec32{duration<std::uint32_t>{4000000000u}}};
   const StampInts stamp_ints{1795342530, -1, 1500, 4000000000u};

   const Event event{"deploy",
                     sys_millis{milliseconds{1795342530123}},
                     sys_nanos{nanoseconds{1795342530123456789}},
                     {sys_seconds{seconds{0}}, sys_seconds{seconds{-86400}}},
                     nanoseconds{9007199254740993},
                     minutes32{-90}};
   const EventInts event_ints{"deploy", 1795342530123, 1795342530123456789, {0, -86400},
                              9007199254740993, -90};

// ── Compile-time facts ─────────────────────────────────────────────────────
static_assert(psio::TimeValue<seconds> && psio::DurationValue<seconds>);
static_assert(psio::TimeValue<sys_seconds> && psio::TimePointValue<sys_seconds>);
static_assert(!psio::TimeValue<std::int64_t> && !psio::TimeValue<std::string>);
static_assert(std::is_same_v<psio::time_rep_t<sys_sec32>, std::uint32_t>);
static_assert(psio::TextTimePoint<sys_seconds> && psio::TextTimePoint<sys_nanos> &&
              psio::TextTimePoint<sys_deci>);
static_assert(!psio::TextTimePoint<sys_time<minutes>>);  // no RFC 3339 form
static_assert(!psio::TextTimePoint<seconds>);
// The text codec is constexpr: no locale, no allocation, no library calls.
static_assert(psio::from_rfc3339<sys_seconds>("2026-11-22T10:15:30Z") ==
              sys_seconds{seconds{1795342530}});
static_assert(psio::from_rfc3339<sys_micros>("1969-12-31T23:59:59.999999Z") ==
              sys_micros{microseconds{-1}});
static_assert(!psio::from_rfc3339<sys_seconds>("2026-11-22T10:15:60Z"));
// A fixed-layout record of time values is fixed-size in pssz.
static_assert(psio::detail::pssz_impl::is_fixed_v<Stamps>);
static_assert(psio::detail::pssz_impl::fixed_size_of<Stamps>() == 28);
static_assert(psio::inferred_bound<Stamps>() == psio::inferred_bound<StampInts>());

TEST_CASE("time: text form has exactly the unit's precision", "[time][text]")
{
   REQUIRE(text(sys_seconds{}) == "1970-01-01T00:00:00Z");
   REQUIRE(text(sys_seconds{seconds{1795342530}}) == "2026-11-22T10:15:30Z");
   REQUIRE(text(sys_millis{milliseconds{1795342530123}}) == "2026-11-22T10:15:30.123Z");
   REQUIRE(text(sys_micros{microseconds{1795342530123456}}) == "2026-11-22T10:15:30.123456Z");
   REQUIRE(text(sys_nanos{nanoseconds{1795342530123456789}}) ==
           "2026-11-22T10:15:30.123456789Z");
   // Trailing zeros are kept: the digit count is the unit, not the value.
   REQUIRE(text(sys_millis{}) == "1970-01-01T00:00:00.000Z");
   REQUIRE(text(sys_nanos{nanoseconds{1}}) == "1970-01-01T00:00:00.000000001Z");
   REQUIRE(text(sys_deci{duration<std::int64_t, std::deci>{15}}) == "1970-01-01T00:00:01.5Z");
   REQUIRE(text(sys_sec32{duration<std::uint32_t>{4294967295u}}) == "2106-02-07T06:28:15Z");

   // Before the epoch: floor, never a negative fraction.
   REQUIRE(text(sys_seconds{seconds{-1}}) == "1969-12-31T23:59:59Z");
   REQUIRE(text(sys_millis{milliseconds{-1}}) == "1969-12-31T23:59:59.999Z");
   REQUIRE(text(sys_micros{microseconds{-1000001}}) == "1969-12-31T23:59:58.999999Z");
   REQUIRE(text(sys_seconds{seconds{-2208988800}}) == "1900-01-01T00:00:00Z");

   // Leap days, century rules.
   REQUIRE(text(sys_seconds{seconds{951782400}}) == "2000-02-29T00:00:00Z");
   REQUIRE(text(sys_seconds{seconds{1709251199}}) == "2024-02-29T23:59:59Z");
   REQUIRE(text(sys_seconds{seconds{4107542400}}) == "2100-03-01T00:00:00Z");  // 2100 is not leap

   // The year range, to the last tick.
   REQUIRE(text(sys_seconds{seconds{-62167219200}}) == "0000-01-01T00:00:00Z");
   REQUIRE(text(sys_seconds{seconds{-62162035201}}) == "0000-02-29T23:59:59Z");  // year 0 is leap
   REQUIRE(text(sys_seconds{seconds{-62135596801}}) == "0000-12-31T23:59:59Z");
   REQUIRE(text(sys_seconds{seconds{253402300799}}) == "9999-12-31T23:59:59Z");
   REQUIRE(text(sys_micros{microseconds{253402300799999999}}) == "9999-12-31T23:59:59.999999Z");
   REQUIRE(text(sys_micros{microseconds{-62167219200000000}}) == "0000-01-01T00:00:00.000000Z");
   // Outside it there is no text.
   REQUIRE(text(sys_seconds{seconds{-62167219201}}).empty());
   REQUIRE(text(sys_seconds{seconds{253402300800}}).empty());
   REQUIRE(text(sys_micros{microseconds{253402300800000000}}).empty());
   REQUIRE(text(sys_micros{microseconds{-62167219200000001}}).empty());
   REQUIRE(text(sys_seconds::max()).empty());
   REQUIRE(text(sys_seconds::min()).empty());
   REQUIRE(text(sys_micros::max()).empty());
   REQUIRE(text(sys_micros::min()).empty());
   // Every int64 nanosecond count is inside the range (1677..2262).
   REQUIRE(text(sys_nanos::max()) == "2262-04-11T23:47:16.854775807Z");
   REQUIRE(text(sys_nanos::min()) == "1677-09-21T00:12:43.145224192Z");
}

TEST_CASE("time: reading accepts RFC 3339 and nothing else", "[time][text]")
{
   auto sec = [](std::string_view s) { return psio::from_rfc3339<sys_seconds>(s); };
   auto ms  = [](std::string_view s) { return psio::from_rfc3339<sys_millis>(s); };
   auto ns  = [](std::string_view s) { return psio::from_rfc3339<sys_nanos>(s); };
   const sys_seconds t{seconds{1795342530}};

   REQUIRE(sec("2026-11-22T10:15:30Z") == t);
   REQUIRE(sec("2026-11-22t10:15:30z") == t);  // RFC 3339 5.6: either case
   // Offsets are converted to UTC.
   REQUIRE(sec("2026-11-22T12:15:30+02:00") == t);
   REQUIRE(sec("2026-11-22T04:45:30-05:30") == t);
   REQUIRE(sec("2026-11-22T10:15:30+00:00") == t);
   REQUIRE(sec("2026-11-22T10:15:30-00:00") == t);
   REQUIRE(sec("2026-11-23T00:15:30+14:00") == t);
   REQUIRE(sec("2026-11-21T11:15:30-23:00") == t);
   REQUIRE(sec("2027-01-01T00:30:00+01:00") == sys_seconds{seconds{1798759800}});  // year rollover

   // A fraction of any length, when the unit holds it exactly.
   REQUIRE(sec("2026-11-22T10:15:30.0Z") == t);
   REQUIRE(sec("2026-11-22T10:15:30.000000000000000000000Z") == t);
   REQUIRE(ms("2026-11-22T10:15:30Z") == sys_millis{t});
   REQUIRE(ms("2026-11-22T10:15:30.1Z") == sys_millis{t} + milliseconds{100});
   REQUIRE(ms("2026-11-22T10:15:30.123Z") == sys_millis{t} + milliseconds{123});
   REQUIRE(ms("2026-11-22T10:15:30.123000Z") == sys_millis{t} + milliseconds{123});
   REQUIRE(ms("2026-11-22T12:15:30.123+02:00") == sys_millis{t} + milliseconds{123});
   REQUIRE(ns("2026-11-22T10:15:30.123456789Z") == sys_nanos{t} + nanoseconds{123456789});
   REQUIRE(ns("2026-11-22T10:15:30.1234567890Z") == sys_nanos{t} + nanoseconds{123456789});
   // ... and rejected when it would lose precision.
   REQUIRE_FALSE(sec("2026-11-22T10:15:30.5Z"));
   REQUIRE_FALSE(sec("2026-11-22T10:15:30.000000001Z"));
   REQUIRE_FALSE(ms("2026-11-22T10:15:30.1234Z"));
   REQUIRE_FALSE(ms("2026-11-22T10:15:30.1230001Z"));
   REQUIRE_FALSE(ns("2026-11-22T10:15:30.1234567891Z"));

   // Leap seconds, hour 24 and impossible dates are rejected.
   REQUIRE_FALSE(sec("2016-12-31T23:59:60Z"));
   REQUIRE_FALSE(sec("2026-11-22T24:00:00Z"));
   REQUIRE_FALSE(sec("2026-11-22T10:60:00Z"));
   REQUIRE_FALSE(sec("2026-02-29T00:00:00Z"));
   REQUIRE_FALSE(sec("2100-02-29T00:00:00Z"));
   REQUIRE(sec("2000-02-29T00:00:00Z") == sys_seconds{seconds{951782400}});
   REQUIRE(sec("2024-02-29T23:59:59Z") == sys_seconds{seconds{1709251199}});
   REQUIRE_FALSE(sec("2026-04-31T00:00:00Z"));
   REQUIRE_FALSE(sec("2026-00-10T00:00:00Z"));
   REQUIRE_FALSE(sec("2026-13-10T00:00:00Z"));
   REQUIRE_FALSE(sec("2026-11-00T00:00:00Z"));

   // Not the grammar.
   for (std::string_view bad :
        {"", "2026-11-22", "2026-11-22T10:15:30", "2026-11-22 10:15:30Z", "2026-11-22T10:15Z",
         "2026-11-22T10:15:30.Z", "2026-11-22T10:15:30,5Z", "2026-11-22T10:15:30ZZ",
         "2026-11-22T10:15:30Z ", " 2026-11-22T10:15:30Z", "2026-11-22T10:15:30+0200",
         "2026-11-22T10:15:30+02", "2026-11-22T10:15:30+02:0", "2026-11-22T10:15:30+24:00",
         "2026-11-22T10:15:30+02:60", "2026-11-22T10:15:30 02:00", "20261122T101530Z",
         "2026-11-22T10:15:3Z", "+2026-11-22T10:15:30Z", "-026-11-22T10:15:30Z",
         "02026-11-22T10:15:30Z", "2026-1-22T10:15:30Z", "2026-11-22T1O:15:30Z",
         "2026-11-22T10:15:30.12a4Z", "1795342530"})
   {
      INFO(bad);
      REQUIRE_FALSE(sec(bad));
      REQUIRE_FALSE(ns(bad));
   }

   // The year range applies to the instant, after the offset.
   REQUIRE(sec("0000-01-01T00:00:00Z") == sys_seconds{seconds{-62167219200}});
   REQUIRE(sec("9999-12-31T23:59:59Z") == sys_seconds{seconds{253402300799}});
   REQUIRE_FALSE(sec("0000-01-01T00:00:00+00:01"));
   REQUIRE_FALSE(sec("9999-12-31T23:59:59-00:01"));
   REQUIRE(sec("0000-01-01T00:01:00+00:01") == sys_seconds{seconds{-62167219200}});

   // A tick count that does not fit Rep is rejected, not wrapped.
   auto s32 = [](std::string_view s) { return psio::from_rfc3339<sys_sec32>(s); };
   REQUIRE(s32("2106-02-07T06:28:15Z") == sys_sec32{duration<std::uint32_t>{4294967295u}});
   REQUIRE_FALSE(s32("2106-02-07T06:28:16Z"));
   REQUIRE_FALSE(s32("1969-12-31T23:59:59Z"));
   REQUIRE(ns("2262-04-11T23:47:16.854775807Z") == sys_nanos::max());
   REQUIRE_FALSE(ns("2262-04-11T23:47:16.854775808Z"));
   REQUIRE(ns("1677-09-21T00:12:43.145224192Z") == sys_nanos::min());
   REQUIRE_FALSE(ns("1677-09-21T00:12:43.145224191Z"));
   REQUIRE_FALSE(ns("9999-12-31T23:59:59Z"));
}

TEST_CASE("time: text agrees with std::chrono's calendar and round-trips", "[time][text]")
{
   std::mt19937_64 rng{20261122};
   for (int i = 0; i < 200000; ++i)
   {
      const std::int64_t secs =
         -62167219200 + static_cast<std::int64_t>(rng() % (253402300800ull + 62167219200ull));
      const sys_seconds tp{seconds{secs}};
      const auto        day = floor<days>(tp);
      const year_month_day ymd{day};
      const hh_mm_ss       tod{tp - day};
      char                 want[32];
      std::snprintf(want, sizeof(want), "%04d-%02u-%02uT%02d:%02d:%02dZ", int(ymd.year()),
                    unsigned(ymd.month()), unsigned(ymd.day()), int(tod.hours().count()),
                    int(tod.minutes().count()), int(tod.seconds().count()));
      const std::string got = text(tp);
      REQUIRE(got == want);
      REQUIRE(psio::from_rfc3339<sys_seconds>(got) == tp);

      const std::int64_t sub = static_cast<std::int64_t>(rng() % 1000000);
      const sys_micros   us{microseconds{secs * 1000000 + sub}};
      REQUIRE(psio::from_rfc3339<sys_micros>(text(us)) == us);
      REQUIRE(text(us).substr(0, 19) == got.substr(0, 19));
   }
   for (int i = 0; i < 100000; ++i)
   {
      const sys_nanos ns{nanoseconds{static_cast<std::int64_t>(rng())}};
      REQUIRE(psio::from_rfc3339<sys_nanos>(text(ns)) == ns);
   }
}

TEST_CASE("time: binary formats carry the tick count", "[time][pssz][bin][ssz]")
{
   // Byte-identical to the same record declared with integers.
   REQUIRE(psio::encode(psio::pssz{}, stamps) == psio::encode(psio::pssz{}, stamp_ints));
   REQUIRE(psio::encode(psio::pssz{}, stamps).size() == 28);
   REQUIRE(psio::encode(psio::bin{}, stamps) == psio::encode(psio::bin{}, stamp_ints));
   REQUIRE(psio::encode(psio::ssz{}, stamps) == psio::encode(psio::ssz{}, stamp_ints));
   REQUIRE(psio::encode(psio::pssz{}, event) == psio::encode(psio::pssz{}, event_ints));
   REQUIRE(psio::encode(psio::bin{}, event) == psio::encode(psio::bin{}, event_ints));
   REQUIRE(psio::encode(psio::ssz{}, event) == psio::encode(psio::ssz{}, event_ints));
   REQUIRE(psio::encode(psio::borsh{}, stamps) == psio::encode(psio::borsh{}, stamp_ints));
   REQUIRE(psio::encode(psio::borsh{}, event) == psio::encode(psio::borsh{}, event_ints));
   REQUIRE(psio::encode(psio::bincode{}, stamps) == psio::encode(psio::bincode{}, stamp_ints));
   REQUIRE(psio::encode(psio::bincode{}, event) == psio::encode(psio::bincode{}, event_ints));

   auto round = [](auto fmt, const auto& v) {
      using T          = std::remove_cvref_t<decltype(v)>;
      const auto bytes = psio::encode(fmt, v);
      REQUIRE(psio::size_of(fmt, v) == bytes.size());
      REQUIRE(psio::validate<T>(fmt, std::span<const char>{bytes}).ok());
      REQUIRE(psio::decode<T>(fmt, std::span<const char>{bytes}) == v);
   };
   round(psio::pssz{}, stamps);
   round(psio::pssz{}, event);
   round(psio::bin{}, stamps);
   round(psio::bin{}, event);
   round(psio::ssz{}, stamps);
   round(psio::ssz{}, event);
   round(psio::borsh{}, stamps);
   round(psio::borsh{}, event);
   round(psio::bincode{}, stamps);
   round(psio::bincode{}, event);
   round(psio::key{}, stamps);
   round(psio::key{}, event);

   // Bare values, at the width of Rep, little-endian.
   auto hex = [](const std::vector<char>& b) {
      std::string out;
      for (unsigned char c : b)
      {
         out += "0123456789abcdef"[c >> 4];
         out += "0123456789abcdef"[c & 15];
      }
      return out;
   };
   REQUIRE(hex(psio::encode(psio::pssz{}, sys_seconds{seconds{1795342530}})) == "c2c0026b00000000");
   REQUIRE(hex(psio::encode(psio::pssz{}, sys_micros{microseconds{-1}})) == "ffffffffffffffff");
   REQUIRE(hex(psio::encode(psio::pssz{}, minutes32{-90})) == "a6ffffff");
   REQUIRE(hex(psio::encode(psio::bin{}, sys_sec32{duration<std::uint32_t>{1}})) == "01000000");
   REQUIRE(hex(psio::encode(psio::ssz{}, milliseconds{258})) == "0201000000000000");
   round(psio::pssz{}, sys_nanos::min());
   round(psio::pssz{}, std::vector<sys_seconds>{sys_seconds::min(), {}, sys_seconds::max()});
   round(psio::pssz{}, std::optional<sys_millis>{sys_millis{milliseconds{5}}});
   round(psio::pssz{}, std::optional<sys_millis>{});
   round(psio::pssz{}, std::array<milliseconds, 2>{milliseconds{1}, milliseconds{-1}});
   round(psio::pssz{}, std::tuple{sys_seconds{seconds{9}}, std::string{"x"}});
   // Zero-copy views read time members by value.
   {
      const auto wire = psio::encode(psio::pssz{}, stamps);
      auto       view = psio::view<Stamps, psio::pssz, psio::storage::const_borrow>{wire};
      REQUIRE(view.get<0>().get() == stamps.created);
      REQUIRE(view.get<1>().get() == stamps.updated);
      REQUIRE(view.get<2>().get() == stamps.ttl);
      REQUIRE(view.get<3>().get() == stamps.legacy);
      const auto wire2 = psio::encode(psio::pssz{}, event);
      auto       view2 = psio::view<Event, psio::pssz, psio::storage::const_borrow>{wire2};
      REQUIRE(view2.get<1>().get() == event.at);
      REQUIRE(view2.get<2>().has_value());
      REQUIRE(view2.get<2>().get().get() == *event.precise);
      REQUIRE(view2.get<4>().get() == event.elapsed);
      REQUIRE(view2.get<5>().get() == event.timeout);
   }
   // A wrong-size payload is rejected.
   REQUIRE_FALSE(psio::validate<sys_seconds>(psio::pssz{}, std::span<const char>{"1234567", 7}).ok());
}

TEST_CASE("time: json and ijson", "[time][json]")
{
   REQUIRE(psio::encode(psio::json{}, stamps) ==
           R"({"created":"2026-11-22T10:15:30Z","updated":"1969-12-31T23:59:59.999999Z",)"
           R"("ttl":1500,"legacy":"2096-10-02T07:06:40Z"})");
   REQUIRE(psio::encode(psio::ijson{}, stamps) == psio::encode(psio::json{}, stamps));

   // A duration follows the integer rules of the format: under ijson a
   // 64-bit count beyond 2^53 - 1 is a string.
   const std::string json =
      R"({"name":"deploy","at":"2026-11-22T10:15:30.123Z",)"
      R"("precise":"2026-11-22T10:15:30.123456789Z",)"
      R"("history":["1970-01-01T00:00:00Z","1969-12-31T00:00:00Z"],)"
      R"("elapsed":9007199254740993,"timeout":-90})";
   std::string ijson = json;
   ijson.replace(ijson.find("9007199254740993"), 16, "\"9007199254740993\"");
   REQUIRE(psio::encode(psio::json{}, event) == json);
   REQUIRE(psio::encode(psio::ijson{}, event) == ijson);
   REQUIRE(psio::decode<Event>(psio::json{}, std::span<const char>{json}) == event);
   REQUIRE(psio::decode<Event>(psio::ijson{}, std::span<const char>{ijson}) == event);
   REQUIRE(psio::validate<Event>(psio::ijson{}, std::span<const char>{ijson}).ok());
   // ijson refuses the unsafe bare number, as for any 64-bit integer.
   REQUIRE_FALSE(psio::validate<Event>(psio::ijson{}, std::span<const char>{json}).ok());

   // Member integer-form annotations apply to durations.
   const Forced forced{seconds{5}, seconds{6}};
   REQUIRE(psio::encode(psio::json{}, forced) == R"({"small":5,"forced":"6"})");
   REQUIRE(psio::encode(psio::ijson{}, forced) == R"({"small":5,"forced":"6"})");
   REQUIRE(psio::decode<Forced>(psio::json{}, std::string_view{R"({"small":5,"forced":"6"})"}) ==
           forced);

   // Bare values and containers.
   REQUIRE(psio::encode(psio::json{}, sys_seconds{}) == R"("1970-01-01T00:00:00Z")");
   REQUIRE(psio::encode(psio::json{}, std::optional<sys_seconds>{}) == "null");
   REQUIRE(psio::encode(psio::json{}, minutes32{7}) == "7");
   REQUIRE(psio::encode(psio::ijson{}, nanoseconds::max()) == R"("9223372036854775807")");
   REQUIRE(psio::encode(psio::json{}, nanoseconds::min()) == "-9223372036854775808");

   auto read = [](std::string_view s) {
      sys_millis out{};
      const bool ok = psio::decode_json_checked(psio::json{}, std::span<const char>{s}, out).ok();
      sys_millis out2{};
      REQUIRE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{s}, out2).ok() == ok);
      REQUIRE(psio::validate<sys_millis>(psio::json{}, std::span<const char>{s}).ok() == ok);
      REQUIRE(out == out2);
      return ok ? std::optional{out} : std::nullopt;
   };
   REQUIRE(read(R"("2026-11-22T12:15:30.5+02:00")") == sys_millis{milliseconds{1795342530500}});
   REQUIRE(read(R"( "2026-11-22T10:15:30Z" )") == sys_millis{milliseconds{1795342530000}});
   REQUIRE_FALSE(read(R"("2026-11-22T10:15:30.1234Z")"));  // finer than the unit
   REQUIRE_FALSE(read(R"("2026-11-22T10:15:60Z")"));       // leap second
   REQUIRE_FALSE(read(R"("2026-11-22T10:15:30")"));        // no offset
   REQUIRE_FALSE(read("1795342530000"));                   // a time point is not a number
   REQUIRE_FALSE(read("null"));
   // The JSON string's value is what is read, however it was escaped.
   REQUIRE(read("\"2026-11-22T10:15:30\\u005a\"") == sys_millis{milliseconds{1795342530000}});
   REQUIRE(read("\"2026-11-22\\u005410:15:30.25\\u002b00:00\"") ==
           sys_millis{milliseconds{1795342530250}});
   // A duration is not a string of anything but an integer.
   milliseconds d{};
   REQUIRE_FALSE(psio::decode_json_checked(psio::json{}, std::string_view{R"("PT1S")"}, d).ok());
   REQUIRE_FALSE(psio::decode_json_checked(psio::json{}, std::string_view{"1.5"}, d).ok());
   REQUIRE(psio::decode_json_checked(psio::json{}, std::string_view{"-3"}, d).ok());
   REQUIRE(d == milliseconds{-3});

   // A time point outside years 0000..9999 has no text: the checked writer
   // reports it and leaves the output alone; the fail-fast one throws.
   std::string out = "keep";
   REQUIRE_FALSE(psio::encode_json_checked(psio::json{}, sys_seconds::max(), out).ok());
   REQUIRE(out == "keep");
   Stamps far = stamps;
   far.updated = sys_micros::min();
   REQUIRE_FALSE(psio::encode_json_checked(psio::ijson{}, far, out).ok());
   REQUIRE(out == "keep");
   REQUIRE_THROWS_AS(psio::encode(psio::json{}, far), psio::codec_exception);
}

TEST_CASE("time: schema marks the tick integer with a time id", "[time][schema]")
{
   namespace ir = psio::schema_types;
   psio::SchemaBuilder builder;
   auto check = [&]<typename T>(std::type_identity<T>, std::string_view id, std::uint32_t bits,
                                bool is_signed) {
      auto        type   = builder.insert<T>();
      const auto& custom = std::get<ir::Custom>(type.resolve(builder.peek())->value);
      INFO(id);
      REQUIRE(custom.id == id);
      const auto& integer = std::get<ir::Int>(custom.type->resolve(builder.peek())->value);
      REQUIRE(integer.bits == bits);
      REQUIRE(integer.isSigned == is_signed);
      REQUIRE(psio::time_schema_id<T>() == id);
      // The id alone recovers point/duration and the period.
      const auto kind = psio::parse_time_schema_id(id);
      REQUIRE(kind.has_value());
      REQUIRE(kind->point == psio::TimePointValue<T>);
      REQUIRE(kind->num == psio::time_traits<T>::num);
      REQUIRE(kind->den == psio::time_traits<T>::den);
      REQUIRE(psio::time_schema_id(*kind) == id);
   };
   check(std::type_identity<sys_seconds>{}, "time-point-seconds", 64, true);
   check(std::type_identity<sys_millis>{}, "time-point-millis", 64, true);
   check(std::type_identity<sys_micros>{}, "time-point-micros", 64, true);
   check(std::type_identity<sys_nanos>{}, "time-point-nanos", 64, true);
   check(std::type_identity<sys_sec32>{}, "time-point-seconds", 32, false);
   check(std::type_identity<sys_deci>{}, "time-point-1/10", 64, true);
   check(std::type_identity<seconds>{}, "duration-seconds", 64, true);
   check(std::type_identity<milliseconds>{}, "duration-millis", 64, true);
   check(std::type_identity<microseconds>{}, "duration-micros", 64, true);
   check(std::type_identity<nanoseconds>{}, "duration-nanos", 64, true);
   check(std::type_identity<minutes32>{}, "duration-60/1", 32, true);
   check(std::type_identity<duration<std::int16_t, std::ratio<1, 60>>>{}, "duration-1/60", 16, true);
   check(std::type_identity<sys_time<duration<std::int32_t, std::ratio<86400>>>>{},
         "time-point-86400/1", 32, true);

   // Only canonical spellings are ids.
   for (std::string_view bad : {"time-point", "time-point-", "duration-1/1", "duration-1/1000",
                                "duration-2/4", "duration-01/7", "duration-1/0", "duration-0/1",
                                "duration-7", "duration-1/7 ", "time-seconds", "Duration-seconds",
                                "duration-minutes", "hex"})
   {
      INFO(bad);
      REQUIRE_FALSE(psio::parse_time_schema_id(bad).has_value());
   }
   REQUIRE(psio::time_fraction_digits(1, 1) == 0);
   REQUIRE(psio::time_fraction_digits(1, 1000000000) == 9);
   REQUIRE(psio::time_fraction_digits(1, 60) == -1);
   REQUIRE(psio::time_fraction_digits(60, 1) == -1);
}

// ── Monotonic time points (a clock other than system_clock) ────────────────
namespace
{
   using mono_seconds = time_point<steady_clock, seconds>;
   using mono_micros  = time_point<steady_clock, microseconds>;
   using mono_ms32    = time_point<steady_clock, duration<std::uint32_t, std::milli>>;
   // A user clock: any clock that is not system_clock is monotonic to PSIO.
   struct boot_clock
   {
      using rep        = std::int64_t;
      using period     = std::nano;
      using duration   = std::chrono::duration<rep, period>;
      using time_point = std::chrono::time_point<boot_clock>;
      static constexpr bool is_steady = true;
   };

   struct Timer
   {
      mono_micros                 expires;
      std::optional<mono_seconds> started;
      std::vector<mono_ms32>      laps;
      friend bool operator==(const Timer&, const Timer&) = default;
   };
   PSIO_REFLECT(Timer, expires, started, laps)
   struct TimerInts
   {
      std::int64_t                expires = 0;
      std::optional<std::int64_t> started;
      std::vector<std::uint32_t>  laps;
   };
   PSIO_REFLECT(TimerInts, expires, started, laps)
}  // namespace

static_assert(psio::TimeValue<mono_micros> && psio::MonotonicValue<mono_micros>);
static_assert(psio::MonotonicValue<steady_clock::time_point> &&
              psio::MonotonicValue<boot_clock::time_point>);
static_assert(!psio::TimePointValue<mono_micros> && !psio::DurationValue<mono_micros>);
static_assert(!psio::MonotonicValue<sys_micros> && !psio::MonotonicValue<microseconds>);
static_assert(psio::TickTextValue<mono_micros> && psio::TickTextValue<seconds> &&
              !psio::TickTextValue<sys_seconds>);
static_assert(!psio::TextTimePoint<mono_micros>);  // no RFC 3339 form, whatever the unit
static_assert(psio::detail::pssz_impl::is_fixed_v<mono_ms32> &&
              psio::detail::pssz_impl::fixed_size_of<mono_ms32>() == 4);
// A time value is its own type in an evolution check.
static_assert(!psio::pssz_evolves_from<mono_micros, sys_micros> &&
              !psio::pssz_evolves_from<mono_micros, microseconds> &&
              !psio::pssz_evolves_from<mono_micros, std::int64_t>);

TEST_CASE("time: a monotonic time point is its tick count in every format", "[time][monotonic]")
{
   const Timer timer{mono_micros{microseconds{9007199254740993}},
                     mono_seconds{seconds{-5}},
                     {mono_ms32{mono_ms32::duration{0}}, mono_ms32::max()}};
   const TimerInts ints{9007199254740993, -5, {0, 4294967295u}};

   // Binary and key: the tick integer.
   REQUIRE(psio::encode(psio::pssz{}, timer) == psio::encode(psio::pssz{}, ints));
   REQUIRE(psio::encode(psio::bin{}, timer) == psio::encode(psio::bin{}, ints));
   REQUIRE(psio::encode(psio::ssz{}, timer) == psio::encode(psio::ssz{}, ints));
   REQUIRE(psio::encode(psio::key{}, timer) == psio::encode(psio::key{}, ints));
   const auto bytes = psio::encode(psio::pssz{}, timer);
   REQUIRE(psio::validate<Timer>(psio::pssz{}, std::span<const char>{bytes}).ok());
   REQUIRE(psio::decode<Timer>(psio::pssz{}, std::span<const char>{bytes}) == timer);
   const auto key = psio::encode(psio::key{}, timer);
   REQUIRE(psio::decode<Timer>(psio::key{}, std::span<const char>{key}) == timer);
   {
      // Key order is tick order (bytes compared unsigned).
      const auto before = psio::to_key(mono_micros{microseconds{-1}});
      const auto after  = psio::to_key(mono_micros{});
      REQUIRE(std::memcmp(before.data(), after.data(), before.size()) < 0);
   }
   REQUIRE(psio::encode(psio::pssz{}, boot_clock::time_point{boot_clock::duration{7}}) ==
           psio::encode(psio::pssz{}, std::int64_t{7}));

   // Text: the tick count under the integer rules, never a date.
   REQUIRE(psio::encode(psio::json{}, timer) ==
           R"({"expires":9007199254740993,"started":-5,"laps":[0,4294967295]})");
   REQUIRE(psio::encode(psio::ijson{}, timer) ==
           R"({"expires":"9007199254740993","started":-5,"laps":[0,4294967295]})");
   REQUIRE(psio::encode(psio::json{}, timer) == psio::encode(psio::json{}, ints));
   REQUIRE(psio::encode(psio::ijson{}, timer) == psio::encode(psio::ijson{}, ints));
   for (const std::string text : {psio::encode(psio::json{}, timer)})
   {
      Timer back;
      REQUIRE(psio::decode_json_checked(psio::json{}, std::span<const char>{text}, back).ok());
      REQUIRE(back == timer);
   }
   {
      const std::string text = psio::encode(psio::ijson{}, timer);
      Timer             back;
      REQUIRE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, back).ok());
      REQUIRE(back == timer);
   }
   Timer             out;
   const std::string date = R"({"expires":"2026-11-22T10:15:30Z","started":null,"laps":[]})";
   REQUIRE_FALSE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{date}, out).ok());
   REQUIRE_FALSE(psio::decode_json_checked(psio::json{}, std::span<const char>{date}, out).ok());
}

TEST_CASE("time: schema marks a monotonic time point with its own id", "[time][monotonic][schema]")
{
   namespace ir = psio::schema_types;
   psio::SchemaBuilder builder;
   auto check = [&]<typename T>(std::type_identity<T>, std::string_view id, std::uint32_t bits,
                                bool is_signed) {
      auto        type   = builder.insert<T>();
      const auto& custom = std::get<ir::Custom>(type.resolve(builder.peek())->value);
      INFO(id);
      REQUIRE(custom.id == id);
      const auto& integer = std::get<ir::Int>(custom.type->resolve(builder.peek())->value);
      REQUIRE(integer.bits == bits);
      REQUIRE(integer.isSigned == is_signed);
      const auto kind = psio::parse_time_schema_id(id);
      REQUIRE(kind.has_value());
      REQUIRE(kind->monotonic);
      REQUIRE_FALSE(kind->point);
      REQUIRE(kind->num == psio::time_traits<T>::num);
      REQUIRE(kind->den == psio::time_traits<T>::den);
      REQUIRE(psio::time_schema_id(*kind) == id);
      REQUIRE(psio::reserved_time_schema_id(id));
   };
   check(std::type_identity<mono_seconds>{}, "monotonic-seconds", 64, true);
   check(std::type_identity<mono_micros>{}, "monotonic-micros", 64, true);
   check(std::type_identity<mono_ms32>{}, "monotonic-millis", 32, false);
   check(std::type_identity<steady_clock::time_point>{}, "monotonic-nanos", 64, true);
   check(std::type_identity<boot_clock::time_point>{}, "monotonic-nanos", 64, true);
   check(std::type_identity<time_point<steady_clock, duration<std::int32_t, std::ratio<60>>>>{},
         "monotonic-60/1", 32, true);
   // A time point of a unit with no RFC 3339 form still has tick text.
   REQUIRE(psio::encode(psio::json{},
                        time_point<steady_clock, duration<std::int32_t, std::ratio<60>>>{}) == "0");

   for (std::string_view bad : {"monotonic", "monotonic-", "monotonic-1/1000", "monotonic-2/4",
                                "monotonic-minutes", "Monotonic-seconds"})
   {
      INFO(bad);
      REQUIRE_FALSE(psio::parse_time_schema_id(bad).has_value());
   }
   REQUIRE(psio::reserved_time_schema_id("monotonic"));
   REQUIRE(psio::reserved_time_schema_id("monotonic-anything"));
   REQUIRE_FALSE(psio::reserved_time_schema_id("monotonically"));
   // A time point and a monotonic time point of one unit are different ids.
   REQUIRE(psio::time_schema_id<sys_micros>() != psio::time_schema_id<mono_micros>());
}
