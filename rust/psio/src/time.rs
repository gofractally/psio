//! Time as a first-class PSIO value (docs/time.md). The Rust counterpart of
//! C++ `std::chrono` durations and `system_clock` time points as seen by
//! psio (cpp/include/psio/time.hpp): identical bytes and text
//! (conformance/time-key-vectors.json).
//!
//! * [`Duration<U, R>`] — a count of ticks of unit `U` held in integer `R`.
//! * [`TimePoint<U, R>`] — ticks since the Unix epoch, UTC, no leap seconds.
//! * [`MonotonicPoint<U, R>`] — ticks since the epoch of some other clock
//!   (steady / boot-relative, simulation): it orders and subtracts like a
//!   time point but has no date. C++ `std::chrono::time_point<Clock, ...>`
//!   for any clock other than `system_clock`.
//!
//! `U` is a [`Unit`] ([`Seconds`], [`Millis`], [`Micros`], [`Nanos`], or any
//! type declaring `NUM / DEN` seconds per tick); `R` is the tick integer
//! (`i8..i64`, `u8..u64`, default `i64`).
//!
//! Every format sees the tick count: pssz writes `R` (fixed size, so records
//! of time values stay fixed-layout), `key` writes `R`'s key form
//! (chronological order), the schema is `Custom{Int, "time-point-micros"}`
//! (`duration-micros`, `monotonic-micros`).
//! In json / ijson a duration is the tick integer and a time point is an
//! RFC 3339 UTC string with exactly the unit's fraction digits
//! (`2026-11-22T10:15:30.123456Z`), years 0000..9999. A monotonic time
//! point is the tick integer, exactly as a duration: never a date.
//!
//! Values compute: `point + duration`, `point - point`, `duration * n` and
//! the rest of the operators are exact and panic on overflow of the tick
//! type in every build profile (`checked_*` return `None`); `to_unit` /
//! `floor` / `ceil` convert between units; `Display` and `FromStr` are the
//! JSON text without the quotes; the values order, hash and default. The
//! cargo feature `chrono` adds exact `TryFrom` conversions with
//! `chrono::DateTime` / `TimeDelta`, and `async-graphql` makes the values
//! GraphQL scalars (`psio::graphql_scalars`).
//!
//! ```
//! use psio::time::{Duration, Micros, Seconds, TimePoint};
//!
//! let t = TimePoint::<Seconds>::from_rfc3339("2026-11-22T12:15:30+02:00").unwrap();
//! assert_eq!(t.ticks(), 1795342530);
//! assert_eq!(t.to_rfc3339().unwrap(), "2026-11-22T10:15:30Z");
//! assert_eq!(t.to_string(), "2026-11-22T10:15:30Z");
//! assert_eq!("2026-11-22T10:15:30Z".parse::<TimePoint<Seconds>>().unwrap(), t);
//! let later = t + Duration::from_ticks(90);
//! assert_eq!((later - t).ticks(), 90);
//! let micros: TimePoint<Micros> = later.to_unit().unwrap();
//! assert_eq!(micros.floor::<Seconds>(), later);
//! // A fraction the unit cannot hold exactly is rejected, never rounded.
//! assert!(TimePoint::<Seconds>::from_rfc3339("2026-11-22T10:15:30.5Z").is_err());
//! assert!(TimePoint::<Micros>::from_rfc3339("2026-11-22T10:15:30.5Z").is_ok());
//! ```

use crate::json::{JsonText, JsonTextError, Reader, Writer};
use crate::key::{KeyPack, KeyReader, KeyResult, KeyUnpack};
use crate::pssz::{PsszFormat, PsszPack, PsszResult, PsszUnpack, PsszValidate};
use crate::pssz_evolves::Shape;
use crate::pssz_schema_ir::{AnyType, AnyValue, Custom, Int, SchemaBuilder, ToPsszSchema};
use crate::pssz_view::PsszView;
use std::marker::PhantomData;
use std::time::SystemTime;

/// Why a time conversion or text was rejected.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TimeError(pub &'static str);

impl std::fmt::Display for TimeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}
impl std::error::Error for TimeError {}

// ── Units ───────────────────────────────────────────────────────────────────

/// A tick period of `NUM / DEN` seconds. `NUM` and `DEN` must be positive
/// and in lowest terms (C++ `std::ratio`).
pub trait Unit: 'static {
    const NUM: u64;
    const DEN: u64;
}

macro_rules! unit {
    ($($(#[$doc:meta])* $name:ident = $num:expr, $den:expr;)*) => {$(
        $(#[$doc])*
        #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
        pub struct $name;
        impl Unit for $name {
            const NUM: u64 = $num;
            const DEN: u64 = $den;
        }
    )*};
}
unit! {
    /// One second.
    Seconds = 1, 1;
    /// 10^-3 seconds.
    Millis = 1, 1_000;
    /// 10^-6 seconds.
    Micros = 1, 1_000_000;
    /// 10^-9 seconds.
    Nanos = 1, 1_000_000_000;
}

mod sealed {
    pub trait Sealed {}
}

/// A tick integer: `i8..i64` or `u8..u64`.
pub trait Tick:
    Copy + Eq + Ord + std::hash::Hash + std::fmt::Debug + Default + Send + Sync + sealed::Sealed + 'static
{
    const BITS: u32;
    const SIGNED: bool;
    const MIN: Self;
    const MAX: Self;
    fn to_i128(self) -> i128;
    fn from_i128(value: i128) -> Option<Self>;
}
macro_rules! tick {
    ($($ty:ty, $signed:expr;)*) => {$(
        impl sealed::Sealed for $ty {}
        impl Tick for $ty {
            const BITS: u32 = <$ty>::BITS;
            const SIGNED: bool = $signed;
            const MIN: Self = <$ty>::MIN;
            const MAX: Self = <$ty>::MAX;
            fn to_i128(self) -> i128 { self as i128 }
            fn from_i128(value: i128) -> Option<Self> { <$ty>::try_from(value).ok() }
        }
    )*};
}
tick! {
    i8, true; i16, true; i32, true; i64, true;
    u8, false; u16, false; u32, false; u64, false;
}

// ── Schema id ───────────────────────────────────────────────────────────────

/// What a time schema id says: time point, monotonic time point or
/// duration, and the period.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TimeKind {
    /// A time point of the Unix epoch (`time-point-<unit>`).
    pub point: bool,
    /// Period = `num / den` seconds, lowest terms.
    pub num: u64,
    pub den: u64,
    /// A time point of some other clock (`monotonic-<unit>`). Neither
    /// `point` nor `monotonic`: a duration.
    pub monotonic: bool,
}

/// Fraction digits of a period of 10^-k seconds (k = 0..9); `None` for any
/// other period, which has no RFC 3339 form.
pub const fn time_fraction_digits(num: u64, den: u64) -> Option<u32> {
    if num != 1 {
        return None;
    }
    let mut k = 0;
    let mut p = 1u64;
    while p < den && k < 9 {
        p *= 10;
        k += 1;
    }
    if p == den {
        Some(k)
    } else {
        None
    }
}

/// `("time-point" | "duration" | "monotonic") "-" unit`; unit is `seconds`, `millis`,
/// `micros`, `nanos`, or `num/den` for any other period.
pub fn time_schema_id(kind: TimeKind) -> String {
    let unit = match (kind.num, kind.den) {
        (1, 1) => "seconds".to_string(),
        (1, 1_000) => "millis".to_string(),
        (1, 1_000_000) => "micros".to_string(),
        (1, 1_000_000_000) => "nanos".to_string(),
        (num, den) => format!("{num}/{den}"),
    };
    let word = if kind.point {
        "time-point"
    } else if kind.monotonic {
        "monotonic"
    } else {
        "duration"
    };
    format!("{word}-{unit}")
}

/// The ids the time presentations reserve: `time-point` / `duration` /
/// `monotonic`, alone or followed by `-`. A reserved id that [`parse_time_schema_id`] does not
/// accept is a malformed schema, never some other custom type.
pub fn reserved_time_schema_id(id: &str) -> bool {
    ["time-point", "duration", "monotonic"]
        .iter()
        .any(|kind| id.strip_prefix(kind).is_some_and(|rest| rest.is_empty() || rest.starts_with('-')))
}

/// The inverse of [`time_schema_id`]; `None` for any other id, including a
/// non-canonical spelling (`1/1000` for millis, `2/4`, leading zeros).
pub fn parse_time_schema_id(id: &str) -> Option<TimeKind> {
    let (point, monotonic, unit) = if let Some(unit) = id.strip_prefix("time-point-") {
        (true, false, unit)
    } else if let Some(unit) = id.strip_prefix("monotonic-") {
        (false, true, unit)
    } else {
        (false, false, id.strip_prefix("duration-")?)
    };
    let (num, den) = match unit {
        "seconds" => (1, 1),
        "millis" => (1, 1_000),
        "micros" => (1, 1_000_000),
        "nanos" => (1, 1_000_000_000),
        _ => {
            fn number(text: &str) -> Option<u64> {
                let bytes = text.as_bytes();
                if bytes.is_empty()
                    || bytes.len() > 18
                    || bytes[0] == b'0'
                    || !bytes.iter().all(u8::is_ascii_digit)
                {
                    return None;
                }
                text.parse().ok()
            }
            let (num, den) = unit.split_once('/')?;
            let (num, den) = (number(num)?, number(den)?);
            let (mut a, mut b) = (num, den);
            while b != 0 {
                (a, b) = (b, a % b);
            }
            if a != 1 || (num == 1 && matches!(den, 1 | 1_000 | 1_000_000 | 1_000_000_000)) {
                return None;
            }
            (num, den)
        }
    };
    Some(TimeKind { point, num, den, monotonic })
}

// ── RFC 3339 text ───────────────────────────────────────────────────────────

/// 0000-01-01T00:00:00Z and 9999-12-31T23:59:59Z as Unix seconds.
const MIN_TEXT_SECONDS: i64 = -62_167_219_200;
const MAX_TEXT_SECONDS: i64 = 253_402_300_799;

const fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = (if y >= 0 { y } else { y - 399 }) / 400;
    let yoe = y - era * 400;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

const fn days_in_month(y: i64, m: i64) -> i64 {
    const DAYS: [i64; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) {
        29
    } else {
        DAYS[(m - 1) as usize]
    }
}

/// Text of `secs` (within the text range) plus `sub` units of 10^-digits s.
fn write_civil(out: &mut String, secs: i64, mut sub: u32, digits: u32) {
    let days = secs.div_euclid(86_400);
    let s = secs.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    push2(out, y / 100);
    push2(out, y % 100);
    out.push('-');
    push2(out, m);
    out.push('-');
    push2(out, d);
    out.push('T');
    push2(out, s / 3600);
    out.push(':');
    push2(out, s / 60 % 60);
    out.push(':');
    push2(out, s % 60);
    if digits > 0 {
        out.push('.');
        let mut frac = [b'0'; 9];
        for slot in frac[..digits as usize].iter_mut().rev() {
            *slot = b'0' + (sub % 10) as u8;
            sub /= 10;
        }
        out.push_str(std::str::from_utf8(&frac[..digits as usize]).expect("ascii digits"));
    }
    out.push('Z');
}

fn push2(out: &mut String, value: i64) {
    out.push((b'0' + (value / 10) as u8) as char);
    out.push((b'0' + (value % 10) as u8) as char);
}

/// The RFC 3339 UTC text of a time point of `ticks` units of 10^-`digits`
/// seconds, with exactly `digits` fraction digits: the text form of the
/// typed [`TimePoint`] and of a `time-point-<unit>` schema node alike. An
/// error for an instant outside years 0000..9999, which has no text.
pub fn time_text_of_ticks(ticks: i128, digits: u32) -> Result<String, TimeError> {
    let scale = 10i128.pow(digits);
    let (secs, sub) = (ticks.div_euclid(scale), ticks.rem_euclid(scale));
    if secs < MIN_TEXT_SECONDS as i128 || secs > MAX_TEXT_SECONDS as i128 {
        return Err(TimeError("time: outside years 0000..9999"));
    }
    let mut out = String::with_capacity(30);
    write_civil(&mut out, secs as i64, sub as u32, digits);
    Ok(out)
}

/// The inverse of [`time_text_of_ticks`] for a tick integer of `bits` bits
/// (8, 16, 32 or 64): any RFC 3339 date-time whose instant is a whole number
/// of ticks that fits the tick type.
pub fn time_ticks_of_text(
    text: &str,
    digits: u32,
    bits: u32,
    signed: bool,
) -> Result<i128, TimeError> {
    let (secs, sub) = parse_civil(text, digits)?;
    let ticks = secs as i128 * 10i128.pow(digits) + sub as i128;
    let (min, max) = if signed {
        (-(1i128 << (bits - 1)), (1i128 << (bits - 1)) - 1)
    } else {
        (0, (1i128 << bits) - 1)
    };
    if ticks < min || ticks > max {
        return Err(TimeError("time: outside the range of the tick type"));
    }
    Ok(ticks)
}

/// Parses an RFC 3339 date-time into UTC Unix seconds plus units of
/// 10^-digits seconds.
fn parse_civil(text: &str, digits: u32) -> Result<(i64, u32), TimeError> {
    const GRAMMAR: TimeError = TimeError("time: not an RFC 3339 date-time");
    let s = text.as_bytes();
    if s.len() < 20 {
        return Err(GRAMMAR);
    }
    let num = |at: usize, n: usize| -> Option<i64> {
        let mut v = 0i64;
        for &c in s.get(at..at + n)? {
            if !c.is_ascii_digit() {
                return None;
            }
            v = v * 10 + (c - b'0') as i64;
        }
        Some(v)
    };
    let fields = (|| {
        let y = num(0, 4)?;
        let mo = num(5, 2)?;
        let d = num(8, 2)?;
        let h = num(11, 2)?;
        let mi = num(14, 2)?;
        let sec = num(17, 2)?;
        (s[4] == b'-' && s[7] == b'-' && (s[10] | 0x20) == b't' && s[13] == b':' && s[16] == b':')
            .then_some((y, mo, d, h, mi, sec))
    })();
    let Some((y, mo, d, h, mi, sec)) = fields else {
        return Err(GRAMMAR);
    };
    if !(1..=12).contains(&mo) || d < 1 || d > days_in_month(y, mo) {
        return Err(TimeError("time: no such calendar date"));
    }
    if sec == 60 {
        return Err(TimeError("time: leap seconds are not representable"));
    }
    if h > 23 || mi > 59 || sec > 59 {
        return Err(TimeError("time: no such time of day"));
    }

    let mut at = 19;
    let mut frac = 0u32;
    if s[at] == b'.' {
        at += 1;
        let mut n = 0;
        while at < s.len() && s[at].is_ascii_digit() {
            if n < digits {
                frac = frac * 10 + (s[at] - b'0') as u32;
            } else if s[at] != b'0' {
                return Err(TimeError("time: fraction is finer than the unit"));
            }
            n += 1;
            at += 1;
        }
        if n == 0 {
            return Err(GRAMMAR);
        }
        while n < digits {
            frac *= 10;
            n += 1;
        }
    }
    let offset = match s.get(at) {
        Some(b'Z' | b'z') => {
            at += 1;
            0
        }
        Some(&sign @ (b'+' | b'-')) => {
            const OFFSET: TimeError = TimeError("time: malformed UTC offset");
            if s.len() - at < 6 || s[at + 3] != b':' {
                return Err(OFFSET);
            }
            let (Some(oh), Some(om)) = (num(at + 1, 2), num(at + 4, 2)) else {
                return Err(OFFSET);
            };
            if oh > 23 || om > 59 {
                return Err(OFFSET);
            }
            at += 6;
            let offset = oh * 3600 + om * 60;
            if sign == b'-' {
                -offset
            } else {
                offset
            }
        }
        _ => return Err(TimeError("time: missing UTC offset")),
    };
    if at != s.len() {
        return Err(GRAMMAR);
    }
    let utc = days_from_civil(y, mo, d) * 86_400 + h * 3600 + mi * 60 + sec - offset;
    if !(MIN_TEXT_SECONDS..=MAX_TEXT_SECONDS).contains(&utc) {
        return Err(TimeError("time: outside years 0000..9999"));
    }
    Ok((utc, frac))
}

/// Fraction digits of `U`'s text form; using it with a unit that is not
/// 10^-k seconds is a compile-time error:
///
/// ```compile_fail
/// use psio::time::{TimePoint, Unit};
/// struct Minutes;
/// impl Unit for Minutes {
///     const NUM: u64 = 60;
///     const DEN: u64 = 1;
/// }
/// // Binary, key and schema are fine; text is not defined for minutes.
/// let _ = TimePoint::<Minutes>::from_ticks(1).to_rfc3339();
/// ```
struct TextDigits<U>(PhantomData<U>);
impl<U: Unit> TextDigits<U> {
    const VALUE: u32 = match time_fraction_digits(U::NUM, U::DEN) {
        Some(digits) => digits,
        None => panic!(
            "psio::time: a time point has RFC 3339 text only for a unit of 1/10^k seconds, k = 0..9"
        ),
    };
}

// ── The value types ─────────────────────────────────────────────────────────

/// Ticks of unit `U` since 1970-01-01T00:00:00Z (UTC, no leap seconds).
/// C++ `std::chrono::sys_time<duration<R, U>>`.
pub struct TimePoint<U, R = i64> {
    ticks: R,
    unit: PhantomData<fn() -> U>,
}

/// Ticks of unit `U` since the epoch of a clock other than the system
/// clock (steady / boot-relative, simulation). The epoch is the clock's own
/// business, so the value orders and subtracts like a [`TimePoint`] but has
/// no date: no calendar or RFC 3339 form, and its text is the tick count.
/// C++ `std::chrono::time_point<Clock, duration<R, U>>` for any `Clock`
/// other than `system_clock`. Two monotonic time points of one unit and
/// tick type are one schema type; keeping clocks apart is up to the caller.
///
/// ```compile_fail
/// use psio::time::{MonotonicPoint, Seconds};
/// let _ = MonotonicPoint::<Seconds>::from_ticks(1).to_rfc3339();
/// ```
pub struct MonotonicPoint<U, R = i64> {
    ticks: R,
    unit: PhantomData<fn() -> U>,
}

/// A count of ticks of unit `U`. C++ `std::chrono::duration<R, U>`.
pub struct Duration<U, R = i64> {
    ticks: R,
    unit: PhantomData<fn() -> U>,
}

const NANOS_PER_SECOND: i128 = 1_000_000_000;

/// nanoseconds → ticks of `U`, exactly.
fn ticks_from_nanos<U: Unit, R: Tick>(nanos: i128) -> Result<R, TimeError> {
    let scaled = nanos.checked_mul(U::DEN as i128).ok_or(TimeError("time: out of range"))?;
    let per_tick = U::NUM as i128 * NANOS_PER_SECOND;
    if scaled % per_tick != 0 {
        return Err(TimeError("time: not a whole number of ticks"));
    }
    R::from_i128(scaled / per_tick).ok_or(TimeError("time: outside the range of the tick type"))
}

/// ticks of `U` → nanoseconds, exactly.
fn nanos_from_ticks<U: Unit, R: Tick>(ticks: R) -> Result<i128, TimeError> {
    let scaled = ticks
        .to_i128()
        .checked_mul(U::NUM as i128 * NANOS_PER_SECOND)
        .ok_or(TimeError("time: out of range"))?;
    if scaled % U::DEN as i128 != 0 {
        return Err(TimeError("time: not a whole number of nanoseconds"));
    }
    Ok(scaled / U::DEN as i128)
}

fn std_duration(nanos: i128) -> Result<std::time::Duration, TimeError> {
    if nanos < 0 {
        return Err(TimeError("time: negative duration"));
    }
    let secs = u64::try_from(nanos / NANOS_PER_SECOND).map_err(|_| TimeError("time: out of range"))?;
    Ok(std::time::Duration::new(secs, (nanos % NANOS_PER_SECOND) as u32))
}

macro_rules! time_value {
    ($name:ident, $label:literal, $point:expr, $monotonic:expr) => {
        impl<U, R> $name<U, R> {
            pub const fn from_ticks(ticks: R) -> Self {
                Self { ticks, unit: PhantomData }
            }
        }
        impl<U, R: Copy> $name<U, R> {
            /// The tick count.
            pub const fn ticks(&self) -> R {
                self.ticks
            }
        }
        impl<U: Unit, R: Tick> $name<U, R> {
            pub const MIN: Self = Self::from_ticks(R::MIN);
            pub const MAX: Self = Self::from_ticks(R::MAX);
            /// Time point, monotonic time point or duration, and the period.
            pub const KIND: TimeKind =
                TimeKind { point: $point, num: U::NUM, den: U::DEN, monotonic: $monotonic };
            /// The Schema IR `Custom` id of this type.
            pub fn schema_id() -> String {
                time_schema_id(Self::KIND)
            }
        }
        impl<U, R: Copy> Clone for $name<U, R> {
            fn clone(&self) -> Self {
                *self
            }
        }
        impl<U, R: Copy> Copy for $name<U, R> {}
        impl<U, R: PartialEq> PartialEq for $name<U, R> {
            fn eq(&self, other: &Self) -> bool {
                self.ticks == other.ticks
            }
        }
        impl<U, R: Eq> Eq for $name<U, R> {}
        impl<U, R: PartialOrd> PartialOrd for $name<U, R> {
            fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
                self.ticks.partial_cmp(&other.ticks)
            }
        }
        impl<U, R: Ord> Ord for $name<U, R> {
            fn cmp(&self, other: &Self) -> std::cmp::Ordering {
                self.ticks.cmp(&other.ticks)
            }
        }
        impl<U, R: std::hash::Hash> std::hash::Hash for $name<U, R> {
            fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
                self.ticks.hash(state)
            }
        }
        impl<U, R: Default> Default for $name<U, R> {
            fn default() -> Self {
                Self::from_ticks(R::default())
            }
        }
        impl<U, R: std::fmt::Debug> std::fmt::Debug for $name<U, R> {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, concat!($label, "({:?})"), self.ticks)
            }
        }

        // pssz: the tick integer.
        impl<F: PsszFormat, U: Unit, R: Tick + PsszPack<F>> PsszPack<F> for $name<U, R> {
            const MIN_ENCODED_SIZE: usize = R::MIN_ENCODED_SIZE;
            const MAX_ENCODED_SIZE: Option<usize> = R::MAX_ENCODED_SIZE;
            const IS_FIXED_SIZE: bool = R::IS_FIXED_SIZE;
            const FIXED_SIZE: usize = R::FIXED_SIZE;
            const AUTO_BOUND: Option<usize> = R::AUTO_BOUND;
            // Not the tick integer's shape: an integer and a time value
            // (or two time values of different kind, unit or tick type) are
            // different types to `pssz_evolves_from`, as in C++.
            const SHAPE: Shape = Shape {
                name: $label,
                children: &[
                    R::SHAPE,
                    Shape::leaf("period", (U::NUM >> 32) as usize, true),
                    Shape::leaf("period", (U::NUM & 0xffff_ffff) as usize, true),
                    Shape::leaf("period", (U::DEN >> 32) as usize, true),
                    Shape::leaf("period", (U::DEN & 0xffff_ffff) as usize, true),
                ],
                ..R::SHAPE
            };
            fn pssz_size(&self) -> usize {
                self.ticks.pssz_size()
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                self.ticks.pssz_pack(out)
            }
            // The tick integer's writers: one node, the integer's.
            #[inline]
            fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
                self.ticks.pssz_pack_at_depth(out, depth)
            }
            #[inline]
            fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
                self.ticks.pssz_pack_fixed(slot, depth)
            }
            #[inline]
            fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
                self.ticks.pssz_size_at_depth(depth)
            }
            #[inline]
            fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> {
                self.ticks.pssz_size_fixed(depth)
            }
        }
        impl<F: PsszFormat, U: Unit, R: Tick + PsszUnpack<F>> PsszUnpack<F> for $name<U, R> {
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                R::pssz_unpack(bytes).map(Self::from_ticks)
            }
            #[inline]
            fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
                R::pssz_unpack_at_depth(bytes, depth).map(Self::from_ticks)
            }
            #[inline]
            fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
                Self::from_ticks(R::pssz_unpack_trusted(bytes))
            }
        }
        impl<F: PsszFormat, U: Unit, R: Tick + PsszValidate<F>> PsszValidate<F> for $name<U, R> {
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                R::pssz_validate(bytes)
            }
            #[inline]
            fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
                R::pssz_validate_at_depth(bytes, depth)
            }
        }
        impl<'a, F: PsszFormat, U: Unit, R: Tick + PsszUnpack<F>> PsszView<'a, $name<U, R>, F> {
            /// The value (the view was validated when it was made).
            pub fn get(&self) -> $name<U, R> {
                $name::from_ticks(
                    R::pssz_unpack(&self.bytes()[..(R::BITS / 8) as usize]).unwrap_or_default(),
                )
            }
        }

        // key: the tick integer's key form, so byte order is time order.
        impl<U: Unit, R: Tick + KeyPack> KeyPack for $name<U, R> {
            const FIXED_SIZE: Option<usize> = R::FIXED_SIZE;
            fn key_size(&self) -> usize {
                self.ticks.key_size()
            }
            fn key_pack(&self, out: &mut Vec<u8>) {
                self.ticks.key_pack(out)
            }
        }
        impl<U: Unit, R: Tick + KeyUnpack> KeyUnpack for $name<U, R> {
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                R::key_unpack(r).map(Self::from_ticks)
            }
        }

        // schema: the tick integer carrying the time id.
        impl<U: Unit, R: Tick> ToPsszSchema for $name<U, R> {
            fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
                Ok(AnyType::new(AnyValue::Custom(Custom {
                    r#type: Box::new(AnyType::new(AnyValue::Int(Int {
                        bits: R::BITS,
                        isSigned: R::SIGNED,
                    }))),
                    id: Self::schema_id(),
                })))
            }
        }
    };
}
time_value!(TimePoint, "TimePoint", true, false);
time_value!(MonotonicPoint, "MonotonicPoint", false, true);
time_value!(Duration, "Duration", false, false);

// A point moved by a duration, and the duration between two points of one
// clock: exact, `None` when the result does not fit the tick type.
macro_rules! point_arithmetic {
    ($name:ident) => {
        impl<U: Unit, R: Tick> $name<U, R> {
            /// The point `by` later; `None` on overflow of the tick type.
            pub fn checked_add(self, by: Duration<U, R>) -> Option<Self> {
                R::from_i128(self.ticks.to_i128() + by.ticks.to_i128()).map(Self::from_ticks)
            }
            /// The point `by` earlier; `None` on overflow of the tick type.
            pub fn checked_sub(self, by: Duration<U, R>) -> Option<Self> {
                R::from_i128(self.ticks.to_i128() - by.ticks.to_i128()).map(Self::from_ticks)
            }
            /// `self - earlier`; `None` when it does not fit the tick type.
            pub fn checked_duration_since(self, earlier: Self) -> Option<Duration<U, R>> {
                R::from_i128(self.ticks.to_i128() - earlier.ticks.to_i128())
                    .map(Duration::from_ticks)
            }
        }
    };
}
point_arithmetic!(TimePoint);
point_arithmetic!(MonotonicPoint);

impl<U: Unit, R: Tick> TimePoint<U, R> {
    /// The Unix epoch.
    pub fn epoch() -> Self {
        Self::from_ticks(R::default())
    }

    /// The RFC 3339 UTC text with exactly the unit's fraction digits. An
    /// error for an instant outside years 0000..9999, which has no text.
    /// (Compile-time error for a unit that is not 10^-k seconds.)
    pub fn to_rfc3339(&self) -> Result<String, TimeError> {
        time_text_of_ticks(self.ticks.to_i128(), TextDigits::<U>::VALUE)
    }

    /// Reads any RFC 3339 date-time: `T` / `Z` in either case, `Z` or a
    /// `+HH:MM` / `-HH:MM` offset, any number of fraction digits as long as
    /// those beyond the unit's precision are zero. Rejects leap seconds,
    /// impossible dates, instants outside years 0000..9999 and tick counts
    /// that do not fit `R`.
    pub fn from_rfc3339(text: &str) -> Result<Self, TimeError> {
        let ticks = time_ticks_of_text(text, TextDigits::<U>::VALUE, R::BITS, R::SIGNED)?;
        Ok(Self::from_ticks(R::from_i128(ticks).expect("checked against the tick type")))
    }
}

/// Exact: fails when the instant is not a whole number of ticks or does not
/// fit the tick type.
impl<U: Unit, R: Tick> TryFrom<SystemTime> for TimePoint<U, R> {
    type Error = TimeError;
    fn try_from(time: SystemTime) -> Result<Self, TimeError> {
        let nanos = match time.duration_since(SystemTime::UNIX_EPOCH) {
            Ok(after) => after.as_nanos() as i128,
            Err(before) => -(before.duration().as_nanos() as i128),
        };
        ticks_from_nanos::<U, R>(nanos).map(Self::from_ticks)
    }
}
/// Exact: fails when the instant is not a whole number of nanoseconds or is
/// outside what `SystemTime` holds on this platform.
impl<U: Unit, R: Tick> TryFrom<TimePoint<U, R>> for SystemTime {
    type Error = TimeError;
    fn try_from(time: TimePoint<U, R>) -> Result<Self, TimeError> {
        let nanos = nanos_from_ticks::<U, R>(time.ticks)?;
        let out = if nanos >= 0 {
            SystemTime::UNIX_EPOCH.checked_add(std_duration(nanos)?)
        } else {
            SystemTime::UNIX_EPOCH.checked_sub(std_duration(-nanos)?)
        };
        out.ok_or(TimeError("time: outside the range of SystemTime"))
    }
}
/// Exact: fails when the duration is not a whole number of ticks or does not
/// fit the tick type.
impl<U: Unit, R: Tick> TryFrom<std::time::Duration> for Duration<U, R> {
    type Error = TimeError;
    fn try_from(duration: std::time::Duration) -> Result<Self, TimeError> {
        ticks_from_nanos::<U, R>(duration.as_nanos() as i128).map(Self::from_ticks)
    }
}
/// Exact: fails for a negative duration, or one that is not a whole number
/// of nanoseconds or exceeds `std::time::Duration`.
impl<U: Unit, R: Tick> TryFrom<Duration<U, R>> for std::time::Duration {
    type Error = TimeError;
    fn try_from(duration: Duration<U, R>) -> Result<Self, TimeError> {
        std_duration(nanos_from_ticks::<U, R>(duration.ticks)?)
    }
}

// ── json / ijson ────────────────────────────────────────────────────────────

/// The tick count under the format's integer rules (member integer-form
/// attributes apply): a duration, and a monotonic time point, which is
/// never an RFC 3339 string in either direction.
macro_rules! tick_text {
    ($name:ident) => {
        impl<U: Unit, R: Tick + JsonText> JsonText for $name<U, R> {
            fn write_json(&self, w: &mut Writer) -> Result<(), JsonTextError> {
                self.ticks.write_json(w)
            }
            fn read_json(r: &mut Reader<'_>) -> Result<Self, JsonTextError> {
                // A lone u8 reads as an integer; R::read_json is that reader.
                R::read_json(r).map(Self::from_ticks)
            }
        }
    };
}
tick_text!(Duration);
tick_text!(MonotonicPoint);

/// An RFC 3339 UTC string.
impl<U: Unit, R: Tick> JsonText for TimePoint<U, R> {
    fn write_json(&self, w: &mut Writer) -> Result<(), JsonTextError> {
        match self.to_rfc3339() {
            Ok(text) => w.string(text.as_bytes()),
            Err(_) => Err(w.err("json: time point outside years 0000..9999 has no text")),
        }
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self, JsonTextError> {
        r.ws();
        let at = r.position();
        let text = r.string()?;
        Self::from_rfc3339(&text).map_err(|e| r.err_at(at, e.0))
    }
}

// ── Arithmetic operators ────────────────────────────────────────────────────
//
// `point + duration`, `point - duration`, `point - point`, `duration +
// duration`, `duration - duration`, `-duration`, `duration * n`, `duration /
// n`, with the assigning forms. Exact: a result that does not fit the tick
// type panics, in every build profile (never wraps); the `checked_*`
// functions return `None` instead.

const OVERFLOW: &str = "psio::time: arithmetic overflow of the tick type";

macro_rules! point_operators {
    ($name:ident) => {
        impl<U: Unit, R: Tick> std::ops::Add<Duration<U, R>> for $name<U, R> {
            type Output = Self;
            #[track_caller]
            fn add(self, by: Duration<U, R>) -> Self {
                self.checked_add(by).expect(OVERFLOW)
            }
        }
        impl<U: Unit, R: Tick> std::ops::Sub<Duration<U, R>> for $name<U, R> {
            type Output = Self;
            #[track_caller]
            fn sub(self, by: Duration<U, R>) -> Self {
                self.checked_sub(by).expect(OVERFLOW)
            }
        }
        impl<U: Unit, R: Tick> std::ops::Sub for $name<U, R> {
            type Output = Duration<U, R>;
            #[track_caller]
            fn sub(self, earlier: Self) -> Duration<U, R> {
                self.checked_duration_since(earlier).expect(OVERFLOW)
            }
        }
        impl<U: Unit, R: Tick> std::ops::AddAssign<Duration<U, R>> for $name<U, R> {
            #[track_caller]
            fn add_assign(&mut self, by: Duration<U, R>) {
                *self = *self + by;
            }
        }
        impl<U: Unit, R: Tick> std::ops::SubAssign<Duration<U, R>> for $name<U, R> {
            #[track_caller]
            fn sub_assign(&mut self, by: Duration<U, R>) {
                *self = *self - by;
            }
        }
    };
}
point_operators!(TimePoint);
point_operators!(MonotonicPoint);

impl<U: Unit, R: Tick> Duration<U, R> {
    /// No time at all.
    pub fn zero() -> Self {
        Self::from_ticks(R::default())
    }
    pub fn is_zero(&self) -> bool {
        self.ticks == R::default()
    }
    /// `self + other`; `None` when it does not fit the tick type.
    pub fn checked_add(self, other: Self) -> Option<Self> {
        R::from_i128(self.ticks.to_i128() + other.ticks.to_i128()).map(Self::from_ticks)
    }
    /// `self - other`; `None` when it does not fit the tick type.
    pub fn checked_sub(self, other: Self) -> Option<Self> {
        R::from_i128(self.ticks.to_i128() - other.ticks.to_i128()).map(Self::from_ticks)
    }
    /// `-self`; `None` when it does not fit the tick type (an unsigned tick
    /// type holds no negative duration).
    pub fn checked_neg(self) -> Option<Self> {
        R::from_i128(-self.ticks.to_i128()).map(Self::from_ticks)
    }
    /// `self * factor`; `None` when it does not fit the tick type.
    pub fn checked_mul(self, factor: R) -> Option<Self> {
        self.ticks.to_i128().checked_mul(factor.to_i128()).and_then(R::from_i128).map(Self::from_ticks)
    }
    /// `self / divisor`, toward zero; `None` for a zero divisor or a
    /// quotient that does not fit.
    pub fn checked_div(self, divisor: R) -> Option<Self> {
        self.ticks.to_i128().checked_div(divisor.to_i128()).and_then(R::from_i128).map(Self::from_ticks)
    }
}
impl<U: Unit, R: Tick> std::ops::Add for Duration<U, R> {
    type Output = Self;
    #[track_caller]
    fn add(self, other: Self) -> Self {
        self.checked_add(other).expect(OVERFLOW)
    }
}
impl<U: Unit, R: Tick> std::ops::Sub for Duration<U, R> {
    type Output = Self;
    #[track_caller]
    fn sub(self, other: Self) -> Self {
        self.checked_sub(other).expect(OVERFLOW)
    }
}
impl<U: Unit, R: Tick> std::ops::Neg for Duration<U, R> {
    type Output = Self;
    #[track_caller]
    fn neg(self) -> Self {
        self.checked_neg().expect(OVERFLOW)
    }
}
impl<U: Unit, R: Tick> std::ops::Mul<R> for Duration<U, R> {
    type Output = Self;
    #[track_caller]
    fn mul(self, factor: R) -> Self {
        self.checked_mul(factor).expect(OVERFLOW)
    }
}
impl<U: Unit, R: Tick> std::ops::Div<R> for Duration<U, R> {
    type Output = Self;
    #[track_caller]
    fn div(self, divisor: R) -> Self {
        self.checked_div(divisor).expect("psio::time: division by zero or overflow of the tick type")
    }
}
impl<U: Unit, R: Tick> std::ops::AddAssign for Duration<U, R> {
    #[track_caller]
    fn add_assign(&mut self, other: Self) {
        *self = *self + other;
    }
}
impl<U: Unit, R: Tick> std::ops::SubAssign for Duration<U, R> {
    #[track_caller]
    fn sub_assign(&mut self, other: Self) {
        *self = *self - other;
    }
}
impl<U: Unit, R: Tick> std::iter::Sum for Duration<U, R> {
    #[track_caller]
    fn sum<I: Iterator<Item = Self>>(iter: I) -> Self {
        iter.fold(Self::zero(), |total, item| total + item)
    }
}

// ── Unit and tick-type conversions ──────────────────────────────────────────

/// How a conversion to a coarser unit treats a value between two ticks.
#[derive(Clone, Copy)]
enum Rounding {
    /// Only a whole number of ticks converts.
    Exact,
    /// Toward negative infinity (the tick at or before).
    Floor,
    /// Toward positive infinity (the tick at or after).
    Ceil,
}

/// ticks of `U` → ticks of `V` in `S`. `Err` says why not.
fn convert_ticks<U: Unit, V: Unit, R: Tick, S: Tick>(ticks: R, rounding: Rounding) -> Result<S, TimeError> {
    // ticks * (U::NUM / U::DEN) / (V::NUM / V::DEN)
    let numerator = (U::NUM as i128).checked_mul(V::DEN as i128).ok_or(TimeError("time: out of range"))?;
    let denominator = (U::DEN as i128).checked_mul(V::NUM as i128).ok_or(TimeError("time: out of range"))?;
    let scaled = ticks.to_i128().checked_mul(numerator).ok_or(TimeError("time: out of range"))?;
    let (quotient, remainder) = (scaled.div_euclid(denominator), scaled.rem_euclid(denominator));
    let value = match rounding {
        Rounding::Exact if remainder != 0 => return Err(TimeError("time: not a whole number of ticks")),
        Rounding::Ceil if remainder != 0 => quotient + 1,
        _ => quotient,
    };
    S::from_i128(value).ok_or(TimeError("time: outside the range of the tick type"))
}

macro_rules! conversions {
    ($name:ident) => {
        impl<U: Unit, R: Tick> $name<U, R> {
            /// The same value in unit `V`, exactly: an error when it is not
            /// a whole number of `V` ticks or does not fit the tick type.
            /// Seconds to micros always is whole; micros to seconds only
            /// for a whole second (use [`floor`](Self::floor) or
            /// [`ceil`](Self::ceil) otherwise).
            pub fn to_unit<V: Unit>(self) -> Result<$name<V, R>, TimeError> {
                convert_ticks::<U, V, R, R>(self.ticks, Rounding::Exact).map($name::from_ticks)
            }
            /// The value in unit `V` at or before this one (toward negative
            /// infinity; C++ `std::chrono::floor`). Panics when the result
            /// does not fit the tick type, which a conversion to a coarser
            /// unit never does; [`checked_floor`](Self::checked_floor)
            /// returns `None` instead.
            #[track_caller]
            pub fn floor<V: Unit>(self) -> $name<V, R> {
                self.checked_floor().expect(OVERFLOW)
            }
            /// The value in unit `V` at or after this one (toward positive
            /// infinity; C++ `std::chrono::ceil`). Panics when the result
            /// does not fit the tick type; [`checked_ceil`](Self::checked_ceil)
            /// returns `None` instead.
            #[track_caller]
            pub fn ceil<V: Unit>(self) -> $name<V, R> {
                self.checked_ceil().expect(OVERFLOW)
            }
            pub fn checked_floor<V: Unit>(self) -> Option<$name<V, R>> {
                convert_ticks::<U, V, R, R>(self.ticks, Rounding::Floor).ok().map($name::from_ticks)
            }
            pub fn checked_ceil<V: Unit>(self) -> Option<$name<V, R>> {
                convert_ticks::<U, V, R, R>(self.ticks, Rounding::Ceil).ok().map($name::from_ticks)
            }
            /// The same tick count held in another tick integer; an error
            /// when it does not fit.
            pub fn to_tick_type<S: Tick>(self) -> Result<$name<U, S>, TimeError> {
                S::from_i128(self.ticks.to_i128())
                    .map($name::from_ticks)
                    .ok_or(TimeError("time: outside the range of the tick type"))
            }
        }
    };
}
conversions!(TimePoint);
conversions!(MonotonicPoint);
conversions!(Duration);

impl<U: Unit, R: Tick> TimePoint<U, R> {
    /// The time since the Unix epoch.
    pub fn since_epoch(self) -> Duration<U, R> {
        Duration::from_ticks(self.ticks)
    }
    /// The time point `since_epoch` after the Unix epoch.
    pub fn from_since_epoch(since_epoch: Duration<U, R>) -> Self {
        Self::from_ticks(since_epoch.ticks)
    }
    /// The current time of the system clock, at or before now in unit `U`.
    /// An error when the tick type cannot hold it.
    pub fn now() -> Result<Self, TimeError> {
        let nanos = match SystemTime::now().duration_since(SystemTime::UNIX_EPOCH) {
            Ok(after) => after.as_nanos() as i128,
            Err(before) => -(before.duration().as_nanos() as i128),
        };
        let per_tick = (U::NUM as i128 * NANOS_PER_SECOND, U::DEN as i128);
        let ticks = (nanos * per_tick.1).div_euclid(per_tick.0);
        R::from_i128(ticks).map(Self::from_ticks).ok_or(TimeError("time: outside the range of the tick type"))
    }
}

// ── Display / FromStr ───────────────────────────────────────────────────────
//
// The text a value has in JSON, without the quotes: a time point is its
// RFC 3339 UTC text (C++ `psio::to_rfc3339` / `from_rfc3339`), a duration
// and a monotonic time point are the tick count in decimal.

/// The RFC 3339 UTC text with exactly the unit's fraction digits
/// ([`to_rfc3339`](TimePoint::to_rfc3339)). An instant outside years
/// 0000..9999 has no RFC 3339 text: it is written as its tick count in
/// parentheses, `(ticks)`, which [`FromStr`](std::str::FromStr) does not
/// read (C++ `to_rfc3339` returns the empty string there).
impl<U: Unit, R: Tick> std::fmt::Display for TimePoint<U, R> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.to_rfc3339() {
            Ok(text) => f.write_str(&text),
            Err(_) => write!(f, "({})", self.ticks.to_i128()),
        }
    }
}
/// Any RFC 3339 date-time ([`from_rfc3339`](TimePoint::from_rfc3339)).
impl<U: Unit, R: Tick> std::str::FromStr for TimePoint<U, R> {
    type Err = TimeError;
    fn from_str(text: &str) -> Result<Self, TimeError> {
        Self::from_rfc3339(text)
    }
}

macro_rules! tick_display {
    ($name:ident) => {
        /// The tick count in decimal.
        impl<U: Unit, R: Tick> std::fmt::Display for $name<U, R> {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, "{}", self.ticks.to_i128())
            }
        }
        /// The tick count in decimal: an optional `-`, then digits with no
        /// leading zero (the JSON integer grammar), within the tick type.
        impl<U: Unit, R: Tick> std::str::FromStr for $name<U, R> {
            type Err = TimeError;
            fn from_str(text: &str) -> Result<Self, TimeError> {
                let digits = text.strip_prefix('-').unwrap_or(text);
                if digits.is_empty()
                    || !digits.bytes().all(|b| b.is_ascii_digit())
                    || (digits.len() > 1 && digits.starts_with('0'))
                    || text == "-0"
                {
                    return Err(TimeError("time: a tick count is a decimal integer"));
                }
                text.parse::<i128>()
                    .ok()
                    .and_then(R::from_i128)
                    .map(Self::from_ticks)
                    .ok_or(TimeError("time: outside the range of the tick type"))
            }
        }
    };
}
tick_display!(Duration);
tick_display!(MonotonicPoint);

// ── chrono (feature `chrono`) ───────────────────────────────────────────────

#[cfg(feature = "chrono")]
mod chrono_conversions {
    use super::*;

    fn nanos_of<Tz: chrono::TimeZone>(time: &chrono::DateTime<Tz>) -> i128 {
        time.timestamp() as i128 * NANOS_PER_SECOND + time.timestamp_subsec_nanos() as i128
    }

    /// Exact: fails when the instant is not a whole number of ticks or does
    /// not fit the tick type. `TimePoint::from_chrono_floor` rounds down.
    impl<U: Unit, R: Tick, Tz: chrono::TimeZone> TryFrom<chrono::DateTime<Tz>> for TimePoint<U, R> {
        type Error = TimeError;
        fn try_from(time: chrono::DateTime<Tz>) -> Result<Self, TimeError> {
            ticks_from_nanos::<U, R>(nanos_of(&time)).map(Self::from_ticks)
        }
    }
    /// Exact: fails when the instant is not a whole number of nanoseconds
    /// or is outside what chrono holds.
    impl<U: Unit, R: Tick> TryFrom<TimePoint<U, R>> for chrono::DateTime<chrono::Utc> {
        type Error = TimeError;
        fn try_from(time: TimePoint<U, R>) -> Result<Self, TimeError> {
            let nanos = nanos_from_ticks::<U, R>(time.ticks)?;
            let secs = i64::try_from(nanos.div_euclid(NANOS_PER_SECOND))
                .map_err(|_| TimeError("time: outside the range of chrono::DateTime"))?;
            chrono::DateTime::from_timestamp(secs, nanos.rem_euclid(NANOS_PER_SECOND) as u32)
                .ok_or(TimeError("time: outside the range of chrono::DateTime"))
        }
    }
    impl<U: Unit, R: Tick> TimePoint<U, R> {
        /// The time point at or before `time` in unit `U` (a `DateTime`
        /// holds nanoseconds); `None` when the tick type cannot hold it.
        pub fn from_chrono_floor<Tz: chrono::TimeZone>(time: chrono::DateTime<Tz>) -> Option<Self> {
            let per_tick = U::NUM as i128 * NANOS_PER_SECOND;
            R::from_i128((nanos_of(&time) * U::DEN as i128).div_euclid(per_tick)).map(Self::from_ticks)
        }
    }
    /// Exact: fails when the duration is not a whole number of ticks or
    /// does not fit the tick type.
    impl<U: Unit, R: Tick> TryFrom<chrono::TimeDelta> for Duration<U, R> {
        type Error = TimeError;
        fn try_from(delta: chrono::TimeDelta) -> Result<Self, TimeError> {
            let nanos = delta.num_seconds() as i128 * NANOS_PER_SECOND + delta.subsec_nanos() as i128;
            ticks_from_nanos::<U, R>(nanos).map(Self::from_ticks)
        }
    }
    /// Exact: fails when the duration is not a whole number of nanoseconds
    /// or is outside what chrono holds.
    impl<U: Unit, R: Tick> TryFrom<Duration<U, R>> for chrono::TimeDelta {
        type Error = TimeError;
        fn try_from(duration: Duration<U, R>) -> Result<Self, TimeError> {
            let nanos = nanos_from_ticks::<U, R>(duration.ticks)?;
            let secs = i64::try_from(nanos.div_euclid(NANOS_PER_SECOND))
                .map_err(|_| TimeError("time: outside the range of chrono::TimeDelta"))?;
            chrono::TimeDelta::new(secs, nanos.rem_euclid(NANOS_PER_SECOND) as u32)
                .ok_or(TimeError("time: outside the range of chrono::TimeDelta"))
        }
    }
}
