"""JSON (RFC 8259) and I-JSON (RFC 7493) text for a value known by its schema (docs/json.md).

A port of ``cpp/include/psio/pssz_ir_json.hpp``.  The text is read by the
exact cursor below, never by ``json.loads``: integers are taken from their
token text, and the rules a generic parser cannot see are enforced (``-0``,
``1.0``, ``1e2`` and leading zeros are not integers; an integer or hex string
must be written without escapes; a repeated key is read again and the last
one wins; an I-JSON variant object has exactly one member; nesting stops at
64).

A 128 / 256-bit integer is decimal (a string under ijson, a bare number under
json; the legacy "0x" string is read).  An Enum is its enumerator's name when
the value is listed and its number otherwise.  A non-finite float has no
text.  A raw string holding bytes that are not UTF-8 has no text.  A Custom
id that is not part of the binary form is its handler's text as one JSON
string, or its inner type's text without one.  The id ``json`` is any JSON
value, inline; a Variant with the attribute ``untagged`` is the bare value of
its alternative, which a reader picks by the kind of JSON value it finds.

This module is internal.
"""

from __future__ import annotations

import re
import struct
from typing import NoReturn, Any, Callable, List, Optional, Tuple

from ._core import MAX_DEPTH, NO_HANDLER, WRITE_MODE, Codec, Ctx, Handler, Node, member_bound_admits, strip
from ._time import text_of, tick_type_admits, ticks_of
from .errors import DecodeError, EncodeError, JsonError, PsioError, SchemaError
from .values import JsonNumber, NaN, Some, Variant

SAFE_INTEGER = (1 << 53) - 1
U64_MASK = (1 << 64) - 1

# ── Floats ──────────────────────────────────────────────────────────────────

_F32 = struct.Struct("<f")
_U32 = struct.Struct("<I")
F32_MAX = 3.4028234663852886e38


def fround(x: float) -> float:
    """``x`` rounded to binary32 (an infinity when out of range)."""
    try:
        return _F32.unpack(_F32.pack(x))[0]
    except OverflowError:
        return float("inf") if x > 0 else float("-inf")


def _exact_decimal(a: float) -> Tuple[str, int]:
    """The exact decimal expansion of a positive finite double: digits and the exponent of the first."""
    num, den = a.as_integer_ratio()
    if den == 1:
        text = str(num)
        exponent = len(text) - 1
    else:
        shift = den.bit_length() - 1  # den = 2^shift
        text = str(num * 5 ** shift)
        exponent = len(text) - 1 - shift
    return text.rstrip("0"), exponent


def _split(text: str) -> Tuple[str, int]:
    """A float's ``repr`` / ``%e`` text as significant digits and the decimal exponent of the first digit."""
    mantissa, _, exp = text.partition("e")
    e = int(exp) if exp else 0
    whole, _, frac = mantissa.partition(".")
    digits = whole + frac
    point = len(whole)
    stripped = digits.lstrip("0")
    if not stripped:
        return "0", 0
    lead = len(digits) - len(stripped)
    return stripped.rstrip("0") or "0", e + point - 1 - lead


def _shortest_single(a: float) -> Tuple[str, int]:
    """Shortest decimal that reads back as the same binary32, closest to it."""
    for precision in range(1, 10):
        digits, exponent = _exp_digits(a, precision)
        scale = exponent - (precision - 1)

        def value(d: int) -> float:
            return float(f"{d}e{scale}")

        middle = int(digits)
        best: Optional[int] = None
        if fround(value(middle)) == a:
            best = middle
        else:
            # At a power of two the rounding interval is lopsided: the closest
            # decimal can fall outside it while a neighbour is inside.
            for candidate in (middle - 1, middle + 1):
                if candidate <= 0 or fround(value(candidate)) != a:
                    continue
                if best is None or abs(value(candidate) - a) < abs(value(best) - a):
                    best = candidate
        if best is not None:
            text = str(best)
            return text.rstrip("0") or "0", exponent + (len(text) - precision)
    return _exp_digits(a, 9)


def _exp_digits(a: float, precision: int) -> Tuple[str, int]:
    """``a`` to ``precision`` significant digits (not stripped) and its exponent."""
    mantissa, _, exp = ("%.*e" % (precision - 1, a)).partition("e")
    return mantissa.replace(".", ""), int(exp)


def format_float(x: float, single: bool) -> str:
    """Shortest round-trip decimal in the layout of C++ ``std::to_chars``.

    Fixed or scientific, whichever is shorter (fixed on a tie), with a signed
    exponent of at least two digits.  ``x`` is finite.
    """
    if x == 0:
        return "-0" if str(x)[0] == "-" else "0"
    sign = "-" if x < 0 else ""
    a = abs(x)
    digits, exponent = _shortest_single(a) if single else _split(repr(a))
    # When the value lies exactly halfway between two shortest candidates
    # std::to_chars keeps the even last digit; decide it from the exact expansion.
    if len(digits) <= 17:
        probe, probe_exp = _exp_digits(a, len(digits) + 1)
        if probe_exp == exponent and probe.endswith("5"):
            exact, exact_exp = _exact_decimal(a)
            if exact_exp == exponent and exact == probe:
                def reads(candidate: str) -> bool:
                    v = float(f"{candidate}e{exponent - (len(candidate) - 1)}")
                    return (fround(v) if single else v) == a
                down = exact[:len(digits)]
                up = str(int(down) + 1)
                even = (ord(down[-1]) - 48) % 2 == 0
                if reads(down) and (even or len(up) != len(down) or not reads(up)):
                    digits = down
                elif len(up) == len(down) and reads(up):
                    digits = up
    n = len(digits)
    magnitude = abs(exponent)
    scientific = n + (1 if n > 1 else 0) + 2 + (3 if magnitude >= 100 else 2)
    fixed = 1 - exponent + n if exponent < 0 else exponent + 1 if exponent >= n - 1 else n + 1
    if fixed <= scientific:
        if exponent < 0:
            return f"{sign}0.{'0' * (-exponent - 1)}{digits}"
        if exponent >= n - 1:
            return sign + str(int(a))  # an integer: every digit of its exact value
        return f"{sign}{digits[:exponent + 1]}.{digits[exponent + 1:]}"
    return f"{sign}{digits[0]}{'.' + digits[1:] if n > 1 else ''}e{'-' if exponent < 0 else '+'}{magnitude:02d}"


_NUMBER_PARTS = re.compile(r"^([0-9]+)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?$")


def _compare_token(token: str, d: float) -> int:
    """Compares the unsigned RFC 8259 number ``token`` with the positive double ``d``."""
    m = _NUMBER_PARTS.match(token)
    assert m is not None
    integer = m.group(1)
    digits = integer + (m.group(2) or "")
    stripped = digits.lstrip("0")
    if not stripped:
        return -1
    first = len(digits) - len(stripped)
    exponent = len(integer) - 1 - first + int(m.group(3) or "0")
    stripped = stripped.rstrip("0")
    exact, exact_exp = _exact_decimal(d)
    if exponent != exact_exp:
        return -1 if exponent < exact_exp else 1
    return -1 if stripped < exact else 1 if stripped > exact else 0


def parse_single(token: str) -> Optional[float]:
    """A JSON number token as a correctly rounded binary32, or ``None`` when it is not finite there."""
    d = float(token)
    if d != d or d in (float("inf"), float("-inf")):
        return None
    negative = token[0] == "-"
    a = abs(d)
    f = fround(a)
    if f != a:
        # The floats either side of `a`; 2^128 stands in for infinity.
        top = 2.0 ** 128
        if f > a:
            if f == float("inf"):
                hi, lo = top, F32_MAX
            else:
                hi = f
                lo = _F32.unpack(_U32.pack(_U32.unpack(_F32.pack(f))[0] - 1))[0]
        else:
            lo = f
            up = _F32.unpack(_U32.pack(_U32.unpack(_F32.pack(f))[0] + 1))[0]
            hi = up if up != float("inf") else top
        if a - lo == hi - a:
            order = _compare_token(token[1:] if negative else token, a)
            if order < 0:
                f = lo
            elif order > 0:
                f = float("inf") if hi == top else hi
    if f == float("inf"):
        return None
    return -f if negative else f


# ── Syntax: the cursor ──────────────────────────────────────────────────────

_WS = re.compile(r"[ \t\n\r]*")
_PLAIN_STRING = re.compile(r'"([^"\\\x00-\x1f]*)"')
_SPECIAL = re.compile(r'["\\\x00-\x1f]')
_NUMBER = re.compile(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")
_NONCHAR = re.compile("[﷐-﷯￾￿" + "".join(
    f"{chr(p << 16 | 0xfffe)}{chr(p << 16 | 0xffff)}" for p in range(1, 17)) + "]")
_SIMPLE_ESCAPE = {'"': '"', "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}
_HEX4 = re.compile(r"[0-9a-fA-F]{4}")


def is_noncharacter(cp: int) -> bool:
    return 0xFDD0 <= cp <= 0xFDEF or cp & 0xFFFE == 0xFFFE


class Cursor:
    """Forward-only, single-pass JSON text cursor.

    It owns whitespace, punctuation, string escapes, the RFC 8259 number
    grammar, the nesting limit and skipping a value.  ``nonchar`` applies the
    I-JSON string rule (no Unicode noncharacters), also inside skipped values.
    """

    __slots__ = ("s", "p", "end", "depth", "fresh", "nonchar")

    def __init__(self, s: str, nonchar: bool) -> None:
        self.s = s
        self.p = 0
        self.end = len(s)
        self.depth = 0
        self.fresh: List[bool] = []
        self.nonchar = nonchar
        if not s:
            self.fail("json: empty input")
        if nonchar and not s.isascii():
            m = _NONCHAR.search(s)
            if m is not None:
                self.p = m.start()
                self.fail("ijson: Unicode noncharacter in string")

    def fail(self, what: str) -> Any:
        raise JsonError(what, offset=self.p)

    def _ws(self) -> None:
        self.p = _WS.match(self.s, self.p).end()

    def _take(self, c: str) -> bool:
        self._ws()
        if self.s.startswith(c, self.p):
            self.p += 1
            return True
        return False

    def kind(self) -> str:
        self._ws()
        if self.p >= self.end:
            self.fail("json: unexpected end of input")
        c = self.s[self.p]
        if c == "{":
            return "object"
        if c == "[":
            return "array"
        if c == '"':
            return "string"
        if c in "tf":
            return "boolean"
        if c == "n":
            return "null"
        if c == "-" or "0" <= c <= "9":
            return "number"
        return self.fail("json: unexpected character at value start")

    def _open(self, c: str, expected: str) -> None:
        if not self._take(c):
            self.fail(expected)
        if self.depth >= MAX_DEPTH:
            self.fail("json: max depth exceeded")
        self.fresh.append(True)
        self.depth += 1

    def _advance(self, close: str, expected: str) -> bool:
        if self.depth == 0:
            self.fail("json: cursor is not inside a container")
        first = self.fresh[-1]
        self.fresh[-1] = False
        if self._take(close):
            self.depth -= 1
            self.fresh.pop()
            return False
        if not first and not self._take(","):
            self.fail(expected)
        return True

    def begin_object(self) -> None:
        self._open("{", "json: expected '{'")

    def begin_array(self) -> None:
        self._open("[", "json: expected '['")

    def next_field(self) -> Optional[str]:
        """The next member's key (unescaped), or ``None`` once the object is closed."""
        if not self._advance("}", "json: expected ',' or '}' in object"):
            return None
        key = self.string()[0]
        if not self._take(":"):
            self.fail("json: expected ':' after object key")
        return key

    def next_element(self) -> bool:
        """Positions at the next element; ``False`` once the array is closed."""
        return self._advance("]", "json: expected ',' or ']' in array")

    def _hex4(self) -> int:
        if self.end - self.p < 4:
            self.fail("json: truncated \\uXXXX escape")
        m = _HEX4.match(self.s, self.p)
        if m is None:
            self.fail("json: bad hex digit in \\uXXXX")
        self.p += 4
        return int(m.group(), 16)

    def string(self) -> Tuple[str, bool]:
        """A string value, unescaped, and whether the text between the quotes differed from it."""
        self._ws()
        s = self.s
        m = _PLAIN_STRING.match(s, self.p)
        if m is not None:
            self.p = m.end()
            return m.group(1), False
        if not s.startswith('"', self.p):
            self.fail("json: expected a string")
        self.p += 1
        out: List[str] = []
        while True:
            m2 = _SPECIAL.search(s, self.p)
            if m2 is None:
                self.p = self.end
                self.fail("json: unterminated string")
            at = m2.start()
            out.append(s[self.p:at])
            self.p = at
            c = s[at]
            if c == '"':
                self.p += 1
                return "".join(out), True
            if c != "\\":
                self.fail("json: unescaped control character in string")
            if self.end - self.p < 2:
                self.fail("json: trailing backslash in string")
            e = s[at + 1]
            self.p += 2
            simple = _SIMPLE_ESCAPE.get(e)
            if simple is not None:
                out.append(simple)
                continue
            if e != "u":
                self.p -= 1
                self.fail("json: invalid escape character in string")
            cp = self._hex4()
            if 0xDC00 <= cp <= 0xDFFF:
                self.fail("json: unpaired surrogate escape")
            if 0xD800 <= cp <= 0xDBFF:
                if not s.startswith("\\u", self.p):
                    self.fail("json: unpaired surrogate escape")
                self.p += 2
                lo = self._hex4()
                if not 0xDC00 <= lo <= 0xDFFF:
                    self.fail("json: unpaired surrogate escape")
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
            if self.nonchar and is_noncharacter(cp):
                self.fail("ijson: Unicode noncharacter in string")
            out.append(chr(cp))

    def number(self) -> str:
        """A number as its RFC 8259 token text."""
        self._ws()
        m = _NUMBER.match(self.s, self.p)
        if m is None:
            c = self.s[self.p:self.p + 1]
            if not (c == "-" or "0" <= c <= "9") or c == "":
                self.fail("json: expected a number")
            self.fail("json: number missing integer part")
        self.p = m.end()
        # `1.` and `1e` are not numbers: the grammar needs digits there.
        nxt = self.s[self.p:self.p + 1]
        if nxt == "." or nxt in ("e", "E"):
            self.fail("json: number missing fractional digits" if nxt == "." else "json: number missing exponent digits")
        return m.group()

    def boolean(self) -> bool:
        self._ws()
        if self.s.startswith("true", self.p):
            self.p += 4
            return True
        if self.s.startswith("false", self.p):
            self.p += 5
            return False
        return self.fail("json: expected true or false")

    def null(self) -> None:
        self._ws()
        if not self.s.startswith("null", self.p):
            self.fail("json: malformed keyword")
        self.p += 4

    def skip(self) -> None:
        """Skips one well-formed value of any shape (the value of an unknown key)."""
        self._skip(self.depth)

    def _skip(self, depth: int) -> None:
        self._ws()
        if self.p >= self.end:
            self.fail("json: unexpected end of input")
        c = self.s[self.p]
        if c == "{":
            if depth >= MAX_DEPTH:
                self.fail("json: max depth exceeded")
            self.p += 1
            if self._take("}"):
                return
            while True:
                self._ws()
                if not self.s.startswith('"', self.p):
                    self.fail("json: expected a string")
                self.string()
                if not self._take(":"):
                    self.fail("json: expected ':' after object key")
                self._skip(depth + 1)
                if self._take("}"):
                    return
                if not self._take(","):
                    self.fail("json: expected ',' or '}' in object")
        elif c == "[":
            if depth >= MAX_DEPTH:
                self.fail("json: max depth exceeded")
            self.p += 1
            if self._take("]"):
                return
            while True:
                self._skip(depth + 1)
                if self._take("]"):
                    return
                if not self._take(","):
                    self.fail("json: expected ',' or ']' in array")
        elif c == '"':
            self.string()
        elif c == "t":
            if not self.s.startswith("true", self.p):
                self.fail("json: malformed keyword")
            self.p += 4
        elif c == "f":
            if not self.s.startswith("false", self.p):
                self.fail("json: malformed keyword")
            self.p += 5
        elif c == "n":
            self.null()
        elif c == "-" or "0" <= c <= "9":
            self.number()
        else:
            self.fail("json: unexpected character at value start")

    def finish(self) -> None:
        """Nothing but whitespace remains."""
        self._ws()
        if self.p != self.end:
            self.fail("json: trailing bytes after value")


# ── Any JSON value (the Custom id "json") ───────────────────────────────────

_PLAIN_INTEGER = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")
_NUMBER_TEXT = re.compile(_NUMBER.pattern + r"\Z")


def read_json_value(c: Cursor) -> Any:
    """The JSON value at the cursor, of any shape: ``None``, ``bool``, ``int``, :class:`JsonNumber`,
    ``str``, ``list``, ``dict``.  A repeated key keeps its last value, at the position of the first."""
    kind = c.kind()
    if kind == "string":
        return c.string()[0]
    if kind == "number":
        token = c.number()
        # A plain integer is exact as an int; any other token is kept as written.
        return int(token) if _PLAIN_INTEGER.match(token) is not None else JsonNumber(token)
    if kind == "object":
        members: dict = {}
        c.begin_object()
        while True:
            key = c.next_field()
            if key is None:
                return members
            members[key] = read_json_value(c)
    if kind == "array":
        items: List[Any] = []
        c.begin_array()
        while c.next_element():
            items.append(read_json_value(c))
        return items
    if kind == "boolean":
        return c.boolean()
    c.null()
    return None


def json_value_from_text(text: str) -> Any:
    """The JSON value a UTF-8 string of a "json" node holds: any well-formed JSON text."""
    try:
        cursor = Cursor(text, False)
        value = read_json_value(cursor)
        cursor.finish()
    except (JsonError, RecursionError):
        raise DecodeError("pssz IR: the text of a json value is not JSON") from None
    return value


def _json_value(value: Any, nonchar: bool, out: List[str], depth: int) -> None:
    if value is None:
        out.append("null")
    elif value is True:
        out.append("true")
    elif value is False:
        out.append("false")
    elif type(value) is str:
        out.append(quote(value, nonchar))
    elif type(value) is int:
        out.append(str(value))
    elif type(value) is JsonNumber:
        if _NUMBER_TEXT.match(value.text) is None:
            _fail("json: a json_value number is not a JSON number")
        out.append(value.text)
    elif isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            _fail("json: a json_value number is not a JSON number")
        out.append(format_float(float(value), False))
    elif isinstance(value, dict):
        if depth >= MAX_DEPTH:
            _fail("json: max depth exceeded")
        out.append("{")
        first = True
        for key, item in value.items():
            if type(key) is not str:
                _fail("json: the keys of a JSON object are strings")
            out.append(("" if first else ",") + quote(key, nonchar) + ":")
            first = False
            _json_value(item, nonchar, out, depth + 1)
        out.append("}")
    elif isinstance(value, (list, tuple)):
        if depth >= MAX_DEPTH:
            _fail("json: max depth exceeded")
        out.append("[")
        for i, item in enumerate(value):
            if i:
                out.append(",")
            _json_value(item, nonchar, out, depth + 1)
        out.append("]")
    elif isinstance(value, int) and not isinstance(value, bool):
        out.append(str(int(value)))
    else:
        _fail(f"json: {type(value).__name__} is not a JSON value (None, bool, int, float, str, list, dict)")


def json_value_text(value: Any, nonchar: bool = False) -> str:
    """The compact JSON text of a JSON value (what a "json" node holds in PSSZ).  ``nonchar``: the
    I-JSON string rule."""
    out: List[str] = []
    try:
        _json_value(value, nonchar, out, 0)
    except RecursionError:
        _fail("json: max depth exceeded")
    return "".join(out)


# ── Untagged variants: the kinds of JSON value a type is written as ─────────

K_NULL, K_BOOLEAN, K_NUMBER, K_STRING, K_ARRAY, K_OBJECT = 1, 2, 4, 8, 16, 32
K_ANY = 63
_KIND_BITS = {"null": K_NULL, "boolean": K_BOOLEAN, "number": K_NUMBER, "string": K_STRING, "array": K_ARRAY,
              "object": K_OBJECT}
UNTAGGED_AMBIGUOUS = "json: the alternatives of an untagged variant are not told apart by the kind of JSON value"


def json_kinds(codec: Codec, node: Node, depth: int = 0) -> int:
    """The kinds of JSON value ``node`` may be written as, in either format (docs/json.md "Untagged
    variants"; C++ ``pssz_ir::json_kinds_of``)."""
    if depth > MAX_DEPTH:
        return K_ANY
    k = node.kind
    if k == "int":
        if node.time is not None:
            return K_STRING if node.time.point else K_NUMBER | K_STRING
        if node.enumerators is not None:
            return K_STRING | K_NUMBER
        return K_NUMBER | K_STRING if node.bits >= 64 else K_NUMBER
    if k in ("float", "char"):
        return K_NUMBER
    if k == "bool":
        return K_BOOLEAN
    if k == "string":
        return K_ANY if node.json else K_STRING
    if k == "hex":
        return K_ARRAY | K_STRING
    if k in ("list", "array"):
        return K_OBJECT | K_ARRAY if node.assoc == "map" else K_ARRAY
    if k == "tuple":
        return K_ARRAY
    if k == "record":
        return K_OBJECT | K_ARRAY if node.tuple is not None else K_OBJECT
    if k == "option":
        return K_NULL | json_kinds(codec, node.text_child, depth + 1)
    if k == "variant":
        if not node.untagged:
            return K_ARRAY | K_OBJECT
        kinds = 0
        for m in node.members:
            kinds |= json_kinds(codec, m.text, depth + 1)
        return kinds
    if k == "nested":
        return json_kinds(codec, node.text_child, depth + 1)
    if k == "custom":
        handler = None if node.boxed else node.handler
        if handler is not None and (handler.to_text is not None or handler.from_text is not None):
            return K_STRING
        return json_kinds(codec, node.inner, depth + 1) if node.inner is not None else K_ANY
    return K_ANY


def check_untagged(codec: Codec, node: Node) -> None:
    """A typed declaration: the alternatives of an untagged union are told apart by their JSON kinds."""
    seen = 0
    for m in node.members:
        kinds = json_kinds(codec, m.text)
        if kinds & seen:
            raise SchemaError("psio: the alternatives of an untagged variant must be told apart by the kind of "
                              f'JSON value each is written as ("{m.name}" can be written as a kind an earlier '
                              "alternative can)")
        seen |= kinds


# ── Types: reading ──────────────────────────────────────────────────────────

_INTEGER = re.compile(r"(0|-?[1-9][0-9]*)\Z")
_WIDE_HEX = re.compile(r"(-?)0[xX]([0-9a-fA-F]+)\Z")
_HEX_STRING = re.compile(r"[0-9a-fA-F]*\Z")
# Longest integer text: a 256-bit decimal has 78 digits.
MAX_INTEGER_TEXT = 80


def _in_range(value: int, signed: bool, bits: int) -> Optional[int]:
    if not signed:
        return value if 0 <= value < 1 << bits else None
    half = 1 << (bits - 1)
    return value if -half <= value < half else None


def _parse_integer(text: str, signed: bool, bits: int) -> Optional[int]:
    """``0 | -?[1-9][0-9]*`` within the range of an integer of ``bits`` bits."""
    if len(text) > MAX_INTEGER_TEXT or not text.isascii() or _INTEGER.match(text) is None:
        return None
    return _in_range(int(text), signed, bits)


def _parse_wide(text: str, signed: bool, bits: int) -> Optional[int]:
    """Canonical decimal, or the legacy ``[-]0x<hex>`` form (input only)."""
    if len(text) > MAX_INTEGER_TEXT or not text.isascii():
        return None
    m = _WIDE_HEX.match(text)
    if m is None:
        return _parse_integer(text, signed, bits)
    if len(m.group(2)) > bits // 4 or (m.group(1) and not signed):
        return None
    magnitude = int(m.group(2), 16)
    return _in_range(-magnitude if m.group(1) else magnitude, signed, bits)


def _bytes_admit(container: Node, n: int) -> bool:
    if container.kind == "array":
        return n == container.count
    return not container.bounded or n <= container.count or bool(WRITE_MODE.fast)


class _KeyText:
    """How the key type of a map appears in text (docs/json.md, "Maps and sets")."""

    __slots__ = ("form", "node", "handler", "missing", "outer")

    def __init__(self, form: str, node: Optional[Node] = None, handler: Optional[Handler] = None,
                 missing: bool = False, outer: Optional[List[Node]] = None) -> None:
        # pairs: not a string by type, the map is an array of [k, v].  string:
        # a string node.  handler: a custom type whose handler has a text form.
        self.form = form
        self.node = node
        self.handler = handler
        self.missing = missing
        #: The wrappers above ``node`` (outermost first): their bindings apply to a key.
        self.outer = outer or []

    def lower(self, value: Any) -> Any:
        """A key as the typed layer holds it -> the value at ``node``."""
        for n in self.outer:
            if n.bind is not None and n.bind.pre is not None:
                value = n.bind.pre(value)
        return value

    def lift(self, value: Any) -> Any:
        for n in reversed(self.outer):
            if n.bind is not None and n.bind.post is not None:
                value = n.bind.post(value)
        return value


def _key_text_of(codec: Codec, key: Node) -> _KeyText:
    at = key
    outer: List[Node] = []
    for _ in range(4097):
        if at.kind == "string":
            return _KeyText("string", at, outer=outer)
        if at.kind != "custom":
            break
        if not at.boxed:
            # A handler that presents the type decides the key's text.  One
            # that only checks it leaves the text to the wire type.
            handler = at.handler
            if handler is not None and (handler.to_text is not None or handler.from_text is not None):
                return _KeyText("handler", at, handler, outer=outer)
            if handler is None and codec.require_handlers:
                return _KeyText("pairs", missing=True)
        outer.append(at)
        at = at.inner
    return _KeyText("pairs")


_PAIRS = _KeyText("pairs")


class Reader:
    def __init__(self, c: Cursor, ijson: bool, codec: Codec) -> None:
        self.c = c
        self.ijson = ijson
        # The codec of the values being read: the automatic one inside a nested value.
        self.codec = codec

    def _violated(self, message: str) -> Any:
        """An invariant refused the value read: the error is the type's own."""
        raise JsonError(message, offset=self.c.p, invariant=True)

    def _checked(self, node: Node, value: Any) -> None:
        """The handler's invariant on a value just read (as ``decode`` gives it)."""
        try:
            self.codec.check_custom(node, value, "json")
        except PsioError as e:
            e.offset = self.c.p
            raise

    def _from_text(self, node: Node, handler: Handler, text: str) -> Any:
        """The text of a custom type as its handler reads it: a value of the wire type."""
        c = self.c
        try:
            if handler.from_text is None:
                raise ValueError("no text form to read")
            value = handler.from_text(text)
        except Exception:
            return c.fail("json: text is not a value of the custom type")
        try:
            return self.codec.canonical(node.inner, value)
        except PsioError:
            return c.fail("json: custom handler produced a value that is not the inner type")
        except Exception:
            return c.fail("json: custom handler produced a value that is not the inner type")

    def _checked_inside(self, ref: Node, value: Any) -> None:
        """The checks of the custom types inside a value that did not pass through ``read``."""
        if not self.codec.checks_values:
            return
        try:
            self.codec.encoder(ref)(value, 0)
        except PsioError as e:
            if e.invariant:
                self._violated(e.reason)
            self.c.fail(e.reason)

    def _presented(self, node: Node) -> Any:
        """A custom presentation: one JSON string, turned by its handler into a value of the inner type."""
        c = self.c
        if c.kind() != "string":
            c.fail("json: expected a string (custom text form)")
        value = self._from_text(node, node.handler, c.string()[0])
        self._checked_inside(node.inner, value)
        self._checked(node, value)
        return value

    def _assoc(self, node: Node, form: str) -> Any:
        """A map / set: any order, no repeated key or element; the value is in canonical order."""
        c = self.c
        is_map = node.assoc == "map"
        limit = node.count if node.bounded else None
        items: List[Any] = []
        key = _key_text_of(self.codec, node.key_ref) if is_map else _PAIRS
        if key.missing:
            c.fail(NO_HANDLER)
        if key.form == "pairs":
            # A set's elements take the member's integer form; a map entry is
            # a tuple, whose elements take the natural one.
            self._array_items(limit, False, lambda: items.append(
                self.read(node.text_child, "natural" if is_map else form, 0)))
        else:
            if c.kind() != "object":
                c.fail("json: expected an object (a map with a string key)")
            c.begin_object()
            key_ref, value_ref = node.key_ref, node.value_ref
            target: Node = key.node
            while True:
                name = c.next_field()
                if name is None:
                    break
                if limit is not None and len(items) >= limit:
                    c.fail("json: array exceeds its bound")
                # A member name is a key value: what `read` makes of the same
                # text as a JSON string of the key type, invariants included.
                if key.form == "string":
                    data = name.encode("utf-8")
                    if not _bytes_admit(target.child, len(data)):
                        c.fail("json: string exceeds its bound")
                    k: Any = data if target.raw else name
                else:
                    k = self._from_text(target, key.handler, name)
                if target.bind is not None and target.bind.post is not None:
                    k = target.bind.post(k)
                k = key.lift(k)
                self._checked_inside(key_ref, k)
                items.append((k, self.read(value_ref, "natural", 0)))
        try:
            order = self.codec.assoc_order(node, items)
        except PsioError as e:
            return c.fail(e.reason)
        if order is None:
            return c.fail("json: duplicate map key" if is_map else "json: duplicate set element")
        try:
            return self.codec.assoc_value(node, [items[i] for i in order])
        except PsioError as e:
            return c.fail(e.reason)

    def _wide_integer(self, bits: int, signed: bool, form: str) -> int:
        c = self.c
        if c.kind() == "string":
            if form == "number":
                c.fail("json: expected a number, found a string")
            text, escaped = c.string()
            value = None if escaped else _parse_wide(text, signed, bits)
            if value is None:
                return c.fail("json: integer string is not a canonical in-range decimal")
            return value
        if form == "string":
            c.fail("json: expected an integer string")
        if c.kind() != "number":
            c.fail("json: expected an integer")
        value = _parse_wide(c.number(), signed, bits)
        if value is None:
            return c.fail("json: integer is not a canonical in-range decimal")
        if self.ijson and form == "natural" and not -SAFE_INTEGER <= value <= SAFE_INTEGER:
            c.fail("ijson: bare integer outside the safe range must be a string")
        return value

    def _enumeration(self, node: Node, form: str) -> int:
        """An Enum: the enumerator's name, the number (or numeric string), or both (``"active:2"``)."""
        c = self.c
        bits, signed = node.bits, node.signed
        if bits > 64 or bits < 8:
            c.fail("pssz IR: an enum needs an underlying integer of 8 to 64 bits")
        if c.kind() != "string":
            return self._integer(bits, signed, form)
        text, _ = c.string()
        first = text[:1]
        value: Optional[int]
        if first == "-" or "0" <= first <= "9":
            value = _parse_integer(text, signed, bits)
            if value is None:
                return c.fail("json: enum number is not a canonical in-range decimal")
            return value
        name, colon, number = text.partition(":")
        found = None
        for m in node.enumerators or ():
            if m["name"] == name:
                found = m
                break
        if found is None:
            return c.fail("json: unknown enumerator name")
        if colon:
            value = _parse_integer(number, signed, bits)
            if value is None:
                return c.fail("json: enum number is not a canonical in-range decimal")
            if value & U64_MASK != found["value"]:
                c.fail("json: enumerator name and number disagree")
            return value
        raw = found["value"]
        value = _in_range(raw - (1 << 64) if signed and raw >> 63 else raw, signed, bits)
        if value is None:
            return c.fail("json: enumerator value is outside its underlying integer")
        return value

    def _integer(self, bits: int, signed: bool, form: str) -> int:
        c = self.c
        if bits > 64:
            return self._wide_integer(bits, signed, form)
        wide = self.ijson and bits == 64 and form == "natural"
        kind = c.kind()
        if kind == "string":
            if form != "string" and not wide:
                c.fail("json: expected a number, found a string")
            text, escaped = c.string()
            value = None if escaped or len(text) > 21 else _parse_integer(text, signed, bits)
            if value is None:
                return c.fail("json: integer string is not a canonical in-range decimal")
            return value
        if form == "string":
            c.fail("json: expected an integer string")
        if kind != "number":
            c.fail("json: expected an integer")
        token = c.number()
        value = None if len(token) > 21 else _parse_integer(token, signed, bits)
        if value is None:
            return c.fail("json: integer is not a canonical in-range decimal")
        # Natural ijson: a bare number only where a double is exact.
        if wide and not -SAFE_INTEGER <= value <= SAFE_INTEGER:
            c.fail("ijson: bare integer outside the safe range must be a string")
        return value

    def _time(self, node: Node, form: str) -> int:
        c = self.c
        if not node.time.point:
            return self._integer(node.bits, node.signed, form)
        if node.time_digits < 0:
            c.fail("json: this time point unit has no RFC 3339 text form")
        if c.kind() != "string":
            c.fail("json: expected an RFC 3339 date-time string")
        text, _ = c.string()
        # A date-time is ASCII; anything else fails the grammar.
        if len(text) > 256 or not text.isascii():
            text = ""
        out = ticks_of(text, node.time_digits, node.bits, node.signed)
        if isinstance(out, str):
            return c.fail(out)
        return out[0]

    def _array_items(self, limit: Optional[int], exact: bool, item: Callable[[], Any]) -> None:
        c = self.c
        c.begin_array()
        count = 0
        while c.next_element():
            if limit is not None and count >= limit:
                c.fail("json: array has too many elements" if exact else "json: array exceeds its bound")
            count += 1
            item()
        if exact and limit is not None and count != limit:
            c.fail("json: array has too few elements")

    def _arguments(self, node: Node) -> Any:
        """An argument list written as the positional array: exactly one element per parameter."""
        c = self.c
        members = node.members
        ijson = self.ijson
        values: List[Any] = []

        def item() -> None:
            m = members[len(values)]
            value = self.read(m.text, m.ijson_int if ijson else m.json_int, 0)
            if m.bound is not None and not (member_bound_admits(m.bound, m.node, value)
                                            and self.codec.nested_value_admits(m.bound, m.node, m.text, value)):
                c.fail("json: member length_bound violated")
            values.append(value)
        self._array_items(len(members), True, item)
        return self._made(node, values)

    def _made(self, node: Node, slots: List[Any]) -> Any:
        self.codec.check_order(node, slots, self.c.fail)
        bind = node.bind
        if bind is not None and bind.make is not None:
            return bind.make(slots)
        if node.tuple == "positional":
            return tuple(slots)
        return dict(zip([m.name for m in node.members], slots))

    def _record(self, node: Node) -> Any:
        c = self.c
        # An argument list (docs/json.md "Method arguments"): the object when
        # every parameter has a name, and the positional array always.
        if node.tuple is not None and (node.tuple == "positional" or c.kind() == "array"):
            return self._arguments(node)
        c.begin_object()
        # One slot per member, filled as keys arrive; a repeated key overwrites
        # its slot (last one wins).
        members = node.members
        missing = Reader  # a sentinel no value is
        slots: List[Any] = [missing] * len(members)
        by_name = node.by_name
        ijson = self.ijson
        while True:
            key = c.next_field()
            if key is None:
                break
            index = by_name.get(key)
            if index is None:
                c.skip()
                continue
            m = members[index]
            value = self.read(m.text, m.ijson_int if ijson else m.json_int, 0)
            if m.bound is not None and not (member_bound_admits(m.bound, m.node, value)
                                            and self.codec.nested_value_admits(m.bound, m.node, m.text, value)):
                c.fail("json: member length_bound violated")
            slots[index] = value
        for i, v in enumerate(slots):
            if v is missing:
                # A missing Option member is empty; any other is an error.
                if members[i].node.kind != "option":
                    # (a typed record names itself too: C++ `json: missing required member "event" of Ev`)
                    cls = node.bind.cls if node.bind is not None else None
                    c.fail(f'json: missing required member "{members[i].name}"'
                           + (f" of {cls.__name__}" if isinstance(cls, type) else ""))
                slots[i] = None
        return self._made(node, slots)

    def _variant(self, node: Node) -> Any:
        c = self.c
        members = node.members
        if node.untagged:
            # The alternative whose values are written as the kind of value found.
            bit = _KIND_BITS[c.kind()]
            index = -1
            for i, m in enumerate(members):
                if not json_kinds(self.codec, m.text) & bit:
                    continue
                if index >= 0:
                    c.fail(UNTAGGED_AMBIGUOUS)
                index = i
            if index < 0:
                c.fail("json: the value is none of the untagged variant's alternatives")
            payload = self.read(members[index].text, "natural", 0)
        elif self.ijson:
            if c.kind() != "object":
                c.fail("ijson: expected a variant object")
            c.begin_object()
            key = c.next_field()
            if key is None:
                return c.fail("ijson: variant object must have one key")
            index = -1
            for i, m in enumerate(members):
                if m.name == key:
                    index = i
                    break
            if index < 0:
                c.fail("ijson: unknown variant alternative")
            payload = self.read(members[index].text, "natural", 0)
            if c.next_field() is not None:
                c.fail("ijson: variant object must have one key")
        else:
            if c.kind() != "array":
                c.fail("json: expected a [index, value] variant")
            c.begin_array()
            if not c.next_element():
                c.fail("json: expected a [index, value] variant")
            index = self._integer(32, False, "natural")
            if index >= len(members):
                c.fail("json: variant index out of range")
            if not c.next_element():
                c.fail("json: expected ',' in variant")
            payload = self.read(members[index].text, "natural", 0)
            if c.next_element():
                c.fail("json: expected ']' after variant value")
        bind = node.bind
        if bind is not None and bind.join is not None:
            return bind.join(index, payload)
        return Variant(members[index].name, payload)

    def read(self, node: Node, form: str, n: int) -> Any:
        value = self._read(node, form, n)
        bind = node.bind
        if bind is not None and bind.post is not None:
            return bind.post(value)
        return value

    def _read(self, node: Node, form: str, n: int) -> Any:
        # `n` counts schema steps that consume no text (nested Options); it
        # restarts inside every array / object, whose nesting the cursor limits.
        c = self.c
        if n > MAX_DEPTH:
            c.fail("json: max depth exceeded")
        k = node.kind
        if k == "int":
            if node.time is not None:
                return self._time(node, form)
            if node.enumerators is not None:
                return self._enumeration(node, form)
            return self._integer(node.bits, node.signed, form)
        if k == "string":
            if node.json:
                # Any JSON value, inline.
                return read_json_value(c)
            if c.kind() != "string":
                c.fail("json: expected a string")
            text, _ = c.string()
            container = node.child
            if container.bounded:
                size = len(text) if text.isascii() else len(text.encode("utf-8"))
                if not _bytes_admit(container, size):
                    c.fail("json: string exceeds its bound")
            # A raw string is its bytes (here: the UTF-8 of the text).
            return text.encode("utf-8") if node.raw else text
        if k == "record":
            return self._record(node)
        if k in ("list", "array"):
            if node.assoc:
                return self._assoc(node, form)
            items: List[Any] = []
            child = node.text_child
            self._array_items(node.count if node.bounded else None, node.exact,
                              lambda: items.append(self.read(child, form, 0)))
            return items
        if k == "option":
            if c.kind() == "null":
                c.null()
                return None
            inner = self.read(node.text_child, form, n + 1)
            return Some(inner) if node.child.kind == "option" else inner
        if k == "bool":
            return c.boolean()
        if k == "float":
            token = c.number()
            value = parse_single(token) if node.size == 4 else float(token)
            if value is None or value in (float("inf"), float("-inf")):
                return c.fail("json: number out of range")
            return value
        if k == "char":
            return self._integer(8, False, form)
        if k in ("bitvector", "bitlist"):
            return c.fail("json: bit types have no JSON form")
        if k == "hex":
            container = node.child
            if not self.ijson:
                out: List[int] = []
                self._array_items(container.count if container.bounded else None, container.exact,
                                  lambda: out.append(self._integer(8, False, form)))
                return bytes(out)
            if c.kind() != "string":
                c.fail("ijson: expected a hex string")
            text, escaped = c.string()
            if escaped or not text.isascii() or _HEX_STRING.match(text) is None:
                c.fail("ijson: non-hex character in hex string")
            if len(text) % 2:
                c.fail("ijson: odd-length hex string")
            if not _bytes_admit(container, len(text) // 2):
                c.fail("ijson: hex string has the wrong length" if container.exact else "json: array exceeds its bound")
            return bytes.fromhex(text)
        if k == "tuple":
            members = node.members
            values: List[Any] = []
            self._array_items(len(members), True,
                              lambda: values.append(self.read(members[len(values)].text, "natural", 0)))
            return tuple(values)
        if k == "variant":
            return self._variant(node)
        if k == "nested":
            # The inner value's own text.  Its values are those of the
            # automatic profile, whatever profile the value around it has.
            outer = self.codec
            self.codec = outer.auto
            try:
                inner = self.read(node.text_child, form, n + 1)
            finally:
                self.codec = outer
            if not outer.is_opaque(node):
                return inner
            # An opaque nested value is its bytes: the canonical encoding of what was read.
            try:
                return outer.auto.quietly(lambda: outer.auto.encoder(node.text_child)(inner, 0))
            except PsioError:
                return c.fail("json: nested value has no encoding")
        if k == "custom":
            # "box" is the inner type's text.  A custom type is presented and
            # checked by its handler; with none it is its wire type (or, under
            # a strict registry, an error).
            if node.boxed:
                return self.read(node.inner, form, n + 1)
            handler = node.handler
            if handler is None and self.codec.require_handlers:
                c.fail(NO_HANDLER)
            if handler is not None and handler.from_text is not None:
                return self._presented(node)
            value = self.read(node.inner, form, n + 1)
            if handler is not None and handler.check is not None:
                # The wire type's text; the custom values inside it were
                # checked as they were read, then this one as `decode` gives it.
                try:
                    held = self.codec.canonical(node.inner, value)
                except PsioError:
                    return c.fail("pssz IR: custom value does not match its inner type")
                self._checked(node, held)
            return value
        return c.fail("json: unsupported type")


def json_to_value(codec: Codec, node: Node, format: str, text: Any) -> Any:
    """Reads exactly one value of ``node`` from JSON / I-JSON text (``str``, or UTF-8 ``bytes``)."""
    if format not in ("json", "ijson"):
        raise JsonError('json: format must be "json" or "ijson"')
    if isinstance(text, (bytes, bytearray, memoryview)):
        try:
            text = bytes(text).decode("utf-8")
        except UnicodeDecodeError as e:
            raise JsonError("json: string is not valid UTF-8", offset=e.start) from None
    elif not isinstance(text, str):
        raise JsonError("json: text must be str or bytes")
    elif not text.isascii():
        try:
            text.encode("utf-8")
        except UnicodeEncodeError as e:
            raise JsonError("json: text holds an unpaired surrogate", offset=e.start) from None
    ijson = format == "ijson"
    cursor = Cursor(text, ijson)
    try:
        value = Reader(cursor, ijson, codec).read(node, "natural", 0)
    except RecursionError:
        raise JsonError("json: max depth exceeded", offset=cursor.p) from None
    cursor.finish()
    return value


# ── Types: writing ──────────────────────────────────────────────────────────

def _fail(message: str) -> NoReturn:
    raise EncodeError(message)


_ESCAPE = re.compile(r'["\\\x00-\x1f]')
_ESCAPES = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}


def _escape(m: "re.Match[str]") -> str:
    c = m.group()
    return _ESCAPES.get(c) or f"\\u00{ord(c):02x}"


def quote(s: str, nonchar: bool) -> str:
    """Quotes and escapes ``s``.  ``nonchar`` also refuses Unicode noncharacters."""
    if not s.isascii():
        try:
            s.encode("utf-8")
        except UnicodeEncodeError:
            _fail("json: string is not valid UTF-8")
        if nonchar and _NONCHAR.search(s) is not None:
            _fail("ijson: string holds a Unicode noncharacter")
    if _ESCAPE.search(s) is None:
        return f'"{s}"'
    return '"' + _ESCAPE.sub(_escape, s) + '"'


class Writer:
    def __init__(self, ijson: bool, codec: Codec) -> None:
        self.out: List[str] = []
        self.ijson = ijson
        # The codec of the values being written: the automatic one inside a nested value.
        self.codec = codec
        #: Whether the handlers' ``check`` runs on the values written.
        self.checks = True

    def _held(self, ref: Node, value: Any) -> Any:
        """The value of a custom node as ``decode`` gives it, the custom values inside checked on the way."""
        codec = self.codec
        try:
            enc = codec.encoder(ref)
            data = enc(value, 0) if self.checks else codec.quietly(lambda: enc(value, 0))
            return codec.quietly(lambda: codec.decoder(ref)(data, 0, len(data), 0, Ctx(False)))
        except PsioError as e:
            if e.invariant or e.reason == NO_HANDLER:
                raise
            return _fail("pssz IR: custom value does not match its inner type")

    def _presented_text(self, handler: Handler, inner: Any) -> None:
        try:
            if handler.to_text is None:
                raise ValueError("no text form to write")
            text = handler.to_text(inner)
        except Exception:
            return _fail("json: custom handler has no text for the value")
        if type(text) is not str:
            return _fail("json: custom handler has no text for the value")
        self.out.append(quote(text, self.ijson))

    def _assoc(self, node: Node, value: Any, form: str, n: int) -> None:
        """A map / set: canonical order; an object when the key is a string by type, an array otherwise."""
        out = self.out
        is_map = node.assoc == "map"
        codec = self.codec
        items = codec.assoc_sorted(node, value, _fail, "json")
        if node.bounded and len(items) > node.count and not WRITE_MODE.fast:
            _fail("json: array exceeds its bound")
        key = _key_text_of(codec, node.key_ref) if is_map else _PAIRS
        if key.missing:
            _fail(NO_HANDLER)
        is_object = key.form != "pairs"
        out.append("{" if is_object else "[")
        first = True
        for item in items:
            if not first:
                out.append(",")
            first = False
            if not is_object:
                # A set element, or a map entry as the [k, v] tuple it is.
                self.write(node.text_child, item, "natural" if is_map else form, n + 1)
                continue
            # The key is written from its value, not through `write`: the
            # checks of its type's custom nodes run here.
            if self.checks and codec.checks_values:
                codec.encoder(node.key_ref)(item[0], 0)
            k = key.lower(item[0])
            target: Node = key.node
            if key.form == "string":
                self.write(target, k, "natural", n + 1)
            else:
                if target.bind is not None and target.bind.pre is not None:
                    k = target.bind.pre(k)
                try:
                    inner = codec.canonical(target.inner, k)
                except PsioError:
                    return _fail("pssz IR: custom value does not match its inner type")
                self._presented_text(key.handler, inner)
            out.append(":")
            self.write(node.value_ref, item[1], "natural", n + 1)
        out.append("}" if is_object else "]")

    def _integer(self, bits: int, signed: bool, form: str, value: Any, enumerators: Any = None) -> None:
        if not isinstance(value, int):
            _fail("pssz IR: integer value does not match the schema")
        v = int(value)
        if _in_range(v, signed, bits) is None:
            _fail("pssz IR: integer value does not match the schema")
        if enumerators is not None:
            if bits > 64 or bits < 8:
                _fail("pssz IR: an enum needs an underlying integer of 8 to 64 bits")
            # A listed value is its (first) name; any other its number.
            raw = v & U64_MASK
            for m in enumerators:
                if m["value"] == raw:
                    self.out.append(quote(m["name"], self.ijson))
                    return
        # 64 bits: a string beyond 2^53 - 1 under ijson.  Wider: always a
        # string under ijson, a bare number under json.
        quoted = form == "string" or (form == "natural" and self.ijson and (
            bits > 64 or (bits == 64 and not -SAFE_INTEGER <= v <= SAFE_INTEGER)))
        self.out.append(f'"{v}"' if quoted else str(v))

    def write(self, node: Node, value: Any, form: str, n: int) -> None:
        # A decoded value is at most MAX_DEPTH nodes deep and text nests at
        # most MAX_DEPTH containers; anything far beyond is a cyclic object.
        if n > 4 * MAX_DEPTH:
            _fail("pssz IR: node depth limit exceeded")
        bind = node.bind
        if bind is not None and bind.pre is not None:
            value = bind.pre(value)
        out = self.out
        k = node.kind
        if k == "int":
            if node.time is not None and node.time.point:
                if type(value) is not int or not tick_type_admits(node.bits, node.signed, value):
                    return _fail("pssz IR: integer value does not match the schema")
                if node.time_digits < 0:
                    return _fail("json: this time point unit has no RFC 3339 text form")
                text = text_of(value, node.time_digits)
                if isinstance(text, str):
                    return _fail(text)
                out.append('"' + text[0] + '"')
                return
            return self._integer(node.bits, node.signed, form, value, node.enumerators)
        if k == "string":
            if node.json:
                # Any JSON value: its compact text, inline.
                text = json_value_text(value, self.ijson)
                if not _bytes_admit(node.child, len(text) if text.isascii() else len(text.encode("utf-8"))):
                    _fail("json: string exceeds its bound")
                out.append(text)
                return
            if type(value) is str:
                container = node.child
                if container.bounded or container.kind == "array":
                    try:
                        size = len(value.encode("utf-8"))
                    except UnicodeEncodeError:
                        return _fail("json: string is not valid UTF-8")
                    if not _bytes_admit(container, size):
                        _fail("json: string exceeds its bound")
                out.append(quote(value, self.ijson))
                return
            if node.raw and isinstance(value, (bytes, bytearray, memoryview)):
                # A raw string holding bytes that are not UTF-8 has no JSON text.
                data = bytes(value)
                if not _bytes_admit(node.child, len(data)):
                    _fail("json: string exceeds its bound")
                try:
                    out.append(quote(data.decode("utf-8"), self.ijson))
                except UnicodeDecodeError:
                    _fail("json: string is not valid UTF-8")
                return
            return _fail("pssz IR: string value required")
        if k == "record":
            values = self.codec.record_getter(node)(value)
            self.codec.check_order(node, values, _fail)
            # A positional argument list is the array of its arguments.
            named = node.tuple != "positional"
            out.append("{" if named else "[")
            first = True
            ijson = self.ijson
            for m, field in zip(node.members, values):
                if m.bound is not None and not (member_bound_admits(m.bound, m.node, field)
                                                and self.codec.nested_value_admits(m.bound, m.node, m.text, field)):
                    _fail("json: member length_bound violated")
                out.append(("" if first else ",") + (quote(m.name, False) + ":" if named else ""))
                first = False
                self.write(m.text, field, m.ijson_int if ijson else m.json_int, n + 1)
            out.append("}" if named else "]")
            return
        if k in ("list", "array"):
            if node.assoc:
                return self._assoc(node, value, form, n)
            if not isinstance(value, (list, tuple)):
                return _fail("pssz IR: sequence value required")
            if node.bounded and (len(value) != node.count if node.exact
                                 else len(value) > node.count and not WRITE_MODE.fast):
                _fail("json: array exceeds its bound")
            out.append("[")
            child = node.text_child
            first = True
            for item in value:
                if not first:
                    out.append(",")
                first = False
                self.write(child, item, form, n + 1)
            out.append("]")
            return
        if k == "option":
            if value is None:
                out.append("null")
                return
            if node.child.kind == "option":
                if type(value) is not Some:
                    _fail("pssz: a nested optional needs None or Some(value)")
                value = value.value
            return self.write(node.text_child, value, form, n + 1)
        if k == "bool":
            if value is not True and value is not False:
                return _fail("pssz IR: bool value required")
            out.append("true" if value else "false")
            return
        if k == "float":
            name = "ijson" if self.ijson else "json"
            if type(value) is NaN:
                return _fail(f"{name}: non-finite float has no JSON text")
            if not isinstance(value, (float, int)) or isinstance(value, bool):
                return _fail("pssz IR: float value does not match the schema")
            # The value as the float holds it: a number beyond binary32 is infinite there.
            held = fround(value) if node.size == 4 else float(value)
            if held != held or held in (float("inf"), float("-inf")):
                return _fail(f"{name}: non-finite float has no JSON text")
            out.append(format_float(held, node.size == 4))
            return
        if k == "char":
            return self._integer(8, False, form, value)
        if k in ("bitvector", "bitlist"):
            return _fail("json: bit types have no JSON form")
        if k == "hex":
            if not isinstance(value, (bytes, bytearray, memoryview)):
                return _fail("pssz IR: bytes value required")
            data = bytes(value)
            if not _bytes_admit(node.child, len(data)):
                _fail("json: array exceeds its bound")
            if self.ijson:
                out.append('"' + data.hex().upper() + '"')
            elif form == "string":
                out.append("[" + ",".join(f'"{b}"' for b in data) + "]")
            else:
                out.append("[" + ",".join(map(str, data)) + "]")
            return
        if k == "nested":
            # The inner value's own text.  An opaque nested value holds bytes:
            # they are validated as the inner type (the compatible rule) first.
            outer = self.codec
            inner = value
            if outer.is_opaque(node):
                if not isinstance(value, (bytes, bytearray, memoryview)):
                    return _fail("pssz IR: bytes value required for an opaque nested value")
                data = bytes(value)
                try:
                    inner = outer.auto.decoder(node.text_child)(data, 0, len(data), 0, Ctx(True))
                except PsioError as e:
                    if e.invariant:
                        raise
                    return _fail("json: nested bytes are not a valid encoding of their type")
            self.codec = outer.auto
            try:
                return self.write(node.text_child, inner, form, n + 1)
            finally:
                self.codec = outer
        if k == "tuple":
            members = node.members
            if not isinstance(value, (list, tuple)) or len(value) != len(members):
                return _fail("pssz IR: tuple value count mismatch")
            out.append("[")
            for i, m in enumerate(members):
                if i:
                    out.append(",")
                self.write(m.text, value[i], "natural", n + 1)
            out.append("]")
            return
        if k == "variant":
            index, payload = self.codec.variant_splitter(node)(value)
            m = node.members[index]
            if node.untagged:
                # Untagged: the bare value of the alternative.
                self.write(m.text, payload, "natural", n + 1)
            elif self.ijson:
                out.append("{" + quote(m.name, False) + ":")
                self.write(m.text, payload, "natural", n + 1)
                out.append("}")
            else:
                out.append(f"[{index},")
                self.write(m.text, payload, "natural", n + 1)
                out.append("]")
            return
        if k == "custom":
            inner_node: Node = node.inner
            if not node.boxed:
                handler = node.handler
                if handler is None and self.codec.require_handlers:
                    _fail(NO_HANDLER)
                check = self.checks and handler is not None and handler.check is not None
                if handler is not None and (handler.to_text is not None or check):
                    # The inner value as `decode` gives it — which checks the
                    # custom values inside it — then this one's check, then
                    # the handler's text as one JSON string.
                    held = self._held(inner_node, value)
                    if check:
                        self.codec.check_custom(node, held, "value")
                    if handler.to_text is not None:
                        return self._presented_text(handler, held)
                    # The wire type's text; everything under it is checked.
                    outer_checks = self.checks
                    self.checks = False
                    try:
                        return self.write(inner_node, value, form, n + 1)
                    finally:
                        self.checks = outer_checks
            # "box", or a custom type with no handler: the inner type.
            return self.write(inner_node, value, form, n + 1)
        _fail("json: unsupported type")


def value_to_json(codec: Codec, node: Node, format: str, value: Any, checks: bool = True) -> str:
    """The JSON / I-JSON text of ``value``.

    The handlers' checks run on the value's custom types before any text is
    returned (``checks`` false: the value was just decoded with the same registry).
    """
    if format not in ("json", "ijson"):
        raise EncodeError('json: format must be "json" or "ijson"')
    writer = Writer(format == "ijson", codec)
    writer.checks = checks
    writer.write(node, value, "natural", 0)
    return "".join(writer.out)
