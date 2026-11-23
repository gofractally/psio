"""Schema-driven entry points: a Schema IR and no Python classes.

    schema = psio.Schema.from_pssz(schema_bytes)
    Transfer = schema.type("Transfer")
    data = Transfer.json_to_pssz('{"from":"alice","amount":"18446744073709551615"}')
    value = Transfer.from_pssz(data)          # {'from': 'alice', 'amount': 18446744073709551615}
    text = Transfer.pssz_to_json(data)        # I-JSON by default

Values of the dynamic model are described in ``psio.values``.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, Iterable, Mapping, Optional, Union

from . import ir as _ir
from ._core import (
    FUNC_FIELDS, FUNC_SIZE, MAX_TUPLE_ELEMENTS, MEMBER_BOUND, ROOT, Binding, Codec, Ctx, Handler, LengthBound, Node,
    Order, Pos, _check_attributes, _parse_cap, _parse_length_bound, member_bound_admits, nested_member,
    parse_max_fields, reserved_custom_id, strip,
)
from ._key import decode_key, encode_key, encode_key_prefix
from ._time import reserved_time_id
from .values import Variant
from .errors import DecodeError, EncodeError, InternalError, JsonError, PsioError, SchemaError

__all__ = ["Schema", "SchemaType", "SchemaMethod", "Status", "trusted", "Handler"]

Width = Union[str, int]


class trusted:
    """The caller's statement that ``data`` is a valid encoding: ``from_pssz(T, trusted(data))``.

    The decode then skips what trust makes unnecessary: length bounds and
    caps, the canonical-form rules (pruned optionals, map / set order) and
    the invariants of custom types.  Use it only for bytes this program wrote
    or validated: for any other input the result is a value of the wrong
    shape or a ``DecodeError``.  The frames are still parsed in full, so
    nothing outside ``data`` is ever read.
    """

    __slots__ = ("data",)

    def __init__(self, data: Any) -> None:
        self.data = data


class Status:
    """The outcome of a ``validate_*``: truthy when the input was accepted.  Never an exception."""

    __slots__ = ("ok", "has_unknown", "error")

    def __init__(self, ok: bool, has_unknown: bool = False, error: Optional[PsioError] = None) -> None:
        self.ok = ok
        #: Unknown appended optional fields were present (PSSZ).
        self.has_unknown = has_unknown
        self.error = error

    def __bool__(self) -> bool:
        return self.ok

    @property
    def reason(self) -> str:
        return "" if self.error is None else self.error.reason

    def __repr__(self) -> str:
        if self.ok:
            return f"Status(ok=True, has_unknown={self.has_unknown})"
        return f"Status(ok=False, reason={self.reason!r})"


def _as_bytes(data: Any, what: str = "pssz") -> bytes:
    if type(data) is bytes:
        return data
    if isinstance(data, (bytes, bytearray, memoryview)):
        return bytes(data)
    raise DecodeError(f"{what}: input must be bytes")


_WRITE_ERRORS = (AttributeError, TypeError, KeyError, IndexError, ValueError, OverflowError)


def _writing(run: Callable[[], Any]) -> Any:
    """A writer: anything a wrong value raises is an EncodeError."""
    try:
        return run()
    except PsioError:
        raise
    except RecursionError:
        raise EncodeError("pssz: max depth exceeded") from None
    except _WRITE_ERRORS as e:
        raise EncodeError(f"psio: value does not fit its type: {type(e).__name__}: {e}") from e


def _reading(run: Callable[[], Any], kind: Any = DecodeError) -> Any:
    try:
        return run()
    except PsioError:
        raise
    except RecursionError:
        raise kind("pssz: max depth exceeded") from None
    except Exception as e:  # a typed constructor refusing the decoded members
        raise kind(f"psio: {type(e).__name__}: {e}") from e


class SchemaType:
    """One type of a schema under one width profile."""

    __slots__ = ("_codec", "_ref", "_node", "_boxed", "_enc", "_dec", "python_type", "_pos")

    def __init__(self, codec: Codec, ref: Node, python_type: Any = None, pos: Pos = ROOT) -> None:
        self._codec = codec
        self._ref = ref
        self._node, self._boxed = strip(ref)
        #: Where the value sits (spec 3.6): a root, unless a member-like bound gives a plain list its count.
        self._pos = pos
        self._enc = codec.encoder(ref, pos)
        self._dec = codec.decoder(ref, pos)
        #: The Python type this codec reads and writes, when it came from the typed layer.
        self.python_type = python_type

    @property
    def width(self) -> int:
        """The offset width of this type's own header / slots / offset table."""
        if self._boxed and self._codec.W == 0:
            return 4
        return self._codec.width_of(self._node, self._pos)

    def _count(self) -> Optional[LengthBound]:
        # The count a member-like bound gives a plain list (a method result under ``returns``).
        if self._pos[1] is None or not (self._node.kind == "list" and not self._node.bounded):
            return None
        annotation = LengthBound()
        annotation.max = self._pos[1]
        return annotation

    @property
    def inferred_bound(self) -> Optional[int]:
        """The inferred bound (spec 3.2): the number offset widths are chosen from; ``None`` when unbounded.

        It never reads the type's own cap (a capped record inside counts as
        its cap).  It is not the largest encoding of a capped type: that is
        ``max_encoded_size``.
        """
        if self._boxed:
            return None
        return self._codec.auto.bound_of(self._node, self._count(), False, [])

    @property
    def max_encoded_size(self) -> Optional[int]:
        """The largest encoding (spec 3.7): no automatic-profile encoding of a value of the type is longer;
        ``None`` when there is no finite maximum.

        Every header, slot and offset table is counted at the width that is
        written, what the type holds at its own maximum, and a capped record
        is the smaller of its cap and that sum - the cap alone when
        something inside is unbounded.  Conservative by one selector byte
        per optional and by ceil(N / 8) + 1 for a bitlist.
        """
        if self._boxed:
            return None
        return self._codec.auto.max_of(self._node, self._count(), 4, [])

    @property
    def max_dynamic_data(self) -> Optional[int]:
        """The explicit ``max_dynamic_data`` cap of a record, or ``None``."""
        return self._node.cap

    @property
    def fixed_size(self) -> Optional[int]:
        """The size of every encoding of a fixed-size type; ``None`` for a variable one."""
        fixed, size, _ = self._codec.layout(self._node)
        return size if fixed else None

    # ── PSSZ ────────────────────────────────────────────────────────────────

    def to_pssz(self, value: Any) -> bytes:
        """The canonical PSSZ bytes of ``value``."""
        codec, node = self._codec, self._node
        if codec.W == 0 or node.cap is None:
            enc = self._enc
            return _writing(lambda: enc(value, 0))
        return _writing(lambda: codec.encode_top(self._ref, value, self._pos))

    def pssz_size(self, value: Any) -> int:
        """The size of the encoding of ``value``."""
        return len(self.to_pssz(value))

    def from_pssz(self, data: Any, *, reject_unknown: bool = False) -> Any:
        """Validates, then decodes.  ``trusted(data)`` skips what trust makes unnecessary (``Codec.decode_trusted``)."""
        codec = self._codec
        if type(data) is trusted:
            raw = _as_bytes(data.data)
            return codec.quietly(lambda: _reading(lambda: codec.decode_trusted(self._ref, raw, self._pos)))
        raw = _as_bytes(data)
        ctx = Ctx(not reject_unknown)
        return _reading(lambda: self._decode_checked(raw, ctx))

    def from_pssz_trusted(self, data: Any) -> Any:
        return self.from_pssz(trusted(data))

    def validate_pssz(self, data: Any, *, strict: bool = False) -> Status:
        """Checks PSSZ bytes against the type.  Never raises.

        ``strict`` also rejects a frame with unknown appended optional fields.
        """
        ctx = Ctx(not strict)
        try:
            raw = _as_bytes(data)
            _reading(lambda: self._decode_checked(raw, ctx))
        except PsioError as e:
            return Status(False, False, e)
        return Status(True, ctx.has_unknown)

    def _decode_checked(self, raw: bytes, ctx: Ctx) -> Any:
        return self._codec.decode_top(self._ref, raw, ctx, self._pos)

    def pssz_view(self, data: Any) -> Any:
        """Reads the value in place: see ``psio.view``."""
        from .view import view_at
        raw = _as_bytes(data)
        codec = self._codec

        def run() -> Any:
            def fail(m: str) -> Any:
                raise DecodeError(m)
            codec.check_top_cap(self._node, len(raw), fail)
            return view_at(codec, self._ref, raw, 0, len(raw), 0, self._pos)
        return _reading(run)

    # ── key ─────────────────────────────────────────────────────────────────

    def to_key(self, value: Any) -> bytes:
        """The ``key`` bytes of ``value``: bytewise order is value order."""
        def run() -> bytes:
            self._codec.check_value(self._ref, value, "value")
            return encode_key(self._codec, self._ref, value)
        return _writing(run)

    def from_key(self, data: Any) -> Any:
        """Decodes key bytes: exactly one canonical key spanning the whole input."""
        raw = _as_bytes(data.data if type(data) is trusted else data, "key")
        return _reading(lambda: decode_key(self._codec, self._ref, raw))

    def validate_key(self, data: Any) -> Status:
        try:
            self.from_key(data)
        except PsioError as e:
            return Status(False, False, e)
        return Status(True)

    def key_prefix(self, elements: Any) -> bytes:
        """The key of the leading ``elements`` of a tuple / record type: what every full key with them starts with."""
        return _writing(lambda: encode_key_prefix(self._codec, self._ref, elements))

    def key_size(self, value: Any) -> int:
        return len(self.to_key(value))

    # ── JSON / I-JSON ───────────────────────────────────────────────────────

    def to_json(self, value: Any, format: str = "json") -> str:
        from ._json import value_to_json
        return _writing(lambda: value_to_json(self._codec, self._ref, format, value))

    def to_ijson(self, value: Any) -> str:
        return self.to_json(value, "ijson")

    def from_json(self, text: Any, format: str = "json") -> Any:
        from ._json import json_to_value
        if type(text) is trusted:
            text = text.data
        return _reading(lambda: json_to_value(self._codec, self._ref, format, text), JsonError)

    def from_ijson(self, text: Any) -> Any:
        return self.from_json(text, "ijson")

    def validate_json(self, text: Any, format: str = "json") -> Status:
        try:
            self.from_json(text, format)
        except PsioError as e:
            return Status(False, False, e)
        return Status(True)

    def validate_ijson(self, text: Any) -> Status:
        return self.validate_json(text, "ijson")

    def json_to_pssz(self, text: Any, format: str = "ijson") -> bytes:
        """JSON / I-JSON text -> canonical PSSZ bytes."""
        from ._json import json_to_value
        value = _reading(lambda: json_to_value(self._codec, self._ref, format, text), JsonError)
        return _writing(lambda: self._codec.encode_top(self._ref, value, self._pos))

    def pssz_to_json(self, data: Any, format: str = "ijson", *, reject_unknown: bool = False) -> str:
        """PSSZ bytes (fully validated) -> JSON / I-JSON text."""
        from ._json import value_to_json
        value = self.from_pssz(data, reject_unknown=reject_unknown)
        # The decode ran the handlers' checks; the writer need not repeat them.
        return _writing(lambda: value_to_json(self._codec, self._ref, format, value, False))


class ResultType(SchemaType):
    """The codec of a method's result when ``returns`` says more than a type can: a length bound that is
    an attribute (``exact`` / ``min``, the Func attribute ``result.lengthBound``) or an order constraint.

    The bytes are those of the result's type; every writer and every checking
    reader also checks the value, as they check a record member's.
    """

    __slots__ = ("_bound", "_order", "_nested")

    def __init__(self, codec: Codec, ref: Node, bound: Optional[LengthBound], order: Optional[Order]) -> None:
        limit = bound.limit if bound is not None and codec.automatic else None
        super().__init__(codec, ref, None, (4, limit, False))
        self._bound = bound
        self._order = order
        self._nested = bound is not None and nested_member(self._node)

    def _verify(self, value: Any, fail: Callable[[str], Any], message: str = MEMBER_BOUND) -> None:
        if self._bound is not None and not self._nested and not member_bound_admits(self._bound, self._node, value):
            fail(message)
        if self._order is not None:
            self._codec._check_order(self._ref, value, self._order, fail)

    def _bytes_admit(self, data: bytes, fail: Callable[[str], Any]) -> None:
        # A nested value's bound counts the bytes of its encoding.
        if self._nested and not self._codec.nested_bound_admits(self._bound, self._node, data, 0, len(data)):
            fail(MEMBER_BOUND)

    def to_pssz(self, value: Any) -> bytes:
        def fail(message: str) -> Any:
            raise EncodeError(message)
        _writing(lambda: self._verify(value, fail))
        out: bytes = super().to_pssz(value)
        self._bytes_admit(out, fail)
        return out

    def _decode_checked(self, raw: bytes, ctx: Ctx) -> Any:
        def fail(message: str) -> Any:
            raise DecodeError(message)
        value = super()._decode_checked(raw, ctx)
        self._verify(value, fail)
        self._bytes_admit(raw, fail)
        return value

    def to_key(self, value: Any) -> bytes:
        def fail(message: str) -> Any:
            raise EncodeError(message)
        _writing(lambda: self._verify(value, fail, "key: member length bound violated"))
        return super().to_key(value)

    def from_key(self, data: Any) -> Any:
        def fail(message: str) -> Any:
            raise DecodeError(message)
        value = super().from_key(data)
        if type(data) is not trusted:
            self._verify(value, fail, "key: member length bound violated")
        return value

    def to_json(self, value: Any, format: str = "json") -> str:
        def fail(message: str) -> Any:
            raise EncodeError(message)
        _writing(lambda: self._verify(value, fail, "json: member length_bound violated"))
        return super().to_json(value, format)

    def from_json(self, text: Any, format: str = "json") -> Any:
        def fail(message: str) -> Any:
            raise JsonError(message)
        value = super().from_json(text, format)
        self._verify(value, fail, "json: member length_bound violated")
        return value

    def json_to_pssz(self, text: Any, format: str = "ijson") -> bytes:
        return self.to_pssz(self.from_json(text, format))


def _handler_map(handlers: Optional[Mapping[str, Any]]) -> Dict[str, Handler]:
    out: Dict[str, Handler] = {}
    for id, handler in (handlers or {}).items():
        if reserved_time_id(id):
            raise SchemaError(f'pssz: custom id "{id}" is a built-in time presentation and takes no handler')
        if reserved_custom_id(id):
            raise SchemaError(f'pssz: custom id "{id}" is part of the binary form and takes no handler')
        if isinstance(handler, Mapping):
            handler = Handler(**handler)
        if not isinstance(handler, Handler):
            raise SchemaError(f'pssz: the handler of custom id "{id}" must be a psio.Handler')
        out[id] = handler
    return out


_meta: Optional["Schema"] = None


class Schema:
    """A Schema IR, ready to hand out codecs for its types.

    ``source`` is a ``psio::Schema`` value (as :meth:`from_pssz` produces) or a
    ``{name: type}`` map in the compact spelling (``psio.ir``).
    """

    def __init__(self, source: Any, *, handlers: Optional[Mapping[str, Any]] = None, require_handlers: bool = False,
                 nested_bytes: str = "typed", _deferred: Iterable[str] = (),
                 _bindings: Optional[Mapping[Any, Binding]] = None, _normalized: bool = False) -> None:
        #: The canonical IR (``psio::Schema`` as this library decodes it).
        self.ir: Dict[str, Any] = source if _normalized else _ir.normalize_schema(source)
        self._handlers = _handler_map(handlers)
        self._require = require_handlers
        if nested_bytes not in ("typed", "opaque"):
            raise SchemaError('pssz: nested_bytes must be "typed" or "opaque"')
        self._nested = nested_bytes
        self._deferred = frozenset(_deferred)
        self._bindings = _bindings
        self._codecs: Dict[int, Codec] = {}
        self._types: Dict[Any, SchemaType] = {}
        self._builder: Any = None

    def _codec(self, w: int) -> Codec:
        codec = self._codecs.get(w)
        if codec is None:
            codec = self._codecs[w] = Codec(
                self.ir, w, self._handlers, self._require, self._nested == "opaque", self._deferred,
                None if w == 0 else self._codec(0), self._bindings)
        return codec

    def derive(self, *, handlers: Optional[Mapping[str, Any]] = None, require_handlers: Optional[bool] = None,
               nested_bytes: Optional[str] = None) -> "Schema":
        """The same schema with these handlers (added to, and replacing by id, the ones it has) and options."""
        merged = dict(self._handlers)
        merged.update(_handler_map(handlers))
        return Schema(self.ir, handlers=merged,
                      require_handlers=self._require if require_handlers is None else require_handlers,
                      nested_bytes=self._nested if nested_bytes is None else nested_bytes,
                      _deferred=self._deferred, _bindings=self._bindings, _normalized=True)

    with_handlers = derive

    @staticmethod
    def meta() -> "Schema":
        """The Schema IR of ``psio::Schema`` itself."""
        global _meta
        if _meta is None:
            _meta = Schema(_ir.META_SCHEMA)
        return _meta

    @classmethod
    def from_pssz(cls, data: Any, **options: Any) -> "Schema":
        """From the PSSZ encoding of ``psio::Schema`` (how a schema travels)."""
        return cls(Schema.meta().type("Schema").from_pssz(data), _normalized=True, **options)

    @classmethod
    def from_json(cls, text: Any, format: str = "ijson", **options: Any) -> "Schema":
        """From the JSON / I-JSON text of ``psio::Schema`` (docs/json.md, "The Schema IR as JSON")."""
        return cls(Schema.meta().type("Schema").from_json(text, format), _normalized=True, **options)

    def to_pssz(self) -> bytes:
        return Schema.meta().type("Schema").to_pssz(self.ir)

    def to_json(self, format: str = "ijson") -> str:
        """The JSON / I-JSON text of this schema, byte for byte the C++ and Rust text."""
        return Schema.meta().type("Schema").to_json(self.ir, format)

    def names(self) -> list:
        return [m["name"] for m in self.ir["types"]]

    def method(self, interface: str, name: str, **options: Any) -> SchemaMethod:
        """A function of an interface of this schema (``interfaces[].funcs[]``), without Python classes.

        The arguments are the argument list of the parameters (the bytes of
        the typed argument tuple) with the function's ``maxDynamicData`` /
        ``maxFields`` caps: named when every parameter has a name, else
        positional (docs/json.md "Method arguments").  The result is encoded
        as the type the function records for it.
        """
        for i in self.ir["interfaces"]:
            if i.get("name") != interface:
                continue
            for f in i.get("funcs", ()):
                if f.get("name") == name:
                    return self._method(interface, f, options)
        raise SchemaError(f'pssz: schema has no function "{name}" of an interface "{interface}"')

    def _method(self, interface: str, f: Mapping[str, Any], options: Mapping[str, Any],
                result_order: Optional[Order] = None) -> SchemaMethod:
        params = f["params"]
        attributes = f.get("attributes", ())
        # The argument list is a tuple (docs/pssz-spec.md 8.4, 16).
        if len(params) > MAX_TUPLE_ELEMENTS:
            raise SchemaError(FUNC_SIZE)
        max_fields = parse_max_fields(attributes)
        if max_fields is not None and len(params) > max_fields:
            raise SchemaError(FUNC_FIELDS)
        cap = _parse_cap(attributes)
        names = [p["name"] for p in params]
        # Positional names are "0", "1", ...: no parameter is named so (C++ ``pssz_ir::argument_list``).
        named = all(n and n != str(k) for k, n in enumerate(names))
        own = [{"name": "tuple", "value": "named" if named else "positional"}]
        own += [a for a in attributes if a.get("name") == "maxFields"]
        own += [a for a in attributes if a.get("name") == "maxDynamicData"]
        members = params if named else [{"name": str(k), "type": p["type"], "attributes": p["attributes"]}
                                        for k, p in enumerate(params)]
        args = self.type({"value": Variant("Object", {"members": members, "attributes": own})}, **options)
        result = None
        if f.get("result") is not None:
            result = self._result(f["result"], attributes, result_order, **options)
        return SchemaMethod(interface, f["name"], names if named else [], args, result, named, cap, max_fields)

    def _result(self, type_ir: Any, attributes: Any, order: Optional[Order] = None, **options: Any) -> SchemaType:
        """The codec of a function's result: its type, under the ``result.*`` attributes of the function."""
        own = [{"name": a["name"][len("result."):], "value": a.get("value")} for a in attributes
               if a.get("name", "").startswith("result.")]
        plain = self.type(type_ir, **options)
        if not own and order is None:
            return plain
        # (what a record member may carry: lengthBound, and the integer text forms)
        _check_attributes(own, "record_member", True)
        bound = _parse_length_bound(own)
        if bound is None and order is None:
            return plain
        return ResultType(plain._codec, plain._ref, bound, order)

    def validate(self) -> "Schema":
        """Checks the whole schema, not only what one root reaches (C++ ``pssz_ir::validate_schema``).

        Every named type, and every function of every interface: at most 255
        parameters and no more than its ``maxFields``, well-formed parameter
        and result types, well-formed caps.  Raises :class:`SchemaError`;
        returns the schema.  A consumer that takes a schema from outside
        calls this once before it dispatches through it.
        """
        _ir.check_ir_version(self.ir)
        for name in self.names():
            self.type(name)
        for i in self.ir["interfaces"]:
            for f in i.get("funcs", ()):
                self._method(i.get("name", ""), f, {})
        return self

    def type(self, root: Any, *, width: Width = "auto", handlers: Optional[Mapping[str, Any]] = None,
             require_handlers: Optional[bool] = None, nested_bytes: Optional[str] = None) -> SchemaType:
        """The codec for a named type or an inline type (either IR spelling).

        Raises :class:`SchemaError` when the type, or anything it reaches, is
        not expressible in PSSZ, and when the schema carries no ``ir_version``.
        """
        if width not in ("auto", 1, 2, 4):
            raise SchemaError('pssz: width must be "auto", 1, 2 or 4')
        if handlers is not None or (require_handlers is not None and require_handlers != self._require) or (
                nested_bytes is not None and nested_bytes != self._nested):
            return self.derive(handlers=handlers, require_handlers=require_handlers,
                               nested_bytes=nested_bytes).type(root, width=width)
        w = 0 if width == "auto" else int(width)
        key = (w, root) if type(root) is str else None
        if key is not None:
            known = self._types.get(key)
            if known is not None:
                return known
        _ir.check_ir_version(self.ir)
        codec = self._codec(w)
        try:
            node = codec.node(root if isinstance(root, dict) and isinstance(root.get("value"), Variant)
                              else _ir.normalize_type(root))
            out = SchemaType(codec, node)
        except PsioError:
            raise
        except RecursionError:
            raise SchemaError("pssz IR: schema nesting too deep") from None
        if key is not None:
            self._types[key] = out
        return out


class SchemaMethod:
    """A function of an interface recorded in a schema: the codecs of its arguments and of its result."""

    __slots__ = ("interface", "name", "params", "args", "result", "named", "max_dynamic_data", "max_fields")

    def __init__(self, interface: str, name: str, params: list, args: SchemaType, result: Optional[SchemaType],
                 named: bool = True, max_dynamic_data: Optional[int] = None,
                 max_fields: Optional[int] = None) -> None:
        self.interface = interface
        self.name = name
        #: The parameter names, in order (empty when the function does not name every parameter).
        self.params = params
        #: Whether every parameter has a name: the arguments are then an object in text (a ``dict`` in
        #: the dynamic model); otherwise the positional array (a ``tuple``).
        self.named = named
        #: The argument list.  Binary is always the positional tuple.
        self.args = args
        #: The result, encoded as the type the function records for it; ``None`` when there is none.
        self.result = result
        #: The cap of the encoded arguments (``maxDynamicData``), or None.
        self.max_dynamic_data = max_dynamic_data
        #: The most parameters the function will ever have (``maxFields``), or None.
        self.max_fields = max_fields


def key_prefix_upper_bound(prefix: Any) -> Optional[bytes]:
    from ._key import key_prefix_upper_bound as f
    return f(prefix)
