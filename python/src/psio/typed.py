"""The typed layer: Python declarations in their natural form.

A ``@dataclass`` (or ``NamedTuple`` / ``TypedDict`` / annotated class), an
``enum.Enum``, a ``NewType``, and the standard generic hints are reflected
once into the Schema IR that C++ ``psio::SchemaBuilder`` produces for the
equivalent C++ type, plus *bindings* that tell the schema-driven codec how to
take a Python object apart and build one.  Every operation then runs through
the one compiled codec: PSSZ, key, JSON, views.

    @dataclass
    class Account:
        id: u64
        owner: Annotated[str, psio.max(32)]
        tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
        balance: u64

    data = psio.to_pssz(Account(7, "alice", ["vip"], 100))
    account = psio.from_pssz(Account, data)
"""

from __future__ import annotations

import dataclasses
import datetime
import enum
import keyword
import operator
import sys
import types
import typing
from typing import (
    Annotated, Any, Callable, Dict, List, NoReturn, Optional, Sequence, Tuple, TypeVar, Union, cast, overload,
)

from . import annotations as A
from . import ir as _ir
from ._core import MAX_RECORD_FIELDS, MAX_TUPLE_ELEMENTS, Binding, Handler, Order, reserved_custom_id
from ._time import TimeKind, schema_id, unit_period
from .errors import DecodeError, EncodeError, PsioError, SchemaError
from .schema import Schema, SchemaType
from .values import Bits, Some, Variant

__all__ = [
    "schema_of", "codec", "record", "enum_type", "transparent", "invariant", "to_text", "from_text",
    "shape_of",
]

_C = TypeVar("_C")
_F = TypeVar("_F")

UTC = datetime.timezone.utc
EPOCH = datetime.datetime(1970, 1, 1, tzinfo=UTC)
NoneType = type(None)


def _fail(message: str) -> NoReturn:
    raise SchemaError(message)


# ── Shapes ──────────────────────────────────────────────────────────────────

class Shape:
    """One declared type: what the builder needs to write its IR, and its Python binding."""

    def __init__(self, k: str, **fields: Any) -> None:
        self.k = k
        self.key: Optional[str] = None
        #: Python identity, part of the interning key (two Python types with one node).
        self.ident = ""
        self.bind: Optional[Binding] = None
        #: The Python class of a value (variant discrimination), or None.
        self.py: Any = None
        self.__dict__.update(fields)

    def __getattr__(self, name: str) -> Any:  # the fields of each kind are set by keyword
        raise AttributeError(name)

    def copy(self, **changes: Any) -> "Shape":
        out = Shape(self.k)
        out.__dict__.update(self.__dict__)
        out.key = None
        out.__dict__.update(changes)
        return out

    def __repr__(self) -> str:
        return f"<Shape {key_of(self)}>"


class Position:
    """The bounds of one position, as written."""

    __slots__ = ("max", "utf8", "element", "keys", "values", "at", "sorted", "unique_keys")

    def __init__(self) -> None:
        self.max: Optional[int] = None
        self.utf8 = False
        #: Order constraints: checked on values, no part of the type (``empty`` does not count them).
        self.sorted: Optional[A.SortedSpec] = None
        self.unique_keys = False
        self.element: Optional[Position] = None
        self.keys: Optional[Position] = None
        self.values: Optional[Position] = None
        self.at: Optional[Dict[str, Position]] = None

    def empty(self) -> bool:
        return self.max is None and not self.utf8 and self.element is None and self.keys is None \
            and self.values is None and self.at is None

    def below(self) -> List["Position"]:
        return [p for p in (self.element, self.keys, self.values) if p is not None] + list((self.at or {}).values())

    def ordered(self) -> bool:
        """Whether an order constraint is written here or below."""
        return self.sorted is not None or self.unique_keys or any(p.ordered() for p in self.below())

    def bounds(self) -> Optional["Position"]:
        """The bounds alone: this position without its order constraints, or None when nothing is left."""
        out = Position()
        out.max, out.utf8 = self.max, self.utf8
        for name in ("element", "keys", "values"):
            child = getattr(self, name)
            if child is not None:
                setattr(out, name, child.bounds())
        if self.at is not None:
            at = {key: child.bounds() for key, child in self.at.items()}
            at = {key: child for key, child in at.items() if child is not None}
            out.at = at or None
        return None if out.empty() else out


class FieldShape:
    __slots__ = ("name", "attr", "type", "length_bound", "position", "json_int", "ijson_int", "order")

    def __init__(self, name: str, attr: str, type: Shape) -> None:
        #: ``psio.sorted`` / ``unique`` / ``unique_keys`` of the member and below it, or None.
        self.order: Optional[Order] = None
        self.name = name
        self.attr = attr
        self.type = type
        self.length_bound: Optional[A.LengthSpec] = None
        self.position: Optional[Position] = None
        self.json_int: Optional[str] = None
        self.ijson_int: Optional[str] = None


class AltShape:
    __slots__ = ("name", "type", "ijson_name")

    def __init__(self, name: str, type: Shape, ijson_name: Optional[str]) -> None:
        self.name = name
        self.type = type
        self.ijson_name = ijson_name


def _json_text(s: str) -> str:
    import json
    return json.dumps(s)


def key_of(shape: Shape, depth: int = 0) -> str:
    """The identity of a type, as C++ type identity: structural for containers and scalars, by name for records."""
    if depth > 256:
        _fail("typed schema: type nesting too deep (use a class for recursion)")
    if shape.key is not None:
        return shape.key
    s = shape
    d = depth + 1
    k = s.k
    if k == "int":
        out = f"{'i' if s.signed else 'u'}{s.bits}"
    elif k == "float":
        out = f"f{s.bits}"
    elif k in ("bool", "char"):
        out = k
    elif k == "bytes":
        out = f"{s.id}:{s.bound}:{s.n}"
    elif k == "json":
        out = "json"
    elif k == "bits":
        out = f"{s.id}:{s.n}"
    elif k == "option":
        out = f"O({key_of(s.inner, d)})"
    elif k == "list":
        out = f"L({key_of(s.inner, d)})"
    elif k == "bounded":
        out = f"B({key_of(s.inner, d)},{s.n})"
    elif k == "array":
        out = f"A({key_of(s.inner, d)},{s.n})"
    elif k == "tuple":
        out = f"T({','.join(key_of(i, d) for i in s.items)})"
    elif k == "assoc":
        out = f"{s.id}({key_of(s.entry, d)}{'' if s.max is None else ',' + str(s.max)})"
    elif k == "nested":
        out = f"N{'D' if s.deferred else ''}({key_of(s.inner, d)})"
    elif k == "transparent":
        out = f"W({_json_text(s.name)})"
    elif k == "annotated":
        out = f"Y({_json_text(s.owner)})"
    elif k == "variant":
        out = ("U(" if s.untagged else "V(") + ",".join(
            f"{_json_text(a.name)}{_json_text(a.ijson_name or '')}{key_of(a.type, d)}" for a in s.alts) + ")"
    elif k == "record":
        out = f"R({_json_text(s.name)})"
    elif k == "args":
        out = f"G({s.uid})"
    elif k == "enum":
        out = f"E({key_of(s.under, d)},{s.members!r})"
    elif k == "time":
        out = f"M({s.id},{key_of(s.tick, d)})"
    elif k == "custom":
        out = f"C({_json_text(s.id)},{key_of(s.inner, d)})"
    elif k == "box":
        out = f"X({key_of(s.inner, d)})"
    else:
        return _fail("typed schema: unresolved definition")
    if s.ident:
        out += "~" + s.ident
    s.key = out
    return out


# ── Class-level declarations ────────────────────────────────────────────────

@overload
def record(cls: _C) -> _C: ...
@overload
def record(cls: _C, *, name: Optional[str] = ..., max_dynamic_data: Optional[int] = ...,
           definition_will_not_change: bool = ..., max_fields: Optional[int] = ...,
           transparent: bool = ..., custom: Optional[str] = ...) -> _C: ...
@overload
def record(*, name: Optional[str] = ..., max_dynamic_data: Optional[int] = ...,
           definition_will_not_change: bool = ..., max_fields: Optional[int] = ...,
           transparent: bool = ..., custom: Optional[str] = ...) -> Callable[[_C], _C]: ...
def record(cls: Any = None, *, name: Optional[str] = None, max_dynamic_data: Optional[int] = None,
           definition_will_not_change: bool = False, max_fields: Optional[int] = None,
           transparent: bool = False, custom: Optional[str] = None) -> Any:
    """Class-level annotations of a record (a dataclass, ``NamedTuple``, ``TypedDict`` or annotated class).

        @psio.record(max_dynamic_data=1024)
        @dataclass
        class Account: ...

    * ``max_dynamic_data``: an explicit cap on the encoded size; fixes the
      offset width for the type's lifetime (C++ ``maxDynamicData(N)``).
    * ``definition_will_not_change``: no header, no evolution.
    * ``transparent``: the class has one field and *is* that field on the wire.
    * ``name``: the type's name in the schema (default: the class name).
    * ``custom``: the id of the type's ``Custom`` schema node, when it has a
      text form or an invariant (default: the name).

    A plain ``@dataclass`` needs none of this.
    """
    def apply(c: Any) -> Any:
        options = _options_of(c, create=True)
        if name is not None:
            options.name = name
        if max_dynamic_data is not None:
            options.max_dynamic_data = max_dynamic_data
        if definition_will_not_change:
            options.definition_will_not_change = True
        if max_fields is not None:
            options.max_fields = max_fields
        if transparent:
            options.transparent = True
        if custom is not None:
            options.custom = custom
        return c
    return apply if cls is None else apply(cls)


def transparent(cls: _C) -> _C:
    """``@psio.transparent``: the class has one field and is that field on the wire (spec 5.10)."""
    return record(cls, transparent=True)


def _options_of(cls: Any, create: bool = False) -> A.RecordOptions:
    options = cls.__dict__.get("__psio_options__")
    if options is None:
        inherited = getattr(cls, "__psio_options__", None)
        options = dataclasses.replace(inherited, name=None) if inherited is not None else A.RecordOptions()
        if create:
            try:
                cls.__psio_options__ = options
            except (AttributeError, TypeError):
                _OPTIONS[cls] = options
        options = _OPTIONS.get(cls, options)
    return options


_OPTIONS: Dict[Any, A.RecordOptions] = {}


class _Marked:
    """A method marked as the type's invariant / text form."""

    def __init__(self, role: str, fn: Any) -> None:
        self.role = role
        self.fn = fn

    def __set_name__(self, owner: Any, attr: str) -> None:
        setattr(owner, attr, self.fn)
        options = _options_of(owner, create=True)
        fn = self.fn
        if self.role == "invariant":
            options.invariant = fn
        elif self.role == "to_text":
            options.to_text = fn
        else:
            options.from_text = fn


def invariant(fn: _F) -> _F:
    """Marks a method as the type's invariant (spec 18).

    It returns ``None`` / ``True`` when the value is a value of the type, and
    a message (or ``False``, or raises) when it is not.  Every writer and
    every checking reader runs it; the trusted decode and views do not.

        @psio.invariant
        def _ordered(self) -> Optional[str]:
            return None if self.lo <= self.hi else "Range: lo is greater than hi"
    """
    return cast(_F, _Marked("invariant", fn))


def to_text(fn: _F) -> _F:
    """Marks a method as the type's text form: one JSON string in both text formats."""
    return cast(_F, _Marked("to_text", fn))


def from_text(fn: _F) -> _F:
    """Marks a ``classmethod`` / ``staticmethod`` as the reader of the type's text form."""
    return cast(_F, _Marked("from_text", fn))


def enum_type(underlying: Any, *, other: bool = False, name: Optional[str] = None,
              invariant: Optional[Callable[[Any], Any]] = None,
              custom: Optional[str] = None) -> Callable[[_C], _C]:
    """``@psio.enum(u8)``: the underlying integer of an ``enum.Enum`` (the wire is that integer).

    ``other``: a wire value that names no member decodes to the plain ``int``
    (and such an ``int`` is written back) instead of being refused.
    """
    def apply(cls: Any) -> Any:
        if not (isinstance(cls, type) and issubclass(cls, enum.Enum)):
            _fail("psio.enum decorates an enum.Enum subclass")
        setattr(cls, "__psio_enum__", (underlying, other, name, invariant, custom))
        return cls
    return apply


# ── From type hints to shapes ───────────────────────────────────────────────

_INT = {(b, s): Shape("int", bits=b, signed=s) for b in (8, 16, 32, 64, 128) for s in (False, True)}
_INT[(256, False)] = Shape("int", bits=256, signed=False)
_F32 = Shape("float", bits=32)
_F64 = Shape("float", bits=64)
_BOOL = Shape("bool")
_CHAR = Shape("char")
_U8 = _INT[(8, False)]
_UTF8 = Shape("bytes", id="utf8", bound="list", n=0)
_RAW = Shape("bytes", id="string", bound="list", n=0)
_BYTES = Shape("bytes", id="hex", bound="list", n=0)
for _s in (_BOOL,):
    _s.py = bool
for _s in _INT.values():
    _s.py = int
_CHAR.py = int
_F32.py = _F64.py = float
_UTF8.py = str
_RAW.py = _BYTES.py = bytes
#: Any JSON value (psio.JsonValue): the Custom id "json" over a UTF-8 string.
_JSON = Shape("json")

_CLASS_SHAPES: Dict[Any, Shape] = {}
_NEWTYPES: Dict[int, Shape] = {}
_KEEP: List[Any] = []


def _split(hint: Any) -> Tuple[Any, Tuple[Any, ...]]:
    """A hint as its base type and its ``Annotated`` metadata (psio specs only)."""
    if typing.get_origin(hint) is Annotated:
        return hint.__origin__, tuple(m for m in hint.__metadata__ if isinstance(m, A.Spec))
    return hint, ()


def _position(specs: Sequence[Any], where: str) -> Tuple[Position, List[Any]]:
    """The bounds among ``specs`` as a Position, and the specs that are not bounds."""
    p = Position()
    rest: List[Any] = []
    for s in specs:
        if isinstance(s, A.LengthSpec):
            if s.min is not None or s.exact is not None:
                _fail(f"psio: {where}: below a member a length bound is max")
            p.max = s.max
        elif isinstance(s, A.Utf8Spec):
            p.utf8 = True
        elif isinstance(s, A.SortedSpec):
            p.sorted = s
        elif isinstance(s, A.UniqueKeysSpec):
            p.unique_keys = True
        elif isinstance(s, A.PositionSpec):
            child, other = _position(s.specs, where)
            if other:
                _fail(f"psio: {where}: {type(other[0]).__name__} is not a bound of a position")
            if s.where == "at":
                if p.at is None:
                    p.at = {}
                p.at[str(s.index)] = child
            else:
                setattr(p, s.where, child)
        else:
            rest.append(s)
    return p, rest


_WRAPPERS = (A.BoxSpec, A.CustomSpec, A.IdentitySpec)
_IGNORED = (A.NameSpec, A.AltSpec, A.IntFormSpec)


def _is_wrapper(m: Any) -> bool:
    return isinstance(m, _WRAPPERS) or (isinstance(m, A.NestedSpec) and m.inner is None)


def shape_of(hint: Any) -> Shape:
    """The shape of a type hint, where the hint's own bounds are those of a wrapper type.

    ``Annotated`` metadata is read in order, innermost first (Python flattens
    nested ``Annotated``): what qualifies the base type (a width, ``array``,
    a time unit), then bounds and the wrappers ``nested`` / ``boxed`` /
    ``custom`` / ``identity`` as they come.
    """
    base, meta = _split(hint)
    modifiers = [m for m in meta if not _is_wrapper(m) and not isinstance(
        m, _IGNORED + (A.LengthSpec, A.BoundedSpec, A.PositionSpec, A.Utf8Spec, A.SortedSpec, A.UniqueKeysSpec))]
    shape = _core(base, modifiers, hint)
    pending: List[Any] = []

    def flush(shape: Shape) -> Shape:
        if not pending:
            return shape
        position, _ = _position([A.LengthSpec(max=m.max) if isinstance(m, A.BoundedSpec) else m for m in pending],
                                repr(hint))
        pending.clear()
        if position.ordered():
            # (spec 5.10: an order constraint is not something a type can say)
            _fail(f"psio: {hint!r}: sorted / unique / unique_keys belong to a record member or a method "
                  "parameter (or a position below one), not to a type")
        return shape if position.empty() else _own(shape, position, repr(hint))

    for m in meta:
        if isinstance(m, _IGNORED) or m in modifiers:
            continue
        if not _is_wrapper(m):
            pending.append(m)
            continue
        shape = flush(shape)
        if isinstance(m, A.NestedSpec):
            # The member is the inner value; on the wire, its encoding as a byte list.
            if m.deferred:
                _fail("psio: a deferred nested value is its bytes: Annotated[bytes, psio.nested(T, deferred=True)]")
            shape = Shape("nested", inner=shape, deferred=False, py=shape.py)
        elif isinstance(m, A.BoxSpec):
            shape = Shape("box", inner=shape, py=shape.py)
        elif isinstance(m, A.CustomSpec):
            if reserved_custom_id(m.id):
                _fail(f'psio: custom id "{m.id}" is reserved')
            shape = Shape("custom", id=m.id, inner=shape, py=shape.py, handler=m.handler,
                          ident=f"{id(m.handler):x}" if m.handler is not None else "")
            _KEEP.append(m)
        else:
            shape = shape.copy(ident=(shape.ident + "/" if shape.ident else "") + m.label)
    return flush(shape)


def _find(markers: Sequence[Any], cls: Any) -> Any:
    for m in markers:
        if isinstance(m, cls):
            return m
    return None


def _tick_binding(py: Any, kind: str, num: int, den: int) -> Optional[Binding]:
    """How ``datetime`` / ``timedelta`` map onto exact tick counts."""
    if py is int:
        return None
    # ticks = microseconds * den / (num * 10^6), exactly or not at all.
    scale_n, scale_d = den, num * 1000000
    if py is datetime.datetime:
        if kind != "point":
            _fail("psio: datetime is a time point; a duration is a timedelta")

        def pre_point(value: Any) -> int:
            if type(value) is not datetime.datetime and not isinstance(value, datetime.datetime):
                raise EncodeError("time: datetime value required")
            if value.tzinfo is None:
                value = value.replace(tzinfo=UTC)  # a naive datetime is UTC
            delta = value - EPOCH
            micros = (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
            ticks, rem = divmod(micros * scale_n, scale_d)
            if rem:
                raise EncodeError("time: the unit cannot represent this datetime exactly")
            return ticks

        def post_point(ticks: int) -> Any:
            micros, rem = divmod(ticks * scale_d, scale_n)
            if rem:
                raise DecodeError("time: time point is not a whole number of microseconds (declare int ticks)")
            try:
                return EPOCH + datetime.timedelta(microseconds=micros)
            except OverflowError:
                raise DecodeError("time: time point outside the range of datetime (declare int ticks)") from None
        return Binding(pre=pre_point, post=post_point)
    if py is datetime.timedelta:
        if kind != "duration":
            _fail("psio: timedelta is a duration; a time point is a datetime")

        def pre_duration(value: Any) -> int:
            if not isinstance(value, datetime.timedelta):
                raise EncodeError("time: timedelta value required")
            micros = (value.days * 86400 + value.seconds) * 1000000 + value.microseconds
            ticks, rem = divmod(micros * scale_n, scale_d)
            if rem:
                raise EncodeError("time: the unit cannot represent this timedelta exactly")
            return ticks

        def post_duration(ticks: int) -> Any:
            micros, rem = divmod(ticks * scale_d, scale_n)
            if rem:
                raise DecodeError("time: duration is not a whole number of microseconds (declare int ticks)")
            try:
                return datetime.timedelta(microseconds=micros)
            except OverflowError:
                raise DecodeError("time: duration outside the range of timedelta (declare int ticks)") from None
        return Binding(pre=pre_duration, post=post_duration)
    return _fail("psio: a time value is a datetime, a timedelta or int ticks")


def _core(base: Any, markers: Sequence[Any], hint: Any) -> Shape:
    time = _find(markers, A.TimeSpec)
    if time is not None:
        num, den = unit_period(time.unit)
        tick = shape_of(time.ticks) if time.ticks is not None else _INT[(64, True)]
        if tick.k != "int" or tick.bits not in (8, 16, 32, 64):
            _fail("psio: the ticks of a time value are an 8/16/32/64-bit integer")
        kind = TimeKind(time.kind == "point", time.kind == "monotonic", num, den)
        out = Shape("time", id=schema_id(kind), tick=tick, py=base)
        out.bind = _tick_binding(base, time.kind, num, den)
        if out.bind is not None:
            out.ident = base.__name__
        return out
    if _find(markers, A.JsonValueSpec) is not None:
        return _JSON
    if base is int:
        spec = _find(markers, A.IntSpec)
        if spec is not None:
            shape = _INT.get((spec.bits, spec.signed))
            return shape if shape is not None else _fail(f"psio: no {spec.bits}-bit integer")
        if _find(markers, A.CharSpec) is not None:
            return _CHAR
        return _fail("psio: int needs a width: use psio.u8 ... psio.u64, psio.i8 ... psio.i64, psio.u128, "
                     "psio.i128 or psio.u256 (they are ints to a type checker)")
    if base is bool:
        return _BOOL
    if base is float:
        spec = _find(markers, A.FloatSpec)
        return _F32 if spec is not None and spec.bits == 32 else _F64
    if base is str:
        return _UTF8
    if base in (bytes, bytearray):
        nested = _find(markers, A.NestedSpec)
        if nested is not None:
            if not nested.deferred:
                _fail("psio: a nested member is its value: Annotated[T, psio.nested()]; as bytes it is "
                      "Annotated[bytes, psio.nested(T, deferred=True)]")
            return Shape("nested", inner=shape_of(nested.inner), deferred=True, py=bytes)
        arr = _find(markers, A.ArraySpec)
        raw = _find(markers, A.RawStringSpec) is not None
        if arr is not None:
            if raw:
                _fail("psio: a fixed array of bytes is Annotated[bytes, psio.array(N)]")
            return Shape("bytes", id="hex", bound="exact", n=arr.count, py=bytes)
        return _RAW if raw else _BYTES
    if base is Bits:
        bits = _find(markers, A.BitsSpec)
        if bits is None:
            _fail("psio: Bits needs psio.bitvector(N) or psio.bitlist(N)")
        return Shape("bits", id=bits.id, n=bits.count, py=Bits)
    if base is datetime.datetime or base is datetime.timedelta:
        return _fail(f"psio: {base.__name__} needs its unit: Annotated[{base.__name__}, "
                     f"psio.{'time_point' if base is datetime.datetime else 'duration'}(\"micros\")]")
    origin = typing.get_origin(base)
    args = typing.get_args(base)
    if origin is Annotated:
        return shape_of(base)
    if origin in (list, typing.List, Sequence) or base is list:
        if len(args) != 1:
            _fail("psio: list needs its element type: list[T]")
        bits = _find(markers, A.BitsSpec)
        if bits is not None:
            if args[0] is not bool:
                _fail("psio: packed bits are psio.Bits or list[bool]")
            out = Shape("bits", id=bits.id, n=bits.count, py=list, ident="bools")
            out.bind = Binding(pre=Bits.from_bools, post=Bits.to_bools)
            return out
        inner = shape_of(args[0])
        arr = _find(markers, A.ArraySpec)
        if arr is not None:
            return Shape("array", inner=inner, n=arr.count, py=list)
        return Shape("list", inner=inner, py=list)
    if origin is tuple or base is tuple:
        if len(args) == 2 and args[1] is Ellipsis:
            # A homogeneous tuple of any length: a list on the wire.
            out = Shape("list", inner=shape_of(args[0]), py=tuple, ident="tuple")
            out.bind = Binding(post=tuple)
            return out
        if not args and origin is None:
            _fail("psio: tuple needs its element types: tuple[A, B]")
        items = [] if args in ((), ((),)) else [shape_of(a) for a in args]
        if len(items) > MAX_TUPLE_ELEMENTS:
            _fail("psio: a tuple has more than 255 elements")
        return Shape("tuple", items=items, py=tuple)
    if origin in (dict, typing.Dict) or base is dict or (isinstance(origin, type) and issubclass(origin, typing.Mapping)):
        if len(args) != 2:
            _fail("psio: dict needs its key and value types: dict[K, V]")
        make = origin if isinstance(origin, type) and issubclass(origin, dict) else dict
        entry = Shape("tuple", items=[shape_of(args[0]), shape_of(args[1])], py=tuple)
        out = Shape("assoc", id="map", entry=entry, max=None, py=make)
        out.bind = Binding(assoc=make)
        if make is not dict:
            out.ident = make.__name__
        return out
    if origin in (set, frozenset, typing.Set, typing.FrozenSet) or base in (set, frozenset):
        if len(args) != 1:
            _fail("psio: set needs its element type: set[T]")
        make = frozenset if origin in (frozenset, typing.FrozenSet) else set
        out = Shape("assoc", id="set", entry=shape_of(args[0]), max=None, py=make)
        out.bind = Binding(assoc=make)
        if make is frozenset:
            out.ident = "frozenset"
        return out
    if origin is Union or (hasattr(types, "UnionType") and origin is types.UnionType):
        members = [a for a in args if a is not NoneType]
        untagged = _find(markers, A.UntaggedSpec) is not None
        if len(members) != len(args):
            if untagged and len(members) == 1:
                _fail("psio: untagged marks a union of two or more alternatives")
            inner = shape_of(members[0]) if len(members) == 1 else _variant(members, untagged)
            return Shape("option", inner=inner, py=inner.py)
        return _variant(members, untagged)
    if origin is Some:
        inner = shape_of(args[0])
        if inner.k != "option":
            _fail("psio: Some[T] is the payload of an optional of an optional: Optional[Some[Optional[T]]]")
        return inner.copy(some=True)
    if isinstance(base, type):
        if issubclass(base, enum.Enum):
            return _enum_shape(base)
        if dataclasses.is_dataclass(base) or typing.is_typeddict(base) or hasattr(base, "__psio_options__") \
                or (issubclass(base, tuple) and hasattr(base, "_fields")) or base in _OPTIONS:
            return _class_shape(base)
    if hasattr(base, "__supertype__") and callable(base):
        # typing.NewType: a transparent type, the same value at run time.
        known = _NEWTYPES.get(id(base))
        if known is None:
            inner = shape_of(base.__supertype__)
            known = Shape("transparent", name=base.__name__, inner=inner, py=inner.py, cls=base)
            _NEWTYPES[id(base)] = known
            _KEEP.append(base)
        return known
    if isinstance(base, (str, typing.ForwardRef)):
        return _fail(f"psio: unresolved forward reference {base!r}")
    return _fail(f"psio: no wire type for {hint!r}")


def _variant(members: Sequence[Any], untagged: bool = False) -> Shape:
    """A union of two or more alternatives: a variant."""
    alts: List[AltShape] = []
    for index, m in enumerate(members):
        base, meta = _split(m)
        spec = _find(meta, A.AltSpec)
        shape = shape_of(m)
        name = spec.name if spec is not None and spec.name is not None else _default_alt_name(base, shape)
        alts.append(AltShape(name, shape, spec.ijson_name if spec is not None else None))
    if len({a.name for a in alts}) != len(alts):
        _fail("psio: the alternatives of a union need distinct names: Annotated[T, psio.alt(\"name\")]")
    out = Shape("variant", alts=alts, untagged=untagged)
    out.bind = _variant_binding(alts)
    return out


def _default_alt_name(base: Any, shape: Shape) -> str:
    if shape.k in ("record", "transparent"):
        return shape.name
    if shape.k == "custom":
        inner = shape.inner
        return inner.name if inner.k in ("record", "transparent") else shape.id
    if isinstance(base, type) and issubclass(base, enum.Enum):
        return base.__name__
    if shape.k == "int":
        return f"{'i' if shape.signed else 'u'}{shape.bits}"
    if shape.k == "float":
        return f"f{shape.bits}"
    if shape.k == "bytes" and shape.bound == "list":
        return {"utf8": "string", "string": "raw_string", "hex": "bytes"}[shape.id]
    if shape.k == "bool":
        return "bool"
    return _fail(f"psio: name this alternative of the union: Annotated[{base!r}, psio.alt(\"name\")]")


def _runtime_class(shape: Shape) -> Any:
    """The class ``type(value)`` gives for a value of the shape, when it has one."""
    s = shape
    while s.k in ("custom", "box") or (s.k == "transparent" and getattr(s, "wrapper", None) is None
                                        and s.bind is None):
        s = s.inner
    return s.py


def _variant_binding(alts: Sequence[AltShape]) -> Binding:
    """Alternatives are told apart by their Python class; those that share one are ``Variant(name, value)``."""
    classes = [_runtime_class(a.type) for a in alts]
    counts: Dict[Any, int] = {}
    for c in classes:
        counts[c] = counts.get(c, 0) + 1
    by_class = {c: i for i, c in enumerate(classes) if c is not None and counts[c] == 1}
    by_name = {a.name: i for i, a in enumerate(alts)}
    names = [a.name for a in alts]
    tagged = [classes[i] not in by_class for i in range(len(alts))]

    def split(value: Any) -> Tuple[int, Any]:
        t = type(value)
        index = by_class.get(t)
        if index is not None:
            return index, value
        if t is Variant:
            try:
                return by_name[value.type], value.value
            except KeyError:
                raise EncodeError(f'pssz: unknown variant alternative "{value.type}"') from None
        for c, i in by_class.items():
            if isinstance(value, c) and not (c is int and t is bool):
                return i, value
        raise EncodeError(f"pssz: {t.__name__} is not an alternative of this union"
                          + (" (alternatives that share a Python class are psio.Variant(name, value))"
                             if any(tagged) else ""))

    def join(index: int, payload: Any) -> Any:
        # (a value that is not of the alternative's own class - an undeclared
        # enum value, None of an optional alternative - is tagged too)
        if tagged[index] or type(payload) is not classes[index]:
            return Variant(names[index], payload)
        return payload
    return Binding(split=split, join=join)


def _enum_shape(cls: Any) -> Shape:
    known = _CLASS_SHAPES.get(cls)
    if known is not None:
        return known
    declared = getattr(cls, "__psio_enum__", None)
    if declared is None:
        _fail(f"psio: enum {cls.__name__} needs its underlying integer: @psio.enum(psio.u8)")
    underlying, other, name, check, custom_id = declared
    under = shape_of(underlying)
    if under.k != "int":
        _fail(f"psio: the underlying type of enum {cls.__name__} is an integer")
    members = []
    by_value: Dict[int, Any] = {}
    for member_name, member in cls.__members__.items():
        value = member.value
        if type(value) is not int and not isinstance(value, int):
            _fail(f"psio: enum {cls.__name__}.{member_name} is not an integer")
        value = int(value)
        lo, hi = (-(1 << (under.bits - 1)), (1 << (under.bits - 1)) - 1) if under.signed else (0, (1 << under.bits) - 1)
        if not lo <= value <= hi:
            _fail(f"psio: enum {cls.__name__}.{member_name} does not fit its underlying integer")
        members.append((member_name, value))
        by_value.setdefault(value, member)
    out = Shape("enum", under=under, members=tuple(members), py=cls, ident=f"{cls.__module__}.{cls.__qualname__}")
    get = by_value.get

    def pre(value: Any) -> int:
        if type(value) is cls:
            return value._value_
        if other and type(value) is int:
            if value in by_value:
                raise EncodeError(f"pssz: {value} is a declared enumerator of {cls.__name__}: write the member")
            return value
        if isinstance(value, cls):
            return value._value_
        raise EncodeError(f"pssz: {cls.__name__} value required")

    def post(value: int) -> Any:
        member = get(value)
        if member is None:
            if other:
                return value
            raise DecodeError(f"pssz: enum value is not a declared enumerator of {cls.__name__}")
        return member
    out.bind = Binding(pre=pre, post=post)
    if check is not None or custom_id is not None:
        id = custom_id or name or cls.__name__
        out = Shape("custom", id=id, inner=out, py=cls,
                    handler=Handler(check=check) if check is not None else None)
    _CLASS_SHAPES[cls] = out
    return out


def _wire_name(attr: str) -> str:
    if attr.endswith("_") and keyword.iskeyword(attr[:-1]):
        return attr[:-1]
    return attr


def _hints_of(cls: Any) -> Dict[str, Any]:
    try:
        return typing.get_type_hints(cls, include_extras=True)
    except NameError as e:
        return _fail(f"psio: {cls.__name__}: {e} (declare it at module level, or pass it to psio.schema_of first)")
    except Exception as e:
        return _fail(f"psio: {cls.__name__}: cannot read its type hints: {type(e).__name__}: {e}")


def _class_shape(cls: Any) -> Shape:
    """A class with fields: a record, a transparent type, or a custom type over one."""
    known = _CLASS_SHAPES.get(cls)
    if known is not None:
        return known
    options = _options_of(cls)
    name = options.name or cls.__name__
    is_tuple = isinstance(cls, type) and issubclass(cls, tuple) and hasattr(cls, "_fields")
    is_dict = typing.is_typeddict(cls)
    if dataclasses.is_dataclass(cls):
        attrs = [f.name for f in dataclasses.fields(cls)]
        extra = {f.name: f.metadata.get("psio", ()) for f in dataclasses.fields(cls)}
    elif is_tuple:
        attrs = list(cls._fields)
        extra = {}
    else:
        attrs = [a for a in _hints_of(cls) if not a.startswith("__")
                 and typing.get_origin(_hints_of(cls)[a]) is not typing.ClassVar]
        extra = {}
    record_shape = Shape("record", name=name, options=options, py=cls, cls=cls, attrs=attrs, extra=extra,
                         fields=None, is_dict=is_dict, is_tuple=is_tuple)
    shape = record_shape
    if options.transparent:
        if len(attrs) != 1:
            _fail(f"psio: transparent type {name} must have exactly one field")
        # (its inner shape is resolved lazily: the member may refer back to the class)
        shape = Shape("transparent", name=name, inner=None, py=cls, cls=cls, wrapper=cls, attr=attrs[0])
        getter = operator.attrgetter(attrs[0])
        shape.bind = Binding(pre=_checked_getter(cls, getter), post=_maker(cls, attrs, is_dict, is_tuple, True))
    else:
        get: Any
        if is_dict:
            get = None
        elif is_tuple:
            get = _tuple_getter(cls)
        else:
            get = _attr_getter(cls, attrs)
        record_shape.bind = Binding(get=get, make=None if is_dict else _maker(cls, attrs, False, is_tuple, False),
                                    cls=cls)
    if options.invariant is not None or options.to_text is not None or options.from_text is not None \
            or options.custom is not None:
        id = options.custom or name
        if reserved_custom_id(id):
            _fail(f'psio: custom id "{id}" is reserved')
        outer = Shape("custom", id=id, inner=shape, py=cls, cls=cls)
        setattr(outer, "handler", _class_handler(cls, options, shape))
        if shape.k == "transparent":
            # The Custom node wraps the member's type: the class binds there.
            outer.bind, shape.bind = shape.bind, None
        shape = outer
    _CLASS_SHAPES[cls] = shape
    return shape


def _checked_getter(cls: Any, getter: Any) -> Any:
    def pre(value: Any) -> Any:
        if not isinstance(value, cls):
            raise EncodeError(f"pssz: {cls.__name__} value required, not {type(value).__name__}")
        return getter(value)
    return pre


def _attr_getter(cls: Any, attrs: Sequence[str]) -> Any:
    if not attrs:
        return lambda value: ()
    getter = operator.attrgetter(*attrs)
    if len(attrs) == 1:
        def get_one(value: Any) -> Any:
            try:
                return (getter(value),)
            except AttributeError:
                raise EncodeError(f"pssz: {cls.__name__} value required, not {type(value).__name__}") from None
        return get_one

    def get(value: Any) -> Any:
        try:
            return getter(value)
        except AttributeError:
            raise EncodeError(f"pssz: {cls.__name__} value required, not {type(value).__name__}") from None
    return get


def _tuple_getter(cls: Any) -> Any:
    count = len(cls._fields)

    def get(value: Any) -> Any:
        if not isinstance(value, tuple) or len(value) != count:
            raise EncodeError(f"pssz: {cls.__name__} value required")
        return value
    return get


def _maker(cls: Any, attrs: Sequence[str], is_dict: bool, is_tuple: bool, single: bool) -> Any:
    """Builds an instance from the member values in declaration order."""
    if is_tuple:
        return cls._make if not single else (lambda v: cls(v))
    positional = False
    if dataclasses.is_dataclass(cls):
        fields = dataclasses.fields(cls)
        positional = all(f.init and not getattr(f, "kw_only", False) for f in fields)
        if not positional and all(f.init for f in fields):
            names = list(attrs)
            if single:
                return lambda v: cls(**{names[0]: v})
            return lambda values: cls(**dict(zip(names, values)))
    if positional:
        return cls if single else (lambda values: cls(*values))
    names = list(attrs)

    def make(values: Any) -> Any:
        obj = cls.__new__(cls)
        for n, v in zip(names, (values,) if single else values):
            object.__setattr__(obj, n, v)
        return obj
    return make


def _class_handler(cls: Any, options: A.RecordOptions, wire: Shape) -> Optional[Handler]:
    """The handler of a custom class: its own methods, on the value the wire type decodes to."""
    check, text, parse = options.invariant, options.to_text, options.from_text
    if isinstance(parse, (classmethod, staticmethod)):
        parse = parse.__get__(None, cls)
    if check is None and text is None and parse is None:
        return None
    if wire.k == "transparent":
        # The handler sees the member's value; the methods want the instance.
        post = wire.bind.post
        pre = wire.bind.pre
        return Handler(
            to_text=(lambda v: text(post(v))) if text is not None else None,
            from_text=(lambda s: pre(parse(s))) if parse is not None else None,
            check=(lambda v: check(post(v))) if check is not None else None)
    return Handler(to_text=text, from_text=parse, check=check)


def _fields_of(shape: Shape) -> List[FieldShape]:
    """The members of a record shape (resolved on first use: a class may refer to itself)."""
    if shape.fields is not None:
        return shape.fields
    cls = shape.cls
    hints = _hints_of(cls)
    out: List[FieldShape] = []
    for attr in shape.attrs:
        if attr not in hints:
            _fail(f"psio: {cls.__name__}.{attr} has no type hint")
        out.append(_field(attr, hints[attr], shape.extra.get(attr, ()), f"{cls.__name__}.{attr}"))
    if len({f.name for f in out}) != len(out):
        _fail(f"psio: {cls.__name__}: two members have the same name")
    _check_field_count(f"record {shape.name}", "fields", len(out), shape.options.max_fields)
    setattr(shape, "fields", out)
    if shape.bind is not None:
        shape.bind.order = _orders(out)
    return out


def _check_field_count(what: str, noun: str, count: int, max_fields: Optional[int]) -> None:
    """``max_fields`` and the 255-field limit (spec 8.4, 16): the declaration is where they are enforced."""
    if count > MAX_RECORD_FIELDS:
        _fail(f"psio: {what} has more than 255 {noun}")
    if max_fields is None:
        return
    if type(max_fields) is not int or not 0 <= max_fields <= MAX_RECORD_FIELDS:
        _fail(f"psio: {what}: max_fields is at most 255, the most {noun} it can have")
    if count > max_fields:
        _fail(f"psio: {what} has more {noun} than its max_fields ({count} > {max_fields})")


def _field(attr: str, hint: Any, extra: Sequence[Any], where: str) -> FieldShape:
    """A record member / method parameter: its plain type, and what is annotated on it."""
    base, meta = _split(hint)
    meta = tuple(meta) + tuple(m for m in extra if isinstance(m, A.Spec))
    name = _wire_name(attr)
    member_specs: List[Any] = []
    type_specs: List[Any] = []
    for m in meta:
        if isinstance(m, A.NameSpec):
            name = m.name
        elif isinstance(m, (A.LengthSpec, A.Utf8Spec, A.PositionSpec, A.IntFormSpec, A.SortedSpec,
                            A.UniqueKeysSpec)):
            member_specs.append(m)
        else:
            type_specs.append(m)
    plain = Annotated[tuple([base] + type_specs)] if type_specs else base
    field = FieldShape(name, attr, shape_of(plain))
    bound: Dict[str, int] = {}
    rest = []
    for m in member_specs:
        if isinstance(m, A.LengthSpec):
            for key in ("max", "min", "exact"):
                v = getattr(m, key)
                if v is not None:
                    if key in bound:
                        _fail(f"psio: {where}: {key} is stated twice")
                    bound[key] = v
        elif isinstance(m, A.IntFormSpec):
            if m.format == "json":
                field.json_int = m.form
            else:
                field.ijson_int = m.form
        else:
            rest.append(m)
    if bound:
        field.length_bound = A.LengthSpec(attribute=any(isinstance(m, A.LengthSpec) and m.attribute
                                                        for m in member_specs), **bound)
    position, _ = _position(rest, where)
    if position.ordered():
        field.order = _order_of(field.type, position, where)
        position = position.bounds() or Position()
    if not position.empty():
        field.position = position
    return field


def _order_of(shape: Shape, a: Position, where: str) -> Optional[Order]:
    """The order constraints of a position and below it, checked against the type they are written on."""
    s = shape
    while s.k == "option":
        s = s.inner
    k = s.k
    out = Order()
    if a.sorted is not None or a.unique_keys:
        if k not in ("list", "bounded", "array"):
            _fail(f"psio: {where}: sorted / unique / unique_keys constrain the elements of a list or an array; "
                  "this position is neither (a map or a set is ordered by its type)")
        if a.sorted is not None:
            out.sorted = (a.sorted.unique, a.sorted.ascending)
        out.unique_keys = a.unique_keys
    if a.element is not None and a.element.ordered():
        if k in ("list", "bounded", "array"):
            out.element = _order_of(s.inner, a.element, f"{where}, element")
        elif k == "assoc" and s.id == "set":
            out.element = _order_of(s.entry, a.element, f"{where}, element")
        else:
            _fail(f"psio: {where}: element(...) is for the elements of a list, an array or a set")
    below = [p for p in (a.keys, a.values) if p is not None and p.ordered()]
    if below and not (k == "assoc" and s.id == "map"):
        _fail(f"psio: {where}: keys(...) / values(...) are for a map")
    if k == "assoc" and s.id == "map":
        key, value = s.entry.items
        if a.keys is not None and a.keys.ordered():
            out.keys = _order_of(key, a.keys, f"{where}, keys")
        if a.values is not None and a.values.ordered():
            out.values = _order_of(value, a.values, f"{where}, values")
    for name, child in (a.at or {}).items():
        if not child.ordered():
            continue
        if k == "tuple" and name.isdigit() and int(name) < len(s.items):
            index, below = int(name), s.items[int(name)]
        elif k == "variant":
            index = _alt_index(s.alts, name, where)
            below = s.alts[index].type
        else:
            _fail(f"psio: {where}: at({name}, ...) is for an element of a tuple or an alternative of a union")
        if out.at is None:
            out.at = {}
        out.at[index] = _order_of(below, child, f"{where}, at {name}")
    return out


def _orders(fields: Sequence[FieldShape]) -> Optional[List[Tuple[int, Order]]]:
    out = [(i, f.order) for i, f in enumerate(fields) if f.order is not None]
    return out or None


def _inner_of(shape: Shape) -> Shape:
    """The member's shape of a transparent class."""
    if shape.inner is None:
        cls = shape.wrapper
        hint = _hints_of(cls)[shape.attr]
        field = dataclasses.fields(cls) if dataclasses.is_dataclass(cls) else ()
        extra = tuple(m for f in field for m in f.metadata.get("psio", ()))
        if extra:
            hint = Annotated[tuple([hint] + list(extra))]
        where = f"{shape.name}.{shape.attr}"
        member = _field(shape.attr, hint, (), where)
        bound = member.length_bound
        if member.json_int or member.ijson_int or member.order is not None or (
                bound is not None and (bound.min is not None or bound.exact is not None or bound.attribute)):
            # (spec 5.10: what a type cannot say is the type's invariant)
            _fail(f"psio: the member of transparent type {shape.name} takes utf8, max and position bounds; "
                  "exact / min, sorted / unique and an integer text form are not a type's (state an invariant)")
        if member.type.k == "option":
            _fail(f"psio: the member of transparent type {shape.name} is not an Optional "
                  "(hold an Optional of the wrapper)")
        position = member.position
        if bound is not None:
            if position is None:
                position = Position()
            position.max = bound.max
        if position is None:
            setattr(shape, "inner", member.type)
        else:
            # Bounds on the member (spec 5.10): the wrapper is that bounded
            # type, a type of its own that the alias refers to
            # (schema_builder.hpp `transparent_member_of`).
            target = member.type
            if position.max is not None and not (
                    (target.k == "bytes" and target.bound == "list") or target.k == "list"
                    or (target.k == "assoc" and target.max is None)):
                _fail(f"psio: {where}: max bounds a string, bytes, a list, a map or a set that has no bound "
                      "of its own")
            _check_position(target, position, True, f'member "{shape.attr}" of transparent type {shape.name}')
            setattr(shape, "inner", Shape("annotated", inner=target, position=position, owner=shape.name,
                                          py=target.py))
    return shape.inner


def _own(shape: Shape, a: Position, where: str) -> Shape:
    """``shape`` under bounds that are its own (a wrapper type): the bounded form at every depth."""
    s = shape
    k = s.k
    if k == "option":
        return s.copy(inner=_own(s.inner, a, where))
    if k == "bytes":
        if a.element or a.keys or a.values or a.at:
            _fail(f"psio: {where}: a string has no positions below it")
        if a.max is None:
            return s
        if s.bound != "list":
            _fail(f"psio: {where}: the type already has a length")
        return s.copy(bound="max", n=a.max)
    if a.utf8:
        _fail(f"psio: {where}: utf8 applies to a raw string")
    if k == "list":
        if a.keys or a.values or a.at:
            _fail(f"psio: {where}: a list takes element(...)")
        inner = _own(s.inner, a.element, where) if a.element else s.inner
        if a.max is None:
            return s.copy(inner=inner)
        return s.copy(k="bounded", inner=inner, n=a.max)
    if k in ("bounded", "array"):
        if a.max is not None or a.keys or a.values or a.at:
            _fail(f"psio: {where}: max bounds a list, not an array")
        return s.copy(inner=_own(s.inner, a.element, where)) if a.element else s
    if k == "assoc":
        if a.at or (s.id == "set" and (a.keys or a.values)) or (s.id == "map" and a.element):
            _fail(f"psio: {where}: a map takes keys(...) / values(...), a set element(...)")
        entry = s.entry
        if s.id == "set":
            if a.element:
                entry = _own(entry, a.element, where)
        elif a.keys or a.values:
            key, value = entry.items
            entry = entry.copy(items=[_own(key, a.keys, where) if a.keys else key,
                                      _own(value, a.values, where) if a.values else value])
        return s.copy(entry=entry, max=a.max if a.max is not None else s.max)
    if a.max is not None:
        _fail(f"psio: {where}: max bounds a string, bytes, a list, a map or a set; this is none of those")
    if k == "tuple":
        if a.element or a.keys or a.values:
            _fail(f"psio: {where}: a tuple takes at(index, ...)")
        items = list(s.items)
        for key, child in (a.at or {}).items():
            if not key.isdigit() or int(key) >= len(items):
                _fail(f"psio: {where}: at({key}): not an element of this tuple")
            items[int(key)] = _own(items[int(key)], child, where)
        return s.copy(items=items)
    if k == "variant":
        alts = list(s.alts)
        for key, child in (a.at or {}).items():
            index = _alt_index(alts, key, where)
            alts[index] = AltShape(alts[index].name, _own(alts[index].type, child, where), alts[index].ijson_name)
        return s.copy(alts=alts)
    return _fail(f"psio: {where}: this type takes no bounds")


def _alt_index(alts: Sequence[AltShape], key: str, where: str) -> int:
    for i, a in enumerate(alts):
        if a.name == key:
            return i
    if key.isdigit() and int(key) < len(alts):
        return int(key)
    return _fail(f"psio: {where}: at({key}): not an alternative of this union")


# ── Schema IR generation (cpp/include/psio/schema_builder.hpp) ──────────────

def _node(kind: str, body: Any) -> Dict[str, Any]:
    return {"value": Variant(kind, body)}


def _ref(name: str) -> Dict[str, Any]:
    return {"value": Variant("Type", {"type": name})}


def _same_ref(a: Dict[str, Any], b: Dict[str, Any]) -> bool:
    return a["value"].type == "Type" and b["value"].type == "Type" and a["value"].value == b["value"].value


def _int_ir(bits: int, signed: bool) -> Dict[str, Any]:
    return _node("Int", {"bits": bits, "isSigned": signed})


def _attribute(name: str, value: str) -> Dict[str, Any]:
    return {"name": name, "value": value}


class Builder:
    """A port of ``psio::SchemaBuilder``.

    A record under its own name, every other type interned as ``@N`` in
    first-visit order with its slot taken before its children; a ``max``-only
    member length bound written into the member's type.
    """

    def __init__(self) -> None:
        self.types: List[Dict[str, Any]] = []
        self.handlers: Dict[str, Handler] = {}
        self.deferred: set = set()
        self.bindings: Dict[Any, Binding] = {}
        self._names: Dict[str, str] = {}
        self._slots: Dict[str, Dict[str, Any]] = {}
        self._records: Dict[str, Shape] = {}
        self._depth = 0
        self._keep: List[Any] = []

    def _put(self, name: str, type: Dict[str, Any]) -> None:
        slot = self._slots.get(name)
        if slot is not None:
            slot["type"] = type
        else:
            slot = {"name": name, "type": type, "attributes": []}
            self._slots[name] = slot
            self.types.append(slot)

    def insert_named(self, name: str, shape: Shape) -> None:
        """``SchemaBuilder::insert<T>(name)``."""
        ref = self.insert(shape)
        if ref["value"].value["type"] == name:
            return
        if name in self._slots:
            _fail(f'typed schema: the name "{name}" is used twice')
        self._put(name, ref)

    def insert(self, shape: Shape) -> Dict[str, Any]:
        """``SchemaBuilder::insert<T>()``: the reference to the interned type."""
        key = key_of(shape)
        if shape.k == "custom":
            self._bind_handler(shape.id, shape.handler)
        known = self._names.get(key)
        if known is not None:
            if shape.k in ("record", "transparent") and self._records.get(known) is not shape:
                _fail(f'typed schema: two different types are named "{known}"')
            return _ref(known)
        # A record and a transparent type are their names; so is a custom type declared over one.
        named = shape
        while named.k == "custom":
            named = named.inner
        name = named.name if named.k in ("record", "transparent") else f"@{len(self._names)}"
        if name in self._slots:
            _fail(f'typed schema: the name "{name}" is used twice')
        self._names[key] = name
        if shape.k in ("record", "transparent"):
            self._records[name] = shape
        if shape.bind is not None:
            self.bindings[name] = shape.bind
        # The slot is taken before the children are visited (declaration order).
        self._put(name, _ref(name))
        self._depth += 1
        if self._depth > 256:
            _fail("typed schema: type nesting too deep")
        self._put(name, self._build(shape, name))
        self._depth -= 1
        return _ref(name)

    def _bind_handler(self, id: str, handler: Optional[Handler]) -> None:
        if handler is None:
            return
        bound = self.handlers.get(id)
        if bound is not None and bound is not handler:
            _fail(f'typed schema: custom id "{id}" is declared with two different handlers')
        self.handlers[id] = handler

    def _inline(self, shape: Shape) -> Dict[str, Any]:
        """The type itself, written inside another node: its binding goes with the node."""
        out = self._build(shape)
        if shape.bind is not None:
            self.bindings[id(out)] = shape.bind
            self._keep.append(out)
        return out

    def _build(self, s: Shape, name: Optional[str] = None) -> Dict[str, Any]:
        k = s.k
        if k == "int":
            return _int_ir(s.bits, s.signed)
        if k == "float":
            return _node("Float", {"exp": 8, "mantissa": 24} if s.bits == 32 else {"exp": 11, "mantissa": 53})
        if k == "bool":
            return _node("Custom", {"type": _int_ir(1, False), "id": "bool"})
        if k == "char":
            return _node("Custom", {"type": _int_ir(8, False), "id": "char"})
        if k == "bytes":
            return _node("Custom", {"type": self._byte_container(s.bound, s.n), "id": s.id})
        if k == "json":
            return _node("Custom", {"type": _node("Custom", {"type": self._byte_container("list", 0), "id": "utf8"}),
                                    "id": "json"})
        if k == "bits":
            return _node("Custom", {
                "type": _node("Array", {"type": _int_ir(1, False), "len": s.n}) if s.id == "bitvector"
                else _node("BoundedList", {"type": _int_ir(1, False), "maxCount": s.n}),
                "id": s.id})
        if k == "option":
            return _node("Option", {"type": self.insert(s.inner)})
        if k == "list":
            return _node("List", {"type": self.insert(s.inner)})
        if k == "bounded":
            return _node("BoundedList", {"type": self.insert(s.inner), "maxCount": s.n})
        if k == "array":
            return _node("Array", {"type": self.insert(s.inner), "len": s.n})
        if k == "tuple":
            return _node("Tuple", {"members": [self.insert(i) for i in s.items]})
        if k == "variant":
            return self._variant(s.alts, lambda alt, i: self.insert(alt.type), s.untagged)
        if k == "record":
            members = [self._member(f) for f in _fields_of(s)]
            attributes = []
            o = s.options
            if o.definition_will_not_change:
                attributes.append(_attribute("definitionWillNotChange", ""))
            if o.max_fields is not None:
                attributes.append(_attribute("maxFields", str(o.max_fields)))
            if o.max_dynamic_data is not None:
                attributes.append(_attribute("maxDynamicData", str(o.max_dynamic_data)))
            return _node("Object", {"members": members, "attributes": attributes})
        if k == "args":
            # A Tuple node has no attributes, so the argument tuple is written
            # as the extensible record it is on the wire, which can carry the cap.
            # Its `tuple` attribute says what it is in text (docs/json.md "Method
            # arguments"): "named" (an object; the array is read too) or
            # "positional" (the members are "0", "1", ...; the array only).
            attributes = [_attribute("tuple", "named" if s.named else "positional")]
            if s.max_fields is not None:
                attributes.append(_attribute("maxFields", str(s.max_fields)))
            if s.max_dynamic_data is not None:
                attributes.append(_attribute("maxDynamicData", str(s.max_dynamic_data)))
            members = [self._parameter(f) for f in s.fields]
            if not s.named:
                for i, m in enumerate(members):
                    m["name"] = str(i)
            return _node("Object", {"members": members, "attributes": attributes})
        if k == "enum":
            return _node("Enum", {"type": self._build(s.under),
                                  "members": [{"name": n, "value": v & 0xFFFFFFFFFFFFFFFF} for n, v in s.members]})
        if k == "time":
            return _node("Custom", {"type": self._build(s.tick), "id": s.id})
        if k == "assoc":
            t = self.insert(s.entry)
            return _node("Custom", {
                "type": _node("List", {"type": t}) if s.max is None
                else _node("BoundedList", {"type": t, "maxCount": s.max}),
                "id": s.id})
        if k == "nested":
            # The validation policy is not part of the schema: the codec is
            # told which named nodes are opaque.
            if s.deferred:
                if name is None:
                    _fail("typed schema: a deferred nested value cannot be the wire type of a custom type; "
                          "wrap it in a transparent type")
                self.deferred.add(name)
            return _node("Nested", {"type": self.insert(s.inner)})
        if k == "transparent":
            # A named alias of the member's type.
            return self.insert(_inner_of(s))
        if k == "annotated":
            # The member of a transparent type under its bounds: the node the annotations describe.
            out = self._annotated_node(s.inner, s.position, True)
            if out["value"].type != "Type" and s.inner.bind is not None and name is not None:
                self.bindings[name] = s.inner.bind
            return out
        if k == "custom":
            self._bind_handler(s.id, s.handler)
            wire = s.inner
            # Over a transparent type: the Custom wraps the member's type.
            return _node("Custom", {
                "type": self.insert(_inner_of(wire)) if wire.k == "transparent" else self._inline(wire),
                "id": s.id})
        if k == "box":
            return _node("Custom", {"type": self.insert(s.inner), "id": "box"})
        return _fail("typed schema: unresolved definition")

    def _byte_container(self, bound: str, n: int) -> Dict[str, Any]:
        t = self.insert(_U8)
        if bound == "list":
            return _node("List", {"type": t})
        if bound == "max":
            return _node("BoundedList", {"type": t, "maxCount": n})
        return _node("Array", {"type": t, "len": n})

    def _annotated(self, shape: Shape, a: Optional[Position], use_bound: bool = True) -> Dict[str, Any]:
        """``annotated_type``: the type of ``shape`` at a position that carries ``a``.

        A ``max`` is the bounded form of a string / list / map / set, ``utf8``
        the UTF-8 string: the nodes the wrapper types produce.  A position
        with nothing to say is the interned type.  ``use_bound`` false: the
        position's own bound is carried elsewhere (the attribute lengthBound).
        """
        if a is None:
            return self.insert(shape)
        out = self._annotated_node(shape, a, use_bound)
        if out["value"].type != "Type" and shape.bind is not None:
            self.bindings[id(out)] = shape.bind
            self._keep.append(out)
        return out

    def _annotated_node(self, s: Shape, a: Position, use_bound: bool) -> Dict[str, Any]:
        k = s.k
        if k == "option":
            # Looked through: the bounds are the payload's.
            inner = self._annotated(s.inner, a, use_bound)
            return self.insert(s) if _same_ref(inner, self.insert(s.inner)) else _node("Option", {"type": inner})
        limit = a.max if use_bound else None

        def list_of(t: Dict[str, Any], bound: Optional[int] = limit) -> Dict[str, Any]:
            return _node("List", {"type": t}) if bound is None else _node("BoundedList", {"type": t, "maxCount": bound})

        if k == "bytes":
            if s.bound != "list":
                return self.insert(s)
            if s.id == "hex":
                return self.insert(s) if limit is None else _node("Custom", {"type": list_of(self.insert(_U8)), "id": "hex"})
            # A raw string that the position says is text, or a `str`: the UTF-8 node.
            text = s.id == "utf8" or a.utf8
            if limit is None and not (s.id == "string" and a.utf8):
                return self.insert(s)
            return _node("Custom", {"type": list_of(self.insert(_U8)), "id": "utf8" if text else "string"})
        if k == "assoc":
            below = bool(a.keys or a.values) if s.id == "map" else bool(a.element)
            if limit is None and not below:
                return self.insert(s)
            if s.id == "set":
                entry = self._annotated(s.entry, a.element)
            elif below:
                # (in this order: the key's types are registered first)
                key, value = s.entry.items
                key_type = self._annotated(key, a.keys)
                entry = _node("Tuple", {"members": [key_type, self._annotated(value, a.values)]})
            else:
                entry = self.insert(s.entry)
            return _node("Custom", {"type": list_of(entry, limit if limit is not None else s.max), "id": s.id})
        if k == "list":
            if limit is None and not a.element:
                return self.insert(s)
            return list_of(self._annotated(s.inner, a.element))
        if k == "bounded":
            # The wrapper's bound is in its type.
            if not a.element:
                return self.insert(s)
            return _node("BoundedList", {"type": self._annotated(s.inner, a.element), "maxCount": s.n})
        if k == "array":
            if not a.element:
                return self.insert(s)
            return _node("Array", {"type": self._annotated(s.inner, a.element), "len": s.n})
        if k == "tuple":
            if not a.at:
                return self.insert(s)
            return _node("Tuple", {"members": [self._annotated(item, a.at.get(str(i))) for i, item in enumerate(s.items)]})
        if k == "variant":
            if not a.at:
                return self.insert(s)
            at = a.at

            def key_for(i: int) -> Optional[Position]:
                if s.alts[i].name in at:
                    return at[s.alts[i].name]
                # (an index that is another alternative's name speaks of that one)
                if any(alt.name == str(i) for alt in s.alts):
                    return None
                return at.get(str(i))
            return self._variant(s.alts, lambda alt, i: self._annotated(alt.type, key_for(i)), s.untagged)
        return self.insert(s)

    def _variant(self, alts: Sequence[AltShape], type_of: Callable[[AltShape, int], Dict[str, Any]],
                 untagged: bool = False) -> Dict[str, Any]:
        return _node("Variant", {
            "members": [{"name": alt.name, "type": type_of(alt, i),
                         "attributes": [] if alt.ijson_name is None else [_attribute("ijsonName", alt.ijson_name)]}
                        for i, alt in enumerate(alts)],
            "attributes": [_attribute("untagged", "")] if untagged else []})

    def _member(self, field: FieldShape) -> Dict[str, Any]:
        """``build_record_member`` / ``apply_member_annotations``."""
        member: Dict[str, Any] = {"name": field.name, "type": self.insert(field.type), "attributes": []}
        direct = field.type
        target = direct
        while target.k == "option":
            target = target.inner
        # A bounded UTF-8 string states its own bound.
        bound = field.length_bound
        if bound is None and direct.k == "bytes" and direct.id == "utf8" and direct.bound == "max":
            bound = A.LengthSpec(max=direct.n)
        position = field.position
        as_attribute = False
        where = f'member "{field.name}"'
        if bound is not None:
            # Elements of a list, entries of a map / set, bytes of a string or
            # of a nested value's encoding.
            applies = target.k in ("list", "assoc", "nested") or (
                target.k == "bytes" and target.bound != "exact" and (target.bound == "list" or target.id == "utf8"))
            if not applies:
                _fail(f"typed schema: {where}: a length bound applies to a string, bytes, a list, a map, a set "
                      "or a nested value")
            only_max = bound.max is not None and bound.exact is None and bound.min is None
            # `has_bounded_form`: the types a `max` turns into a bounded type.
            bounded_form = target.k == "list" or (target.k == "assoc" and target.max is None) or (
                target.k == "bytes" and target.bound == "list" and target.id != "utf8")
            # `str` under max<N>: the node C++ writes for attr(f, utf8 | max<N>) on a std::string.
            utf8_plain = target.k == "bytes" and target.bound == "list" and target.id == "utf8"
            if only_max and not bound.attribute and (bounded_form or (utf8_plain and field.length_bound is not None)):
                if position is None:
                    position = Position()
                position.max = bound.max
            else:
                as_attribute = True
                items = [f"{key}={getattr(bound, key)}" for key in ("exact", "min", "max")
                         if getattr(bound, key) is not None]
                if items:
                    member["attributes"].append(_attribute("lengthBound", ",".join(items)))
        if position is not None:
            _check_position(field.type, position, True, where)
            member["type"] = self._annotated(field.type, position, not as_attribute)
        if field.json_int:
            member["attributes"].append(_attribute("jsonInt", field.json_int))
        if field.ijson_int:
            member["attributes"].append(_attribute("ijsonInt", field.ijson_int))
        return member

    def _parameter(self, field: FieldShape) -> Dict[str, Any]:
        """A parameter of an argument tuple: its annotated type (a Tuple element has no attributes)."""
        where = f'parameter "{field.name}"'
        bound = field.length_bound
        position = field.position
        if bound is not None:
            if bound.exact is not None or bound.min is not None:
                _fail(f"typed schema: {where}: a parameter's length bound is max (the Schema IR has no place "
                      "for exact / min)")
            if position is None:
                position = Position()
            position.max = bound.max
        if field.json_int or field.ijson_int:
            _fail(f"typed schema: {where}: json_int / ijson_int belong to record members")
        if position is not None:
            _check_position(field.type, position, False, where)
            return {"name": field.name, "type": self._annotated(field.type, position), "attributes": []}
        return {"name": field.name, "type": self._annotated(field.type, None), "attributes": []}

    def schema(self, **options: Any) -> Schema:
        ir = {"package": {"name": "", "version": "", "attributes": []}, "interfaces": [], "worlds": [], "uses": [],
              "types": self.types, "ir_version": _ir.IR_VERSION}
        out = Schema(ir, handlers=self.handlers, _deferred=self.deferred, _bindings=self.bindings,
                     _normalized=True, **options)
        out._builder = self
        return out


def _check_position(shape: Shape, a: Position, top: bool, where: str) -> None:
    """``ann::tree_ok``: the bounds fit the type at every depth, with one message per misuse."""
    s = shape
    while s.k == "option":
        s = s.inner
    k = s.k
    raw_bytes = k == "bytes" and s.bound == "list"
    if a.max is not None and not top and not (raw_bytes or k == "list" or (k == "assoc" and s.max is None)):
        _fail(f"typed schema: {where}: max bounds a string or bytes (its bytes) or a list, map or set (its "
              "elements); this position is none of those")
    if a.utf8 and not (k == "bytes" and (s.id == "utf8" or (s.id == "string" and s.bound == "list"))):
        _fail(f"typed schema: {where}: utf8 applies to a raw string; this position is not one")
    if a.element:
        if not (k in ("list", "bounded", "array") or (k == "assoc" and s.id == "set")):
            _fail(f"typed schema: {where}: element bounds the elements of a list, an array or a set; this "
                  "position is none of those (a map takes keys / values, a tuple or union takes at)")
        _check_position(s.entry if k == "assoc" else s.inner, a.element, False, f"{where}, element")
    if a.keys or a.values:
        if not (k == "assoc" and s.id == "map"):
            _fail(f"typed schema: {where}: keys / values bound the keys / mapped values of a map; this position "
                  "is not a map")
        key, value = s.entry.items
        if a.keys:
            _check_position(key, a.keys, False, f"{where}, keys")
        if a.values:
            _check_position(value, a.values, False, f"{where}, values")
    if a.at:
        if k == "tuple":
            for key, child in a.at.items():
                if not key.isdigit() or int(key) >= len(s.items):
                    _fail(f"typed schema: {where}: at {key}: not an element of this tuple")
                _check_position(s.items[int(key)], child, False, f"{where}, at {key}")
        elif k == "variant":
            seen = set()
            for key, child in a.at.items():
                index = _alt_index(s.alts, key, where)
                if index in seen:
                    _fail(f"typed schema: {where}: at {key}: the alternative is named twice")
                seen.add(index)
                _check_position(s.alts[index].type, child, False, f"{where}, at {key}")
        else:
            _fail(f"typed schema: {where}: at bounds an element of a tuple or an alternative of a union; this "
                  "position is none of those (a list, array or set takes element)")


_ARGS = iter(range(1, 1 << 62))


def arguments(params: Any, *, max_dynamic_data: Optional[int] = None, max_fields: Optional[int] = None,
              named: Optional[bool] = None) -> Shape:
    """The argument list of a method: the positional extensible record of its parameters (spec 16).

    ``params`` maps each parameter name to its type hint, annotated like a
    record member; a sequence of hints declares a list without names
    (``psio.arguments([u32, u32])``; ``named=False`` says the same of a
    mapping).  ``max_dynamic_data`` is the cap of the encoded arguments and
    fixes the list's offset width for the method's lifetime; ``max_fields``
    is the most parameters the method will ever have (at most 255).

    A value is the tuple of the arguments (a ``dict`` by parameter name is
    accepted for a named list).  Binary is always the positional tuple.  In
    text (docs/json.md "Method arguments") a named list is written as an
    object keyed by parameter name and read from that object or from the
    positional array; a list without names is the array only.
    """
    def run() -> Shape:
        if isinstance(params, typing.Mapping):
            items = list(params.items())
            is_named = True if named is None else bool(named)
        else:
            items = [(f"p{i}", hint) for i, hint in enumerate(params)]
            if named:
                _fail("psio: a named argument list maps each parameter name to its type")
            is_named = False
        fields = [_field(attr, hint, (), f'parameter "{attr}"') for attr, hint in items]
        if not is_named:
            for i, f in enumerate(fields):
                f.name = str(i)
        names = [f.name for f in fields]
        if len(set(names)) != len(names):
            _fail("psio: two parameters have the same name")
        # A method's argument list is a tuple (docs/pssz-spec.md 8.4).
        if len(fields) > MAX_TUPLE_ELEMENTS:
            _fail("psio: a method has more than 255 parameters")
        _check_field_count("a method", "parameters", len(fields), max_fields)
        if max_dynamic_data is not None and (type(max_dynamic_data) is not int or max_dynamic_data < 0):
            _fail("psio: max_dynamic_data is a non-negative integer")
        count = len(fields)
        out = Shape("args", uid=next(_ARGS), fields=fields, max_dynamic_data=max_dynamic_data,
                    max_fields=max_fields, named=is_named, py=tuple)

        def get(value: Any) -> Any:
            if isinstance(value, dict):
                if not is_named:
                    raise EncodeError("pssz: the arguments of a method without parameter names are a tuple")
                if len(value) > count or any(k not in names for k in value):
                    raise EncodeError("pssz: unknown argument name")
                return [value.get(n) for n in names]
            if not isinstance(value, (tuple, list)) or len(value) > count:
                raise EncodeError("pssz: the arguments are a tuple of at most the declared parameters")
            # Trailing optional parameters may be left out.
            return list(value) + [None] * (count - len(value)) if len(value) < count else value
        out.bind = Binding(get=get, make=tuple, order=_orders(fields))
        return out
    return _declared(run)


# ── Public entry points ─────────────────────────────────────────────────────

_SCHEMAS: Dict[Any, Tuple[Schema, str]] = {}
_CODECS: Dict[Any, SchemaType] = {}
_ORDERED: Dict[Any, bool] = {}


def _has_union(tp: Any, depth: int = 0) -> bool:
    if depth > 64:
        return True
    origin = typing.get_origin(tp)
    if origin is None:
        return False
    args = typing.get_args(tp)
    if origin is Annotated:
        return _has_union(args[0], depth + 1)
    if origin is Union or (hasattr(types, "UnionType") and origin is types.UnionType):
        if len([a for a in args if a is not NoneType]) > 1:
            return True
    return any(_has_union(a, depth + 1) for a in args if a is not Ellipsis)


def _cache_key(tp: Any) -> Any:
    """The key a type hint is cached under.

    Python compares unions without regard to order (``A | B == B | A``), and
    the order of a variant's alternatives is its wire format: a hint that
    holds a union is keyed by its spelling too.  Raises ``TypeError`` for an
    unhashable hint.
    """
    ordered = _ORDERED.get(tp)
    if ordered is None:
        ordered = _ORDERED[tp] = _has_union(tp)
    return (tp, repr(tp)) if ordered else tp


def _declared(run: Callable[[], Any]) -> Any:
    try:
        return run()
    except PsioError:
        raise
    except RecursionError:
        raise SchemaError("typed schema: type nesting too deep (use a class for recursion)") from None
    except Exception as e:
        raise SchemaError(f"typed schema: {type(e).__name__}: {e}") from e


def _root(tp: Any) -> Tuple[Schema, str]:
    """The schema of one Python type and the name the type has in it."""
    try:
        key = _cache_key(tp)
        known = _SCHEMAS.get(key)
    except TypeError:  # an unhashable hint
        key = known = None
    if known is not None:
        return known

    def run() -> Tuple[Schema, str]:
        builder = Builder()
        shape = tp if isinstance(tp, Shape) else shape_of(tp)
        ref = builder.insert(shape)
        return builder.schema(), ref["value"].value["type"]
    out = _declared(run)
    if key is not None:
        _SCHEMAS[key] = out
    return out


def schema_of(*types: Any, names: Optional[Dict[str, Any]] = None, **options: Any) -> Schema:
    """The Schema IR of Python types: what C++ ``psio::SchemaBuilder`` builds for the equivalent types.

    A class is in the schema under its name; every other type is interned as
    ``@N`` in first-visit order.  ``names`` gives further names to types
    (``SchemaBuilder::insert<T>(name)``).  The result carries the bindings
    and handlers of the Python types: ``schema.type(name)`` reads and writes
    Python objects; ``schema.ir`` / ``to_json()`` / ``to_pssz()`` are the IR.
    """
    def run() -> Schema:
        builder = Builder()
        for tp in types:
            builder.insert(tp if isinstance(tp, Shape) else shape_of(tp))
        for name, tp in (names or {}).items():
            builder.insert_named(name, tp if isinstance(tp, Shape) else shape_of(tp))
        return builder.schema(**options)
    if len(types) == 1 and not names and not options:
        declared = types[0].__dict__.get("__psio_interface__") if isinstance(types[0], type) else None
        if declared is not None:
            # A class declared with @psio.interface: the Interface / Func nodes and what they reach.
            return cast(Schema, declared.schema())
        return _root(types[0])[0]
    return _declared(run)


def codec(tp: Any, *, width: Any = "auto", require_handlers: Optional[bool] = None,
          nested_bytes: Optional[str] = None) -> SchemaType:
    """The compiled codec of a Python type (cached): every operation of :class:`psio.SchemaType`."""
    plain = width == "auto" and require_handlers is None and nested_bytes is None
    if plain:
        try:
            key = _cache_key(tp)
            known = _CODECS.get(key)
        except TypeError:
            key = known = None
        if known is not None:
            return known
    if isinstance(tp, SchemaType):
        return tp
    schema, name = _root(tp)
    out = schema.type(name, width=width, require_handlers=require_handlers, nested_bytes=nested_bytes)
    out.python_type = tp
    if plain and key is not None:
        _CODECS[key] = out
    return out
