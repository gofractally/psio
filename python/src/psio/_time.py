"""Time values (docs/time.md): schema ids and the RFC 3339 text of a time point.

A time value is a tick count: a duration, a time point counted from
1970-01-01T00:00:00Z (UTC, no leap seconds), or a monotonic time point.  The
schema-driven codecs carry all three as ``int`` ticks; the typed layer maps
``datetime`` / ``timedelta`` onto them exactly (``psio.typed``).
"""

from __future__ import annotations

import re
from math import gcd
from typing import NamedTuple, Optional, Tuple, Union

from .errors import EncodeError

NAMED = (("seconds", 1), ("millis", 1000), ("micros", 1000000), ("nanos", 1000000000))


class TimeKind(NamedTuple):
    """A time kind and its tick period ``num/den`` seconds in lowest terms."""

    point: bool
    monotonic: bool
    num: int
    den: int


TimeUnit = Union[str, Tuple[int, int]]


def unit_period(unit: TimeUnit) -> Tuple[int, int]:
    """``unit`` (a name, or ``(num, den)`` seconds) as a period in lowest terms."""
    if isinstance(unit, str):
        for name, den in NAMED:
            if name == unit:
                return 1, den
        raise EncodeError(f'time: unknown unit "{unit}"')
    try:
        num, den = unit
    except (TypeError, ValueError):
        raise EncodeError("time: a unit is a name or (num, den)") from None
    if type(num) is not int or type(den) is not int or num <= 0 or den <= 0:
        raise EncodeError("time: a unit period needs positive integers num and den")
    g = gcd(num, den)
    return num // g, den // g


def fraction_digits(num: int, den: int) -> int:
    """Fraction digits of a period of 1/10^k seconds (k = 0..9); -1 otherwise."""
    if num != 1:
        return -1
    k = 0
    p = 1
    while p < den and k < 9:
        p *= 10
        k += 1
    return k if p == den else -1


def schema_id(kind: TimeKind) -> str:
    """``time-point-micros``, ``duration-60/1``, ``monotonic-nanos``, ..."""
    if kind.num <= 0 or kind.den <= 0:
        raise EncodeError("time: a period needs positive num and den")
    if kind.point and kind.monotonic:
        raise EncodeError("time: a time point is of the Unix epoch or monotonic, not both")
    g = gcd(kind.num, kind.den)
    num, den = kind.num // g, kind.den // g
    prefix = "time-point-" if kind.point else "monotonic-" if kind.monotonic else "duration-"
    if num == 1:
        for name, d in NAMED:
            if d == den:
                return prefix + name
    return f"{prefix}{num}/{den}"


_RESERVED = re.compile(r"^(time-point|duration|monotonic)(-|$)")
_RATIO = re.compile(r"^([1-9][0-9]{0,17})/([1-9][0-9]{0,17})$")


def reserved_time_id(id: str) -> bool:
    return _RESERVED.match(id) is not None


def parse_schema_id(id: str) -> Optional[TimeKind]:
    """The inverse of :func:`schema_id`; ``None`` for any other id."""
    point = monotonic = False
    if id.startswith("time-point-"):
        point, rest = True, id[11:]
    elif id.startswith("duration-"):
        rest = id[9:]
    elif id.startswith("monotonic-"):
        monotonic, rest = True, id[10:]
    else:
        return None
    for name, den in NAMED:
        if name == rest:
            return TimeKind(point, monotonic, 1, den)
    m = _RATIO.match(rest)
    if not m:
        return None
    num, den = int(m.group(1)), int(m.group(2))
    if gcd(num, den) != 1:
        return None
    if num == 1 and any(d == den for _, d in NAMED):
        return None
    return TimeKind(point, monotonic, num, den)


# ── RFC 3339 ────────────────────────────────────────────────────────────────

MIN_TEXT_SECONDS = -62167219200  # 0000-01-01T00:00:00Z
MAX_TEXT_SECONDS = 253402300799  # 9999-12-31T23:59:59Z


def _days_from_civil(y: int, m: int, d: int) -> int:
    if m <= 2:
        y -= 1
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (m - 3 if m > 2 else m + 9) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def _days_in_month(y: int, m: int) -> int:
    if m == 2 and y % 4 == 0 and (y % 100 != 0 or y % 400 == 0):
        return 29
    return (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)[m - 1]


def _write_civil(secs: int, sub: int, digits: int) -> str:
    days, rem = divmod(secs, 86400)
    z = days + 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    y = yoe + era * 400 + (1 if m <= 2 else 0)
    frac = "." + str(sub).rjust(digits, "0") if digits > 0 else ""
    return f"{y:04d}-{m:02d}-{d:02d}T{rem // 3600:02d}:{rem // 60 % 60:02d}:{rem % 60:02d}{frac}Z"


def _parse_civil(s: str, digits: int) -> Union[Tuple[int, int], str]:
    grammar = "time: not an RFC 3339 date-time"
    if len(s) < 20:
        return grammar

    def num(at: int, n: int) -> int:
        v = 0
        for c in s[at:at + n]:
            if not "0" <= c <= "9":
                return -1
            v = v * 10 + (ord(c) - 48)
        return v

    y, mo, d, h, mi, sec = num(0, 4), num(5, 2), num(8, 2), num(11, 2), num(14, 2), num(17, 2)
    if (y < 0 or s[4] != "-" or mo < 0 or s[7] != "-" or d < 0 or s[10] not in "Tt" or h < 0
            or s[13] != ":" or mi < 0 or s[16] != ":" or sec < 0):
        return grammar
    if mo < 1 or mo > 12 or d < 1 or d > _days_in_month(y, mo):
        return "time: no such calendar date"
    if sec == 60:
        return "time: leap seconds are not representable"
    if h > 23 or mi > 59 or sec > 59:
        return "time: no such time of day"
    at = 19
    frac = 0
    size = len(s)
    if s[at] == ".":
        at += 1
        n = 0
        while at < size:
            c = s[at]
            if not "0" <= c <= "9":
                break
            if n < digits:
                frac = frac * 10 + (ord(c) - 48)
            elif c != "0":
                return "time: fraction is finer than the unit"
            at += 1
            n += 1
        if n == 0:
            return grammar
        while n < digits:
            frac *= 10
            n += 1
    if at >= size:
        return "time: missing UTC offset"
    offset = 0
    if s[at] in "Zz":
        at += 1
    elif s[at] in "+-":
        oh = -1 if size - at < 6 else num(at + 1, 2)
        om = -1 if size - at < 6 else num(at + 4, 2)
        if oh < 0 or om < 0 or s[at + 3] != ":" or oh > 23 or om > 59:
            return "time: malformed UTC offset"
        offset = oh * 3600 + om * 60
        if s[at] == "-":
            offset = -offset
        at += 6
    else:
        return "time: missing UTC offset"
    if at != size:
        return grammar
    utc = _days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - offset
    if utc < MIN_TEXT_SECONDS or utc > MAX_TEXT_SECONDS:
        return "time: outside years 0000..9999"
    return utc, frac


def tick_type_admits(bits: int, signed: bool, ticks: int) -> bool:
    if signed:
        return -(1 << (bits - 1)) <= ticks < (1 << (bits - 1))
    return 0 <= ticks < (1 << bits)


def text_of(ticks: int, digits: int) -> Union[Tuple[str], str]:
    """``(text,)`` of a time point, or a message when it has no text."""
    scale = 10 ** digits
    secs = ticks // scale
    if secs < MIN_TEXT_SECONDS or secs > MAX_TEXT_SECONDS:
        return "time: time point outside years 0000..9999 has no text"
    return (_write_civil(secs, ticks - secs * scale, digits),)


def ticks_of(text: str, digits: int, bits: int, signed: bool) -> Union[Tuple[int], str]:
    """``(ticks,)`` of RFC 3339 ``text``, or a message saying why it is rejected."""
    parsed = _parse_civil(text, digits)
    if isinstance(parsed, str):
        return parsed
    ticks = parsed[0] * 10 ** digits + parsed[1]
    if not tick_type_admits(bits, signed, ticks):
        return "time: outside the range of the tick type"
    return (ticks,)


def _text_digits(unit: TimeUnit) -> int:
    num, den = unit_period(unit)
    digits = fraction_digits(num, den)
    if digits < 0:
        raise EncodeError("time: a time point has RFC 3339 text only for a unit of 1/10^k seconds, k = 0..9")
    return digits


def time_point_to_iso(ticks: int, unit: TimeUnit) -> str:
    """The RFC 3339 UTC text of a time point, with exactly the unit's fraction digits."""
    out = text_of(ticks, _text_digits(unit))
    if isinstance(out, str):
        raise EncodeError(out)
    return out[0]


def time_point_from_iso(text: str, unit: TimeUnit, bits: int = 64, signed: bool = True) -> int:
    """The tick count of an RFC 3339 date-time (any offset, any fraction the unit holds exactly)."""
    if not isinstance(text, str):
        raise EncodeError("time: text must be a string")
    out = ticks_of(text, _text_digits(unit), bits, signed)
    if isinstance(out, str):
        raise EncodeError(out)
    return out[0]
