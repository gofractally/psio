# Time values

A time value is declared once and is the same value in every PSIO format and
language. There are three kinds, each generic over a **unit** (the tick
period) and a **tick type** (an integer of at most 64 bits):

* a **duration** — a signed or unsigned count of ticks;
* a **time point** — a count of ticks since the Unix epoch
  1970-01-01T00:00:00Z, UTC, with no leap seconds (POSIX time);
* a **monotonic time point** — a count of ticks since the epoch of some
  other clock (a steady / boot-relative clock, a simulation clock). The
  epoch is the clock's own business: PSIO does not know it, so the value
  orders and subtracts like a time point but has **no date**.

| | duration | time point | monotonic time point |
|---|---|---|---|
| C++ | `std::chrono::duration<Rep, Period>` | `std::chrono::time_point<std::chrono::system_clock, duration<Rep, Period>>` (`sys_time<D>`) | `std::chrono::time_point<Clock, duration<Rep, Period>>` for any other `Clock` (`steady_clock`, a user clock) |
| Rust | `psio::time::Duration<U, R = i64>` | `psio::time::TimePoint<U, R = i64>` | `psio::time::MonotonicPoint<U, R = i64>` |
| JS | `bigint` ticks | `bigint` ticks (helpers: `timePointToIso`, `timePointFromIso`, `timePointToDate`, `timePointFromDate`) | `bigint` ticks |
| Schema IR | `Custom{ Int{bits, signed}, "duration-<unit>" }` | `Custom{ Int{bits, signed}, "time-point-<unit>" }` | `Custom{ Int{bits, signed}, "monotonic-<unit>" }` |

C++ accepts any integral `Rep` (not `bool`) and any `std::ratio` period. The
clock decides the kind: `system_clock` is a time point, every other clock is
monotonic (PSIO does not distinguish between them: two monotonic time points
of one unit and tick type are one schema type, as two integers are; keeping
values of different clocks apart is the C++ type system's job). Rust units
are types implementing `psio::time::Unit` (`Seconds`, `Millis`, `Micros`,
`Nanos` are provided; any `NUM/DEN` can be declared).

## Schema id

```
id   = ("time-point" | "duration" | "monotonic") "-" unit
unit = "seconds" | "millis" | "micros" | "nanos"      ; 1, 1/10^3, 1/10^6, 1/10^9 s
     | num "/" den                                    ; any other period, lowest terms
```

`num` and `den` are positive decimal integers without leading zeros, in
lowest terms: `duration-60/1` (minutes), `time-point-86400/1` (days),
`duration-1/60`, `time-point-1/10`, `monotonic-micros`. The four names are the only spelling of
their periods (`duration-1/1000` is not an id). The inner type is the tick
integer, so a reader that does not know the id still knows the layout.

A time id is a **built-in presentation** of the schema's `Custom` mechanism
(`docs/json.md`, "Custom types through a schema"): like any `Custom` id that
is not part of the binary form it is exactly its inner type on the wire, and
the schema-driven codecs of all three languages supply its text themselves.
It takes no handler (C++ `pssz_ir::CustomHandlers` and Rust
`dynamic::CustomHandlers` never consult one registered under a time id; JS
refuses to register one), and it is presented also when every custom id is
required to have a handler. The spellings `time-point`, `duration` and
`monotonic`, alone or followed by `-`, are reserved: a reserved id that is not a time id as
defined above, or a time id over anything but an 8, 16, 32 or 64-bit `Int`
(directly, or through named aliases), is a malformed schema and is refused
by every schema-driven entry point.

In an evolution check a time value is its own type: an integer and a time
point, a time point and a duration, a time point and a monotonic time point,
or two time values of different unit or tick type do not evolve from one
another (C++ `pssz_evolves_from`, Rust
`pssz_evolves_from`), although their bytes coincide.

## Binary and key

Every binary format encodes the tick count exactly as it encodes the tick
integer type: pssz / bin / ssz write `sizeof(Rep)` little-endian bytes, fixed
size, so a record of time values can be fixed-layout. `key` writes the
integer's key form (sign-flipped big-endian for a signed tick type), so key
order is chronological. Every tick count is a valid value.

## Text (json and ijson)

**Duration:** the tick count, under the format's integer rules — a JSON
number; under ijson a 64-bit count is a number while `|n| <= 2^53 - 1` and a
decimal string beyond; member integer-form annotations apply. (ISO 8601
durations are not used: months and years have no fixed length, and one value
would have many spellings.)

**Monotonic time point:** the tick count, exactly as a duration. It is
never an RFC 3339 string in either direction: there is no date to write,
and a date-time string is rejected on input. It has text for every unit
(the "units of `10^-k` seconds" restriction below is the calendar form's).

**Time point:** an RFC 3339 `date-time` string. Defined for units of
`10^-k` seconds, k = 0..9; a time point with any other unit has no text form
(a compile error in the typed C++ / Rust codecs, an error in the schema-driven
ones).

Written form — one per value:

```
YYYY-MM-DDTHH:MM:SSZ                 k = 0   2026-11-22T10:15:30Z
YYYY-MM-DDTHH:MM:SS.fffZ             k = 3   2026-11-22T10:15:30.123Z
YYYY-MM-DDTHH:MM:SS.ffffffZ          k = 6   2026-11-22T10:15:30.123456Z
YYYY-MM-DDTHH:MM:SS.fffffffffZ       k = 9   2026-11-22T10:15:30.123456789Z
```

UTC, upper-case `T` and `Z`, exactly k fraction digits (trailing zeros
kept), proleptic Gregorian calendar, four-digit years **0000..9999**. A value
before `0000-01-01T00:00:00Z` (-62167219200 s) or after
`9999-12-31T23:59:59.9…Z` (253402300799 s) has no text: writing it fails
(the binary and key forms are unaffected).

Accepted on input — any RFC 3339 `date-time`:

* `T` and `Z` in either case;
* `Z`, or an offset `+HH:MM` / `-HH:MM` (HH 00..23, MM 00..59), which is
  applied to obtain UTC; `-00:00` is UTC;
* no fraction, or a fraction of one or more digits of any length, provided
  the digits beyond the unit's precision are all `0`.

Rejected:

* a fraction the unit cannot represent exactly (`.5` for seconds, `.1234`
  for milliseconds) — never rounded;
* a leap second (`:60`). POSIX time has no such instant; mapping it onto a
  neighbour would make two texts mean one value and silently shift time;
* hour 24, minute 60, month 0 or 13, a day the month does not have
  (`2026-02-29`, `2100-02-29`);
* a missing offset, a space instead of `T`, the basic format, a two-digit or
  five-digit or signed year, any other deviation from the grammar;
* an instant outside years 0000..9999 **after** applying the offset
  (`0000-01-01T00:00:00+00:01`);
* an instant whose tick count does not fit the tick type;
* anything that is not a JSON string (a number is not a time point).

The JSON string's value is what is read: `"Z"` for `Z` is the same text.

No time zone database, no locale. The C++ codec is `constexpr`, allocation-
free and works without exceptions (`psio/time.hpp`: `write_time_text`,
`parse_time_text`, `to_rfc3339`, `from_rfc3339`).

## JS representation

`Date` holds milliseconds, so the schema-driven JS codecs decode both kinds
to `bigint` ticks and never convert implicitly. `timePointToDate` throws
unless the value is a whole number of milliseconds within `Date`'s range;
`timePointToIso` / `timePointFromIso` are the exact text codec.
