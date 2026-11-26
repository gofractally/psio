"""The helper names: one set per format, as in C++ (docs/helpers.md).

    to_pssz(v)   from_pssz(T, data)   from_pssz(T, trusted(data))   validate_pssz(T, data)
    pssz_size(v) pssz_view(T, data)
    to_key(v)    from_key(T, data)    validate_key(T, data)   key_prefix(T, elements)
    to_json(v)   from_json(T, text)   validate_json(T, text)  (and the same with ``ijson``)

``T`` is a Python type (a dataclass, ``list[u32]``, ``Annotated[...]``, ...) or
a :class:`psio.SchemaType` from a schema.  The short name is the safe one:
``from_*`` validates.  Where ``T`` is optional it defaults to ``type(v)``.
"""

from __future__ import annotations

from typing import Any, Optional, Type, TypeVar, overload

from .schema import SchemaType, Status, trusted
from .typed import _CODECS, _cache_key, codec

__all__ = [
    "to_pssz", "from_pssz", "from_pssz_trusted", "validate_pssz", "pssz_size", "pssz_view",
    "to_key", "from_key", "validate_key", "key_prefix", "key_size",
    "to_json", "from_json", "validate_json", "to_ijson", "from_ijson", "validate_ijson",
    "json_to_pssz", "pssz_to_json",
]


_T = TypeVar("_T")


def _of(value: Any, T: Any) -> SchemaType:
    if T is None:
        T = type(value)
    if isinstance(T, SchemaType):
        return T
    try:
        known = _CODECS.get(_cache_key(T))
    except TypeError:
        known = None
    return known if known is not None else codec(T)


def to_pssz(value: Any, T: Any = None) -> bytes:
    """The PSSZ bytes of ``value`` (of type ``T``; default ``type(value)``): the FAST write.

    The value is not held against what its type declares (bounds, order, invariants); an
    ``EncodeError`` is a value the format cannot represent (an offset that does not fit its width, a
    ``max_dynamic_data`` cap, a wrong Python type).  ``to_pssz_checked`` is the validated write: use
    it where the bytes leave the program (C++ ``to_pssz`` / ``to_pssz_checked``)."""
    return _of(value, T).to_pssz(value)


def to_pssz_checked(value: Any, T: Any = None) -> bytes:
    """The VALIDATED write: the canonical PSSZ bytes of ``value``, or an ``EncodeError`` for a value
    ``validate_pssz`` would refuse the bytes of."""
    return _of(value, T).to_pssz_checked(value)


@overload
def from_pssz(T: Type[_T], data: Any, *, reject_unknown: bool = False) -> _T: ...
@overload
def from_pssz(T: Any, data: Any, *, reject_unknown: bool = False) -> Any: ...
def from_pssz(T: Any, data: Any, *, reject_unknown: bool = False) -> Any:
    """Validates ``data`` as a ``T``, then decodes it.

    ``from_pssz(T, trusted(data))`` skips what trust makes unnecessary (bounds,
    caps, canonical-form checks and the invariants of custom types).  ``reject_unknown``: a frame carrying
    appended optional fields this type does not know is an error instead of
    being read without them.
    """
    return _of(None, T).from_pssz(data, reject_unknown=reject_unknown)


@overload
def from_pssz_trusted(T: Type[_T], data: Any) -> _T: ...
@overload
def from_pssz_trusted(T: Any, data: Any) -> Any: ...
def from_pssz_trusted(T: Any, data: Any) -> Any:
    """``from_pssz(T, trusted(data))``."""
    return _of(None, T).from_pssz(trusted(data))


def validate_pssz(T: Any, data: Any, *, strict: bool = False) -> Status:
    """Whether ``data`` is a valid ``T``: a :class:`Status`, never an exception."""
    try:
        t = _of(None, T)
    except Exception as e:
        from .errors import PsioError, SchemaError
        return Status(False, False, e if isinstance(e, PsioError) else SchemaError(str(e)))
    return t.validate_pssz(data, strict=strict)


def pssz_size(value: Any, T: Any = None) -> int:
    """The size of the PSSZ encoding of ``value``."""
    return _of(value, T).pssz_size(value)


def pssz_view(T: Any, data: Any) -> Any:
    """A read-only view of ``data`` as a ``T``: one field or element is read at a time (``psio.view``)."""
    return _of(None, T).pssz_view(data)


def to_key(value: Any, T: Any = None) -> bytes:
    """The ``key`` bytes of ``value``: their bytewise order is the value's order (docs/key-spec.md)."""
    return _of(value, T).to_key(value)


def to_key_checked(value: Any, T: Any = None) -> bytes:
    """The validated key write (``to_key`` is the fast write: every value has a key)."""
    return _of(value, T).to_key_checked(value)


@overload
def from_key(T: Type[_T], data: Any) -> _T: ...
@overload
def from_key(T: Any, data: Any) -> Any: ...
def from_key(T: Any, data: Any) -> Any:
    """Decodes key bytes: exactly one canonical key spanning the whole input."""
    return _of(None, T).from_key(data)


def validate_key(T: Any, data: Any) -> Status:
    return _of(None, T).validate_key(data)


def key_prefix(T: Any, elements: Any) -> bytes:
    """The key of the leading ``elements`` of a tuple / record type ``T``."""
    return _of(None, T).key_prefix(elements)


def key_size(value: Any, T: Any = None) -> int:
    return _of(value, T).key_size(value)


def to_json(value: Any, T: Any = None) -> str:
    """Strict JSON text (docs/json.md): 64-bit integers are bare numbers, bytes are number arrays."""
    return _of(value, T).to_json(value, "json")


def to_ijson(value: Any, T: Any = None) -> str:
    """I-JSON text: 64-bit integers beyond +-(2^53 - 1) are decimal strings, bytes are hex,
    a variant is ``{"Name": value}``."""
    return _of(value, T).to_json(value, "ijson")


def to_json_checked(value: Any, T: Any = None) -> str:
    """The validated JSON write (``to_json`` is the fast write)."""
    return _of(value, T).to_json_checked(value, "json")


def to_ijson_checked(value: Any, T: Any = None) -> str:
    """The validated I-JSON write (``to_ijson`` is the fast write)."""
    return _of(value, T).to_json_checked(value, "ijson")


@overload
def from_json(T: Type[_T], text: Any) -> _T: ...
@overload
def from_json(T: Any, text: Any) -> Any: ...
def from_json(T: Any, text: Any) -> Any:
    return _of(None, T).from_json(text, "json")


@overload
def from_ijson(T: Type[_T], text: Any) -> _T: ...
@overload
def from_ijson(T: Any, text: Any) -> Any: ...
def from_ijson(T: Any, text: Any) -> Any:
    return _of(None, T).from_json(text, "ijson")


def validate_json(T: Any, text: Any) -> Status:
    return _of(None, T).validate_json(text, "json")


def validate_ijson(T: Any, text: Any) -> Status:
    return _of(None, T).validate_json(text, "ijson")


def json_to_pssz(T: Any, text: Any, format: str = "ijson") -> bytes:
    """JSON / I-JSON text of a ``T`` -> its canonical PSSZ bytes."""
    return _of(None, T).json_to_pssz(text, format)


def pssz_to_json(T: Any, data: Any, format: str = "ijson") -> str:
    """PSSZ bytes of a ``T`` (validated) -> its JSON / I-JSON text."""
    return _of(None, T).pssz_to_json(data, format)
