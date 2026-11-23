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
from ._core import Binding, Codec, Ctx, Handler, Node, reserved_custom_id, strip
from ._key import decode_key, encode_key, encode_key_prefix
from ._time import reserved_time_id
from .values import Variant
from .errors import DecodeError, EncodeError, InternalError, JsonError, PsioError, SchemaError

__all__ = ["Schema", "SchemaType", "Status", "trusted", "Handler"]

Width = Union[str, int]


class trusted:
    """The caller's statement that ``data`` is a valid encoding: ``from_pssz(T, trusted(data))``.

    The decode then skips what trust makes unnecessary: the invariants of
    custom types are not run.  (This pure-Python reader still bounds-checks
    every frame it walks.)
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

    __slots__ = ("_codec", "_ref", "_node", "_boxed", "_enc", "_dec", "python_type")

    def __init__(self, codec: Codec, ref: Node, python_type: Any = None) -> None:
        self._codec = codec
        self._ref = ref
        self._node, self._boxed = strip(ref)
        self._enc = codec.encoder(ref)
        self._dec = codec.decoder(ref)
        #: The Python type this codec reads and writes, when it came from the typed layer.
        self.python_type = python_type

    @property
    def width(self) -> int:
        """The offset width of this type's own header / slots / offset table."""
        if self._boxed and self._codec.W == 0:
            return 4
        return self._codec.width_of(self._node)

    @property
    def max_encoded_size(self) -> Optional[int]:
        """The inferred size bound (spec 3.2), whatever cap the type declares; ``None`` when unbounded."""
        if self._boxed:
            return None
        return self._codec.auto.bound_of(self._node, None, False, [])

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
        return _writing(lambda: codec.encode_top(self._ref, value))

    def pssz_size(self, value: Any) -> int:
        """The size of the encoding of ``value``."""
        return len(self.to_pssz(value))

    def from_pssz(self, data: Any, *, reject_unknown: bool = False) -> Any:
        """Validates, then decodes.  ``trusted(data)`` skips the invariants of custom types."""
        codec = self._codec
        if type(data) is trusted:
            raw = _as_bytes(data.data)
            return codec.quietly(lambda: _reading(lambda: codec.decode_top(self._ref, raw, Ctx(True))))
        raw = _as_bytes(data)
        ctx = Ctx(not reject_unknown)
        return _reading(lambda: codec.decode_top(self._ref, raw, ctx))

    def from_pssz_trusted(self, data: Any) -> Any:
        return self.from_pssz(trusted(data))

    def validate_pssz(self, data: Any, *, strict: bool = False) -> Status:
        """Checks PSSZ bytes against the type.  Never raises.

        ``strict`` also rejects a frame with unknown appended optional fields.
        """
        ctx = Ctx(not strict)
        try:
            raw = _as_bytes(data)
            _reading(lambda: self._codec.decode_top(self._ref, raw, ctx))
        except PsioError as e:
            return Status(False, False, e)
        return Status(True, ctx.has_unknown)

    def pssz_view(self, data: Any) -> Any:
        """Reads the value in place: see ``psio.view``."""
        from .view import view_at
        raw = _as_bytes(data)
        codec = self._codec

        def run() -> Any:
            def fail(m: str) -> Any:
                raise DecodeError(m)
            codec.check_top_cap(self._node, len(raw), fail)
            return view_at(codec, self._ref, raw, 0, len(raw), 0)
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
        return _writing(lambda: self._codec.encode_top(self._ref, value))

    def pssz_to_json(self, data: Any, format: str = "ijson", *, reject_unknown: bool = False) -> str:
        """PSSZ bytes (fully validated) -> JSON / I-JSON text."""
        from ._json import value_to_json
        value = self.from_pssz(data, reject_unknown=reject_unknown)
        # The decode ran the handlers' checks; the writer need not repeat them.
        return _writing(lambda: value_to_json(self._codec, self._ref, format, value, False))


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


def key_prefix_upper_bound(prefix: Any) -> Optional[bytes]:
    from ._key import key_prefix_upper_bound as f
    return f(prefix)
