"""Methods and interfaces: a class's method signatures described once, for calls, dispatch and the Schema IR.

The Python counterpart of C++ ``PSIO_REFLECT(T, method(...))`` + ``PSIO_INTERFACE(T)`` and of Rust
``#[psio::interface]``.  Nothing restates a method's name, its parameter names or its types:

    @psio.interface(name="token")
    class Token(Protocol):
        @psio.method(max_dynamic_data=1024, max_fields=8)
        def transfer(self, to: Annotated[str, psio.max(16)], amount: u64,
                     memo: Annotated[str, psio.max(80)]) -> None: ...
        def balance_of(self, owner: Annotated[str, psio.max(16)]) -> u64: ...
        def names(self, limit: u32) -> Annotated[list[str], psio.max(4), psio.element(psio.max(16))]: ...
        def add(self, a: u32, b: u32, /) -> u64: ...          # positional-only: no parameter names

    token = psio.interface_of(Token)
    data = token["transfer"].to_pssz("bob", 5, memo="hi")     # the argument tuple
    reply = token.dispatch(ledger, "balance_of", args_bytes)   # validate, call, encode the result
    remote = token.proxy(send)                                 # remote.transfer("bob", 5, "hi")

The arguments of a method are the tuple of its parameters (docs/pssz-spec.md 16): parameters are
annotated like record members, ``max_dynamic_data`` caps the encoded arguments and fixes their
offset width for the method's lifetime, ``max_fields`` is the most parameters it will ever have,
and the only compatible change is an appended ``Optional`` parameter.  In text the arguments are an
object keyed by parameter name (the positional array is read too); a method with positional-only
parameters has no names, and its arguments are the array (docs/json.md "Method arguments").

The return annotation is annotated like a member too (C++ ``returns(...)``): the result is then that
bounded type, and its bytes are that type's (``Method.result``).
"""

from __future__ import annotations

import inspect
import typing
from typing import Any, Callable, Dict, Iterator, List, Optional, TypeVar, Union, overload

from .errors import EncodeError, SchemaError
from .schema import Schema, SchemaType, Status
from .typed import Builder, FieldShape, Shape, _declared, _field, _wire_name, arguments, codec, shape_of

__all__ = ["interface", "interface_of", "method", "Interface", "Method"]


_C = TypeVar("_C")
_F = TypeVar("_F")


@overload
def method(fn: _F) -> _F: ...
@overload
def method(*, max_dynamic_data: Optional[int] = ..., name: Optional[str] = ...,
           max_fields: Optional[int] = ...) -> Callable[[_F], _F]: ...
def method(fn: Any = None, *, max_dynamic_data: Optional[int] = None, name: Optional[str] = None,
           max_fields: Optional[int] = None) -> Any:
    """Method-level annotations: the cap of the encoded arguments, another wire name, the most
    parameters the method will ever have (``max_fields``, at most 255)."""
    def apply(f: Any) -> Any:
        target = f.__func__ if isinstance(f, (staticmethod, classmethod)) else f
        target.__psio_method__ = {"max_dynamic_data": max_dynamic_data, "name": name, "max_fields": max_fields}
        return f
    return apply if fn is None else apply(fn)


class Method:
    """One method of an interface."""

    def __init__(self, index: int, name: str, attr: str, params: List[str], attrs: List[str], shape: Any,
                 result_hint: Any, max_dynamic_data: Optional[int], bound: bool,
                 max_fields: Optional[int] = None, named: bool = True, owner: str = "") -> None:
        self.index = index
        #: The method's name in the interface.
        self.name = name
        #: The Python attribute that implements it.
        self.attr = attr
        #: The parameter names, in order (empty for a method whose parameters have no names).
        self.params = params
        #: Whether every parameter has a name: the text form of the arguments is then an object.
        self.named = named
        self._attrs = attrs
        self._shape = shape
        self.max_dynamic_data = max_dynamic_data
        #: The most parameters the method will ever have, or None.
        self.max_fields = max_fields
        self._result_hint = result_hint
        self._args: Optional[SchemaType] = None
        self._result: Any = False
        self._result_field: Any = False
        self._bound = bound
        self._owner = owner

    @property
    def args(self) -> SchemaType:
        """The codec of the argument tuple: values are tuples of the arguments."""
        if self._args is None:
            self._args = codec(self._shape)
        return self._args

    def _returns(self) -> Optional[FieldShape]:
        """The result as a member-like position: its plain type and what the return annotation says of it."""
        if self._result_field is False:
            hint = self._result_hint
            self._result_field = None if hint in (None, type(None)) else _declared(
                lambda: _field("", hint, (), f"the result of {self._owner}.{self.name}"))
        return self._result_field

    @property
    def result(self) -> Optional[SchemaType]:
        """The codec of the result, or ``None`` for a method that returns nothing.

        The result is encoded as the type its annotation makes of it (C++
        ``returns(...)`` / ``encode_method_result``): ``-> Annotated[list[str],
        psio.max(4), psio.element(psio.max(16))]`` is a bounded list of
        bounded strings, with that type's narrow offset table.
        """
        if self._result is False:
            field = self._returns()
            if field is None:
                self._result = None
            else:
                def run() -> SchemaType:
                    builder = Builder()
                    member = builder._member(field)
                    attributes = [{"name": "result." + a["name"], "value": a["value"]} for a in member["attributes"]]
                    return builder.schema()._result(member["type"], attributes, field.order)
                self._result = _declared(run)
        return self._result

    def result_to_pssz(self, value: Any) -> bytes:
        """The PSSZ bytes of a result (C++ ``encode_method_result``)."""
        return self._result_codec().to_pssz(value)

    def result_from_pssz(self, data: Any) -> Any:
        """Validates and decodes an encoded result (C++ ``decode_method_result``)."""
        return self._result_codec().from_pssz(data)

    def validate_result(self, data: Any) -> Status:
        """Whether ``data`` is a valid encoded result (C++ ``validate_method_result``).  Never raises."""
        return self._result_codec().validate_pssz(data)

    def _result_codec(self) -> SchemaType:
        result = self.result
        if result is None:
            raise SchemaError(f"psio: {self.name} returns nothing")
        return result

    def arguments(self, *args: Any, **kwargs: Any) -> tuple:
        """The argument tuple of a call: positional and keyword arguments in parameter order."""
        count = len(self._attrs)
        if len(args) > count:
            raise EncodeError(f"psio: {self.name} takes {count} arguments")
        out = list(args) + [None] * (count - len(args))
        for key, value in kwargs.items():
            if not self.named:
                raise EncodeError(f"psio: the parameters of {self.name} have no names")
            if key in self._attrs:
                i = self._attrs.index(key)
            elif key in self.params:
                i = self.params.index(key)
            else:
                raise EncodeError(f'psio: {self.name} has no parameter "{key}"')
            if i < len(args):
                raise EncodeError(f'psio: {self.name}: parameter "{key}" is given twice')
            out[i] = value
        return tuple(out)

    def to_pssz(self, *args: Any, **kwargs: Any) -> bytes:
        """The PSSZ bytes of the arguments of a call."""
        return self.args.to_pssz(self.arguments(*args, **kwargs))

    def from_pssz(self, data: Any) -> tuple:
        """Validates and decodes encoded arguments."""
        return self.args.from_pssz(data)

    def __repr__(self) -> str:
        return f"<psio.Method {self.name}({', '.join(self.params if self.named else self._attrs)})>"


class Interface:
    """An interface: its name and its methods, in declaration order."""

    def __init__(self, cls: Any, name: str, methods: List[Method], package: str, version: str,
                 types: List[Any]) -> None:
        self.cls = cls
        self.name = name
        self.methods = methods
        self.package = package
        self.version = version
        self.types = types
        self._by_name = {m.name: m for m in methods}

    def __getitem__(self, key: Union[int, str]) -> Method:
        if isinstance(key, str):
            try:
                return self._by_name[key]
            except KeyError:
                raise KeyError(f'psio: interface {self.name} has no method "{key}"') from None
        return self.methods[key]

    def __iter__(self) -> Iterator[Method]:
        return iter(self.methods)

    def __len__(self) -> int:
        return len(self.methods)

    def index_of(self, name: str) -> Optional[int]:
        m = self._by_name.get(name)
        return None if m is None else m.index

    def dispatch(self, implementation: Any, method: Union[int, str], data: Any) -> bytes:
        """Decodes (and validates) the arguments, calls the implementation, encodes the result.

        A method that returns nothing gives ``b""``.
        """
        try:
            m = self[method]
        except (KeyError, IndexError):
            raise EncodeError(f"psio: interface {self.name} has no method {method!r}") from None
        args = m.args.from_pssz(data)
        out = getattr(implementation, m.attr)(*args)
        return b"" if m.result is None else m.result_to_pssz(out)

    def proxy(self, caller: Callable[[Method, bytes], Any]) -> Any:
        """An object with one function per method that hands the encoded arguments to ``caller``.

        ``caller(method, args_bytes)`` returns the encoded result (or ``None``
        / ``b""`` for a method without one); the proxy validates and decodes it.
        """
        return _Proxy(self, caller)

    def schema(self, *, arguments: bool = False) -> Schema:
        """The Schema IR: the ``Interface`` and ``Func`` nodes, and every type they reach.

        What C++ ``SchemaBuilder::insert_interface`` builds for the same
        declaration (``conformance/interface-vectors.md``): per function its
        name, its parameters (bounded types; a parameter without a name is
        ``""``), its result (the type the return annotation makes of it; a
        bound with no type form is the attribute ``result.lengthBound``) and
        the attributes ``maxFields`` / ``maxDynamicData``.  ``arguments``:
        every argument list is also a named type ``<interface>.<method>``, so
        that a schema-only reader has a root for it.
        """
        def run() -> Schema:
            builder = Builder()
            funcs = []
            for m in self.methods:
                fields = m._shape.fields
                result = m._returns()
                # (the plain types first, as C++ inserts them; then what the annotations make of them)
                for f in fields:
                    builder.insert(f.type)
                if result is not None:
                    builder.insert(result.type)
                params = [builder._member(f) for f in fields]
                if not m.named:
                    for p in params:
                        p["name"] = ""
                attributes = []
                result_type = None
                if result is not None:
                    member = builder._member(result)
                    result_type = member["type"]
                    attributes += [{"name": "result." + a["name"], "value": a["value"]} for a in member["attributes"]]
                if m.max_fields is not None:
                    attributes.append({"name": "maxFields", "value": str(m.max_fields)})
                if m.max_dynamic_data is not None:
                    attributes.append({"name": "maxDynamicData", "value": str(m.max_dynamic_data)})
                funcs.append({"name": m.name, "params": params, "result": result_type, "attributes": attributes})
            type_names = []
            for t in self.types:
                ref = builder.insert(t if isinstance(t, Shape) else shape_of(t))
                type_names.append(ref["value"].value["type"])
            if arguments:
                for m in self.methods:
                    builder.insert_named(f"{self.name}.{m.name}", m._shape)
            out = builder.schema()
            out.ir["interfaces"].append({"name": self.name, "type_names": type_names, "funcs": funcs,
                                         "attributes": []})
            out.ir["package"]["name"] = self.package
            out.ir["package"]["version"] = self.version
            return out
        return _declared(run)

    def __repr__(self) -> str:
        return f"<psio.Interface {self.name}: {', '.join(m.name for m in self.methods)}>"


class _Proxy:
    def __init__(self, interface: Interface, caller: Callable[[Method, bytes], Any]) -> None:
        self.__dict__["_interface"] = interface
        for m in interface.methods:
            self.__dict__[m.attr] = self._bind(m, caller)

    @staticmethod
    def _bind(m: Method, caller: Callable[[Method, bytes], Any]) -> Callable[..., Any]:
        def call(*args: Any, **kwargs: Any) -> Any:
            reply = caller(m, m.to_pssz(*args, **kwargs))
            if m.result is None:
                return None
            return m.result.from_pssz(reply)
        call.__name__ = m.attr
        return call


@overload
def interface(cls: _C) -> _C: ...
@overload
def interface(*, name: Optional[str] = ..., package: str = ..., version: str = ...,
              types: Any = ...) -> Callable[[_C], _C]: ...
def interface(cls: Any = None, *, name: Optional[str] = None, package: str = "", version: str = "",
              types: Any = ()) -> Any:
    """Declares a class's public methods as an interface (a ``Protocol``, an ABC or any class).

    Every function defined in the class body whose name does not start with
    an underscore is a method of the interface, in declaration order;
    ``self`` / ``cls`` are not parameters.  ``name``: the interface's name
    (default: the class name); ``package`` / ``version``: what the schema
    reports; ``types``: the types the interface owns.
    """
    def apply(c: Any) -> Any:
        if version and not package:
            raise SchemaError("psio.interface: version is the package's: give package too")
        c.__psio_interface__ = _reflect(c, name or c.__name__, package, version, list(types))
        return c
    return apply if cls is None else apply(cls)


def interface_of(cls: Any) -> Interface:
    """The :class:`Interface` of a class declared with ``@psio.interface`` (or of any class, reflected now)."""
    known = cls.__dict__.get("__psio_interface__") if isinstance(cls, type) else None
    if known is None:
        known = _reflect(cls, cls.__name__, "", "", [])
        try:
            cls.__psio_interface__ = known
        except (AttributeError, TypeError):
            pass
    return known


def _reflect(cls: Any, name: str, package: str, version: str, types: List[Any]) -> Interface:
    methods: List[Method] = []
    for attr, raw in cls.__dict__.items():
        if attr.startswith("_"):
            continue
        static = isinstance(raw, staticmethod)
        fn = raw.__func__ if isinstance(raw, (staticmethod, classmethod)) else raw
        if not inspect.isfunction(fn):
            continue
        options: Dict[str, Any] = getattr(fn, "__psio_method__", {})
        try:
            hints = typing.get_type_hints(fn, include_extras=True)
        except Exception as e:
            raise SchemaError(f"psio.interface: {cls.__name__}.{attr}: cannot read its type hints: "
                              f"{type(e).__name__}: {e}") from None
        parameters = list(inspect.signature(fn).parameters.values())
        if not static:
            parameters = parameters[1:]  # self / cls
        params: Dict[str, Any] = {}
        for p in parameters:
            if p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD):
                raise SchemaError(f"psio.interface: {cls.__name__}.{attr}: *args / **kwargs have no wire form")
            if p.name not in hints:
                raise SchemaError(f"psio.interface: {cls.__name__}.{attr}: parameter {p.name} has no type hint")
            params[p.name] = hints[p.name]
        cap = options.get("max_dynamic_data")
        max_fields = options.get("max_fields")
        # A positional-only parameter (before `/`) has no name for a caller: the
        # list is then positional, as a C++ method(...) that lists no names.
        named = not any(p.kind is p.POSITIONAL_ONLY for p in parameters)
        shape = arguments(params, max_dynamic_data=cap, max_fields=max_fields, named=named)
        methods.append(Method(len(methods), options.get("name") or _wire_name(attr), attr,
                              [f.name for f in shape.fields] if named else [], list(params), shape,
                              hints.get("return"), cap, not static, max_fields, named, name))
    if len({m.name for m in methods}) != len(methods):
        raise SchemaError(f"psio.interface: {cls.__name__}: two methods have the same name")
    return Interface(cls, name, methods, package, version, types)
