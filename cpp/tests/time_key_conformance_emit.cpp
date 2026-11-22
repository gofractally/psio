// Emitter for conformance/time-key-vectors.json: time values in pssz / key /
// json / ijson and the psio::key format (tuples, ordering, prefix bounds).
//
//   psio_time_key_conformance_emit <out.json>          write the fixture
//   psio_time_key_conformance_emit --check <ref.json>  regenerate and compare
//
// Every row is computed by the C++ implementation and carries an expectation
// stated here from the format rules (accept / reject, order, bound);
// generation aborts when the implementation disagrees, so the fixture cannot
// silently record a bug. Output is deterministic.

#include "time_key_conformance_types.hpp"

#include <psio/schema_builder.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace
{
   using namespace tk_conf;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "time/key conformance emit: %s\n", message.c_str());
      std::exit(2);
   }

   std::vector<Row>      rows;
   std::set<std::string> names;

   Row& add(const std::string& kind, const std::string& type, const std::string& name)
   {
      const std::string full = kind + "/" + type + "/" + name;
      if (!names.insert(full).second)
         die("duplicate row " + full);
      rows.push_back(Row{full, kind, type});
      return rows.back();
   }

   template <typename T>
   std::string pssz_hex(const T& v)
   {
      const auto bytes = psio::encode(psio::pssz{}, v);
      if (!psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok() ||
          !(psio::decode<T>(psio::pssz{}, std::span<const char>{bytes}) == v))
         die("pssz does not round-trip for " + type_id<T>());
      return to_hex(bytes);
   }

   template <typename T>
   std::vector<char> key_of(const T& v)
   {
      const auto bytes = psio::encode(psio::key{}, v);
      if (psio::size_of(psio::key{}, v) != bytes.size() ||
          !psio::validate<T>(psio::key{}, std::span<const char>{bytes}).ok() ||
          psio::encode(psio::key{}, psio::decode<T>(psio::key{}, std::span<const char>{bytes})) !=
             bytes)
         die("key does not round-trip for " + type_id<T>());
      return bytes;
   }

   // ── value ──────────────────────────────────────────────────────────────
   enum class text_form { yes, none };

   template <typename T>
   void value(const std::string& name, const T& v, text_form expect = text_form::yes)
   {
      Row& r = add("value", type_id<T>(), name);
      r.pssz = pssz_hex(v);
      r.key  = to_hex(key_of(v));
      std::string json, ijson;
      const bool  has_json  = psio::encode_json_checked(psio::json{}, v, json).ok();
      const bool  has_ijson = psio::encode_json_checked(psio::ijson{}, v, ijson).ok();
      if (has_json != has_ijson || has_json != (expect == text_form::yes))
         die(r.name + ": unexpected text availability");
      if (!has_json)
         return;
      T back{};
      if (!psio::decode_json_checked(psio::json{}, std::span<const char>{json}, back).ok() ||
          !(back == v) ||
          !psio::decode_json_checked(psio::ijson{}, std::span<const char>{ijson}, back).ok() ||
          !(back == v))
         die(r.name + ": text does not round-trip");
      r.json  = json;
      r.ijson = ijson;
   }

   // ── text-accept / text-reject ──────────────────────────────────────────
   template <typename T>
   void accept(const std::string& name, const std::string& text, const T& expect)
   {
      Row& r = add("text-accept", type_id<T>(), name);
      for (int ij = 0; ij < 2; ++ij)
      {
         T          got{};
         const auto st = ij ? psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, got)
                            : psio::decode_json_checked(psio::json{}, std::span<const char>{text}, got);
         if (!st.ok() || !(got == expect))
            die(r.name + ": expected to be accepted as the given value");
      }
      r.text = text;
      r.pssz = pssz_hex(expect);
      r.json = psio::encode(psio::json{}, expect);
   }

   template <typename T>
   void reject(const std::string& name, const std::string& text)
   {
      Row& r = add("text-reject", type_id<T>(), name);
      T    got{};
      if (psio::decode_json_checked(psio::json{}, std::span<const char>{text}, got).ok() ||
          psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, got).ok() ||
          psio::validate<T>(psio::json{}, std::span<const char>{text}).ok())
         die(r.name + ": expected to be rejected");
      r.text = text;
   }

   std::string q(const std::string& s) { return "\"" + s + "\""; }

   // ── key rows ───────────────────────────────────────────────────────────
   template <typename T>
   void key_reject(const std::string& name, const std::string& hex)
   {
      Row&       r     = add("key-reject", type_id<T>(), name);
      const auto bytes = from_hex(hex);
      if (!bytes || psio::validate<T>(psio::key{}, std::span<const char>{*bytes}).ok())
         die(r.name + ": expected to be rejected");
      r.key = hex;
   }

   std::optional<std::string> bound_hex(std::span<const char> prefix)
   {
      const auto b = psio::key_prefix_upper_bound(prefix);
      return b ? std::optional{to_hex(*b)} : std::nullopt;
   }

   // Values in ascending order (operator<); keys must ascend bytewise.
   template <typename T>
   void order(const std::string& name, std::vector<T> values)
   {
      std::sort(values.begin(), values.end());
      Row& r = add("key-order", type_id<T>(), name);
      r.values.emplace();
      std::vector<char> last;
      bool              first = true;
      for (const T& v : values)
      {
         const auto k = key_of(v);
         if (!first && k == last)
            continue;  // equal values (duplicates, -0 / +0)
         if (!first && !bytes_less(last, k))
            die(r.name + ": key order is not value order");
         r.values->push_back(pssz_hex(v));
         last  = k;
         first = false;
      }
   }

   // NaN rows. `==` and `<` say nothing about a NaN, so these are checked
   // and identified by bytes alone: the PSSZ bytes carry the exact bit
   // pattern, and the key must give it back.
   template <typename T>
   std::string pssz_bits_hex(const T& v)
   {
      const auto bytes = psio::encode(psio::pssz{}, v);
      if (!psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok() ||
          psio::encode(psio::pssz{}, psio::decode<T>(psio::pssz{}, std::span<const char>{bytes})) !=
             bytes)
         die("pssz does not round-trip bit for bit for " + type_id<T>());
      return to_hex(bytes);
   }
   template <typename T>
   std::vector<char> key_bits_of(const T& v)
   {
      const auto bytes = psio::encode(psio::key{}, v);
      if (psio::size_of(psio::key{}, v) != bytes.size() ||
          !psio::validate<T>(psio::key{}, std::span<const char>{bytes}).ok() ||
          psio::encode(psio::pssz{}, psio::decode<T>(psio::key{}, std::span<const char>{bytes})) !=
             psio::encode(psio::pssz{}, v))
         die("key does not give the bit pattern back for " + type_id<T>());
      return bytes;
   }
   // A `value` row for a value holding a NaN: it has a key and no text.
   template <typename T>
   void nan_value(const std::string& name, const T& v)
   {
      Row& r = add("value", type_id<T>(), name);
      r.pssz = pssz_bits_hex(v);
      r.key  = to_hex(key_bits_of(v));
      std::string json, ijson;
      if (psio::encode_json_checked(psio::json{}, v, json).ok() ||
          psio::encode_json_checked(psio::ijson{}, v, ijson).ok())
         die(r.name + ": a NaN has no text");
   }
   // Values in the given order, which is key order and not `<` (NaNs).
   template <typename T>
   void order_bits(const std::string& name, const std::vector<T>& values)
   {
      Row& r = add("key-order-bits", type_id<T>(), name);
      r.values.emplace();
      std::vector<char> last;
      for (std::size_t i = 0; i < values.size(); ++i)
      {
         const auto k = key_bits_of(values[i]);
         if (i && !bytes_less(last, k))
            die(r.name + ": keys are not strictly ascending");
         r.values->push_back(pssz_bits_hex(values[i]));
         last = k;
      }
   }

   template <typename T>
   void prefixes(const std::string& name, const T& v)
   {
      const auto        full = key_of(v);
      const std::size_t n    = psio::detail::key_impl::key_elements<T>::count;
      for (std::size_t k = 0; k <= n; ++k)
      {
         Row&       r = add("key-prefix", type_id<T>(), name + "/" + std::to_string(k));
         const auto p = prefix_of(v, k);
         if (p.size() > full.size() || !std::equal(p.begin(), p.end(), full.begin()))
            die(r.name + ": prefix is not a byte-prefix of the full key");
         if (k == n && p != full)
            die(r.name + ": the full prefix is not the key");
         r.pssz   = pssz_hex(v);
         r.count  = static_cast<std::uint32_t>(k);
         r.prefix = to_hex(p);
         r.bound  = bound_hex(p);
         if (r.bound && !bytes_less(full, *from_hex(*r.bound)))
            die(r.name + ": key is not below the prefix bound");
      }
   }

   void bound(const std::string& name, const std::string& prefix, const std::optional<std::string>& expect)
   {
      Row& r   = add("bound", "bytes", name);
      r.prefix = prefix;
      r.bound  = bound_hex(*from_hex(prefix));
      if (r.bound != expect)
         die(r.name + ": unexpected bound");
   }

   // ── Random values that stress the framing ──────────────────────────────
   struct Gen
   {
      std::mt19937_64 rng{0x9e3779b97f4a7c15ull};
      std::uint64_t   u(std::uint64_t n) { return rng() % n; }

      template <typename I>
         requires std::is_integral_v<I>
      void operator()(I& v)
      {
         const I edges[] = {std::numeric_limits<I>::min(),
                            static_cast<I>(std::numeric_limits<I>::min() + 1),
                            static_cast<I>(-1),
                            0,
                            1,
                            static_cast<I>(std::numeric_limits<I>::max() - 1),
                            std::numeric_limits<I>::max()};
         v = u(2) ? edges[u(std::size(edges))] : static_cast<I>(rng());
      }
      void operator()(bool& v) { v = u(2); }
      void operator()(double& v)
      {
         const double edges[] = {0.0, -0.0, 1.0, -1.0, 0.5, 1e300, -1e300, 5e-324, -5e-324,
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()};
         v = u(2) ? edges[u(std::size(edges))]
                  : static_cast<double>(static_cast<std::int64_t>(rng())) / 1048576.0;
      }
      void operator()(float& v)
      {
         double d;
         (*this)(d);
         v = static_cast<float>(d);
      }
      // Valid UTF-8 (a Rust String must be able to hold it), with embedded
      // U+0000 / U+0001, multi-byte characters and shared prefixes.
      void operator()(std::string& v)
      {
         static const std::string_view alphabet[] = {std::string_view("\0", 1), "\x01", "\x02", "a",
                                                     "b", "\x7f", "\xc2\x80", "\xc3\xbf",
                                                     "\xf0\x90\x80\x80"};
         v.clear();
         for (std::size_t n = u(4); n > 0; --n)
            v += alphabet[u(std::size(alphabet))];
      }
      void operator()(Bytes& v)
      {
         static constexpr std::uint8_t alphabet[] = {0x00, 0x01, 0x02, 0x7f, 0x80, 0xfe, 0xff};
         v.clear();
         for (std::size_t n = u(4); n > 0; --n)
            v.push_back(alphabet[u(std::size(alphabet))]);
      }
      template <typename T>
      void operator()(std::optional<T>& v)
      {
         if (u(3) == 0)
            v.reset();
         else
            (*this)(v.emplace());
      }
      template <typename T>
      void operator()(std::vector<T>& v)
      {
         v.clear();
         for (std::size_t n = u(4); n > 0; --n)
            (*this)(v.emplace_back());
      }
      template <typename T, std::size_t N>
      void operator()(std::array<T, N>& v)
      {
         for (auto& e : v)
            (*this)(e);
      }
      template <typename Rep, typename Period>
      void operator()(duration<Rep, Period>& v)
      {
         Rep r;
         (*this)(r);
         v = duration<Rep, Period>{r};
      }
      template <typename C, typename D>
      void operator()(time_point<C, D>& v)
      {
         D d;
         (*this)(d);
         v = time_point<C, D>{d};
      }
      template <typename... Ts>
      void operator()(std::variant<Ts...>& v)
      {
         const std::size_t i = u(sizeof...(Ts));
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (void)((i == Is ? ((*this)(v.template emplace<Is>()), true) : false) || ...);
         }(std::index_sequence_for<Ts...>{});
      }
      template <typename... Ts>
      void operator()(std::tuple<Ts...>& v)
      {
         std::apply([&](auto&... e) { ((*this)(e), ...); }, v);
      }
      void operator()(Account& v)
      {
         (*this)(v.shard);
         (*this)(v.owner);
         (*this)(v.seq);
      }
      void operator()(Stamps& v)
      {
         (*this)(v.created);
         (*this)(v.updated);
         (*this)(v.ttl);
         (*this)(v.legacy);
      }
      void operator()(Timer& v)
      {
         (*this)(v.expires);
         (*this)(v.started);
         (*this)(v.laps);
      }
   };

   template <typename T>
   std::vector<T> random_values(std::size_t count)
   {
      Gen            gen;
      std::vector<T> out(count);
      for (T& v : out)
         gen(v);
      return out;
   }

   // ── Time values ────────────────────────────────────────────────────────
   // The text range in seconds: 0000-01-01T00:00:00Z .. 9999-12-31T23:59:59Z.
   constexpr std::int64_t kMinSec = -62167219200, kMaxSec = 253402300799;

   template <typename D>
   void emit_time_point(std::int64_t per_second)
   {
      using TP  = sys_time<D>;
      using Rep = typename D::rep;
      auto at   = [&](std::int64_t secs, std::int64_t sub = 0) {
         return TP{D{static_cast<Rep>(secs * per_second + sub)}};
      };
      const bool wide = per_second <= 1000000;  // int64 ticks reach past the text range

      value<TP>("epoch", at(0));
      value<TP>("one_tick", at(0, 1));
      value<TP>("minus_one_tick", at(0, -1));
      value<TP>("today", at(1795342530, per_second / 8));
      value<TP>("pre_1970", at(-86400 * 365));
      value<TP>("year_1900", at(-2208988800));
      value<TP>("leap_day_2000", at(951782400));
      value<TP>("leap_day_2024_last_second", at(1709251199, per_second - 1));
      value<TP>("not_leap_2100_mar_1", at(4107542400));
      value<TP>("far_future_2200", at(7258118400));
      if (wide)
      {
         value<TP>("year_0000_first", at(kMinSec));
         value<TP>("year_0000_leap_day", at(-62162035201, per_second - 1));
         value<TP>("year_9999_last", at(kMaxSec, per_second - 1));
         value<TP>("before_year_0000", at(kMinSec, -1), text_form::none);
         value<TP>("after_year_9999", at(kMaxSec + 1), text_form::none);
         value<TP>("rep_max", TP::max(), text_form::none);
         value<TP>("rep_min", TP::min(), text_form::none);
      }
      else
      {
         // Every int64 nanosecond count lies in 1677..2262.
         value<TP>("rep_max", TP::max());
         value<TP>("rep_min", TP::min());
      }
      order<TP>("random", random_values<TP>(40));
   }

   // The time values whose text is the tick count: durations and monotonic
   // time points.
   template <psio::TickTextValue D>
   void emit_duration()
   {
      using Rep = psio::time_rep_t<D>;
      auto at   = [](Rep ticks) { return psio::time_from_ticks<D>(ticks); };
      value<D>("zero", at(0));
      value<D>("one", at(1));
      if constexpr (std::is_signed_v<Rep>)
         value<D>("minus_one", at(-1));
      value<D>("day_of_seconds", at(86400));
      value<D>("rep_max", D::max());
      value<D>("rep_min", D::min());
      if constexpr (sizeof(Rep) == 8)
      {
         // The I-JSON number / string boundary of a 64-bit count.
         value<D>("safe_max", at(9007199254740991));
         value<D>("safe_max_plus_one", at(9007199254740992));
         value<D>("safe_min", at(-9007199254740991));
         value<D>("safe_min_minus_one", at(-9007199254740992));
      }
      accept<D>("number", "86400", at(86400));
      reject<D>("iso_8601_duration", "\"PT1S\"");
      reject<D>("fraction", "1.5");
      reject<D>("exponent", "1e3");
      reject<D>("null", "null");
      reject<D>("date_time", "\"1970-01-01T00:00:01Z\"");
      if constexpr (!std::is_signed_v<Rep>)
         reject<D>("negative", "-1");
      order<D>("random", random_values<D>(40));
      key_reject<D>("short", std::string(2 * sizeof(Rep) - 2, '0'));
      key_reject<D>("long", std::string(2 * sizeof(Rep) + 2, '0'));
   }

   void emit_time()
   {
      emit_time_point<seconds>(1);
      emit_time_point<milliseconds>(1000);
      emit_time_point<microseconds>(1000000);
      emit_time_point<nanoseconds>(1000000000);
      emit_duration<seconds>();
      emit_duration<milliseconds>();
      emit_duration<microseconds>();
      emit_duration<nanoseconds>();
      emit_duration<minutes32>();
      // Monotonic time points: the tick count in every format.
      emit_duration<mono_seconds>();
      emit_duration<mono_micros>();
      emit_duration<mono_ms32>();

      // An unsigned 32-bit seconds stamp.
      value<sys_sec32>("epoch", sys_sec32{});
      value<sys_sec32>("today", sys_sec32{seconds32{1795342530}});
      value<sys_sec32>("rep_max", sys_sec32::max());
      accept<sys_sec32>("rep_max", q("2106-02-07T06:28:15Z"), sys_sec32::max());
      reject<sys_sec32>("past_rep_max", q("2106-02-07T06:28:16Z"));
      reject<sys_sec32>("before_epoch", q("1969-12-31T23:59:59Z"));
      reject<sys_sec32>("before_epoch_by_offset", q("1970-01-01T00:00:00+00:01"));
      order<sys_sec32>("random", random_values<sys_sec32>(40));

      // ── Text input ──────────────────────────────────────────────────────
      const sys_seconds t{seconds{1795342530}};
      const sys_millis  tm{t};
      const sys_micros  tu{t};
      const sys_nanos   tn{t};
      accept<sys_seconds>("canonical", q("2026-11-22T10:15:30Z"), t);
      accept<sys_seconds>("lower_case_t_z", q("2026-11-22t10:15:30z"), t);
      accept<sys_seconds>("offset_plus", q("2026-11-22T12:15:30+02:00"), t);
      accept<sys_seconds>("offset_minus_half_hour", q("2026-11-22T04:45:30-05:30"), t);
      accept<sys_seconds>("offset_plus_zero", q("2026-11-22T10:15:30+00:00"), t);
      accept<sys_seconds>("offset_minus_zero", q("2026-11-22T10:15:30-00:00"), t);
      accept<sys_seconds>("offset_plus_14", q("2026-11-23T00:15:30+14:00"), t);
      accept<sys_seconds>("offset_minus_23_59", q("2026-11-21T10:16:30-23:59"), t);
      accept<sys_seconds>("offset_crosses_year", q("2027-01-01T00:30:00+01:00"),
                          sys_seconds{seconds{1798759800}});
      accept<sys_seconds>("zero_fraction", q("2026-11-22T10:15:30.0Z"), t);
      accept<sys_seconds>("long_zero_fraction", q("2026-11-22T10:15:30.000000000000000000000Z"), t);
      accept<sys_seconds>("whitespace_around", " " + q("2026-11-22T10:15:30Z") + "\n", t);
      accept<sys_seconds>("escaped_characters", "\"2026-11-22\\u005410:15:30\\u005a\"", t);
      accept<sys_seconds>("leap_day_2000", q("2000-02-29T00:00:00Z"), sys_seconds{seconds{951782400}});
      accept<sys_seconds>("leap_day_2024", q("2024-02-29T23:59:59Z"), sys_seconds{seconds{1709251199}});
      accept<sys_seconds>("year_0000", q("0000-01-01T00:00:00Z"), sys_seconds{seconds{kMinSec}});
      accept<sys_seconds>("year_0000_by_offset", q("0000-01-01T00:01:00+00:01"),
                          sys_seconds{seconds{kMinSec}});
      accept<sys_seconds>("year_9999", q("9999-12-31T23:59:59Z"), sys_seconds{seconds{kMaxSec}});
      accept<sys_seconds>("year_9999_by_offset", q("9999-12-31T23:58:59-00:01"),
                          sys_seconds{seconds{kMaxSec}});

      accept<sys_millis>("no_fraction", q("2026-11-22T10:15:30Z"), tm);
      accept<sys_millis>("short_fraction", q("2026-11-22T10:15:30.1Z"), tm + milliseconds{100});
      accept<sys_millis>("exact_fraction", q("2026-11-22T10:15:30.123Z"), tm + milliseconds{123});
      accept<sys_millis>("padded_fraction", q("2026-11-22T10:15:30.123000Z"), tm + milliseconds{123});
      accept<sys_millis>("fraction_with_offset", q("2026-11-22T12:15:30.123+02:00"),
                         tm + milliseconds{123});
      accept<sys_micros>("short_fraction", q("2026-11-22T10:15:30.5Z"), tu + microseconds{500000});
      accept<sys_micros>("exact_fraction", q("2026-11-22T10:15:30.123456Z"),
                         tu + microseconds{123456});
      accept<sys_micros>("padded_fraction", q("2026-11-22T10:15:30.123456000Z"),
                         tu + microseconds{123456});
      accept<sys_micros>("pre_1970_fraction", q("1969-12-31T23:59:59.999999Z"),
                         sys_micros{microseconds{-1}});
      accept<sys_nanos>("exact_fraction", q("2026-11-22T10:15:30.123456789Z"),
                        tn + nanoseconds{123456789});
      accept<sys_nanos>("padded_fraction", q("2026-11-22T10:15:30.1234567890Z"),
                        tn + nanoseconds{123456789});
      accept<sys_nanos>("rep_max", q("2262-04-11T23:47:16.854775807Z"), sys_nanos::max());
      accept<sys_nanos>("rep_min", q("1677-09-21T00:12:43.145224192Z"), sys_nanos::min());

      // Precision the unit cannot hold.
      reject<sys_seconds>("fraction_half", q("2026-11-22T10:15:30.5Z"));
      reject<sys_seconds>("fraction_tiny", q("2026-11-22T10:15:30.000000001Z"));
      reject<sys_millis>("fraction_four_digits", q("2026-11-22T10:15:30.1234Z"));
      reject<sys_millis>("fraction_late_digit", q("2026-11-22T10:15:30.1230001Z"));
      reject<sys_micros>("fraction_seven_digits", q("2026-11-22T10:15:30.1234567Z"));
      reject<sys_nanos>("fraction_ten_digits", q("2026-11-22T10:15:30.1234567891Z"));
      // Outside Rep or the year range.
      reject<sys_nanos>("past_rep_max", q("2262-04-11T23:47:16.854775808Z"));
      reject<sys_nanos>("before_rep_min", q("1677-09-21T00:12:43.145224191Z"));
      reject<sys_nanos>("year_9999", q("9999-12-31T23:59:59Z"));
      reject<sys_seconds>("before_year_0000_by_offset", q("0000-01-01T00:00:00+00:01"));
      reject<sys_seconds>("after_year_9999_by_offset", q("9999-12-31T23:59:59-00:01"));
      reject<sys_seconds>("five_digit_year", q("10000-01-01T00:00:00Z"));
      reject<sys_seconds>("signed_year", q("+2026-11-22T10:15:30Z"));
      reject<sys_seconds>("negative_year", q("-0001-11-22T10:15:30Z"));
      // Leap seconds, hour 24, impossible dates.
      reject<sys_seconds>("leap_second", q("2016-12-31T23:59:60Z"));
      reject<sys_micros>("leap_second_fraction", q("2016-12-31T23:59:60.5Z"));
      reject<sys_seconds>("hour_24", q("2026-11-22T24:00:00Z"));
      reject<sys_seconds>("minute_60", q("2026-11-22T10:60:00Z"));
      reject<sys_seconds>("feb_29_non_leap", q("2026-02-29T00:00:00Z"));
      reject<sys_seconds>("feb_29_2100", q("2100-02-29T00:00:00Z"));
      reject<sys_seconds>("april_31", q("2026-04-31T00:00:00Z"));
      reject<sys_seconds>("month_00", q("2026-00-10T00:00:00Z"));
      reject<sys_seconds>("month_13", q("2026-13-10T00:00:00Z"));
      reject<sys_seconds>("day_00", q("2026-11-00T00:00:00Z"));
      // Not the grammar.
      reject<sys_seconds>("empty", q(""));
      reject<sys_seconds>("date_only", q("2026-11-22"));
      reject<sys_seconds>("no_offset", q("2026-11-22T10:15:30"));
      reject<sys_seconds>("space_separator", q("2026-11-22 10:15:30Z"));
      reject<sys_seconds>("no_seconds", q("2026-11-22T10:15Z"));
      reject<sys_seconds>("empty_fraction", q("2026-11-22T10:15:30.Z"));
      reject<sys_seconds>("comma_fraction", q("2026-11-22T10:15:30,5Z"));
      reject<sys_seconds>("double_z", q("2026-11-22T10:15:30ZZ"));
      reject<sys_seconds>("trailing_space_inside", q("2026-11-22T10:15:30Z "));
      reject<sys_seconds>("leading_space_inside", q(" 2026-11-22T10:15:30Z"));
      reject<sys_seconds>("offset_without_colon", q("2026-11-22T10:15:30+0200"));
      reject<sys_seconds>("offset_hours_only", q("2026-11-22T10:15:30+02"));
      reject<sys_seconds>("offset_hour_24", q("2026-11-22T10:15:30+24:00"));
      reject<sys_seconds>("offset_minute_60", q("2026-11-22T10:15:30+02:60"));
      reject<sys_seconds>("basic_format", q("20261122T101530Z"));
      reject<sys_seconds>("one_digit_month", q("2026-1-22T10:15:30Z"));
      reject<sys_seconds>("one_digit_second", q("2026-11-22T10:15:3Z"));
      reject<sys_seconds>("letter_in_number", q("2026-11-22T1O:15:30Z"));
      reject<sys_nanos>("letter_in_fraction", q("2026-11-22T10:15:30.12a4Z"));
      reject<sys_seconds>("unix_seconds_string", q("1795342530"));
      reject<sys_seconds>("number", "1795342530");
      reject<sys_seconds>("null", "null");
      reject<sys_seconds>("array", "[\"2026-11-22T10:15:30Z\"]");

      // Records.
      const Stamps stamps{t, sys_micros{microseconds{-1}}, milliseconds{1500},
                          sys_sec32{seconds32{4000000000u}}};
      value<Stamps>("mixed", stamps);
      value<Stamps>("zero", Stamps{});
      Stamps far  = stamps;
      far.updated = sys_micros::min();
      value<Stamps>("member_without_text", far, text_form::none);
      order<Stamps>("random", random_values<Stamps>(40));
      const Event event{"deploy",
                        tm + milliseconds{123},
                        tn + nanoseconds{123456789},
                        {sys_seconds{}, sys_seconds{seconds{-86400}}},
                        nanoseconds{9007199254740993},
                        minutes32{-90}};
      value<Event>("full", event);
      value<Event>("empty", Event{});
      accept<Event>("offsets",
                    R"({"name":"x","at":"2026-11-22T12:15:30.5+02:00","precise":null,)"
                    R"("history":["1970-01-01T01:00:00+01:00"],"elapsed":7,"timeout":0})",
                    Event{"x", tm + milliseconds{500}, std::nullopt, {sys_seconds{}}, nanoseconds{7},
                          minutes32{0}});
      reject<Event>("member_leap_second",
                    R"({"name":"x","at":"2026-11-22T10:15:60Z","precise":null,"history":[],)"
                    R"("elapsed":0,"timeout":0})");
      reject<Event>("element_finer_than_unit",
                    R"({"name":"x","at":"2026-11-22T10:15:30Z","precise":null,)"
                    R"("history":["2026-11-22T10:15:30.5Z"],"elapsed":0,"timeout":0})");

      // Monotonic time points as members: tick counts, never dates.
      using lap = mono_ms32::duration;
      const Timer timer{mono_micros{microseconds{9007199254740993}},
                        mono_seconds{seconds{-5}},
                        {mono_ms32{lap{0}}, mono_ms32{lap{1500}}, mono_ms32::max()}};
      value<Timer>("full", timer);
      value<Timer>("empty", Timer{});
      accept<Timer>("ticks",
                    R"({"laps":[7],"expires":12,"started":null})",
                    Timer{mono_micros{microseconds{12}}, std::nullopt, {mono_ms32{lap{7}}}});
      reject<Timer>("date_for_ticks",
                    R"({"expires":"2026-11-22T10:15:30Z","started":null,"laps":[]})");
      reject<Timer>("lap_out_of_range", R"({"expires":0,"started":null,"laps":[4294967296]})");
      order<Timer>("random", random_values<Timer>(40));
   }

   // ── Keys ───────────────────────────────────────────────────────────────
   template <typename T>
   void key_type(std::vector<T> picked, std::size_t random = 48)
   {
      for (std::size_t i = 0; i < picked.size(); ++i)
      {
         bool text = true;
         std::string json;
         text = psio::encode_json_checked(psio::ijson{}, picked[i], json).ok();
         value<T>("v" + std::to_string(i), picked[i], text ? text_form::yes : text_form::none);
         if constexpr (is_composite_v<T>)
            prefixes<T>("v" + std::to_string(i), picked[i]);
      }
      auto all = random_values<T>(random);
      all.insert(all.end(), picked.begin(), picked.end());
      order<T>("mixed", std::move(all));
      // No key is a proper prefix of another: one byte short never reads.
      if (!picked.empty())
      {
         const auto k = key_of(picked.back());
         if (!k.empty())
            key_reject<T>("truncated", to_hex(std::span<const char>{k.data(), k.size() - 1}));
         auto longer = k;
         longer.push_back('\0');
         key_reject<T>("trailing_byte", to_hex(longer));
      }
   }

   void emit_keys()
   {
      using namespace std::string_literals;
      const std::string nul("\0", 1);

      key_type<KEmpty>({KEmpty{}}, 0);
      key_type<KShard>({{0, ""}, {7, "bob"}, {7, "bo"}, {7, "bob" + nul}, {7, nul + "bob"},
                        {8, ""}, {0xffffffffu, "\xc3\xbf"}, {0xffffffffu, ""}});
      key_type<KAccount>({{7, "bob", std::nullopt}, {7, "bob", 0}, {7, "bob", -1},
                          {7, "bob", std::numeric_limits<std::int64_t>::min()},
                          {7, "bob", std::numeric_limits<std::int64_t>::max()},
                          {0xffffffffu, "", std::nullopt}});
      key_type<Account>({{7, "bob", std::nullopt}, {7, "bob", 5}, {0, "", std::nullopt},
                         {0xffffffffu, "\xf0\x90\x80\x80", -5}});
      key_type<KStrU8>({{"", 0}, {"", 255}, {nul, 0}, {"\x01", 0}, {"a", 1}, {"a" + nul, 1},
                        {"a" + nul + nul, 1}, {"a\x01", 1}, {"ab", 0}});
      key_type<KStrStr>({{"", ""}, {"", "a"}, {"a", ""}, {"a", "b"}, {"ab", ""}, {nul, nul},
                         {"a" + nul, "b"}, {"a", nul + "b"}});
      key_type<KBytes>({{{}, 0}, {{}, -128}, {{}, 127}, {{0}, 0}, {{0, 0}, 0}, {{0, 1}, 0},
                        {{1}, 0}, {{0xff}, -1}, {{0xff, 0xff}, 1}, {{0xff, 0}, 0}});
      key_type<KList>({{{}, 0}, {{0}, 0}, {{-1}, 0}, {{1}, 0}, {{-32768, 32767}, -1}, {{0, 0}, 0},
                       {{256}, 0}, {{1, 0}, 0}});
      key_type<KOptStr>({{std::nullopt, ""}, {std::nullopt, "a"}, {""s, ""}, {""s, "a"},
                         {nul, ""}, {"a"s, ""}});
      using SL = std::vector<std::string>;
      key_type<KStrList>({{SL{}, ""}, {SL{}, "a"}, {SL{""}, ""}, {SL{"", ""}, ""}, {SL{"a"}, ""},
                          {SL{"a", "b"}, "c"}, {SL{nul}, ""}, {SL{"\x01"}, ""}});
      key_type<KInts>({{false, 0, 0, 0, 0, 0, 0, 0, 0},
                       {true, -1, -1, -1, -1, 255, 65535, 0xffffffffu, 0xffffffffffffffffull},
                       {true, -128, -32768, std::numeric_limits<std::int32_t>::min(),
                        std::numeric_limits<std::int64_t>::min(), 1, 258, 0x01020304u,
                        0x0102030405060708ull},
                       {false, 127, 32767, std::numeric_limits<std::int32_t>::max(),
                        std::numeric_limits<std::int64_t>::max(), 128, 32768, 0x80000000u,
                        0x8000000000000000ull}});
      key_type<KFloats>({{0.0f, 0.0}, {1.5f, -1.5}, {-1.5f, 1.5}, {3.4028235e38f, 1e300},
                         {-3.4028235e38f, -1e300}, {1e-45f, 5e-324}, {-1e-45f, -5e-324}});
      key_type<KTime>({{sys_micros{}, "", seconds{0}},
                       {sys_micros{microseconds{-1}}, "a", seconds{-1}},
                       {sys_micros{microseconds{1795342530123456}}, "deploy", seconds{3600}},
                       {sys_micros{microseconds{253402300799999999}}, "", seconds::max()}});
      key_type<KMono>({{mono_micros{}, ""},
                       {mono_micros{microseconds{-1}}, "a"},
                       {mono_micros{microseconds{1795342530123456}}, "timer"},
                       {mono_micros::max(), ""},
                       {mono_micros::min(), "z"}});
      key_type<KArray>({{{0, 0}, ""}, {{1, 2}, "a"}, {{0xffff, 0xffff}, ""}, {{0, 0xffff}, "z"}});
      key_type<KVariant>({{std::uint8_t{0}, 0}, {std::uint8_t{255}, 255}, {""s, 0}, {"a"s, 1},
                          {nul, 2}});
      key_type<KNested>({{{"", 0}, ""}, {{"a", 1}, "b"}, {{"a" + nul, 255}, nul}});

      // Infinities order with the finite values; text has no form for them
      // under ijson, so they appear in order rows only.
      const float  finf = std::numeric_limits<float>::infinity();
      const double dinf = std::numeric_limits<double>::infinity();
      order<KFloats>("infinities", {{finf, 0}, {-finf, 0}, {0, dinf}, {0, -dinf}, {0, 0},
                                    {-0.0f, -0.0}, {1, 1}, {-1, -1}, {finf, dinf}, {-finf, -dinf}});

      // NaN: unordered for `<`, ordered as a key. A NaN with the sign bit
      // clear sorts above +inf, one with it set below -inf, and every
      // payload is its own key and decodes to the same bits.
      const auto f32 = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
      const auto f64 = [](std::uint64_t bits) { return std::bit_cast<double>(bits); };
      nan_value<KFloats>("nan_quiet", {f32(0x7fc00000u), f64(0x7ff8000000000000ull)});
      nan_value<KFloats>("nan_quiet_negative", {f32(0xffc00000u), f64(0xfff8000000000000ull)});
      nan_value<KFloats>("nan_signaling", {f32(0x7f800001u), f64(0x7ff0000000000001ull)});
      nan_value<KFloats>("nan_payload", {f32(0x7fc12345u), f64(0x7ff8000000abcdefull)});
      nan_value<KFloats>("nan_all_ones", {f32(0xffffffffu), f64(0xffffffffffffffffull)});
      nan_value<KFloats>("nan_f32_only", {f32(0x7fc00000u), 1.5});
      nan_value<KFloats>("nan_f64_only", {1.5f, f64(0x7ff8000000000000ull)});
      order_bits<KFloats>("nan_f32",
                          {{f32(0xffffffffu), 0}, {f32(0xffc00000u), 0}, {f32(0xff800001u), 0},
                           {-finf, 0}, {-1, 0}, {0, 0}, {1, 0}, {finf, 0},
                           {f32(0x7f800001u), 0}, {f32(0x7fc00000u), 0}, {f32(0x7fc00001u), 0},
                           {f32(0x7fffffffu), 0}});
      order_bits<KFloats>("nan_f64",
                          {{0, f64(0xffffffffffffffffull)}, {0, f64(0xfff8000000000000ull)},
                           {0, f64(0xfff0000000000001ull)}, {0, -dinf}, {0, -1}, {0, 0}, {0, 1},
                           {0, dinf}, {0, f64(0x7ff0000000000001ull)},
                           {0, f64(0x7ff8000000000000ull)}, {0, f64(0x7ff8000000000001ull)},
                           {0, f64(0x7fffffffffffffffull)}});
      // The first element decides before the second is looked at.
      order_bits<KFloats>("nan_then_second",
                          {{f32(0xffc00000u), dinf}, {-finf, f64(0x7ff8000000000000ull)},
                           {finf, f64(0xfff8000000000000ull)}, {f32(0x7fc00000u), -dinf}});

      // Non-canonical or malformed encodings.
      key_reject<KShard>("escape_02", "00000007" "610002" "0000");
      key_reject<KShard>("unterminated_string", "00000007" "6100");
      key_reject<KShard>("missing_string", "00000007");
      key_reject<KAccount>("optional_marker_02", "00000007" "0000" "02");
      key_reject<KAccount>("optional_marker_missing", "00000007" "0000");
      key_reject<KAccount>("optional_payload_short", "00000007" "0000" "01" "80000000");
      key_reject<KInts>("bool_02", "02" "80" "8000" "80000000" "8000000000000000" "00" "0000"
                                   "00000000" "0000000000000000");
      key_reject<KList>("list_marker_02", "02" "8000" "00" "80");
      key_reject<KList>("list_unterminated", "01" "8000");
      key_reject<KFloats>("negative_zero_f32", "7fffffff" "8000000000000000");
      key_reject<KFloats>("negative_zero_f64", "80000000" "7fffffffffffffff");
      key_reject<KVariant>("variant_index_02", "02" "00" "00");
      key_reject<KEmpty>("not_empty", "00");

      // ── Prefix bounds on raw bytes ──────────────────────────────────────
      using O = std::optional<std::string>;
      bound("empty", "", O{});
      bound("ff", "ff", O{});
      bound("ffff", "ffff", O{});
      bound("ffffffffffffffff", "ffffffffffffffff", O{});
      bound("00", "00", O{"01"});
      bound("7f", "7f", O{"80"});
      bound("80", "80", O{"81"});
      bound("fe", "fe", O{"ff"});
      bound("1234", "1234", O{"1235"});
      bound("12ff", "12ff", O{"13"});
      bound("12ffff", "12ffff", O{"13"});
      bound("00ff", "00ff", O{"01"});
      bound("fffe", "fffe", O{"ffff"});
      bound("feff", "feff", O{"ff"});
      bound("0000", "0000", O{"0001"});
      bound("ff00", "ff00", O{"ff01"});
      bound("ff00ff", "ff00ff", O{"ff01"});
   }

   // ── Transparent wrappers, custom types, invariants ────────────────────
   // Range's wire type, without its invariant.
   struct AnyRange
   {
      std::uint32_t lo = 0;
      std::uint32_t hi = 0;
   };
   PSIO_REFLECT(AnyRange, lo, hi)

   // The key of a wrapper is the key of the value it wraps.
   template <typename T, typename Inner>
   void same_key(const std::string& name, const T& v, const Inner& inner)
   {
      if (psio::encode(psio::key{}, v) != psio::encode(psio::key{}, inner))
         die(type_id<T>() + "/" + name + ": the key is not the key of the wire type");
      if (psio::encode(psio::pssz{}, v) != psio::encode(psio::pssz{}, inner))
         die(type_id<T>() + "/" + name + ": the bytes are not those of the wire type");
      const auto a = psio::to_key(v);
      const auto b = psio::to_key(inner);
      if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin()))
         die(type_id<T>() + "/" + name + ": to_key differs from the wire type's");
   }
   template <typename T>
   void custom_values(const std::vector<T>& values)
   {
      for (std::size_t i = 0; i < values.size(); ++i)
         value<T>("v" + std::to_string(i), values[i]);
      order<T>("ascending", values);
      const auto k = key_of(values.back());
      key_reject<T>("trailing_byte", to_hex(k) + "00");
      if (!k.empty())
         key_reject<T>("truncated", to_hex(std::span<const char>{k.data(), k.size() - 1}));
   }
   // `plain` is a value of T's wire type that violates an invariant: its
   // key and its bytes are well-formed, and no typed reader of T takes them.
   template <typename T, typename Plain>
   void key_invariant(const std::string& name, const Plain& plain, const std::string& reason)
   {
      Row&       r     = add("key-invariant", type_id<T>(), name);
      const auto key   = psio::encode(psio::key{}, plain);
      const auto bytes = psio::encode(psio::pssz{}, plain);
      auto       k     = psio::validate<T>(psio::key{}, std::span<const char>{key});
      if (k.ok() || k.error().what != reason)
         die(r.name + ": key validate does not report '" + reason + "'");
      if (psio::validate_strict<T>(psio::key{}, std::span<const char>{key}).ok())
         die(r.name + ": key validate_strict accepts");
      bool threw = false;
      try
      {
         (void)psio::decode<T>(psio::key{}, std::span<const char>{key});
      }
      catch (const psio::codec_exception& e)
      {
         threw = e.error().what == reason;
      }
      if (!threw) die(r.name + ": key decode does not fail with the invariant");
      auto p = psio::validate<T>(psio::pssz{}, std::span<const char>{bytes});
      if (p.ok() || p.error().what != reason)
         die(r.name + ": pssz validate does not report '" + reason + "'");
      // The trusted pssz decode yields the value; no key writer takes it.
      const T value = psio::decode<T>(psio::pssz{}, std::span<const char>{bytes});
      threw         = false;
      try
      {
         (void)psio::encode(psio::key{}, value);
      }
      catch (const psio::codec_exception& e)
      {
         threw = e.error().what == reason;
      }
      if (!threw) die(r.name + ": key encode writes a value that violates its invariant");
      threw = false;
      try
      {
         (void)psio::to_key(value);
      }
      catch (const psio::codec_exception&)
      {
         threw = true;
      }
      if (!threw) die(r.name + ": to_key writes a value that violates its invariant");
      r.key    = to_hex(key);
      r.pssz   = to_hex(bytes);
      r.reason = reason;
   }

   void emit_custom_types()
   {
      using namespace custom_conf;
      using namespace std::string_literals;
      using u64 = std::uint64_t;
      using u32 = std::uint32_t;
      using u8  = std::uint8_t;
      const u64 top = ~u64{0};

      same_key<Id, u64>("zero", Id{0}, 0);
      same_key<Id, u64>("max", Id{top}, top);
      custom_values<Id>({Id{0}, Id{1}, Id{256}, Id{top}});
      static_assert(std::tuple_size_v<decltype(psio::to_key(Id{}))> == 8);  // a fixed-width key

      same_key<Blob, Bytes>("bytes", Blob{{0, 1, 0}}, Bytes{0, 1, 0});
      custom_values<Blob>({Blob{}, Blob{{0}}, Blob{{0, 0}}, Blob{{0, 1}}, Blob{{1}}, Blob{{255}}});

      V6 loopback;
      loopback.address[15] = 1;
      loopback.port        = 443;
      custom_values<Endpoint>({Endpoint{V4{}}, Endpoint{V4{{10, 0, 0, 7}, 1}},
                               Endpoint{V4{{127, 0, 0, 1}, 8080}}, Endpoint{V6{}},
                               Endpoint{loopback}});
      custom_values<Ratio>({Ratio{-32768, 0}, Ratio{-3, 4}, Ratio{0, 0}, Ratio{0, 1},
                            Ratio{32767, 65535}});

      same_key<Ref, std::variant<u64, std::string>>("name", Ref{"alice"s}, "alice"s);
      custom_values<Ref>({Ref{u64{0}}, Ref{u64{7}}, Ref{top}, Ref{"a"s}, Ref{"alice"s},
                          Ref{"b c"s}});

      const u64 name60 = u64{1} << 60;
      same_key<Name, u64>("largest", Name{name60 - 1}, name60 - 1);
      custom_values<Name>({Name{0}, Name{5}, Name{name60 - 1}});
      static_assert(std::tuple_size_v<decltype(psio::to_key(Name{}))> == 8);
      key_invariant<Name, u64>("first_non_canonical", name60,
                               "Name: value is not a canonical name");
      key_invariant<Name, u64>("all_ones", top, "Name: value is not a canonical name");

      same_key<DerKey, Bytes>("sequence", DerKey{{0x30, 0, 1}}, Bytes{0x30, 0, 1});
      custom_values<DerKey>({DerKey{{0x30}}, DerKey{{0x30, 0}}, DerKey{{0x30, 0, 0}},
                             DerKey{{0x30, 1}}});
      key_invariant<DerKey, Bytes>("empty", Bytes{}, "DerKey: not a DER sequence");
      key_invariant<DerKey, Bytes>("wrong_tag", Bytes{0x31, 0}, "DerKey: not a DER sequence");

      custom_values<Range>({Range{0, 0}, Range{0, 9}, Range{3, 3}, Range{3, 9}});
      key_invariant<Range, AnyRange>("reversed", AnyRange{9, 3},
                                          "Range: lo is greater than hi");

      custom_values<Level>({Level::low, Level::mid, Level::high, Level::top});
      key_invariant<Level, u8>("unlisted", u8{4}, "Level: not a listed level");

      const ShardKey shard{KShard{7, "alice"}};
      same_key<ShardKey, KShard>("shard", shard, shard.key);
      custom_values<ShardKey>({ShardKey{KShard{0, ""}}, ShardKey{KShard{7, "alice"}},
                               ShardKey{KShard{7, "bob"}}, ShardKey{KShard{8, ""}}});
      // key_prefix<ShardKey>(7) is key_prefix<tuple<u32, string>>(7).
      prefixes<ShardKey>("shard", shard);
      if (psio::key_prefix<ShardKey>(7u) != psio::key_prefix<KShard>(7u))
         die("ShardKey: the prefix is not that of the tuple it wraps");

      // Invariants inside a composite key: each element where it occurs.
      // The wire type, spelled without the custom types (an enum without an
      // invariant stands for Level: a list of enums is not a byte string).
      enum class AnyLevel : u8 {};
      const auto L = [](unsigned v) { return static_cast<AnyLevel>(v); };
      using Plain = Tup<u64, std::optional<AnyRange>, std::vector<AnyLevel>, Bytes>;
      const KGuard good{Name{5}, Range{1, 2}, {Level::low, Level::top}, DerKey{{0x30, 9}}};
      value<KGuard>("v0", good);
      value<KGuard>("v1", KGuard{Name{0}, std::nullopt, {}, DerKey{{0x30}}});
      prefixes<KGuard>("v0", good);
      same_key<KGuard, Plain>("v0", good,
                              Plain{5, AnyRange{1, 2}, {L(0), L(3)}, Bytes{0x30, 9}});
      key_invariant<KGuard, Plain>("name", Plain{name60, AnyRange{1, 2}, {L(0), L(3)},
                                                 Bytes{0x30, 9}},
                                   "Name: value is not a canonical name");
      key_invariant<KGuard, Plain>("optional_range", Plain{5, AnyRange{2, 1}, {L(0), L(3)},
                                                           Bytes{0x30, 9}},
                                   "Range: lo is greater than hi");
      key_invariant<KGuard, Plain>("level_in_list", Plain{5, AnyRange{1, 2}, {L(0), L(7)},
                                                          Bytes{0x30, 9}},
                                   "Level: not a listed level");
      key_invariant<KGuard, Plain>("derkey", Plain{5, AnyRange{1, 2}, {L(0), L(3)},
                                                   Bytes{9}},
                                   "DerKey: not a DER sequence");
   }

   std::string quoted(const std::string& s) { return psio::encode(psio::json{}, s); }

   std::string render()
   {
      psio::SchemaBuilder builder;
      std::string         types;
      for_each_type([&]<typename T>(std::type_identity<T>) {
         builder.insert<T>(type_id<T>());
         types += std::string(types.empty() ? "" : ",\n") + "{\"id\":" + quoted(type_id<T>()) +
                  ",\"composite\":" + (is_composite_v<T> ? "true" : "false") + "}";
      });
      const psio::Schema schema = std::move(builder).build();
      std::string        out    = "{\"version\":1,\"schema_ir\":\"" +
                                  to_hex(psio::encode(psio::pssz{}, schema)) + "\",\"types\":[\n" +
                                  types + "\n],\"vectors\":[\n";
      for (std::size_t i = 0; i < rows.size(); ++i)
      {
         const Row& r = rows[i];
         out += "{\"name\":" + quoted(r.name) + ",\"kind\":" + quoted(r.kind) + ",\"type\":" +
                quoted(r.type);
         auto field = [&](const char* key, const std::optional<std::string>& v) {
            if (v)
               out += std::string(",\"") + key + "\":" + quoted(*v);
         };
         field("pssz", r.pssz);
         field("key", r.key);
         field("json", r.json);
         field("ijson", r.ijson);
         field("text", r.text);
         field("prefix", r.prefix);
         field("reason", r.reason);
         if (r.kind == "key-prefix" || r.kind == "bound")
            out += ",\"bound\":" + (r.bound ? quoted(*r.bound) : std::string("null"));
         if (r.count)
            out += ",\"count\":" + std::to_string(*r.count);
         if (r.values)
         {
            out += ",\"values\":[";
            for (std::size_t j = 0; j < r.values->size(); ++j)
               out += (j ? "," : "") + quoted((*r.values)[j]);
            out += "]";
         }
         out += i + 1 < rows.size() ? "},\n" : "}\n";
      }
      out += "]}\n";
      return out;
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   if (!check && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json>\n", argv[0]);
      return 2;
   }
   emit_time();
   emit_keys();
   emit_custom_types();

   const std::string text = render();
   Fixture           parsed;
   if (!psio::decode_json_checked(psio::json{}, std::span<const char>{text}, parsed).ok() ||
       parsed.vectors.size() != rows.size())
      die("the rendered fixture does not read back");

   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "time/key conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended format change\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("wrote %zu vectors to %s\n", rows.size(), argv[1]);
   return out ? 0 : 1;
}
