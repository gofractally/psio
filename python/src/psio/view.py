"""Read-only views: values read in place from PSSZ bytes.

``psio.pssz_view(T, data)`` returns, for a composite, a view that reads one
field or element at a time: a record by attribute (``view.owner``; or
``view["owner"]``), a tuple / list / array by index, a variant by ``.type`` /
``.index`` / ``.value``.  A scalar or text is returned as its value; raw
strings and byte lists as a ``memoryview`` of the input (no copy).

A view checks the frames and values it reads, not the rest: validate first
when the whole encoding must be known to be canonical.  Views never write:
there is no mutable view in this package (decode, change, encode).
"""

from __future__ import annotations

from typing import Any, Iterator, List, Optional, Tuple

from ._core import CAP, DEPTH, LIST_BOUND, MAX_DEPTH, Codec, Ctx, Node, strip
from .errors import DecodeError, PsioError
from .values import Some

__all__ = ["View", "RecordView", "TupleView", "ListView", "VariantView"]


def _bytes(message: str) -> Any:
    raise DecodeError(message)


class View:
    """A composite value read in place."""

    __slots__ = ("_codec", "_node", "_data", "_start", "_end", "_depth", "_ref", "_spans")

    def __init__(self, codec: Codec, node: Node, data: bytes, start: int, end: int, depth: int, ref: Node) -> None:
        self._codec = codec
        self._node = node
        self._data = data
        self._start = start
        self._end = end
        self._depth = depth
        self._ref = ref
        self._spans: Any = None

    def view_bytes(self) -> memoryview:
        """The encoding of this value: a slice of the input, not a copy."""
        return memoryview(self._data)[self._start:self._end]

    def to_value(self, *, reject_unknown: bool = False) -> Any:
        """Decodes this value in full, with everything ``from_pssz`` checks."""
        try:
            return self._codec.decoder(self._ref)(self._data, self._start, self._end, self._depth,
                                                  Ctx(not reject_unknown))
        except PsioError:
            raise
        except RecursionError:
            raise DecodeError(DEPTH) from None
        except Exception as e:
            raise DecodeError(f"psio: {type(e).__name__}: {e}") from e

    def _frame(self) -> Tuple[List[int], List[int], List[bool], bool]:
        if self._spans is None:
            codec, node = self._codec, self._node
            record = node.kind == "record"
            if record and codec.over_cap(node, self._end - self._start):
                _bytes(CAP)
            self._spans = codec.record_spans(self._data, self._start, self._end, codec.width_of(node),
                                             record and node.stable, codec.field_layouts(node))
        return self._spans  # type: ignore[no-any-return]

    def _member(self, i: int) -> Any:
        begins, ends, present, _ = self._frame()
        if not present[i]:
            return None
        return view_at(self._codec, self._node.members[i].text, self._data, begins[i], ends[i], self._depth + 1)


class RecordView(View):
    """A record read in place: ``view.name`` or ``view["name"]`` reads one field."""

    __slots__ = ()

    def __getattr__(self, name: str) -> Any:
        index = self._node.by_name.get(name)
        if index is None:
            raise AttributeError(f'psio view: no field "{name}"')
        return self._member(index)

    def __getitem__(self, name: str) -> Any:
        index = self._node.by_name.get(name)
        if index is None:
            raise KeyError(name)
        return self._member(index)

    def __setattr__(self, name: str, value: Any) -> None:
        if name.startswith("_"):
            return object.__setattr__(self, name, value)
        raise AttributeError("psio: a view is read-only")

    def view_fields(self) -> List[str]:
        """The field names of the type, in declaration order."""
        return [m.name for m in self._node.members]

    def view_has_unknown(self) -> bool:
        """The writer appended optional fields this type does not know."""
        return self._frame()[3]

    def __repr__(self) -> str:
        return f"<psio.RecordView {', '.join(self.view_fields())}>"


class TupleView(View):
    """A tuple read in place."""

    __slots__ = ()

    def __len__(self) -> int:
        return len(self._node.members)

    def __getitem__(self, index: int) -> Any:
        count = len(self._node.members)
        if type(index) is not int or not -count <= index < count:
            raise IndexError("psio view: tuple index out of range")
        return self._member(index % count)

    def __iter__(self) -> Iterator[Any]:
        return (self._member(i) for i in range(len(self._node.members)))


class ListView(View):
    """A list, array, map (its entries) or set read in place."""

    __slots__ = ()

    def _measure(self) -> Tuple[int, int, int]:
        m = self._spans
        if m is not None:
            return m  # type: ignore[no-any-return]
        codec, node = self._codec, self._node
        size = self._end - self._start
        w = codec.width_of(node)
        fixed, esize, _ = codec.layout(node.child)
        if node.exact:
            count = node.count
        elif size == 0:
            count = 0
        elif fixed:
            if not esize:
                _bytes("pssz: nonempty zero-size vector is unsupported")
            if size % esize:
                _bytes("pssz: vector span not a multiple of element size")
            count = size // esize
        else:
            if w > size:
                _bytes("pssz: variable list missing offset table")
            first = int.from_bytes(self._data[self._start:self._start + w], "little")
            if not first or first % w or first > size:
                _bytes("pssz: invalid first list offset")
            count = first // w
        if node.bounded and (count != node.count if node.exact else count > node.count):
            _bytes(LIST_BOUND)
        if fixed:
            if count * esize != size:
                _bytes("pssz: fixed primitive span mismatch")
        elif count > size // w or (not count and size):
            _bytes("pssz: array offset table truncated")
        self._spans = (count, esize if fixed else -1, w)
        return self._spans  # type: ignore[no-any-return]

    def __len__(self) -> int:
        return self._measure()[0]

    def __getitem__(self, index: int) -> Any:
        count, esize, w = self._measure()
        if type(index) is not int or not -count <= index < count:
            raise IndexError("psio view: list index out of range")
        index %= count
        codec, data, start = self._codec, self._data, self._start
        element = self._node.text_child
        if esize >= 0:
            at = start + index * esize
            return view_at(codec, element, data, at, at + esize, self._depth + 1)
        size = self._end - start
        at = start + index * w
        begin = int.from_bytes(data[at:at + w], "little")
        stop = int.from_bytes(data[at + w:at + 2 * w], "little") if index + 1 < count else size
        if begin < count * w or begin > stop or stop > size:
            _bytes("pssz: list offset out of range")
        return view_at(codec, element, data, start + begin, start + stop, self._depth + 1)

    def __iter__(self) -> Iterator[Any]:
        return (self[i] for i in range(len(self)))


class VariantView(View):
    """A variant read in place."""

    __slots__ = ()

    @property
    def index(self) -> int:
        """The index of the stored alternative."""
        if self._end == self._start:
            _bytes("pssz: variant span out of bounds")
        selector = self._data[self._start]
        if selector >= len(self._node.members):
            _bytes("pssz: variant selector out of range")
        return selector

    @property
    def type(self) -> str:
        """The name of the stored alternative."""
        return self._node.members[self.index].name

    @property
    def value(self) -> Any:
        """What the alternative holds: a value or a view."""
        m = self._node.members[self.index]
        return view_at(self._codec, m.text, self._data, self._start + 1, self._end, self._depth + 1)


def view_at(codec: Codec, ref: Node, data: bytes, start: int, end: int, n: int) -> Any:
    """What a view returns for the value of ``ref`` (a type as written) at ``[start, end)``."""
    if n > MAX_DEPTH:
        _bytes(DEPTH)
    node = strip(ref)[0]
    k = node.kind
    if k == "record":
        return RecordView(codec, node, data, start, end, n, ref)
    if k == "tuple":
        return TupleView(codec, node, data, start, end, n, ref)
    if k in ("list", "array"):
        return ListView(codec, node, data, start, end, n, ref)
    if k == "variant":
        return VariantView(codec, node, data, start, end, n, ref)
    if k == "nested":
        if codec.is_opaque(node):
            if end > start and n + 1 > MAX_DEPTH:
                _bytes(DEPTH)
            return memoryview(data)[start:end]
        return view_at(codec.auto, node.text_child, data, start, end, n + 1)
    if k == "option":
        size = end - start
        if codec.can_be_empty(node.child):
            if size == 0:
                _bytes("pssz: optional<var> missing selector")
            if data[start] == 0:
                if size != 1:
                    _bytes("pssz: None has trailing bytes")
                return None
            if data[start] != 1:
                _bytes("pssz: optional selector not 0/1")
            start += 1
        elif size == 0:
            return None
        inner = view_at(codec, node.text_child, data, start, end, n + 1)
        return Some(inner) if node.child.kind == "option" else inner
    if k == "hex" or (k == "string" and node.raw):
        # Checked like a decode (the bound), returned without a copy.
        codec.quietly(lambda: codec.decoder(node)(data, start, end, n, Ctx(True)))
        return memoryview(data)[start:end]
    # A scalar or text: its value, as the typed layer presents it (no invariant is run).
    return codec.quietly(lambda: codec.decoder(ref)(data, start, end, n, Ctx(True)))
