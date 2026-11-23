"""The ``key`` format (docs/key-spec.md), schema-driven.

Byte strings whose bytewise order is the value's order: the counterpart of
C++ ``psio::key`` over the same nodes and the same Python values as the PSSZ
codec.  This module is internal.
"""

from __future__ import annotations

import struct
from typing import NoReturn, Any, List, Optional, Sequence

from ._core import (
    MAX_DEPTH, QUIET_NAN_32, QUIET_NAN_64, Codec, Ctx, Node, member_bound_admits, packed_byte_count, strip,
)
from .errors import DecodeError, EncodeError, SchemaError
from .values import Bits, NaN, Some, Variant

MAP_ORDER = "key: map entries are not in strictly ascending key order"
SET_ORDER = "key: set elements are not in strictly ascending order"


def _value(message: str) -> NoReturn:
    raise EncodeError(message)


def _bytes(message: str) -> NoReturn:
    raise DecodeError(message)


def _schema(message: str) -> NoReturn:
    raise SchemaError(message)


def octet_list(lst: Node) -> bool:
    """A list whose elements are single key bytes shares the string framing: u8, i8 or char as written."""
    e = lst.text_child
    return e.kind == "char" or (e.kind == "int" and e.bits == 8 and e.time is None and e.enumerators is None)


def _container_admits(container: Node, n: int) -> bool:
    if container.kind == "array":
        return n == container.count
    return not container.bounded or n <= container.count


def _escaped(b: bytes) -> bytes:
    """Bytes with 00 -> 00 01, then the terminator 00 00."""
    return b.replace(b"\x00", b"\x00\x01") + b"\x00\x00"


def _write_list(codec: Codec, node: Node, value: Sequence[Any], out: List[bytes], n: int) -> None:
    if node.bounded and len(value) > node.count:
        _value("key: list exceeds its bound")
    element = node.text_child
    if octet_list(node):
        signed = element.kind == "int" and element.signed
        pre = element.bind.pre if element.bind is not None else None
        try:
            if pre is not None:
                value = [pre(v) for v in value]
            if signed:
                data = bytes([v + 128 for v in value])
            else:
                data = bytes(value)
        except (ValueError, TypeError):
            return _value("key: byte value out of range")
        out.append(_escaped(data))
        return
    for item in value:
        out.append(b"\x01")
        write_key(codec, element, item, out, n + 1)
    out.append(b"\x00")


def write_key(codec: Codec, node: Node, value: Any, out: List[bytes], n: int) -> None:
    # A decoded value is at most MAX_DEPTH nodes deep; far beyond is a cycle.
    if n > 4 * MAX_DEPTH:
        _value("key: node depth limit exceeded")
    bind = node.bind
    if bind is not None and bind.pre is not None:
        value = bind.pre(value)
    k = node.kind
    if k == "int":
        if node.bits == 1:
            _schema("key: a one-bit integer has no key encoding")
        if type(value) is not int and not isinstance(value, int):
            _value("key: integer value required")
        if node.signed:
            value += 1 << (node.bits - 1)  # the sign bit flipped
        try:
            out.append(value.to_bytes(node.size, "big"))
        except OverflowError:
            _value("key: signed integer overflow" if node.signed else "key: unsigned integer overflow")
    elif k == "float":
        size = node.size
        if type(value) is NaN:
            if value.size != size:
                _value("key: NaN bits of the wrong float width")
            bits = value.bits
        elif not isinstance(value, (float, int)) or isinstance(value, bool):
            return _value("key: float value required")
        elif value != value:
            bits = QUIET_NAN_32 if size == 4 else QUIET_NAN_64
        else:
            try:
                packed = struct.pack(">f" if size == 4 else ">d", value)
            except (OverflowError, struct.error):
                packed = struct.pack(">f", float("inf") if value > 0 else float("-inf"))
            bits = int.from_bytes(packed, "big")
        width = size * 8
        sign = 1 << (width - 1)
        if bits == sign:
            bits = 0  # -0 is +0
        bits = (~bits & ((1 << width) - 1)) if bits & sign else bits ^ sign
        out.append(bits.to_bytes(size, "big"))
    elif k == "bool":
        if value is not True and value is not False:
            _value("key: bool value required")
        out.append(b"\x01" if value else b"\x00")
    elif k == "char":
        if type(value) is not int or not 0 <= value <= 255:
            _value("key: char byte value required")
        out.append(bytes((value,)))
    elif k == "string":
        if type(value) is str:
            try:
                data = value.encode("utf-8")
            except UnicodeEncodeError:
                return _value("key: string is not valid UTF-8")
        elif node.raw and isinstance(value, (bytes, bytearray, memoryview)):
            data = bytes(value)
        else:
            return _value("key: raw string value (bytes or str) required" if node.raw else "key: str value required")
        if not _container_admits(node.child, len(data)):
            _value("key: string exceeds its bound")
        out.append(data if node.child.kind == "array" else _escaped(data))
    elif k == "hex":
        if not isinstance(value, (bytes, bytearray, memoryview)):
            _value("key: bytes value required")
        data = bytes(value)
        if not _container_admits(node.child, len(data)):
            _value("key: bytes exceed their bound")
        out.append(data if node.child.kind == "array" else _escaped(data))
    elif k == "bitvector":
        if type(value) is not Bits or value.bit_count != node.size or len(value.data) != packed_byte_count(node.size):
            _value("key: packed Bits value of the declared size required")
        if node.size % 8 and value.data[-1] >> (node.size % 8):
            _value("key: packed bits above logical count")
        out.append(bytes(value.data))
    elif k == "bitlist":
        _schema("key: bitlist has no key encoding")
    elif k == "option":
        if value is None:
            out.append(b"\x00")
            return
        if node.child.kind == "option":
            if type(value) is not Some:
                _value("key: a nested optional needs None or Some(value)")
            value = value.value
        out.append(b"\x01")
        write_key(codec, node.text_child, value, out, n + 1)
    elif k == "list":
        if not node.assoc:
            if not isinstance(value, (list, tuple)):
                _value("key: sequence value required")
            _write_list(codec, node, value, out, n)
        else:
            # A map / set: the key of the list of its entries in ascending key
            # order, whatever order the value holds them in.
            _write_list(codec, node, codec.assoc_sorted(node, value, _value, "key"), out, n)
    elif k == "nested":
        # The key of the byte list holding the inner value's canonical
        # encoding (the octet framing): not the key of the inner value.
        if codec.is_opaque(node):
            if not isinstance(value, (bytes, bytearray, memoryview)):
                _value("key: bytes value required for an opaque nested value")
            data = bytes(value)
        else:
            data = codec.auto.encoder(node.text_child)(value, n + 1)
        out.append(_escaped(data))
    elif k == "array":
        if not isinstance(value, (list, tuple)):
            _value("key: sequence value required")
        if len(value) != node.count:
            _value("key: array length does not match the schema")
        for item in value:
            write_key(codec, node.text_child, item, out, n + 1)
    elif k == "tuple":
        if not isinstance(value, (list, tuple)) or len(value) != len(node.members):
            _value("key: tuple value of the declared arity required")
        for m, item in zip(node.members, value):
            write_key(codec, m.text, item, out, n + 1)
    elif k == "record":
        values = codec.record_getter(node)(value)
        for m, item in zip(node.members, values):
            _write_member(codec, m, item, out, n)
    elif k == "variant":
        index, payload = codec.variant_splitter(node)(value)
        out.append(bytes((index,)))
        write_key(codec, node.members[index].text, payload, out, n + 1)
    elif k == "custom":
        # A wrapper has the key of its inner type: a text form plays no part.
        write_key(codec, node.inner, value, out, n)


def _write_member(codec: Codec, m: Any, value: Any, out: List[bytes], n: int) -> None:
    if m.bound is not None and not member_bound_admits(m.bound, m.node, value):
        _value("key: member length bound violated")
    write_key(codec, m.text, value, out, n + 1)


def encode_key(codec: Codec, node: Node, value: Any) -> bytes:
    """The key bytes of ``value``.  No handler is consulted."""
    out: List[bytes] = []
    try:
        write_key(codec, node, value, out, 0)
    except RecursionError:
        _value("key: node depth limit exceeded")
    return b"".join(out)


def encode_key_prefix(codec: Codec, ref: Node, elements: Any) -> bytes:
    """The key of the first ``len(elements)`` elements / fields of a composite type."""
    node = strip(ref)[0]
    if node.kind not in ("tuple", "record"):
        _schema("key: a prefix key needs a tuple or record type")
    if not isinstance(elements, (list, tuple)):
        _value("key: prefix elements must be a sequence")
    if len(elements) > len(node.members):
        _value("key: more prefix elements than the key type has")
    out: List[bytes] = []
    for m, item in zip(node.members, elements):
        if codec.checks_values:
            codec.check_value(m.text, item, "value")
        if node.kind == "record":
            _write_member(codec, m, item, out, 0)
        else:
            write_key(codec, m.text, item, out, 1)
    return b"".join(out)


def key_prefix_upper_bound(prefix: bytes) -> Optional[bytes]:
    """The exclusive upper bound of a prefix scan, or ``None`` when there is none.

    The smallest byte string greater than every byte string that starts with
    ``prefix`` (trailing FF bytes dropped, last byte incremented).
    """
    if not isinstance(prefix, (bytes, bytearray, memoryview)):
        raise DecodeError("key: input must be bytes")
    data = bytes(prefix).rstrip(b"\xff")
    if not data:
        return None
    return data[:-1] + bytes((data[-1] + 1,))


# ── Reading ─────────────────────────────────────────────────────────────────

class _Source:
    __slots__ = ("b", "p")

    def __init__(self, b: bytes) -> None:
        self.b = b
        self.p = 0

    def byte(self) -> int:
        if self.p >= len(self.b):
            _bytes("key: truncated")
        v = self.b[self.p]
        self.p += 1
        return v

    def take(self, n: int) -> bytes:
        if len(self.b) - self.p < n:
            _bytes("key: truncated")
        out = self.b[self.p:self.p + n]
        self.p += n
        return out

    def escaped(self) -> bytes:
        """Unescaped bytes up to the 00 00 terminator, which is consumed."""
        b = self.b
        q = self.p
        while True:
            q = b.find(b"\x00", q)
            if q < 0 or q + 1 >= len(b):
                _bytes("key: unterminated string")
            nxt = b[q + 1]
            if nxt == 0:
                break
            if nxt != 1:
                _bytes("key: invalid escape")
            q += 2
        out = b[self.p:q].replace(b"\x00\x01", b"\x00")
        self.p = q + 2
        return out


def _read_assoc(codec: Codec, node: Node, src: _Source, n: int) -> Any:
    is_map = node.assoc == "map"
    element = node.text_child
    items: List[Any] = []
    if not is_map and octet_list(node):
        signed = element.kind == "int" and element.signed
        raw = src.escaped()
        if node.bounded and len(raw) > node.count:
            _bytes("key: list exceeds its bound")
        for i in range(1, len(raw)):
            if raw[i - 1] >= raw[i]:
                _bytes(SET_ORDER)
        items = [b - 128 for b in raw] if signed else list(raw)
        post = element.bind.post if element.bind is not None else None
        if post is not None:
            items = [post(v) for v in items]
    else:
        previous: Optional[bytes] = None
        entry = node.child
        while True:
            tag = src.byte()
            if tag == 0:
                break
            if tag != 1:
                _bytes("key: invalid list element marker")
            if node.bounded and len(items) >= node.count:
                _bytes("key: list exceeds its bound")
            begin = src.p
            if is_map:
                # The entry is the tuple (key, value): no framing of its own.
                if n + 1 > MAX_DEPTH:
                    _bytes("key: node depth limit exceeded")
                k = read_key(codec, entry.members[0].text, src, n + 2)
                key = src.b[begin:src.p]
                item: Any = (k, read_key(codec, entry.members[1].text, src, n + 2))
            else:
                item = read_key(codec, element, src, n + 1)
                key = src.b[begin:src.p]
            if previous is not None and previous >= key:
                _bytes(MAP_ORDER if is_map else SET_ORDER)
            previous = key
            items.append(item)
    try:
        return codec.assoc_value(node, items)
    except DecodeError as e:
        raise DecodeError(e.reason.replace("pssz: ", "key: ")) from None


def read_key(codec: Codec, node: Node, src: _Source, n: int) -> Any:
    value = _read_key(codec, node, src, n)
    bind = node.bind
    if bind is not None and bind.post is not None:
        return bind.post(value)
    return value


def _read_key(codec: Codec, node: Node, src: _Source, n: int) -> Any:
    if n > MAX_DEPTH:
        _bytes("key: node depth limit exceeded")
    k = node.kind
    if k == "int":
        if node.bits == 1:
            _schema("key: a one-bit integer has no key encoding")
        v = int.from_bytes(src.take(node.size), "big")
        if node.signed:
            v -= 1 << (node.bits - 1)
        return v
    if k == "float":
        size = node.size
        width = size * 8
        sign = 1 << (width - 1)
        bits = int.from_bytes(src.take(size), "big")
        bits = bits ^ sign if bits & sign else ~bits & ((1 << width) - 1)
        if bits == sign:
            _bytes("key: negative zero is not canonical")
        value = struct.unpack(">f" if size == 4 else ">d", bits.to_bytes(size, "big"))[0]
        if value != value and bits != (QUIET_NAN_32 if size == 4 else QUIET_NAN_64):
            return NaN(bits, size)
        return value
    if k == "bool":
        b = src.byte()
        if b > 1:
            _bytes("key: invalid bool")
        return b == 1
    if k == "char":
        return src.byte()
    if k == "string":
        if node.child.kind == "array":
            data = src.take(node.child.count)
        else:
            data = src.escaped()
            if not _container_admits(node.child, len(data)):
                _bytes("key: string exceeds its bound")
        if node.raw:
            return data
        try:
            return data.decode("utf-8")
        except UnicodeDecodeError:
            return _bytes("key: string is not valid UTF-8")
    if k == "hex":
        if node.child.kind == "array":
            return src.take(node.child.count)
        data = src.escaped()
        if not _container_admits(node.child, len(data)):
            _bytes("key: bytes exceed their bound")
        return data
    if k == "bitvector":
        count = packed_byte_count(node.size)
        data = src.take(count)
        if node.size % 8 and data[count - 1] >> (node.size % 8):
            _bytes("key: bitvector padding bits are set")
        return Bits(data, node.size)
    if k == "bitlist":
        return _schema("key: bitlist has no key encoding")
    if k == "option":
        tag = src.byte()
        if tag == 0:
            return None
        if tag != 1:
            _bytes("key: invalid optional marker")
        inner = read_key(codec, node.text_child, src, n + 1)
        return Some(inner) if node.child.kind == "option" else inner
    if k == "nested":
        # The byte list framing; the bytes are a valid encoding of the inner
        # type (the compatible rule) unless the node is opaque.
        data = src.escaped()
        if codec.is_opaque(node):
            return data
        return codec.auto.decoder(node.text_child)(data, 0, len(data), n + 1, Ctx(True))
    if k == "list":
        if node.assoc:
            return _read_assoc(codec, node, src, n)
        element = node.text_child
        if octet_list(node):
            data = src.escaped()
            if node.bounded and len(data) > node.count:
                _bytes("key: list exceeds its bound")
            out = [b - 128 for b in data] if element.kind == "int" and element.signed else list(data)
            post = element.bind.post if element.bind is not None else None
            return [post(v) for v in out] if post is not None else out
        out = []
        while True:
            tag = src.byte()
            if tag == 0:
                return out
            if tag != 1:
                _bytes("key: invalid list element marker")
            if node.bounded and len(out) >= node.count:
                _bytes("key: list exceeds its bound")
            out.append(read_key(codec, element, src, n + 1))
    if k == "array":
        out = []
        for i in range(node.count):
            before = src.p
            out.append(read_key(codec, node.text_child, src, n + 1))
            if src.p == before and node.count - i > 1 << 20:
                _bytes("key: zero-size array count exceeds the supported limit")
        return out
    if k == "tuple":
        return tuple([read_key(codec, m.text, src, n + 1) for m in node.members])
    if k == "record":
        values = []
        for m in node.members:
            v = read_key(codec, m.text, src, n + 1)
            if m.bound is not None and not member_bound_admits(m.bound, m.node, v):
                _bytes("key: member length bound violated")
            values.append(v)
        bind = node.bind
        if bind is not None and bind.make is not None:
            return bind.make(values)
        return dict(zip([m.name for m in node.members], values))
    if k == "variant":
        index = src.byte()
        if index >= len(node.members):
            _bytes("key: variant index out of range")
        m = node.members[index]
        payload = read_key(codec, m.text, src, n + 1)
        bind = node.bind
        if bind is not None and bind.join is not None:
            return bind.join(index, payload)
        return Variant(m.name, payload)
    if k == "custom":
        return read_key(codec, node.inner, src, n)
    return _schema("key: unsupported type")


def decode_key(codec: Codec, ref: Node, data: bytes) -> Any:
    """Reads exactly one value spanning the whole buffer; canonical bytes only."""
    src = _Source(data)
    value = read_key(codec, ref, src, 0)
    if src.p != len(data):
        _bytes("key: trailing bytes")
    codec.check_value(ref, value, "bytes")
    return value
