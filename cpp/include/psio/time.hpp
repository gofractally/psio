#pragma once
//
// psio/time.hpp — time as a first-class PSIO value (docs/time.md).
//
// Two C++ type families are time values:
//
//   std::chrono::duration<Rep, Period>                      a duration
//   std::chrono::time_point<std::chrono::system_clock, D>   a time point:
//                                                           D since the Unix
//                                                           epoch, UTC, no
//                                                           leap seconds
//
// Rep must be an integer of at most 64 bits (not bool). A time_point on any
// other clock has no portable epoch and is rejected at compile time.
//
// Every format sees a time value as its tick count, an integer of type Rep:
//
//   binary formats   the Rep integer, exactly as the format writes that
//                    integer type: pssz, bin, ssz, borsh, bincode
//   psio::key        the Rep integer's key form (sign-flipped big-endian), so
//                    byte order is chronological order
//   schema           Custom{Int{bits, signed}, id}; `id` is time_schema_id
//   json / ijson     duration: the tick count under the format's integer
//                    rules. time point: an RFC 3339 UTC string (below)
//
// Schema id:  ("time-point" | "duration") "-" unit, where unit is
// "seconds" | "millis" | "micros" | "nanos" for periods 1, 1/10^3, 1/10^6,
// 1/10^9 and "<num>/<den>" (lowest terms, e.g. "60/1", "1/60") for any other
// std::ratio. The four names are the only spelling of their periods.
//
// Time point text. Defined for periods of 1/10^k seconds, k = 0..9:
//
//   YYYY-MM-DDTHH:MM:SSZ            k = 0
//   YYYY-MM-DDTHH:MM:SS.fffZ        exactly k fraction digits otherwise
//
// Always UTC with an upper-case `Z`, proleptic Gregorian, years 0000..9999.
// A value outside 0000-01-01T00:00:00Z .. 9999-12-31T23:59:59.9…Z has no
// text: writing it fails. Reading accepts any RFC 3339 date-time: `T`/`Z` in
// either case, an offset `+HH:MM` / `-HH:MM` (converted to UTC), and any
// number of fraction digits as long as the digits beyond the unit's
// precision are all zero. It rejects everything else: a missing offset, a
// space separator, hour 24, a leap second (`:60` has no Unix time), an
// impossible date, a fraction the unit cannot hold exactly, an instant
// outside the year range after applying the offset, and a tick count that
// does not fit Rep.
//
// No time zone database, no locale, no allocation, no exceptions.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>

namespace psio {

   namespace detail::time_impl {
      template <typename Rep>
      inline constexpr bool tick_rep_v = std::is_integral_v<Rep> &&
                                         !std::is_same_v<Rep, bool> &&
                                         sizeof(Rep) <= 8;
   }

   // ── time_traits ───────────────────────────────────────────────────────
   //
   // The single description of a time type that every format consults.
   template <typename T>
   struct time_traits
   {
      static constexpr bool is_time = false;
   };

   template <typename Rep, typename Period>
   struct time_traits<std::chrono::duration<Rep, Period>>
   {
      static_assert(detail::time_impl::tick_rep_v<Rep>,
                    "psio: a std::chrono::duration needs an integer Rep of at "
                    "most 64 bits");
      using value_type = std::chrono::duration<Rep, Period>;
      using rep        = Rep;
      static constexpr bool          is_time  = true;
      static constexpr bool          is_point = false;
      static constexpr std::intmax_t num      = Period::num;
      static constexpr std::intmax_t den      = Period::den;

      static constexpr rep ticks(const value_type& v) noexcept { return v.count(); }
      static constexpr value_type from_ticks(rep r) noexcept { return value_type{r}; }
   };

   template <typename Clock, typename Duration>
   struct time_traits<std::chrono::time_point<Clock, Duration>>
   {
      static_assert(std::is_same_v<Clock, std::chrono::system_clock>,
                    "psio: only std::chrono::system_clock time points are "
                    "values (Unix epoch, UTC); other clocks have no portable "
                    "epoch");
      using value_type = std::chrono::time_point<Clock, Duration>;
      using rep        = typename time_traits<Duration>::rep;
      static constexpr bool          is_time  = true;
      static constexpr bool          is_point = true;
      static constexpr std::intmax_t num      = time_traits<Duration>::num;
      static constexpr std::intmax_t den      = time_traits<Duration>::den;

      static constexpr rep ticks(const value_type& v) noexcept
      {
         return v.time_since_epoch().count();
      }
      static constexpr value_type from_ticks(rep r) noexcept
      {
         return value_type{Duration{r}};
      }
   };

   template <typename T>
   concept TimeValue = time_traits<std::remove_cvref_t<T>>::is_time;
   template <typename T>
   concept TimePointValue = TimeValue<T> && time_traits<std::remove_cvref_t<T>>::is_point;
   template <typename T>
   concept DurationValue = TimeValue<T> && !time_traits<std::remove_cvref_t<T>>::is_point;

   // The integer a time value is in every binary format.
   template <TimeValue T>
   using time_rep_t = typename time_traits<std::remove_cvref_t<T>>::rep;

   template <TimeValue T>
   constexpr time_rep_t<T> time_ticks(const T& v) noexcept
   {
      return time_traits<T>::ticks(v);
   }
   template <TimeValue T>
   constexpr T time_from_ticks(time_rep_t<T> r) noexcept
   {
      return time_traits<T>::from_ticks(r);
   }

   // ── Schema id ─────────────────────────────────────────────────────────

   struct time_kind
   {
      bool          point = false;
      std::intmax_t num   = 1;  // period = num/den seconds, lowest terms
      std::intmax_t den   = 1;

      friend bool operator==(const time_kind&, const time_kind&) = default;
   };

   // Fraction digits of a period of 1/10^k seconds (k = 0..9); -1 for any
   // other period, which has no RFC 3339 form.
   constexpr int time_fraction_digits(std::intmax_t num, std::intmax_t den) noexcept
   {
      if (num != 1)
         return -1;
      int           k = 0;
      std::intmax_t p = 1;
      while (p < den && k < 9)
      {
         p *= 10;
         ++k;
      }
      return p == den ? k : -1;
   }

   inline std::string time_schema_id(const time_kind& k)
   {
      std::string out = k.point ? "time-point-" : "duration-";
      if (k.num == 1 && k.den == 1)
         out += "seconds";
      else if (k.num == 1 && k.den == 1000)
         out += "millis";
      else if (k.num == 1 && k.den == 1000000)
         out += "micros";
      else if (k.num == 1 && k.den == 1000000000)
         out += "nanos";
      else
      {
         out += std::to_string(k.num);
         out += '/';
         out += std::to_string(k.den);
      }
      return out;
   }

   template <TimeValue T>
   std::string time_schema_id()
   {
      using Tr = time_traits<std::remove_cvref_t<T>>;
      return time_schema_id(time_kind{Tr::is_point, Tr::num, Tr::den});
   }

   // The inverse of time_schema_id; nullopt for any other id, including a
   // non-canonical spelling ("1/1000" for millis, "2/4", leading zeros).
   inline std::optional<time_kind> parse_time_schema_id(std::string_view id) noexcept
   {
      time_kind k;
      constexpr std::string_view point = "time-point-", duration = "duration-";
      if (id.starts_with(point))
      {
         k.point = true;
         id.remove_prefix(point.size());
      }
      else if (id.starts_with(duration))
         id.remove_prefix(duration.size());
      else
         return std::nullopt;
      if (id == "seconds")
         return k;
      if (id == "millis")
         return k.den = 1000, k;
      if (id == "micros")
         return k.den = 1000000, k;
      if (id == "nanos")
         return k.den = 1000000000, k;
      auto number = [&](std::intmax_t& out) {
         if (id.empty() || id[0] < '1' || id[0] > '9')
            return false;
         std::intmax_t v = 0;
         std::size_t   n = 0;
         while (n < id.size() && id[n] >= '0' && id[n] <= '9')
         {
            if (n >= 18)
               return false;
            v = v * 10 + (id[n] - '0');
            ++n;
         }
         id.remove_prefix(n);
         out = v;
         return true;
      };
      if (!number(k.num) || id.empty() || id[0] != '/')
         return std::nullopt;
      id.remove_prefix(1);
      if (!number(k.den) || !id.empty())
         return std::nullopt;
      // Lowest terms, and not one of the named periods.
      std::intmax_t a = k.num, b = k.den;
      while (b)
      {
         const auto t = a % b;
         a            = b;
         b            = t;
      }
      if (a != 1)
         return std::nullopt;
      if (k.num == 1 && (k.den == 1 || k.den == 1000 || k.den == 1000000 || k.den == 1000000000))
         return std::nullopt;
      return k;
   }

   // ── RFC 3339 text ─────────────────────────────────────────────────────

   // The longest time point text: "YYYY-MM-DDTHH:MM:SS.fffffffffZ".
   inline constexpr std::size_t max_time_text_size = 30;

   namespace detail::time_impl {

      // 0000-01-01T00:00:00Z and 9999-12-31T23:59:59Z as Unix seconds.
      inline constexpr std::int64_t min_text_seconds = -62167219200;
      inline constexpr std::int64_t max_text_seconds = 253402300799;

      inline constexpr std::uint32_t pow10[10] = {1,      10,      100,      1000,      10000,
                                                  100000, 1000000, 10000000, 100000000, 1000000000};

      // Proleptic Gregorian day arithmetic (days since 1970-01-01).
      constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept
      {
         y -= m <= 2;
         const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
         const auto         yoe = static_cast<unsigned>(y - era * 400);
         const unsigned     doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
         const unsigned     doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
         return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
      }

      constexpr unsigned days_in_month(unsigned y, unsigned m) noexcept
      {
         constexpr unsigned char days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
         if (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))
            return 29;
         return days[m - 1];
      }

      constexpr char* put2(char* p, unsigned v) noexcept
      {
         p[0] = static_cast<char>('0' + v / 10);
         p[1] = static_cast<char>('0' + v % 10);
         return p + 2;
      }

      // Writes the text of `secs` Unix seconds plus `sub` units of
      // 10^-digits seconds. secs must be within the text range. Writes at
      // most max_time_text_size bytes; returns the end.
      constexpr char* write_civil(char* p, std::int64_t secs, std::uint32_t sub, int digits) noexcept
      {
         std::int64_t days = secs / 86400;
         std::int64_t rem  = secs % 86400;
         if (rem < 0)
         {
            rem += 86400;
            --days;
         }
         // civil_from_days. Only January and February of year 0000 lie
         // before the 0000-03-01 origin of the era arithmetic.
         const std::int64_t z   = days + 719468;
         const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
         const auto         doe = static_cast<unsigned>(z - era * 146097);
         const unsigned     yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
         const unsigned     doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
         const unsigned     mp  = (5 * doy + 2) / 153;
         const unsigned     d   = doy - (153 * mp + 2) / 5 + 1;
         const unsigned     m   = mp < 10 ? mp + 3 : mp - 9;
         const auto         y   = static_cast<unsigned>(yoe + era * 400 + (m <= 2));
         const auto         s   = static_cast<unsigned>(rem);

         p    = put2(p, y / 100);
         p    = put2(p, y % 100);
         *p++ = '-';
         p    = put2(p, m);
         *p++ = '-';
         p    = put2(p, d);
         *p++ = 'T';
         p    = put2(p, s / 3600);
         *p++ = ':';
         p    = put2(p, s / 60 % 60);
         *p++ = ':';
         p    = put2(p, s % 60);
         if (digits > 0)
         {
            *p++ = '.';
            for (int i = digits - 1; i >= 0; --i)
            {
               p[i] = static_cast<char>('0' + sub % 10);
               sub /= 10;
            }
            p += digits;
         }
         *p++ = 'Z';
         return p;
      }

      // Parses an RFC 3339 date-time into UTC Unix seconds plus `sub` units
      // of 10^-digits seconds. Returns nullptr, or why the text is rejected.
      constexpr const char* parse_civil(std::string_view s,
                                        int              digits,
                                        std::int64_t&    secs,
                                        std::uint32_t&   sub) noexcept
      {
         // "YYYY-MM-DDTHH:MM:SS" + "Z" at the least.
         if (s.size() < 20)
            return "time: not an RFC 3339 date-time";
         auto num = [&](std::size_t at, std::size_t n, unsigned& out) {
            unsigned v = 0;
            for (std::size_t i = at; i < at + n; ++i)
            {
               if (s[i] < '0' || s[i] > '9')
                  return false;
               v = v * 10 + static_cast<unsigned>(s[i] - '0');
            }
            out = v;
            return true;
         };
         unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
         if (!num(0, 4, y) || s[4] != '-' || !num(5, 2, mo) || s[7] != '-' || !num(8, 2, d) ||
             (s[10] != 'T' && s[10] != 't') || !num(11, 2, h) || s[13] != ':' || !num(14, 2, mi) ||
             s[16] != ':' || !num(17, 2, sec))
            return "time: not an RFC 3339 date-time";
         if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo))
            return "time: no such calendar date";
         if (sec == 60)
            return "time: leap seconds are not representable";
         if (h > 23 || mi > 59 || sec > 59)
            return "time: no such time of day";

         std::size_t   at   = 19;
         std::uint32_t frac = 0;
         if (s[at] == '.')
         {
            ++at;
            int n = 0;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9')
            {
               if (n < digits)
                  frac = frac * 10 + static_cast<std::uint32_t>(s[at] - '0');
               else if (s[at] != '0')
                  return "time: fraction is finer than the unit";
               ++n;
               ++at;
            }
            if (n == 0)
               return "time: not an RFC 3339 date-time";
            for (; n < digits; ++n)
               frac *= 10;
         }
         if (at >= s.size())
            return "time: missing UTC offset";
         std::int64_t offset = 0;
         if (s[at] == 'Z' || s[at] == 'z')
            ++at;
         else if (s[at] == '+' || s[at] == '-')
         {
            unsigned oh = 0, om = 0;
            if (s.size() - at < 6 || !num(at + 1, 2, oh) || s[at + 3] != ':' ||
                !num(at + 4, 2, om) || oh > 23 || om > 59)
               return "time: malformed UTC offset";
            offset = static_cast<std::int64_t>(oh * 3600 + om * 60);
            if (s[at] == '-')
               offset = -offset;
            at += 6;
         }
         else
            return "time: missing UTC offset";
         if (at != s.size())
            return "time: not an RFC 3339 date-time";

         const std::int64_t utc =
            days_from_civil(y, mo, d) * 86400 + static_cast<std::int64_t>(h * 3600 + mi * 60 + sec) -
            offset;
         if (utc < min_text_seconds || utc > max_text_seconds)
            return "time: outside years 0000..9999";
         secs = utc;
         sub  = frac;
         return nullptr;
      }

      // ticks → (seconds, sub-second units); false outside the text range.
      template <typename Rep>
      constexpr bool split_ticks(Rep t, int digits, std::int64_t& secs, std::uint32_t& sub) noexcept
      {
         const std::uint32_t scale = pow10[digits];
         if constexpr (std::is_signed_v<Rep>)
         {
            const std::int64_t v = t;
            std::int64_t       q = v / static_cast<std::int64_t>(scale);
            std::int64_t       r = v % static_cast<std::int64_t>(scale);
            if (r < 0)
            {
               r += scale;
               --q;
            }
            if (q < min_text_seconds || q > max_text_seconds)
               return false;
            secs = q;
            sub  = static_cast<std::uint32_t>(r);
         }
         else
         {
            const std::uint64_t v = t;
            const std::uint64_t q = v / scale;
            if (q > static_cast<std::uint64_t>(max_text_seconds))
               return false;
            secs = static_cast<std::int64_t>(q);
            sub  = static_cast<std::uint32_t>(v % scale);
         }
         return true;
      }

      // (seconds, sub-second units) → ticks; false when Rep cannot hold it.
      template <typename Rep>
      constexpr bool join_ticks(std::int64_t secs, std::uint32_t sub, int digits, Rep& out) noexcept
      {
         const __int128 v = static_cast<__int128>(secs) * pow10[digits] + sub;
         if (v < static_cast<__int128>(std::numeric_limits<Rep>::min()) ||
             v > static_cast<__int128>(std::numeric_limits<Rep>::max()))
            return false;
         out = static_cast<Rep>(v);
         return true;
      }

   }  // namespace detail::time_impl

   template <typename T>
   concept TextTimePoint =
      TimePointValue<T> && time_fraction_digits(time_traits<std::remove_cvref_t<T>>::num,
                                                time_traits<std::remove_cvref_t<T>>::den) >= 0;

   // Fraction digits of T's text form.
   template <TextTimePoint T>
   inline constexpr int time_text_digits_v = time_fraction_digits(
      time_traits<std::remove_cvref_t<T>>::num, time_traits<std::remove_cvref_t<T>>::den);

   // Writes the RFC 3339 text of `v` at `out` (max_time_text_size bytes
   // available) and returns its end, or nullptr when `v` lies outside years
   // 0000..9999 and so has no text.
   template <TextTimePoint T>
   constexpr char* write_time_text(const T& v, char* out) noexcept
   {
      std::int64_t  secs = 0;
      std::uint32_t sub  = 0;
      if (!detail::time_impl::split_ticks(time_ticks(v), time_text_digits_v<T>, secs, sub))
         return nullptr;
      return detail::time_impl::write_civil(out, secs, sub, time_text_digits_v<T>);
   }

   // Reads RFC 3339 text into `out`. Returns nullptr on success, or a
   // static message saying why the text is rejected (`out` is untouched).
   template <TextTimePoint T>
   constexpr const char* parse_time_text(std::string_view text, T& out) noexcept
   {
      std::int64_t  secs = 0;
      std::uint32_t sub  = 0;
      if (const char* err = detail::time_impl::parse_civil(text, time_text_digits_v<T>, secs, sub))
         return err;
      time_rep_t<T> ticks{};
      if (!detail::time_impl::join_ticks(secs, sub, time_text_digits_v<T>, ticks))
         return "time: outside the range of the tick type";
      out = time_from_ticks<T>(ticks);
      return nullptr;
   }

   // ── Runtime-typed forms, for schema-driven codecs ─────────────────────
   //
   // The tick type is known only as (bits, signed) and the unit as its
   // fraction digit count; ticks travel as a 128-bit integer.

   // As write_time_text: nullptr when the value has no text.
   inline char* write_time_text(__int128 ticks, int digits, char* out) noexcept
   {
      const __int128 scale = detail::time_impl::pow10[digits];
      __int128       q     = ticks / scale;
      __int128       r     = ticks % scale;
      if (r < 0)
      {
         r += scale;
         --q;
      }
      if (q < detail::time_impl::min_text_seconds || q > detail::time_impl::max_text_seconds)
         return nullptr;
      return detail::time_impl::write_civil(out, static_cast<std::int64_t>(q),
                                            static_cast<std::uint32_t>(r), digits);
   }

   // As parse_time_text, for a tick integer of `bits` bits (at most 64).
   inline const char* parse_time_text(std::string_view text,
                                      int              digits,
                                      std::uint32_t    bits,
                                      bool             is_signed,
                                      __int128&        ticks) noexcept
   {
      std::int64_t  secs = 0;
      std::uint32_t sub  = 0;
      if (const char* err = detail::time_impl::parse_civil(text, digits, secs, sub))
         return err;
      const __int128 v    = static_cast<__int128>(secs) * detail::time_impl::pow10[digits] + sub;
      const __int128 one  = 1;
      const __int128 low  = is_signed ? -(one << (bits - 1)) : 0;
      const __int128 high = is_signed ? (one << (bits - 1)) - 1 : (one << bits) - 1;
      if (bits == 0 || bits > 64 || v < low || v > high)
         return "time: outside the range of the tick type";
      ticks = v;
      return nullptr;
   }

   // Convenience forms. An empty string / nullopt is the failure result.
   template <TextTimePoint T>
   std::string to_rfc3339(const T& v)
   {
      char        buf[max_time_text_size];
      const char* end = write_time_text(v, buf);
      return end ? std::string(buf, static_cast<std::size_t>(end - buf)) : std::string{};
   }

   template <TextTimePoint T>
   constexpr std::optional<T> from_rfc3339(std::string_view text) noexcept
   {
      T out{};
      if (parse_time_text(text, out))
         return std::nullopt;
      return out;
   }

}  // namespace psio
