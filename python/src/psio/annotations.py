"""The annotation vocabulary: what is written in ``typing.Annotated[...]``.

Types stay the types the program wants; everything psio needs to know beyond
the type hint is an object in the hint's ``Annotated`` metadata (or in
``dataclasses.field(metadata={"psio": [...]})``).  The names are those of the
C++ / Rust / TypeScript vocabulary (docs/annotations.md, "Master table").

    @dataclass
    class Account:
        id: u64
        owner: Annotated[str, psio.max(32)]
        tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
        balance: u64
"""

from __future__ import annotations

import datetime
import sys
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Optional, Tuple, Union

if sys.version_info >= (3, 9):
    from typing import Annotated
else:  # pragma: no cover
    raise ImportError("psio needs Python 3.10 or newer")

from .errors import SchemaError
from .values import Bits

__all__ = [
    "u8", "u16", "u32", "u64", "u128", "u256", "i8", "i16", "i32", "i64", "i128", "f32", "f64", "char",
    "RawString", "length", "exact", "element", "keys", "values", "at", "utf8", "bounded", "array", "bitvector",
    "bitlist", "nested", "boxed", "identity", "name", "alt", "custom", "json_number", "json_string", "ijson_number",
    "ijson_string", "time_point", "duration", "monotonic", "TimePointSec", "TimePointMs", "TimePointUs",
    "TimePointNs", "DurationSec", "DurationMs", "DurationUs", "DurationNs", "sorted", "unique", "unique_keys", "untagged", "JsonValue",
]


class Spec:
    """Base class of everything psio reads from ``Annotated`` metadata."""

    __slots__ = ()


# ── Scalars ─────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class IntSpec(Spec):
    bits: int
    signed: bool


@dataclass(frozen=True)
class FloatSpec(Spec):
    bits: int


@dataclass(frozen=True)
class CharSpec(Spec):
    pass


u8 = Annotated[int, IntSpec(8, False)]
u16 = Annotated[int, IntSpec(16, False)]
u32 = Annotated[int, IntSpec(32, False)]
u64 = Annotated[int, IntSpec(64, False)]
u128 = Annotated[int, IntSpec(128, False)]
u256 = Annotated[int, IntSpec(256, False)]
i8 = Annotated[int, IntSpec(8, True)]
i16 = Annotated[int, IntSpec(16, True)]
i32 = Annotated[int, IntSpec(32, True)]
i64 = Annotated[int, IntSpec(64, True)]
i128 = Annotated[int, IntSpec(128, True)]
f32 = Annotated[float, FloatSpec(32)]
f64 = Annotated[float, FloatSpec(64)]
#: One byte of a raw string (C++ ``char``); an ``int`` 0..255.
char = Annotated[int, CharSpec()]


@dataclass(frozen=True)
class RawStringSpec(Spec):
    pass


#: A raw string: any bytes, a JSON string in text (C++ ``std::string``, Rust
#: ``RawString``).  Plain ``bytes`` is a byte list (hex in I-JSON); ``str`` is UTF-8 text.
RawString = Annotated[bytes, RawStringSpec()]


# ── Bounds ──────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class LengthSpec(Spec):
    """A length bound: bytes of a string, elements of a list, map or set, bytes of a nested value."""

    max: Optional[int] = None
    min: Optional[int] = None
    exact: Optional[int] = None
    #: Recorded in the Schema IR as the member attribute ``lengthBound`` (``psio.length``), not in the type.
    attribute: bool = False

    def __post_init__(self) -> None:
        for v in (self.max, self.min, self.exact):
            if v is not None and (type(v) is not int or v < 0):
                raise TypeError("psio: a length bound is a non-negative integer")


def max(n: int) -> LengthSpec:  # noqa: A001 - the vocabulary's name
    """At most ``n``: ``Annotated[str, psio.max(32)]``."""
    return LengthSpec(max=n)


def min(n: int) -> LengthSpec:  # noqa: A001
    """At least ``n`` (the member itself only; bounds nothing for the width)."""
    return LengthSpec(min=n)


def exact(n: int) -> LengthSpec:
    """Exactly ``n`` (the member itself only)."""
    return LengthSpec(exact=n)


def length(*, max: Optional[int] = None, min: Optional[int] = None, exact: Optional[int] = None) -> LengthSpec:  # noqa: A002
    """A range as one spec: ``psio.length(min=2, max=8)``.

    The Schema IR records it as the member attribute ``lengthBound`` (C++
    ``psio::length_bound{...}``); the checks and the width are those of
    ``psio.max`` / ``psio.min`` / ``psio.exact``.
    """
    return LengthSpec(max=max, min=min, exact=exact, attribute=True)


@dataclass(frozen=True)
class BoundedSpec(Spec):
    """The type's own bound, as a wrapper type states it (C++ ``psio::bounded<T, N>``, ``utf8_string<N>``)."""

    max: int


def bounded(n: int) -> BoundedSpec:
    """An opt-in bounded type: ``Name = Annotated[str, psio.bounded(16)]``.

    The bytes and checks are those of ``psio.max(n)``; the difference is only
    where the Schema IR records the bound (the type, not the member).
    """
    return BoundedSpec(n)


@dataclass(frozen=True)
class Utf8Spec(Spec):
    pass


#: On a :data:`RawString` member: it is UTF-8 text (C++ ``attr(f, utf8)``).  In Python write ``str``.
utf8 = Utf8Spec()


@dataclass(frozen=True)
class SortedSpec(Spec):
    """The elements of a list or an array are in order (spec 3.5 "Order constraints")."""

    unique: bool = False
    ascending: bool = True

    def __call__(self, *, descending: bool = False) -> "SortedSpec":
        """``psio.sorted(descending=True)``: the same order, descending."""
        return SortedSpec(self.unique, not descending)


@dataclass(frozen=True)
class UniqueKeysSpec(Spec):
    pass


#: The elements of the list / array are in non-descending order: ``Annotated[list[u32], psio.sorted]``.
#: Order and equality are those of maps and sets: bytewise comparison of the elements' ``key``
#: encodings.  Every writer refuses a value that is not in order and every checking reader rejects
#: its bytes or text.  No byte, width or bound changes, and the Schema IR does not record it (a
#: schema-driven reader does not know it).  On a record member, a method parameter or a position
#: below one (``psio.element(psio.sorted)``).
sorted = SortedSpec()  # noqa: A001 - the vocabulary's name
#: The elements are in strictly ascending order (no two are equal).
unique = SortedSpec(unique=True)
#: No two elements have the same key: the element itself, or element 0 of a tuple element.  No order is required.
unique_keys = UniqueKeysSpec()


@dataclass(frozen=True)
class PositionSpec(Spec):
    """Annotations for a position below the member."""

    where: str
    index: Any
    specs: Tuple[Any, ...]


def element(*specs: Any) -> PositionSpec:
    """Every element of a list, an array or a set."""
    return PositionSpec("element", None, specs)


def keys(*specs: Any) -> PositionSpec:
    """Every key of a map."""
    return PositionSpec("keys", None, specs)


def values(*specs: Any) -> PositionSpec:
    """Every mapped value of a map."""
    return PositionSpec("values", None, specs)


def at(index: Union[int, str], *specs: Any) -> PositionSpec:
    """Element ``index`` of a tuple, or alternative ``index`` (its position or name) of a union."""
    return PositionSpec("at", index, specs)


# ── Shapes the type hint alone cannot say ───────────────────────────────────

@dataclass(frozen=True)
class ArraySpec(Spec):
    count: int


def array(n: int) -> ArraySpec:
    """A fixed-count array: ``Annotated[list[u32], psio.array(4)]``, ``Annotated[bytes, psio.array(32)]``."""
    return ArraySpec(n)


@dataclass(frozen=True)
class BitsSpec(Spec):
    id: str
    count: int


def bitvector(n: int) -> BitsSpec:
    """Exactly ``n`` packed bits: ``Annotated[psio.Bits, psio.bitvector(8)]`` (or ``list[bool]``)."""
    return BitsSpec("bitvector", n)


def bitlist(n: int) -> BitsSpec:
    """At most ``n`` packed bits."""
    return BitsSpec("bitlist", n)


@dataclass(frozen=True)
class NestedSpec(Spec):
    inner: Any = None
    deferred: bool = False


def nested(inner: Any = None, *, deferred: bool = False) -> NestedSpec:
    """A nested value: a byte list holding the PSSZ encoding of a value (spec 5.8).

    ``Annotated[TransferArgs, psio.nested()]``: the member is a
    ``TransferArgs``, validated by every reader.  ``Annotated[bytes,
    psio.nested(TransferArgs, deferred=True)]``: the member is the bytes,
    not looked at by a reader of the enclosing value.
    """
    return NestedSpec(inner, deferred)


@dataclass(frozen=True)
class BoxSpec(Spec):
    pass


#: The value is boxed in C++ (``psio::box<T>``): same bytes, but no inferred size bound.
boxed = BoxSpec()


@dataclass(frozen=True)
class IdentitySpec(Spec):
    label: str


def identity(label: str) -> IdentitySpec:
    """Gives a structurally identical type its own entry in the Schema IR.

    C++ has pairs of types with one wire form (``std::pair`` / ``std::tuple``,
    ``std::map`` / ``std::unordered_map``); a schema generated from them holds
    both.  Bytes are unaffected.
    """
    return IdentitySpec(label)


@dataclass(frozen=True)
class NameSpec(Spec):
    name: str


def name(text: str) -> NameSpec:
    """The member's name on the wire and in text, when it is not the attribute's.

    (A trailing underscore after a Python keyword is dropped without this:
    ``from_`` is ``from``.)
    """
    return NameSpec(text)


@dataclass(frozen=True)
class AltSpec(Spec):
    name: str = ""


def alt(name: str) -> AltSpec:
    """States the label of an alternative of a union: ``Annotated[Circle, psio.alt("circle")] | Square``.

    An alternative has one label: the key of its I-JSON form (``{"circle": ...}``) and its name in the
    Schema IR.  Without ``alt`` it is the name of the alternative's type — a class's name, and ``bool``,
    ``int8`` .. ``uint64``, ``int128``, ``uint128``, ``float32``, ``double``, ``string``, ``uint8[]``
    (bytes), ``T[]``, ``T[N]``, ``T?``, ``tuple_A_B``, ``map_K_V``, ``set_K``, ``variant_A_B`` for the
    types that have no declared name — or, for a type with no name and for a type name two alternatives
    of the union share, its position (``"0"``, ``"1"``, ...).  Stated labels are nonempty and distinct.
    Use it to match the labels of the type's twin in another language (docs/annotations.md "Variant
    alternative labels").
    """
    if not isinstance(name, str) or not name:
        raise SchemaError("psio: alt(name): the label of an alternative is a nonempty string")
    return AltSpec(name)


@dataclass(frozen=True)
class UntaggedSpec(Spec):
    pass


#: An untagged union: ``Annotated[Union[str, list[str]], psio.untagged]`` (C++ ``psio_is_untagged``,
#: the Variant attribute ``untagged``).  In JSON / I-JSON it is the bare value of its alternative, and
#: a reader picks the alternative by the kind of JSON value it finds (docs/json.md "Untagged
#: variants"), so the kinds the alternatives are written as must not overlap.  The bytes are those of
#: any union.
untagged = UntaggedSpec()


@dataclass(frozen=True)
class JsonValueSpec(Spec):
    pass


#: Any JSON value (C++ ``psio::json_value``; the Custom id ``json``): ``None``, ``bool``, ``int``,
#: ``float`` / ``psio.JsonNumber``, ``str``, ``list`` or ``dict`` with ``str`` keys.  In JSON / I-JSON
#: it is that value, inline; in PSSZ a UTF-8 string holding its compact JSON text.
JsonValue = Annotated[Any, JsonValueSpec()]


@dataclass(frozen=True, eq=False)
class CustomSpec(Spec):
    id: str
    handler: Any


def custom(id: str, *, to_text: Optional[Callable[[Any], str]] = None,
           from_text: Optional[Callable[[str], Any]] = None,
           check: Optional[Callable[[Any], Any]] = None) -> CustomSpec:
    """A custom type over any type, by annotation: a text form and / or an invariant under a schema id.

        Endpoint = Annotated[Union[V4, V6], psio.custom("Endpoint", to_text=show, from_text=parse)]

    The functions work on the value itself.  (A class states the same with
    ``@psio.to_text`` / ``@psio.from_text`` / ``@psio.invariant`` methods.)
    """
    from ._core import Handler
    if to_text is None and from_text is None and check is None:
        return CustomSpec(id, None)
    return CustomSpec(id, Handler(to_text, from_text, check))


@dataclass(frozen=True)
class IntFormSpec(Spec):
    format: str
    form: str


#: The form of the member's integers in strict JSON / in I-JSON.
json_number = IntFormSpec("json", "number")
json_string = IntFormSpec("json", "string")
ijson_number = IntFormSpec("ijson", "number")
ijson_string = IntFormSpec("ijson", "string")


# ── Time ────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class TimeSpec(Spec):
    kind: str  # point | duration | monotonic
    unit: Any
    ticks: Any = field(default=None, compare=True)


def time_point(unit: Any = "micros", ticks: Any = None) -> TimeSpec:
    """A time point of the Unix epoch: ``Annotated[datetime, psio.time_point("micros")]``.

    ``unit``: ``"seconds"``, ``"millis"``, ``"micros"``, ``"nanos"`` or
    ``(num, den)`` seconds per tick; ``ticks``: the tick integer (default
    ``i64``).  On ``int`` instead of ``datetime`` the value is the tick count.
    """
    return TimeSpec("point", unit, ticks)


def duration(unit: Any = "micros", ticks: Any = None) -> TimeSpec:
    """A duration: ``Annotated[timedelta, psio.duration("millis")]`` (or ``int`` ticks)."""
    return TimeSpec("duration", unit, ticks)


def monotonic(unit: Any = "nanos", ticks: Any = None) -> TimeSpec:
    """A time point of some other clock: ``Annotated[int, psio.monotonic("nanos")]`` (ticks)."""
    return TimeSpec("monotonic", unit, ticks)


TimePointSec = Annotated[datetime.datetime, time_point("seconds")]
TimePointMs = Annotated[datetime.datetime, time_point("millis")]
TimePointUs = Annotated[datetime.datetime, time_point("micros")]
#: Nanosecond ticks: an ``int`` (``datetime`` holds microseconds).
TimePointNs = Annotated[int, time_point("nanos")]
DurationSec = Annotated[datetime.timedelta, duration("seconds")]
DurationMs = Annotated[datetime.timedelta, duration("millis")]
DurationUs = Annotated[datetime.timedelta, duration("micros")]
DurationNs = Annotated[int, duration("nanos")]


# ── Class-level declarations ────────────────────────────────────────────────

@dataclass
class RecordOptions:
    name: Optional[str] = None
    max_dynamic_data: Optional[int] = None
    definition_will_not_change: bool = False
    max_fields: Optional[int] = None
    transparent: bool = False
    custom: Optional[str] = None
    invariant: Optional[Callable[[Any], Any]] = None
    to_text: Optional[Callable[[Any], str]] = None
    from_text: Optional[Callable[[str], Any]] = None
    extra: Dict[str, Any] = field(default_factory=dict)


_UNSET: Any = object()

__all__ += ["Bits", "Annotated", "max", "min"]
