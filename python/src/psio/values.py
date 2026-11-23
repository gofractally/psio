"""The few value classes the dynamic (schema-driven) value model needs.

Everything else is a plain Python value: ``bool``, ``int`` (every integer
width, enums and time ticks), ``float``, ``str`` (UTF-8 strings), ``bytes``
(raw strings, byte lists), ``list`` (lists and arrays), ``tuple`` (tuples and
pairs), ``dict`` (records by member name; maps), ``frozenset`` (sets) and
``None`` (an absent optional).  A value of the type "any JSON" (``psio.JsonValue``,
the Custom id ``json``) is the JSON value itself: ``None``, ``bool``, ``int``,
``float`` / :class:`JsonNumber`, ``str``, ``list`` and ``dict`` with ``str`` keys.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Generic, TypeVar

__all__ = ["Variant", "Some", "Bits", "NaN", "JsonNumber"]

T = TypeVar("T")


@dataclass(frozen=True)
class Variant:
    """A variant value in the dynamic model: the alternative's name and its payload."""

    __slots__ = ("type", "value")
    type: str
    value: Any


@dataclass(frozen=True)
class Some(Generic[T]):
    """``Some(x)`` of an optional whose payload is itself optional.

    Python's ``Optional[Optional[T]]`` is ``Optional[T]``, so the outer level
    is explicit: the type is ``Optional[Some[Optional[T]]]`` and its values
    are ``None``, ``Some(None)`` and ``Some(x)``.
    """

    __slots__ = ("value",)
    value: T


@dataclass(frozen=True)
class Bits:
    """A bitvector / bitlist value: packed bits, least significant first."""

    __slots__ = ("data", "bit_count")
    data: bytes
    bit_count: int

    @classmethod
    def from_bools(cls, bits: Any) -> "Bits":
        bits = list(bits)
        out = bytearray((len(bits) + 7) // 8)
        for i, b in enumerate(bits):
            if b:
                out[i >> 3] |= 1 << (i & 7)
        return cls(bytes(out), len(bits))

    def to_bools(self) -> "list[bool]":
        d = self.data
        return [bool(d[i >> 3] >> (i & 7) & 1) for i in range(self.bit_count)]

    def __len__(self) -> int:
        return self.bit_count

    def __getitem__(self, i: int) -> bool:
        if not 0 <= i < self.bit_count:
            raise IndexError(i)
        return bool(self.data[i >> 3] >> (i & 7) & 1)


class NaN(float):
    """A NaN other than the quiet NaN (``0x7fc00000`` / ``0x7ff8000000000000``).

    PSSZ keeps NaN payloads bit for bit; a decoded float is a plain ``float``
    unless it is one of these, and the encoders write ``bits`` back.
    """

    __slots__ = ("bits", "size")
    bits: int
    size: int

    def __new__(cls, bits: int, size: int) -> "NaN":
        self = super().__new__(cls, "nan")
        self.bits = bits
        self.size = size
        return self

    def __repr__(self) -> str:
        return f"NaN(bits={self.bits:#x}, size={self.size})"

    def __eq__(self, other: object) -> bool:
        return isinstance(other, NaN) and other.bits == self.bits and other.size == self.size

    def __hash__(self) -> int:
        return hash((self.bits, self.size))


class JsonNumber(float):
    """A number of a JSON value (``psio.JsonValue``) that is not written as a plain integer: a ``float``
    that remembers its token.

    A JSON value keeps its numbers **as they were written** (C++ ``psio::json_numeral``): ``1.50``,
    ``1e3`` and ``-0`` are written back unchanged.  A token that is a plain integer (``7``,
    ``-18446744073709551616``) is read as an ``int``, which is exact at any size; every other number
    is read as a ``JsonNumber``: equal to, and usable as, the ``float`` nearest its token, with the
    token in ``text``.  ``JsonNumber("1.50")`` builds one; a plain ``float`` is written as its
    shortest round-trip decimal.
    """

    __slots__ = ("text",)
    text: str

    def __new__(cls, text: str) -> "JsonNumber":
        if type(text) is not str:
            raise TypeError("JsonNumber takes the number's text")
        try:
            self = super().__new__(cls, text)
        except ValueError:
            # (not a number at all: refused where it is written)
            self = super().__new__(cls, "nan")
        self.text = text
        return self

    def __repr__(self) -> str:
        return f"JsonNumber({self.text!r})"
