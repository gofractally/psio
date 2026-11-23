"""Schema-driven PSSZ codec over the Schema IR (docs/pssz-spec.md).

A port of the checked runtime codec ``cpp/include/psio/pssz_ir.hpp`` (and its
Rust and JavaScript twins): the same width derivation, the same frame parser,
the same accept / reject set.  Every type is compiled once into a pair of
closures (``Codec.encoder`` / ``Codec.decoder``); the typed layer
(``psio.typed``) reuses the same compiled plan and only supplies *bindings*:
how a Python class gives up its members and is built from them.

This module is internal.  The public API is ``psio`` (``psio/__init__.py``).

The seam for a native accelerator is here: ``Codec.encoder(node)`` and
``Codec.decoder(node)`` return plain callables, so an optional extension can
replace the callables of the node kinds it implements and leave the rest.
"""

from __future__ import annotations

import struct
from typing import NoReturn, Any, Callable, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from ._time import TimeKind, fraction_digits, parse_schema_id, reserved_time_id
from .errors import DecodeError, EncodeError, PsioError, SchemaError, error_of
from .values import Bits, NaN, Some, Variant

#: ``kMaxValidationDepth``: value nodes below the root, and JSON nesting.
MAX_DEPTH = 64

# A fixed-count array of zero-size elements carries no bytes, so its count is
# not bounded by the input.  Refuse more than this many rather than loop on a
# schema-supplied number.
ZERO_SIZE_ARRAY_LIMIT = 1 << 20

U64_MAX = (1 << 64) - 1

QUIET_NAN_32 = 0x7FC00000
QUIET_NAN_64 = 0x7FF8000000000000

# Custom ids that are part of the binary form (pssz_ir.hpp "Custom ids").
# Any other id is "box" or a custom type: its inner type on the wire.
WIRE_CUSTOM_IDS = frozenset(("bool", "char", "string", "utf8", "hex", "bitvector", "bitlist", "map", "set", "unknown"))

DEPTH = "pssz: max depth exceeded"
CAP = "pssz: encoded size exceeds maxDynamicData cap"
MEMBER_BOUND = "pssz: member length_bound violated"
LIST_BOUND = "pssz: bounded list exceeds its bound"
STRING_BOUND = "pssz: utf8_string exceeds its bound"
NOT_PRUNED = "pssz: trailing absent optional field was not pruned"
MAP_ORDER = "pssz: map entries are not in strictly ascending key order"
SET_ORDER = "pssz: set elements are not in strictly ascending order"
NO_HANDLER = "pssz IR: no handler registered for a custom type"
INVARIANT = "pssz IR: invariant violated"


def reserved_custom_id(id: str) -> bool:
    """Ids a handler cannot be registered for."""
    return id == "box" or id in WIRE_CUSTOM_IDS or reserved_time_id(id)


def _schema(message: str) -> NoReturn:
    raise SchemaError(message)


def _bytes(message: str) -> NoReturn:
    raise DecodeError(message)


def _value(message: str) -> NoReturn:
    raise EncodeError(message)


class Handler:
    """What a custom type (a ``Custom`` id that is not part of the binary form) adds to its wire type.

    * ``to_text`` / ``from_text``: the text form, ONE JSON string in both text
      formats.  ``to_text`` gets the decoded value of the wire type,
      ``from_text`` returns one; either may raise for a value / text it does
      not accept.
    * ``check``: the invariant.  It gets the decoded value of the wire type
      and returns a message (or raises an exception with one) when the value
      is not a value of the type; ``None`` / ``True`` accepts it.
    """

    __slots__ = ("to_text", "from_text", "check")

    def __init__(self, to_text: Optional[Callable[[Any], str]] = None,
                 from_text: Optional[Callable[[str], Any]] = None,
                 check: Optional[Callable[[Any], Any]] = None) -> None:
        for name, f in (("to_text", to_text), ("from_text", from_text), ("check", check)):
            if f is not None and not callable(f):
                raise SchemaError(f"pssz: {name} of a custom handler must be callable")
        self.to_text = to_text
        self.from_text = from_text
        self.check = check


class LengthBound:
    __slots__ = ("exact", "max", "min")

    def __init__(self) -> None:
        self.exact: Optional[int] = None
        self.max: Optional[int] = None
        self.min: Optional[int] = None

    def admits(self, n: int) -> bool:
        return not ((self.max is not None and n > self.max) or (self.exact is not None and n != self.exact)
                    or (self.min is not None and n < self.min))

    @property
    def limit(self) -> Optional[int]:
        return self.exact if self.exact is not None else self.max


def strip(ref: "Node") -> Tuple["Node", bool]:
    """The node a reference is on the wire (wrappers looked through), and whether a "box" was on the way."""
    boxed = False
    hops = 0
    while ref.kind == "custom":
        if ref.inner is None or hops > 4096:
            _schema("pssz IR: cyclic named aliases")
        boxed = boxed or ref.boxed
        ref = ref.inner
        hops += 1
    return ref, boxed


class Member:
    """One member of a record / tuple / variant."""

    __slots__ = ("name", "text", "bound", "json_int", "ijson_int", "ijson_name", "_wire")

    def __init__(self, name: str, text: "Node", bound: Optional[LengthBound] = None, json_int: str = "natural",
                 ijson_int: str = "natural", ijson_name: str = "") -> None:
        self.name = name
        #: The member's type as written: wrappers included (the text walk).
        self.text = text
        self.bound = bound
        self.json_int = json_int
        self.ijson_int = ijson_int
        #: Variant alternative's I-JSON name.
        self.ijson_name = ijson_name
        self._wire: Optional[Tuple[Node, bool]] = None

    @property
    def node(self) -> "Node":
        """The member's type on the wire."""
        if self._wire is None:
            self._wire = strip(self.text)
        return self._wire[0]

    @property
    def boxed(self) -> bool:
        if self._wire is None:
            self._wire = strip(self.text)
        return self._wire[1]


class Node:
    """One resolved IR type under one codec."""

    __slots__ = (
        "kind", "bits", "signed", "size", "text_child", "_wire_child", "raw", "assoc", "opaque", "key_nested",
        "inner", "id", "boxed", "handler", "count", "bounded", "exact", "members", "stable", "cap", "time",
        "time_digits", "enumerators", "by_name", "layout_memo", "empty_memo", "width", "bound_memo",
        "fixed_payload_memo", "fields", "enc", "dec", "bind", "extra",
    )

    def __init__(self) -> None:
        # int | float | bool | char | string | hex | bitvector | bitlist | option | list | array |
        # tuple | record | variant | nested | custom
        self.kind = "int"
        self.bits = 0
        self.signed = False
        self.size = 0
        # option payload; list / array element; string / hex byte container, as written.
        self.text_child: Node = None
        self._wire_child: Optional[Tuple[Node, bool]] = None
        self.raw = False
        self.assoc: Optional[str] = None
        self.opaque = False
        self.key_nested: Optional[bool] = None
        self.inner: Optional[Node] = None
        self.id = ""
        self.boxed = False
        self.handler: Optional[Handler] = None
        self.count = 0
        self.bounded = False
        self.exact = False
        self.members: List[Member] = []
        self.stable = False
        self.cap: Optional[int] = None
        self.time: Optional[TimeKind] = None
        self.time_digits = -1
        self.enumerators: Optional[List[Dict[str, Any]]] = None
        self.by_name: Dict[str, int] = {}
        self.layout_memo: Optional[list] = None
        self.empty_memo: Optional[list] = None
        self.width: Optional[int] = None
        self.bound_memo: List[Any] = [False, False]
        self.fixed_payload_memo: Optional[bool] = None
        self.fields: Optional[List[Tuple[int, bool, bool]]] = None
        self.enc: Optional[Callable[[Any, int], bytes]] = None
        self.dec: Optional[Callable[[bytes, int, int, int, "Ctx"], Any]] = None
        #: The typed layer's binding of this node to a Python type, or None.
        self.bind: Any = None
        self.extra: Dict[str, Any] = {}

    @property
    def child(self) -> "Node":
        """``text_child`` on the wire."""
        if self._wire_child is None:
            self._wire_child = strip(self.text_child)
        return self._wire_child[0]

    @property
    def child_boxed(self) -> bool:
        if self._wire_child is None:
            self._wire_child = strip(self.text_child)
        return self._wire_child[1]

    @property
    def key_ref(self) -> "Node":
        """map / set: the key type as written (map: the entry's first member)."""
        return strip(self.text_child)[0].members[0].text if self.assoc == "map" else self.text_child

    @property
    def value_ref(self) -> "Node":
        """map: the mapped type as written."""
        return strip(self.text_child)[0].members[1].text


class Binding:
    """How the typed layer maps one node onto a Python type.

    Every hook is optional.  ``pre`` turns the Python value into the value the
    plain codec expects before any writer sees it; ``post`` is its inverse
    after any reader.  ``get`` / ``make`` replace the ``dict`` of a record
    (the member values in declaration order, and back), ``split`` / ``join``
    the :class:`Variant` of a variant (``(index, payload)`` and back), and
    ``assoc`` builds a map / set from its entries in canonical order.
    """

    __slots__ = ("pre", "post", "get", "make", "split", "join", "assoc", "deferred", "cls")

    def __init__(self, pre: Any = None, post: Any = None, get: Any = None, make: Any = None, split: Any = None,
                 join: Any = None, assoc: Any = None, deferred: bool = False, cls: Any = None) -> None:
        self.pre = pre
        self.post = post
        self.get = get
        self.make = make
        self.split = split
        self.join = join
        self.assoc = assoc
        self.deferred = deferred
        self.cls = cls


def _find_attribute(attributes: Sequence[Mapping[str, Any]], name: str) -> Optional[Mapping[str, Any]]:
    for a in attributes:
        if a["name"] == name:
            return a
    return None


def _parse_u64(text: str) -> Optional[int]:
    if not (text.isascii() and text.isdigit()) or (text != "0" and text[0] == "0") or len(text) > 20:
        return None
    v = int(text)
    return None if v > U64_MAX else v


def _parse_length_bound(attributes: Sequence[Mapping[str, Any]]) -> Optional[LengthBound]:
    attribute = _find_attribute(attributes, "lengthBound")
    if attribute is None:
        return None
    if not attribute["value"]:
        _schema("pssz IR: malformed lengthBound attribute")
    bound = LengthBound()
    for item in attribute["value"].split(","):
        key, eq, text = item.partition("=")
        n = _parse_u64(text) if eq else None
        if n is None or key not in ("max", "exact", "min") or getattr(bound, key) is not None:
            _schema("pssz IR: malformed lengthBound attribute")
        setattr(bound, key, n)
    return bound


def _parse_cap(attributes: Sequence[Mapping[str, Any]]) -> Optional[int]:
    attribute = _find_attribute(attributes, "maxDynamicData")
    if attribute is None:
        return None
    n = None if attribute["value"] is None else _parse_u64(attribute["value"])
    if n is None:
        _schema("pssz IR: malformed maxDynamicData attribute")
    return n


def _int_form(attributes: Sequence[Mapping[str, Any]], name: str) -> str:
    a = _find_attribute(attributes, name)
    if a is not None and a["value"] is not None:
        return "string" if a["value"] == "string" else "number"
    return "natural"


def _check_attributes(values: Sequence[Mapping[str, Any]], site: str, caps: bool) -> None:
    for a in values:
        name, value = a["name"], a["value"]
        if name in ("doc", "description", "visible"):
            continue
        if site == "record" and name == "definitionWillNotChange":
            continue
        if site == "record" and (name == "maxDynamicData" or (caps and name == "maxFields")):
            continue
        if site == "record_member" and name in ("lengthBound", "jsonInt", "ijsonInt"):
            if name != "lengthBound" and value not in ("number", "string"):
                _schema("pssz IR: malformed integer form attribute")
            continue
        if site == "variant_member" and name == "ijsonName" and value is not None:
            continue
        _schema("pssz IR: unsupported schema constraint or annotation")


def width_class(bound: Optional[int]) -> int:
    """The offset width a size bound selects (spec 3)."""
    if bound is not None and bound <= 0xFF:
        return 1
    if bound is not None and bound <= 0xFFFF:
        return 2
    return 4


def _bound_add(a: Optional[int], b: Optional[int]) -> Optional[int]:
    if a is None or b is None:
        return None
    s = a + b
    return None if s > U64_MAX else s


def _bound_multiply(count: int, value: Optional[int]) -> Optional[int]:
    if count == 0:
        return 0
    if value is None:
        return None
    p = count * value
    return None if p > U64_MAX else p


def packed_byte_count(count: int) -> int:
    return (count + 7) // 8


def utf8_reason(b: bytes) -> str:
    """Why ``b`` is not UTF-8, in the words of C++ ``utf8_spec`` (the fixtures' reasons)."""
    i = 0
    n = len(b)
    while i < n:
        c = b[i]
        if c < 0x80:
            take = 1
        elif c & 0xE0 == 0xC0:
            take = 2
        elif c & 0xF0 == 0xE0:
            take = 3
        elif c & 0xF8 == 0xF0:
            take = 4
        else:
            return "utf8_spec: invalid leading byte"
        if i + take > n:
            return "utf8_spec: truncated sequence"
        for k in range(1, take):
            if b[i + k] & 0xC0 != 0x80:
                return "utf8_spec: bad continuation"
        if take == 1:
            cp = c
        elif take == 2:
            cp = (c & 0x1F) << 6 | b[i + 1] & 0x3F
        elif take == 3:
            cp = (c & 0x0F) << 12 | (b[i + 1] & 0x3F) << 6 | b[i + 2] & 0x3F
        else:
            cp = (c & 0x07) << 18 | (b[i + 1] & 0x3F) << 12 | (b[i + 2] & 0x3F) << 6 | b[i + 3] & 0x3F
        if cp < (0, 0, 0x80, 0x800, 0x10000)[take]:
            return "utf8_spec: overlong encoding"
        if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
            return "utf8_spec: codepoint out of range"
        i += take
    return "utf8_spec: invalid leading byte"


class Ctx:
    """Per-call decode state."""

    __slots__ = ("allow_unknown", "has_unknown")

    def __init__(self, allow_unknown: bool = True) -> None:
        self.allow_unknown = allow_unknown
        self.has_unknown = False


def unwrap_option(node: Node, value: Any, fail: Callable[[str], Any]) -> Tuple[bool, Any]:
    """Splits an Option value: ``(is_some, payload)``."""
    if value is None:
        return False, None
    if node.child.kind != "option":
        return True, value
    if type(value) is not Some:
        fail("pssz: a nested optional needs None or Some(value)")
    return True, value.value


def member_bound_admits(bound: LengthBound, node: Node, value: Any) -> bool:
    """The length a member ``lengthBound`` constrains, looking through Option."""
    while node.kind == "option":
        if value is None:
            return True
        if node.child.kind == "option":
            if type(value) is not Some:
                return True
            value = value.value
        node = node.child
    # A nested member's bound counts the bytes of its encoding: checked where
    # the bytes exist (`nested_bound_admits`), not on the inner value.
    if node.kind == "nested":
        return True
    if type(value) is str:
        try:
            return bound.admits(len(value) if value.isascii() else len(value.encode("utf-8")))
        except UnicodeEncodeError:
            return True
    try:
        return bound.admits(len(value))
    except TypeError:
        return True


def nested_member(node: Node) -> bool:
    """Whether a member type is a nested value, looking through Option."""
    hops = 0
    while node.kind == "option" and hops <= MAX_DEPTH:
        node = node.child
        hops += 1
    return node.kind == "nested"


def assoc_items(value: Any) -> Optional[list]:
    """The entries of a map / the elements of a set value, as a list."""
    if isinstance(value, Mapping):
        return list(value.items())
    if isinstance(value, (str, bytes, bytearray, memoryview)) or value is None:
        return None
    try:
        return list(value)
    except TypeError:
        return None


def _hashable(ref: Node, seen: List[Node]) -> bool:
    """Whether the dynamic value of the type is hashable (a ``dict`` key / ``frozenset`` element)."""
    node = ref
    while node.kind == "custom":
        if node.bind is not None and node.bind.post is not None:
            return True  # a typed value: the typed layer vouches for it
        node = node.inner
    if node.bind is not None and (node.bind.post is not None or node.bind.make is not None
                                 or node.bind.join is not None or node.bind.assoc is not None):
        return True
    if any(node is s for s in seen):
        return False
    seen.append(node)
    try:
        k = node.kind
        if k in ("int", "float", "bool", "char", "string", "hex", "bitvector", "bitlist"):
            return True
        if k == "option":
            return _hashable(node.text_child, seen)
        if k in ("tuple", "variant"):
            return all(_hashable(m.text, seen) for m in node.members)
        if k == "list" and node.assoc == "set":
            return _hashable(node.text_child, seen)
        if k == "nested":
            return node.opaque or _hashable(node.text_child, seen)
        return False
    finally:
        seen.pop()


class _State:
    __slots__ = ("quiet", "opaque")

    def __init__(self) -> None:
        self.quiet = 0
        self.opaque = 0


class Codec:
    """The codec for one schema under one width profile.

    ``W`` = 1, 2 or 4: the explicit profile (one inherited width).  ``W`` = 0:
    the automatic profile (every record, array and list uses its own
    type-derived width).
    """

    def __init__(self, schema: Mapping[str, Any], W: int = 0, handlers: Optional[Mapping[str, Handler]] = None,
                 require_handlers: bool = False, opaque_nested: bool = False,
                 deferred: Iterable[str] = (), auto: Optional["Codec"] = None,
                 bindings: Optional[Mapping[Any, Binding]] = None) -> None:
        self.schema = schema
        self.W = W
        self.handlers: Mapping[str, Handler] = handlers or {}
        self.require_handlers = require_handlers
        self.opaque_nested = opaque_nested
        self.deferred = frozenset(deferred)
        #: name (named types) or ``id(ir type)`` (inline types) -> Binding.
        self.bindings: Mapping[Any, Binding] = bindings or {}
        self.named: Dict[str, Any] = {}
        for m in schema["types"]:
            self.named[m["name"]] = None if m["name"] in self.named else m
        self.nodes: Dict[Any, Node] = {}
        self._keep: List[Any] = []
        self.automatic = W == 0
        #: The codec of the bytes inside a Nested node: always the automatic profile.
        self.auto: Codec = self if W == 0 else (
            auto or Codec(schema, 0, handlers, require_handlers, opaque_nested, deferred, None, bindings))
        self.state: _State = _State() if W == 0 else self.auto.state
        checks = require_handlers
        for h in self.handlers.values():
            checks = checks or h.check is not None
        #: A handler has a ``check``, or every custom id needs a handler.
        self.checks_values = checks

    # ── Scoped switches ──────────────────────────────────────────────────────

    def quietly(self, run: Callable[[], Any]) -> Any:
        """Runs ``run`` with no handler consulted: the structure only."""
        s = self.state
        s.quiet += 1
        try:
            return run()
        finally:
            s.quiet -= 1

    def rawly(self, run: Callable[[], Any]) -> Any:
        """Runs ``run`` with no handler consulted and every Nested node opaque."""
        s = self.state
        s.quiet += 1
        s.opaque += 1
        try:
            return run()
        finally:
            s.quiet -= 1
            s.opaque -= 1

    def is_opaque(self, node: Node) -> bool:
        """Whether the bytes of the Nested node are not looked at."""
        return node.opaque or self.opaque_nested or self.state.opaque > 0

    # ── Building the node graph ──────────────────────────────────────────────

    def _resolve(self, input: Mapping[str, Any], via: Optional[List[str]] = None) -> Mapping[str, Any]:
        out = input
        for _ in range(len(self.schema["types"]) + 1):
            v = out["value"]
            if v.type != "Type":
                return out
            name = v.value["type"]
            if name not in self.named:
                _schema(f'pssz IR: unresolved named type "{name}"')
            found = self.named[name]
            if found is None:
                _schema("pssz IR: duplicate named type")
            if via is not None:
                via.append(found["name"])
            out = found["type"]
        return _schema("pssz IR: cyclic named aliases")

    def _direct(self, input: Mapping[str, Any]) -> Variant:
        """The type an adapter or an Enum sits on: found through named aliases only."""
        t = self._resolve(input)["value"]
        if t.type == "Enum" or (t.type == "Custom" and t.value["id"] not in WIRE_CUSTOM_IDS):
            _schema("pssz IR: an adapter or enum must sit directly on its wire type")
        return t

    def node(self, root: Mapping[str, Any]) -> Node:
        """Builds (once) the node for ``root`` and everything it reaches."""
        added: List[Any] = []
        try:
            out = self._build(root, added, 0)
            # The key type of a map / set has a key encoding: checked once the
            # graph is complete (a key type may be recursive).
            for key in added:
                node = self.nodes[key]
                if node.assoc:
                    self._has_key(node.key_ref, [])
            return out
        except RecursionError:
            for key in added:
                self.nodes.pop(key, None)
            return _schema("pssz IR: schema nesting too deep")
        except BaseException:
            for key in added:
                self.nodes.pop(key, None)
            raise

    def _build(self, input: Mapping[str, Any], added: List[Any], depth: int) -> Node:
        if depth > 512:
            _schema("pssz IR: schema nesting too deep")
        via: List[str] = []
        type_ = self._resolve(input, via)
        # The typed layer's binding of this occurrence: of the names on the
        # way (outermost first), then of the type itself.
        bindings = self.bindings
        bind = None
        if bindings:
            chain = [bindings[n] for n in via if n in bindings]
            own = bindings.get(id(input))
            if own is not None:
                chain.insert(0, own)
            if id(type_) != id(input):
                own = bindings.get(id(type_))
                if own is not None:
                    chain.append(own)
            if chain:
                bind = chain[0] if len(chain) == 1 else _compose(chain)
        key: Any = id(type_) if bind is None else (id(type_), id(bind) if len(chain) == 1 else tuple(map(id, chain)))
        known = self.nodes.get(key)
        if known is not None:
            return known
        node = Node()
        node.bind = bind
        self.nodes[key] = node
        self._keep.append(type_)
        if bind is not None:
            self._keep.append(bind)
        added.append(key)
        d = depth + 1
        t = type_["value"]
        kind, v = t.type, t.value
        if kind == "Int":
            bits, signed = v["bits"], v["isSigned"]
            node.kind = "int"
            node.bits = bits
            node.signed = signed
            if bits == 1:
                node.size = 1
            elif bits in (8, 16, 32, 64, 128):
                node.size = bits // 8
            elif bits == 256 and not signed:
                node.size = 32
            else:
                _schema("pssz IR: unsupported integer width")
        elif kind == "Enum":
            # On the wire an Enum is exactly its underlying integer.
            under = None
            try:
                under = self._direct(v["type"])
            except SchemaError:
                pass
            if under is None or under.type != "Int":
                _schema("pssz IR: an enum needs an underlying integer")
            integer = strip(self._build(v["type"], added, d))[0]
            node.kind = "int"
            node.bits = integer.bits
            node.signed = integer.signed
            node.size = integer.size
            node.enumerators = v["members"]
        elif kind == "Float":
            exp, mantissa = v["exp"], v["mantissa"]
            node.kind = "float"
            if exp == 8 and mantissa in (23, 24):
                node.size = 4
            elif exp == 11 and mantissa in (52, 53):
                node.size = 8
            else:
                _schema("pssz IR: unsupported IEEE float width")
        elif kind == "Option":
            node.kind = "option"
            node.text_child = self._build(v["type"], added, d)
        elif kind == "List":
            node.kind = "list"
            node.text_child = self._build(v["type"], added, d)
        elif kind in ("BoundedList", "Array"):
            node.kind = "array" if kind == "Array" else "list"
            node.bounded = True
            node.exact = kind == "Array"
            node.count = v["len"] if kind == "Array" else v["maxCount"]
            node.text_child = self._build(v["type"], added, d)
        elif kind == "Tuple":
            node.kind = "tuple"
            node.members = [Member(str(i), self._build(m, added, d)) for i, m in enumerate(v["members"])]
        elif kind in ("Struct", "Object", "Variant"):
            record = kind != "Variant"
            node.kind = "record" if record else "variant"
            _check_attributes(v["attributes"], "record" if record else "variant", self.automatic)
            if record:
                node.stable = kind == "Struct" or any(a["name"] == "definitionWillNotChange" for a in v["attributes"])
                node.cap = _parse_cap(v["attributes"])
            elif not v["members"] or len(v["members"]) > 256:
                _schema("pssz IR: invalid variant schema")
            seen = set()
            for m in v["members"]:
                if m["name"] in seen:
                    _schema("pssz IR: duplicate schema member")
                seen.add(m["name"])
                _check_attributes(m["attributes"], "record_member" if record else "variant_member", self.automatic)
                name = None if record else _find_attribute(m["attributes"], "ijsonName")
                node.members.append(Member(
                    m["name"], self._build(m["type"], added, d),
                    _parse_length_bound(m["attributes"]) if record else None,
                    _int_form(m["attributes"], "jsonInt") if record else "natural",
                    _int_form(m["attributes"], "ijsonInt") if record else "natural",
                    name["value"] if name is not None and name["value"] is not None else m["name"],
                ))
            node.by_name = {m.name: i for i, m in enumerate(node.members)}
        elif kind == "Custom":
            self._build_custom(node, v, added, d)
        elif kind == "Nested":
            # A byte list holding the encoding of the inner type, which is
            # always in the automatic profile: its nodes are that codec's.
            node.kind = "nested"
            node.opaque = bool(via) and via[-1] in self.deferred or bool(bind is not None and bind.deferred)
            node.text_child = self._build(v["type"], added, d) if self.automatic else self.auto.node(v["type"])
        else:
            _schema("pssz IR: unsupported schema type")
        return node

    def _build_custom(self, node: Node, v: Mapping[str, Any], added: List[Any], d: int) -> None:
        id = v["id"]
        time = parse_schema_id(id)
        if time is not None:
            # A built-in presentation: its tick integer on the wire, int ticks
            # as a value, and the text form of docs/time.md.
            ticks = self._direct(v["type"])
            bits = ticks.value["bits"] if ticks.type == "Int" else 0
            if bits not in (8, 16, 32, 64):
                _schema("pssz IR: time value requires an 8/16/32/64-bit tick integer")
            node.kind = "int"
            node.bits = bits
            node.signed = ticks.value["isSigned"]
            node.size = bits // 8
            node.time = time
            node.time_digits = fraction_digits(time.num, time.den) if time.point else -1
            return
        if reserved_time_id(id):
            _schema("pssz IR: unsupported custom adapter")
        if id in ("map", "set"):
            # A list whose entries are in strictly ascending key order.
            is_map = id == "map"
            shape = ("pssz IR: a map must sit directly on a list of two-member tuples" if is_map
                     else "pssz IR: a set must sit directly on a list")
            lst = self._direct(v["type"])
            if lst.type not in ("List", "BoundedList"):
                _schema(shape)
            if is_map:
                entry = self._resolve(lst.value["type"])["value"]
                if entry.type != "Tuple" or len(entry.value["members"]) != 2:
                    _schema(shape)
            node.kind = "list"
            node.assoc = id
            if lst.type == "BoundedList":
                node.bounded = True
                node.count = lst.value["maxCount"]
            node.text_child = self._build(lst.value["type"], added, d)
            return
        if id not in WIRE_CUSTOM_IDS:
            # "box" or a custom type: the inner type on the wire.
            node.kind = "custom"
            node.id = id
            node.boxed = id == "box"
            if not node.boxed:
                node.handler = self.handlers.get(id)
            node.inner = self._build(v["type"], added, d)
            return
        inner = self._direct(v["type"])

        def unsigned(x: Variant, bits: int) -> bool:
            return x.type == "Int" and x.value["bits"] == bits and not x.value["isSigned"]

        if id in ("bool", "char"):
            if not unsigned(inner, 1 if id == "bool" else 8):
                _schema("pssz IR: primitive adapter has incompatible inner schema")
            node.kind = id
            node.size = 1
        elif id in ("bitvector", "bitlist"):
            if id == "bitvector":
                if inner.type != "Array":
                    _schema("pssz IR: bitvector requires array bit metadata")
                element, bound = inner.value["type"], inner.value["len"]
            else:
                if inner.type != "BoundedList":
                    _schema("pssz IR: bitlist requires bounded bit metadata")
                element, bound = inner.value["type"], inner.value["maxCount"]
            if not unsigned(self._direct(element), 1):
                _schema("pssz IR: packed bit element must be unsigned one-bit integer")
            if bound > (1 << 53) - 1:
                _schema("pssz IR: bit count exceeds platform capacity")
            node.kind = id
            node.size = bound
        elif id in ("string", "utf8", "hex"):
            if inner.type not in ("List", "BoundedList", "Array"):
                _schema("pssz IR: byte adapter requires byte container")
            element = self._direct(inner.value["type"])
            if element.type == "Custom" and element.value["id"] == "char":
                if not unsigned(self._direct(element.value["type"]), 8):
                    _schema("pssz IR: primitive adapter has incompatible inner schema")
            elif not unsigned(element, 8):
                _schema("pssz IR: byte adapter requires unsigned bytes")
            # "string" is raw bytes; "utf8" is validated on every read and write.
            node.kind = "hex" if id == "hex" else "string"
            node.raw = id == "string"
        else:
            _schema("pssz IR: unsupported custom adapter")
        node.text_child = self._build(v["type"], added, d)

    def _has_key(self, ref: Node, active: List[Node]) -> None:
        """Whether ``ref`` has a ``key`` encoding (docs/key-spec.md); a schema error otherwise."""
        none = "pssz IR: the key type of a map / set has no key encoding"
        node, boxed = strip(ref)
        if boxed:
            _schema(none)
        if any(node is a for a in active):
            return
        if len(active) > MAX_DEPTH:
            _schema("pssz IR: node depth limit exceeded")
        active.append(node)
        k = node.kind
        if k == "int":
            if node.bits == 1:
                _schema(none)
        elif k == "bitlist":
            _schema(none)
        elif k in ("option", "array"):
            self._has_key(node.text_child, active)
        elif k == "list":
            if not node.assoc:
                self._has_key(node.text_child, active)
            elif node.assoc == "map":
                self._has_key(node.value_ref, active)
        elif k in ("tuple", "record", "variant"):
            for m in node.members:
                self._has_key(m.text, active)
        active.pop()

    def key_has_nested(self, node: Node) -> bool:
        """Whether a Nested node occurs in (a part of) the key type of a map / set."""
        if node.key_nested is not None:
            return node.key_nested
        seen: List[Node] = []

        def walk(ref: Node) -> bool:
            at = strip(ref)[0]
            if any(at is s for s in seen):
                return False
            seen.append(at)
            if at.kind == "nested":
                return True
            if at.kind in ("option", "list", "array"):
                return walk(at.text_child)
            if at.kind in ("tuple", "record", "variant"):
                return any(walk(m.text) for m in at.members)
            return False

        node.key_nested = walk(node.key_ref)
        return node.key_nested

    # ── Layout ───────────────────────────────────────────────────────────────

    # A memo is [value, error, height]: the result together with the schema
    # recursion it needs, so a schema too deep for C++ fails here as well, and
    # one that recurses forever through fixed layouts is an error, not a hang.
    def _memoized(self, node: Node, slot: str, compute: Callable[[], Tuple[Any, int]]) -> Any:
        memo = getattr(node, slot)
        if memo is None:
            setattr(node, slot, [None, "pssz IR: node depth limit exceeded", 0])
            try:
                value, height = compute()
                memo = [None, "pssz IR: node depth limit exceeded", height] if height > MAX_DEPTH else [value, None, height]
            except PsioError as e:
                memo = [None, e.reason, 0]
            setattr(node, slot, memo)
        if memo[1] is not None:
            _schema(memo[1])
        return memo[0]

    def layout(self, node: Node) -> Tuple[bool, int, bool]:
        """``(fixed, size, optional)`` of a wire node."""

        def compute() -> Tuple[Tuple[bool, int, bool], int]:
            def child(n: Node) -> Tuple[Tuple[bool, int, bool], int]:
                value = self.layout(n)
                return value, n.layout_memo[2] + 1

            k = node.kind
            if k in ("int", "float", "bool", "char"):
                return (True, node.size, False), 0
            if k == "bitvector":
                return (True, packed_byte_count(node.size), False), 0
            if k == "option":
                return (False, 0, True), 0
            if k == "array":
                value, height = child(node.child)
                return (value[0], value[1] * node.count if value[0] else 0, False), height
            if k in ("string", "hex"):
                return child(node.child)
            if k == "record":
                if not node.stable:
                    return (False, 0, False), 0
                fixed, size, height = True, 0, 0
                for m in node.members:
                    c, h = child(m.node)
                    fixed = fixed and c[0]
                    size += c[1]
                    height = max(height, h)
                return (fixed, size if fixed else 0, False), height
            return (False, 0, False), 0

        return self._memoized(node, "layout_memo", compute)

    def can_be_empty(self, node: Node) -> bool:
        """Whether a value of the type can encode to zero bytes (spec 5.5)."""

        def compute() -> Tuple[bool, int]:
            k = node.kind
            if k in ("list", "nested"):
                return True, 0
            if k == "option":
                value = not self.can_be_empty(node.child)
                return value, node.child.empty_memo[2] + 1
            if k == "array":
                if node.count == 0:
                    return True, 0
                info = self.layout(node.child)
                return info[0] and info[1] == 0, node.child.layout_memo[2] + 1
            if k == "record":
                if not node.stable:
                    return False, 0
                info = self.layout(node)
                return info[0] and info[1] == 0, node.layout_memo[2]
            if k == "bitvector":
                return node.size == 0, 0
            if k in ("string", "hex"):
                value = self.can_be_empty(node.child)
                return value, node.child.empty_memo[2] + 1
            return False, 0

        return self._memoized(node, "empty_memo", compute)

    # ── Width derivation (spec 3) ────────────────────────────────────────────

    #: Bytes one offset slot or header reserves in a size bound (spec 3.2):
    #: the bound selects W, so it cannot depend on W.
    SLOT = 4

    def _fixed_payload(self, node: Node, active: List[Node]) -> bool:
        if any(node is a for a in active):
            return False
        if node.fixed_payload_memo is not None:
            return node.fixed_payload_memo
        active.append(node)
        out = False
        k = node.kind
        if k in ("int", "float", "bool", "char"):
            out = True
        elif k == "array":
            out = self._fixed_payload(node.child, active)
        elif k in ("string", "hex"):
            if node.child.kind == "array":
                out = self._fixed_payload(node.child, active)
        elif k == "record":
            if node.stable:
                out = True
                for m in node.members:
                    out = self._fixed_payload(m.node, active) and out
        active.pop()
        if not active:
            node.fixed_payload_memo = out
        return out

    def bound_of(self, node: Node, annotation: Optional[LengthBound], reserve_cap: bool,
                 active: List[Node]) -> Optional[int]:
        """``max_encoded_size`` (spec 3.2); ``None`` when unbounded."""
        if any(node is a for a in active):
            return None
        if reserve_cap and node.cap is not None:
            return node.cap
        slot = 1 if reserve_cap else 0
        if annotation is None and node.bound_memo[slot] is not False:
            return node.bound_memo[slot]
        active.append(node)
        SLOT = self.SLOT

        def elements(child: Node, boxed: bool, count: int) -> Optional[int]:
            if count == 0:
                return 0
            # A boxed value has no inferred bound (the typed codecs give it none).
            element = None if boxed else self.bound_of(child, None, True, active)
            if not self._fixed_payload(child, []):
                element = _bound_add(element, SLOT)
            return _bound_multiply(count, element)

        def sequence(container: Node) -> Optional[int]:
            if container.kind == "list" and not container.bounded:
                count = annotation.limit if annotation is not None else None
                return elements(container.child, container.child_boxed, count) if count is not None else None
            return elements(container.child, container.child_boxed, container.count)

        out: Optional[int] = None
        k = node.kind
        if k in ("int", "float", "bool", "char"):
            out = node.size
        elif k in ("list", "array"):
            out = sequence(node)
        elif k == "option":
            # One selector byte is reserved for every optional.
            out = None if node.child_boxed else _bound_add(self.bound_of(node.child, annotation, True, active), 1)
        elif k in ("string", "hex"):
            out = None if node.child_boxed else sequence(node.child)
        elif k in ("record", "tuple"):
            # A tuple is the extensible positional record of its elements.
            total: Optional[int] = 0 if (k == "record" and node.stable) else SLOT
            for m in node.members:
                payload = None if m.boxed else self.bound_of(m.node, m.bound if k == "record" else None, True, active)
                if not self._fixed_payload(m.node, []):
                    payload = _bound_add(payload, SLOT)
                total = _bound_add(total, payload)
            out = total
        elif k == "nested":
            # No bound of its own; a member lengthBound bounds its bytes.
            out = annotation.limit if annotation is not None else None
        elif k == "variant":
            # One selector byte, then the widest alternative's payload.
            widest: Optional[int] = 0
            for m in node.members:
                payload = None if m.boxed else self.bound_of(m.node, None, True, active)
                if payload is None or widest is None:
                    widest = None
                elif payload > widest:
                    widest = payload
            out = _bound_add(widest, 1)
        active.pop()
        if annotation is None:
            node.bound_memo[slot] = out
        return out

    def width_of(self, node: Node) -> int:
        """Width of a record's header and slots, or of a list / array offset table.

        THE one place that decides an offset width.  A list's own table is
        handled by :meth:`_list_table_width`.
        """
        if not self.automatic:
            return self.W
        if node.width is not None:
            return node.width
        if node.kind == "list":
            out = self._list_table_width(node)
        elif node.cap is not None:
            out = width_class(node.cap)
        else:
            out = width_class(self.bound_of(node, None, False, []))
        node.width = out
        return out

    def _list_table_width(self, node: Node) -> int:
        """The width of a list's own offset table under the automatic profile (spec 3.1, 5.3.2).

        The single home of the list-table rule: today a list's table is 4
        wide, bounded or not.
        """
        return 4

    def over_cap(self, node: Node, size: int) -> bool:
        return self.automatic and node.cap is not None and size > node.cap

    def check_top_cap(self, root: Node, size: int, fail: Callable[[str], Any]) -> None:
        """Explicit profiles: the cap of the top-level type bounds the whole encoding."""
        if not self.automatic and root.cap is not None and size > root.cap:
            fail(CAP)

    def encode_top(self, root: Node, value: Any) -> bytes:
        out = self.encoder(root)(value, 0)
        self.check_top_cap(strip(root)[0], len(out), _value)
        return out

    def decode_top(self, root: Node, data: bytes, ctx: Ctx) -> Any:
        self.check_top_cap(strip(root)[0], len(data), _bytes)
        return self.decoder(root)(data, 0, len(data), 0, ctx)

    # ── Custom types: handlers on the binary entry points ────────────────────

    def check_custom(self, node: Node, value: Any, kind: str) -> None:
        """Runs the handler of a custom node on a value of it; raises an error of ``kind``."""
        if node.boxed or self.state.quiet > 0:
            return
        handler = node.handler
        if handler is None:
            if self.require_handlers:
                raise error_of(kind, NO_HANDLER)
            return
        if handler.check is None:
            return
        try:
            verdict = handler.check(value)
        except PsioError as e:
            verdict = e.reason or INVARIANT
        except Exception as e:  # the invariant's own exception: its message
            verdict = str(e) or INVARIANT
        if verdict is None or verdict is True:
            return
        if verdict is False or verdict == "":
            verdict = INVARIANT
        raise error_of(kind, str(verdict), invariant=True)

    def canonical(self, node: Node, value: Any) -> Any:
        """``value`` as ``decode`` gives it: encoded and read back with no handler consulted."""
        def run() -> Any:
            data = self.encoder(node)(value, 0)
            return self.decoder(node)(data, 0, len(data), 0, Ctx(False))
        return self.quietly(run)

    def check_value(self, ref: Node, value: Any, kind: str) -> None:
        """Runs the checks of the custom nodes ``value`` is a value of; an error of ``kind``."""
        if not self.checks_values:
            return
        try:
            self.encoder(ref)(value, 0)
        except PsioError as e:
            if e.kind == kind:
                raise
            raise error_of(kind, e.reason, invariant=e.invariant) from None

    # ── Maps and sets ────────────────────────────────────────────────────────

    def _assoc_keys(self, node: Node, items: Sequence[Any]) -> List[bytes]:
        from ._key import encode_key
        key = node.key_ref
        s = self.state
        s.quiet += 1
        try:
            if node.assoc != "map":
                return [encode_key(self, key, item) for item in items]
            out = []
            for item in items:
                if not isinstance(item, (tuple, list)) or len(item) != 2:
                    _value("pssz: a map entry is a (key, value) pair")
                out.append(encode_key(self, key, item[0]))
            return out
        finally:
            s.quiet -= 1

    def assoc_order(self, node: Node, items: Sequence[Any]) -> Optional[List[int]]:
        """Indices into ``items``, ascending by key bytes; ``None`` when two entries have equal key bytes."""
        keys = self._assoc_keys(node, items)
        order = sorted(range(len(items)), key=keys.__getitem__)
        for i in range(1, len(order)):
            if keys[order[i - 1]] == keys[order[i]]:
                return None
        return order

    def assoc_sorted(self, node: Node, value: Any, fail: Callable[[str], Any], prefix: str = "pssz") -> list:
        """The entries / elements of a map / set value in canonical order."""
        items = assoc_items(value)
        if items is None:
            fail(f"{prefix}: sequence value required")
        order = self.assoc_order(node, items)
        if order is None:
            fail(f"{prefix}: map has duplicate keys" if node.assoc == "map" else f"{prefix}: set has duplicate elements")
        return [items[i] for i in order]

    def assoc_value(self, node: Node, items: list) -> Any:
        """The map / set value of a node from its entries / elements in canonical order."""
        make = node.extra.get("assoc")
        if make is None:
            bind = node.bind
            if bind is not None and bind.assoc is not None:
                make = bind.assoc
            elif not _hashable(node.key_ref, []):
                make = list
            else:
                make = dict if node.assoc == "map" else frozenset
            node.extra["assoc"] = make
        out = make(items)
        # Distinct key bytes are distinct Python keys; never a silent loss.
        if make is not list and len(out) != len(items):
            _bytes("pssz: map container merged distinct keys" if node.assoc == "map"
                   else "pssz: set container merged distinct elements")
        return out

    def nested_bound_admits(self, bound: LengthBound, node: Node, data: bytes, start: int, end: int) -> bool:
        """A nested member's ``lengthBound`` (through Option) against its encoded form."""
        while node.kind == "option":
            node = node.child
            selector = self.can_be_empty(node)
            if start == end or (selector and data[start] == 0):
                return True
            if selector:
                start += 1
        return bound.admits(end - start)

    def field_layouts(self, node: Node) -> List[Tuple[int, bool, bool]]:
        """Per member: ``(fixed_size, variable, optional)`` under this node's width."""
        if node.fields is None:
            w = self.width_of(node)
            out = []
            for m in node.members:
                fixed, size, optional = self.layout(m.node)
                out.append((size if fixed else w, not fixed, optional))
            node.fields = out
        return node.fields

    # ── Encode ───────────────────────────────────────────────────────────────

    def encoder(self, node: Node) -> Callable[[Any, int], bytes]:
        """The compiled writer of ``node``: ``(value, depth) -> bytes``."""
        f = node.enc
        if f is not None:
            return f
        if "enc_busy" in node.extra:
            # A recursive type: forward to the closure once it exists.
            return lambda value, n: node.enc(value, n)
        node.extra["enc_busy"] = True
        try:
            f = self._compile_encoder(node)
            bind = node.bind
            if bind is not None and bind.pre is not None:
                raw, pre = f, bind.pre
                f = lambda value, n: raw(pre(value), n)  # noqa: E731
            node.enc = f
        finally:
            del node.extra["enc_busy"]
        return f

    def _compile_encoder(self, node: Node) -> Callable[[Any, int], bytes]:
        return getattr(self, "_enc_" + node.kind)(node)

    def _enc_int(self, node: Node) -> Callable[[Any, int], bytes]:
        size, signed, bits = node.size, node.signed, node.bits
        if bits == 1:
            lo, hi = (-1, 0) if signed else (0, 1)

            def enc_bit(value: Any, n: int) -> bytes:
                if n > MAX_DEPTH:
                    _value(DEPTH)
                if type(value) not in (int, bool):
                    _value("pssz: integer value required")
                if not lo <= value <= hi:
                    _value("pssz: integer overflow")
                return b"\xff" if value == -1 else b"\x01" if value else b"\x00"
            return enc_bit

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            try:
                return value.to_bytes(size, "little", signed=signed)
            except OverflowError:
                return _value("pssz: signed integer overflow" if signed else "pssz: unsigned integer overflow")
            except AttributeError:
                return _value("pssz: integer value required")
        return enc

    def _enc_float(self, node: Node) -> Callable[[Any, int], bytes]:
        size = node.size
        pack = struct.Struct("<f" if size == 4 else "<d").pack
        quiet = QUIET_NAN_32.to_bytes(4, "little") if size == 4 else QUIET_NAN_64.to_bytes(8, "little")

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            t = type(value)
            if t is float or t is int:
                if value != value:
                    return quiet
                try:
                    return pack(value)
                except (OverflowError, struct.error):
                    # Out of the float's range: it rounds to an infinity.
                    return pack(float("inf") if value > 0 else float("-inf"))
            if t is NaN:
                if value.size != size:
                    _value("pssz: NaN bits of the wrong float width")
                return value.bits.to_bytes(size, "little")
            if isinstance(value, float):
                return quiet if value != value else pack(value)
            return _value("pssz: float value required")
        return enc

    def _enc_bool(self, node: Node) -> Callable[[Any, int], bytes]:
        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if value is True:
                return b"\x01"
            if value is False:
                return b"\x00"
            return _value("pssz: bool value required")
        return enc

    def _enc_char(self, node: Node) -> Callable[[Any, int], bytes]:
        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if type(value) is not int or not 0 <= value <= 255:
                _value("pssz: char byte value required")
            return bytes((value,))
        return enc

    def _byte_bound(self, container: Node, text: bool) -> Callable[[int, Callable[[str], Any]], None]:
        """The check that the byte container under a string / hex adapter admits a byte count."""
        if container.kind == "array":
            count = container.count

            def exact(n: int, fail: Callable[[str], Any]) -> None:
                if n != count:
                    fail("pssz: fixed primitive span mismatch" if fail is _bytes
                         else "pssz: byte array count mismatch")
            return exact
        if container.bounded:
            limit = container.count
            message = STRING_BOUND if text else LIST_BOUND

            def bounded(n: int, fail: Callable[[str], Any]) -> None:
                if n > limit:
                    fail(message)
            return bounded
        return lambda n, fail: None

    def _enc_string(self, node: Node) -> Callable[[Any, int], bytes]:
        raw = node.raw
        container = node.child
        check = self._byte_bound(container, True) if container.kind == "array" or container.bounded else None

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if type(value) is str:
                try:
                    data = value.encode("utf-8")
                except UnicodeEncodeError:
                    return _value("pssz: string is not valid UTF-8")
            elif raw and isinstance(value, (bytes, bytearray, memoryview)):
                data = bytes(value)
            else:
                return _value("pssz: raw string value (bytes or str) required" if raw else "pssz: str value required")
            if check is not None:
                check(len(data), _value)
            return data
        return enc

    def _enc_hex(self, node: Node) -> Callable[[Any, int], bytes]:
        container = node.child
        check = self._byte_bound(container, False) if container.kind == "array" or container.bounded else None

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if type(value) is not bytes:
                if not isinstance(value, (bytes, bytearray, memoryview)):
                    return _value("pssz: bytes value required")
                value = bytes(value)
            if check is not None:
                check(len(value), _value)
            if value and n + 1 > MAX_DEPTH:
                _value(DEPTH)
            return value
        return enc

    def _enc_bits(self, node: Node) -> Callable[[Any, int], bytes]:
        is_list = node.kind == "bitlist"
        size = node.size

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if type(value) is not Bits:
                return _value("pssz: explicit packed Bits value required")
            count, data = value.bit_count, value.data
            if type(count) is not int or count < 0 or (count > size if is_list else count != size):
                _value("pssz: packed bit logical count mismatch")
            if len(data) != packed_byte_count(count):
                _value("pssz: packed bit byte count mismatch")
            if count % 8 and data[-1] >> (count % 8):
                _value("pssz: packed bits above logical count")
            if not is_list:
                return bytes(data)
            out = bytearray(count // 8 + 1)
            out[:len(data)] = data
            out[-1] |= 1 << (count % 8)
            return bytes(out)
        return enc

    _enc_bitvector = _enc_bits
    _enc_bitlist = _enc_bits

    def _enc_option(self, node: Node) -> Callable[[Any, int], bytes]:
        selector = self.can_be_empty(node.child)
        inner = self.encoder(node.text_child)
        nested = node.child.kind == "option"
        none = b"\x00" if selector else b""

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if value is None:
                return none
            if nested:
                if type(value) is not Some:
                    _value("pssz: a nested optional needs None or Some(value)")
                value = value.value
            out = inner(value, n + 1)
            return b"\x01" + out if selector else out
        return enc

    def record_getter(self, node: Node) -> Callable[[Any], Sequence[Any]]:
        """The member values of a record value, in declaration order (an absent optional: None)."""
        get = node.extra.get("get")
        if get is None:
            get = node.extra["get"] = self._record_getter(node)
        return get

    def variant_splitter(self, node: Node) -> Callable[[Any], Tuple[int, Any]]:
        """``(index, payload)`` of a variant value."""
        split = node.extra.get("split")
        if split is None:
            split = node.extra["split"] = self._variant_splitter(node)
        return split

    def _record_getter(self, node: Node) -> Callable[[Any], Sequence[Any]]:
        bind = node.bind
        if bind is not None and bind.get is not None:
            return bind.get
        names = [m.name for m in node.members]
        optional = [self.layout(m.node)[2] for m in node.members]
        by_name = node.by_name
        pairs = list(zip(names, optional))

        def get(value: Any) -> Sequence[Any]:
            if not isinstance(value, Mapping):
                _value("pssz: record value (a dict) required")
            out = []
            found = 0
            for name, opt in pairs:
                try:
                    out.append(value[name])
                    found += 1
                except KeyError:
                    if not opt:
                        _value(f'pssz: missing required record value "{name}"')
                    out.append(None)
            if found != len(value):
                for key in value:
                    if key not in by_name:
                        _value(f'pssz: unknown record value "{key}"')
            return out
        return get

    def _enc_record(self, node: Node) -> Callable[[Any, int], bytes]:
        members = node.members
        count = len(members)
        w = self.width_of(node)
        stable = node.stable
        layouts = [self.layout(m.node) for m in members]
        optional = [info[2] for info in layouts]
        fixed = [info[0] for info in layouts]
        encs = [self.encoder(m.text) for m in members]
        # prefix[k]: the fixed region of the first k fields.
        prefix = [0]
        for info in layouts:
            prefix.append(prefix[-1] + (info[1] if info[0] else w))
        get = self.record_getter(node)
        bounds = [(i, m.bound, m.node, nested_member(m.node)) for i, m in enumerate(members) if m.bound is not None]
        cap = node.cap if self.automatic else None
        plan = list(zip(encs, fixed))
        all_fixed = all(fixed)
        nested_bound_admits = self.nested_bound_admits

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            values = get(value)
            keep = count
            if not stable:
                while keep and optional[keep - 1] and values[keep - 1] is None:
                    keep -= 1
            for i, bound, mnode, nested in bounds:
                if not nested and not member_bound_admits(bound, mnode, values[i]):
                    _value(MEMBER_BOUND)
            n1 = n + 1
            if all_fixed:
                parts = [e(v, n1) for (e, _), v in zip(plan, values)]
                out = b"".join(parts) if stable else prefix[keep].to_bytes(w, "little") + b"".join(parts)
            else:
                off = prefix[keep]
                try:
                    head = [] if stable else [off.to_bytes(w, "little")]
                    tail = []
                    for i in range(keep):
                        e, is_fixed = plan[i]
                        part = e(values[i], n1)
                        if is_fixed:
                            head.append(part)
                        else:
                            head.append(off.to_bytes(w, "little"))
                            tail.append(part)
                            off += len(part)
                except OverflowError:
                    return _value("pssz: value exceeds the static offset width")
                if bounds:
                    j = 0
                    for i in range(keep):
                        if not plan[i][1]:
                            m = members[i]
                            if m.bound is not None and nested_member(m.node) and not nested_bound_admits(
                                    m.bound, m.node, tail[j], 0, len(tail[j])):
                                _value(MEMBER_BOUND)
                            j += 1
                out = b"".join(head) + b"".join(tail)
            if cap is not None and len(out) > cap:
                _value(CAP)
            return out
        return enc

    def _enc_tuple(self, node: Node) -> Callable[[Any, int], bytes]:
        members = node.members
        count = len(members)
        w = self.width_of(node)
        layouts = [self.layout(m.node) for m in members]
        optional = [info[2] for info in layouts]
        encs = [self.encoder(m.text) for m in members]
        prefix = [0]
        for info in layouts:
            prefix.append(prefix[-1] + (info[1] if info[0] else w))
        plan = list(zip(encs, [info[0] for info in layouts]))

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if not isinstance(value, (tuple, list)) or len(value) != count:
                _value("pssz: tuple value count mismatch")
            keep = count
            while keep and optional[keep - 1] and value[keep - 1] is None:
                keep -= 1
            n1 = n + 1
            off = prefix[keep]
            try:
                head = [off.to_bytes(w, "little")]
                tail = []
                for i in range(keep):
                    e, is_fixed = plan[i]
                    part = e(value[i], n1)
                    if is_fixed:
                        head.append(part)
                    else:
                        head.append(off.to_bytes(w, "little"))
                        tail.append(part)
                        off += len(part)
            except OverflowError:
                return _value("pssz: value exceeds the static offset width")
            return b"".join(head) + b"".join(tail)
        return enc

    def _sequence_writer(self, node: Node) -> Callable[[Sequence[Any], int], bytes]:
        """Writes the elements of a list / array / map / set in the order given."""
        w = self.width_of(node)
        fixed, size, _ = self.layout(node.child)
        inner = self.encoder(node.text_child)
        bounded, exact, limit = node.bounded, node.exact, node.count

        def write(items: Sequence[Any], n: int) -> bytes:
            count = len(items)
            if bounded and (count != limit if exact else count > limit):
                _value("pssz: array length does not match the schema" if exact else LIST_BOUND)
            n1 = n + 1
            if fixed:
                if not exact and not size and count:
                    _value("pssz: nonempty zero-size vector is unsupported")
                return b"".join([inner(item, n1) for item in items])
            parts = [inner(item, n1) for item in items]
            off = count * w
            table = []
            try:
                for part in parts:
                    table.append(off.to_bytes(w, "little"))
                    off += len(part)
            except OverflowError:
                return _value("pssz: value exceeds the static offset width")
            return b"".join(table) + b"".join(parts)
        return write

    def _enc_list(self, node: Node) -> Callable[[Any, int], bytes]:
        write = self._sequence_writer(node)
        if node.assoc:
            assoc_sorted = self.assoc_sorted

            def enc_assoc(value: Any, n: int) -> bytes:
                if n > MAX_DEPTH:
                    _value(DEPTH)
                return write(assoc_sorted(node, value, _value), n)
            return enc_assoc

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if type(value) is not list and not isinstance(value, (list, tuple)):
                _value("pssz: sequence value (a list) required")
            return write(value, n)
        return enc

    _enc_array = _enc_list

    def _variant_splitter(self, node: Node) -> Callable[[Any], Tuple[int, Any]]:
        bind = node.bind
        if bind is not None and bind.split is not None:
            return bind.split
        by_name = node.by_name

        def split(value: Any) -> Tuple[int, Any]:
            if type(value) is not Variant:
                _value("pssz: a variant value is Variant(type, value)")
            try:
                return by_name[value.type], value.value
            except (KeyError, TypeError):
                return _value(f'pssz: unknown variant alternative "{value.type}"')
        return split

    def _enc_variant(self, node: Node) -> Callable[[Any, int], bytes]:
        split = self.variant_splitter(node)
        encs = [self.encoder(m.text) for m in node.members]
        tags = [bytes((i,)) for i in range(len(encs))]

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            index, payload = split(value)
            return tags[index] + encs[index](payload, n + 1)
        return enc

    def _enc_nested(self, node: Node) -> Callable[[Any, int], bytes]:
        auto = self.auto
        is_opaque = self.is_opaque
        text_child = node.text_child

        def enc(value: Any, n: int) -> bytes:
            if n > MAX_DEPTH:
                _value(DEPTH)
            if is_opaque(node):
                if not isinstance(value, (bytes, bytearray, memoryview)):
                    _value("pssz: bytes value required for an opaque nested value")
                if value and n + 1 > MAX_DEPTH:
                    _value(DEPTH)
                return bytes(value)
            # The inner value's canonical encoding, always in the automatic
            # profile; its root is one node level below the nested node.
            return auto.encoder(text_child)(value, n + 1)
        return enc

    def _enc_custom(self, node: Node) -> Callable[[Any, int], bytes]:
        inner_node: Node = node.inner
        inner = self.encoder(inner_node)
        handler = node.handler
        checks = not node.boxed and ((handler is not None and handler.check is not None)
                                     or (handler is None and self.require_handlers))
        if not checks:
            return inner
        state = self.state
        has_check = handler is not None and handler.check is not None
        decoder = self.decoder
        check_custom = self.check_custom

        def enc(value: Any, n: int) -> bytes:
            # The values inside were checked as they were written; then this
            # one, as `decode` gives it.
            out = inner(value, n)
            if state.quiet == 0:
                held = None
                if has_check:
                    state.quiet += 1
                    try:
                        held = decoder(inner_node)(out, 0, len(out), n, Ctx(False))
                    finally:
                        state.quiet -= 1
                check_custom(node, held, "value")
            return out
        return enc

    # ── Decode ───────────────────────────────────────────────────────────────

    def record_spans(self, data: bytes, begin: int, end: int, w: int, stable: bool,
                     fields: Sequence[Tuple[int, bool, bool]]) -> Tuple[List[int], List[int], List[bool], bool]:
        """A port of ``pssz_record_spans``: per field ``[begin, end)`` and whether the writer stored it."""
        count = len(fields)
        begins = [0] * count
        ends = [0] * count
        present = [False] * count
        known_size = 0
        for f in fields:
            known_size += f[0]
        fixed_start = begin
        fixed_size = known_size
        if not stable:
            if end - begin < w:
                _bytes("pssz: record header truncated")
            fixed_size = int.from_bytes(data[begin:begin + w], "little")
            fixed_start += w
        if fixed_size > end - fixed_start:
            _bytes("pssz: record fixed region truncated")
        if fixed_size > known_size and (fixed_size - known_size) % w != 0:
            _bytes("pssz: partial unknown optional slot")
        unknown = fixed_size > known_size
        payload_size = end - fixed_start
        previous_field = count
        previous_offset = fixed_size
        seen_variable = False
        cursor = 0
        missing = False
        for i in range(count):
            size, variable, optional = fields[i]
            if missing or size > fixed_size - cursor:
                if cursor != fixed_size:
                    _bytes("pssz: header cuts a known field")
                if not optional or stable:
                    _bytes("pssz: required record field missing")
                missing = True
                continue
            present[i] = True
            at = fixed_start + cursor
            if variable:
                offset = int.from_bytes(data[at:at + w], "little")
                if offset > payload_size or offset < previous_offset or (not seen_variable and offset != fixed_size):
                    _bytes("pssz: invalid record payload offset")
                if previous_field != count:
                    ends[previous_field] = fixed_start + offset
                begins[i] = fixed_start + offset
                previous_field = i
                previous_offset = offset
                seen_variable = True
            else:
                begins[i] = at
                ends[i] = at + size
            cursor += size
        if unknown:
            for at in range(fixed_start + known_size, fixed_start + fixed_size, w):
                offset = int.from_bytes(data[at:at + w], "little")
                if offset > payload_size or offset < previous_offset or (not seen_variable and offset != fixed_size):
                    _bytes("pssz: invalid record payload offset")
                if previous_field != count:
                    ends[previous_field] = fixed_start + offset
                previous_field = count
                previous_offset = offset
                seen_variable = True
        if previous_field != count:
            ends[previous_field] = end
        if not seen_variable and payload_size != fixed_size:
            _bytes("pssz: fixed record has trailing payload")
        return begins, ends, present, unknown

    def _members_reader(self, node: Node, stable: bool) -> Callable[[bytes, int, int, int, Ctx], List[Any]]:
        w = self.width_of(node)
        fields = self.field_layouts(node)
        members = node.members
        count = len(members)
        decs = [self.decoder(m.text) for m in members]
        optional = [f[2] for f in fields]
        bounds = [(i, m.bound, m.node, nested_member(m.node)) for i, m in enumerate(members) if m.bound is not None]
        record_spans = self.record_spans
        nested_bound_admits = self.nested_bound_admits

        def read(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> List[Any]:
            begins, ends, present, unknown = record_spans(data, start, end, w, stable, fields)
            if unknown:
                if not ctx.allow_unknown:
                    _bytes("pssz: unknown fields in exact validation")
                ctx.has_unknown = True
            n1 = n + 1
            # Pruning is mandatory: the last slot a writer emits is never an
            # absent optional.  A frame with unknown appended slots is not judged.
            last = -1
            if not stable and not unknown:
                last = count - 1
                while last >= 0 and not present[last]:
                    last -= 1
                if last >= 0 and optional[last] and begins[last] == ends[last]:
                    _bytes(NOT_PRUNED)
            values = [decs[i](data, begins[i], ends[i], n1, ctx) if present[i] else None for i in range(count)]
            if last >= 0 and optional[last] and values[last] is None:
                _bytes(NOT_PRUNED)
            for i, bound, mnode, nested in bounds:
                if not member_bound_admits(bound, mnode, values[i]):
                    _bytes(MEMBER_BOUND)
                # A nested member's bound counts its encoded bytes.
                if nested and present[i] and not nested_bound_admits(bound, mnode, data, begins[i], ends[i]):
                    _bytes(MEMBER_BOUND)
            return values
        return read

    def decoder(self, node: Node) -> Callable[[bytes, int, int, int, Ctx], Any]:
        """The compiled checked reader of ``node``: ``(data, start, end, depth, ctx) -> value``."""
        f = node.dec
        if f is not None:
            return f
        if "dec_busy" in node.extra:
            return lambda data, start, end, n, ctx: node.dec(data, start, end, n, ctx)
        node.extra["dec_busy"] = True
        try:
            f = getattr(self, "_dec_" + node.kind)(node)
            bind = node.bind
            if bind is not None and bind.post is not None:
                raw, post = f, bind.post
                f = lambda data, start, end, n, ctx: post(raw(data, start, end, n, ctx))  # noqa: E731
            node.dec = f
        finally:
            del node.extra["dec_busy"]
        return f

    def _dec_int(self, node: Node) -> Callable[..., Any]:
        size, signed, bits = node.size, node.signed, node.bits
        from_bytes = int.from_bytes

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if end - start != size:
                _bytes("pssz: fixed primitive span mismatch")
            return from_bytes(data[start:end], "little", signed=signed)
        if bits != 1:
            return dec

        def dec_bit(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            raw = dec(data, start, end, n, ctx)
            if raw not in ((0, -1) if signed else (0, 1)):
                _bytes("pssz: one-bit integer overflow")
            return raw
        return dec_bit

    def _dec_float(self, node: Node) -> Callable[..., Any]:
        size = node.size
        unpack = struct.Struct("<f" if size == 4 else "<d").unpack_from
        quiet = QUIET_NAN_32 if size == 4 else QUIET_NAN_64

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if end - start != size:
                _bytes("pssz: fixed primitive span mismatch")
            value = unpack(data, start)[0]
            if value != value:
                bits = int.from_bytes(data[start:end], "little")
                if bits != quiet:
                    return NaN(bits, size)
            return value
        return dec

    def _dec_bool(self, node: Node) -> Callable[..., Any]:
        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if end - start != 1:
                _bytes("pssz: fixed primitive span mismatch")
            b = data[start]
            if b > 1:
                _bytes("pssz: bool is not 0/1")
            return b == 1
        return dec

    def _dec_char(self, node: Node) -> Callable[..., Any]:
        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if end - start != 1:
                _bytes("pssz: fixed primitive span mismatch")
            return data[start]
        return dec

    def _dec_string(self, node: Node) -> Callable[..., Any]:
        raw = node.raw
        container = node.child
        check = self._byte_bound(container, True) if container.kind == "array" or container.bounded else None

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if check is not None:
                check(end - start, _bytes)
            if raw:
                return data[start:end]
            try:
                return data[start:end].decode("utf-8")
            except UnicodeDecodeError:
                return _bytes(utf8_reason(data[start:end]))
        return dec

    def _dec_hex(self, node: Node) -> Callable[..., Any]:
        container = node.child
        check = self._byte_bound(container, False) if container.kind == "array" or container.bounded else None

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if check is not None:
                check(end - start, _bytes)
            if end > start and n + 1 > MAX_DEPTH:
                _bytes(DEPTH)
            return data[start:end]
        return dec

    def _dec_bits(self, node: Node) -> Callable[..., Any]:
        is_list = node.kind == "bitlist"
        bound = node.size

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            size = end - start
            count = bound
            if is_list:
                if size == 0:
                    _bytes("pssz: bitlist span out of bounds")
                last = data[end - 1]
                if last == 0:
                    _bytes("pssz: bitlist missing delimiter")
                count = (size - 1) * 8 + last.bit_length() - 1
                if count > bound:
                    _bytes("pssz: bitlist bit_count exceeds bound")
            else:
                if size != packed_byte_count(count):
                    _bytes("pssz: fixed primitive span mismatch")
                if count % 8 and data[end - 1] >> (count % 8):
                    _bytes("pssz: nonzero unused bitvector bits")
            packed = bytearray(data[start:start + packed_byte_count(count)])
            if is_list and count % 8:
                packed[-1] &= (1 << (count % 8)) - 1
            return Bits(bytes(packed), count)
        return dec

    _dec_bitvector = _dec_bits
    _dec_bitlist = _dec_bits

    def _dec_option(self, node: Node) -> Callable[..., Any]:
        selector = self.can_be_empty(node.child)
        inner = self.decoder(node.text_child)
        nested = node.child.kind == "option"
        info = self.layout(node.child)
        fixed_size = info[1] if info[0] else None

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if selector:
                if start == end:
                    _bytes("pssz: optional<var> missing selector")
                tag = data[start]
                if tag == 0:
                    if end - start != 1:
                        _bytes("pssz: None has trailing bytes")
                    return None
                if tag != 1:
                    _bytes("pssz: optional selector not 0/1")
                start += 1
            elif start == end:
                return None
            elif fixed_size is not None and end - start != fixed_size:
                _bytes("pssz: optional<fixed> span mismatch")
            value = inner(data, start, end, n + 1, ctx)
            return Some(value) if nested else value
        return dec

    def _dec_record(self, node: Node) -> Callable[..., Any]:
        read = self._members_reader(node, node.stable)
        cap = node.cap if self.automatic else None
        bind = node.bind
        make = bind.make if bind is not None and bind.make is not None else None
        names = [m.name for m in node.members]

        info = self.layout(node)
        zero = info[0] and info[1] == 0

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if cap is not None and end - start > cap:
                _bytes(CAP)
            if zero and end != start:
                _bytes("pssz: zero-size fixed payload must be empty")
            values = read(data, start, end, n, ctx)
            if make is not None:
                return make(values)
            return dict(zip(names, values))
        return dec

    def _dec_tuple(self, node: Node) -> Callable[..., Any]:
        read = self._members_reader(node, False)

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            return tuple(read(data, start, end, n, ctx))
        return dec

    def _sequence_reader(self, node: Node) -> Callable[[bytes, int, int, int, Ctx], List[Any]]:
        w = self.width_of(node)
        fixed, esize, _ = self.layout(node.child)
        inner = self.decoder(node.text_child)
        bounded, exact, limit = node.bounded, node.exact, node.count
        from_bytes = int.from_bytes
        first_message = "pssz: invalid array first offset" if exact else "pssz: invalid first list offset"
        range_message = "pssz: array offset out of range" if exact else "pssz: list offset out of range"

        def read(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> List[Any]:
            size = end - start
            n1 = n + 1
            if fixed:
                if exact:
                    count = limit
                    if count * esize != size:
                        _bytes("pssz: zero-size fixed payload must be empty" if not count * esize
                               else "pssz: fixed primitive span mismatch")
                    if not esize and count > ZERO_SIZE_ARRAY_LIMIT:
                        _bytes("pssz: zero-size array count exceeds the implementation limit")
                elif size == 0:
                    return []
                else:
                    if not esize:
                        _bytes("pssz: nonempty zero-size vector is unsupported")
                    if size % esize:
                        _bytes("pssz: vector span not a multiple of element size")
                    count = size // esize
                    if bounded and count > limit:
                        _bytes(LIST_BOUND)
                return [inner(data, at, at + esize, n1, ctx) for at in range(start, start + count * esize, esize)] \
                    if esize else [inner(data, start, start, n1, ctx) for _ in range(count)]
            if exact:
                count = limit
                if count > size // w:
                    _bytes("pssz: array offset table truncated")
                if not count and size:
                    _bytes("pssz: zero-count array payload must be empty")
            elif size == 0:
                return []
            else:
                if w > size:
                    _bytes("pssz: variable list missing offset table")
                first = from_bytes(data[start:start + w], "little")
                if not first or first % w or first > size:
                    _bytes(first_message)
                count = first // w
                if bounded and count > limit:
                    _bytes(LIST_BOUND)
            table = count * w
            offsets = [from_bytes(data[at:at + w], "little") for at in range(start, start + table, w)]
            offsets.append(size)
            out = []
            previous = table
            for i in range(count):
                begin = offsets[i]
                stop = offsets[i + 1]
                if i == 0 and begin != table:
                    _bytes(first_message)
                if begin < previous or begin > stop or stop > size:
                    _bytes(range_message)
                out.append(inner(data, start + begin, start + stop, n1, ctx))
                previous = begin
            return out
        return read

    def _dec_list(self, node: Node) -> Callable[..., Any]:
        read = self._sequence_reader(node)
        if not node.assoc:
            def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
                if n > MAX_DEPTH:
                    _bytes(DEPTH)
                return read(data, start, end, n, ctx)
            return dec
        is_map = node.assoc == "map"

        def dec_assoc(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            items = read(data, start, end, n, ctx)
            # The order is that of the bytes read.  A decoded Nested value
            # stands for its canonical encoding, which is not what a
            # compatible read saw when the inner bytes carry unknown fields:
            # keys holding a Nested node are taken from the bytes as they are.
            if self.key_has_nested(node) and not self.opaque_nested and self.state.opaque == 0:
                raw = Ctx(ctx.allow_unknown)
                keys = self.rawly(lambda: self._assoc_keys(node, read(data, start, end, n, raw)))
            else:
                keys = self._assoc_keys(node, items)
            for i in range(1, len(keys)):
                if keys[i - 1] >= keys[i]:
                    _bytes(MAP_ORDER if is_map else SET_ORDER)
            return self.assoc_value(node, items)
        return dec_assoc

    _dec_array = _dec_list

    def _dec_variant(self, node: Node) -> Callable[..., Any]:
        decs = [self.decoder(m.text) for m in node.members]
        names = [m.name for m in node.members]
        count = len(decs)
        bind = node.bind
        join = bind.join if bind is not None and bind.join is not None else None

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if start == end:
                _bytes("pssz: variant span out of bounds")
            index = data[start]
            if index >= count:
                _bytes("pssz: variant selector out of range")
            value = decs[index](data, start + 1, end, n + 1, ctx)
            if join is not None:
                return join(index, value)
            return Variant(names[index], value)
        return dec

    def _dec_nested(self, node: Node) -> Callable[..., Any]:
        auto = self.auto
        is_opaque = self.is_opaque
        text_child = node.text_child

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            if n > MAX_DEPTH:
                _bytes(DEPTH)
            if is_opaque(node):
                if end > start and n + 1 > MAX_DEPTH:
                    _bytes(DEPTH)
                return data[start:end]
            # The bytes are read as the inner type, in the automatic profile,
            # under the same unknown-field rule; its root is one level below.
            return auto.decoder(text_child)(data, start, end, n + 1, ctx)
        return dec

    def _dec_custom(self, node: Node) -> Callable[..., Any]:
        inner = self.decoder(node.inner)
        handler = node.handler
        if node.boxed or (handler is None and not self.require_handlers) or (
                handler is not None and handler.check is None):
            return inner
        check_custom = self.check_custom

        def dec(data: bytes, start: int, end: int, n: int, ctx: Ctx) -> Any:
            # The values inside were checked as they were read; then this one.
            value = inner(data, start, end, n, ctx)
            check_custom(node, value, "bytes")
            return value
        return dec


def _compose(chain: Sequence[Binding]) -> Binding:
    """One binding for a chain of aliases, outermost first."""
    out = Binding()
    pres = [b.pre for b in chain if b.pre is not None]
    posts = [b.post for b in reversed(chain) if b.post is not None]
    if pres:
        def pre(value: Any) -> Any:
            for f in pres:
                value = f(value)
            return value
        out.pre = pre if len(pres) > 1 else pres[0]
    if posts:
        def post(value: Any) -> Any:
            for f in posts:
                value = f(value)
            return value
        out.post = post if len(posts) > 1 else posts[0]
    for b in chain:
        for name in ("get", "make", "split", "join", "assoc", "cls"):
            if getattr(b, name) is not None:
                setattr(out, name, getattr(b, name))
        out.deferred = out.deferred or b.deferred
    return out
