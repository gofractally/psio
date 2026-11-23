"""Methods and interfaces: a class's method signatures described once, for calls, dispatch and the Schema IR.

The Python counterpart of C++ ``PSIO_REFLECT(T, method(...))`` + ``PSIO_INTERFACE(T)`` and of Rust
``#[psio::interface]``.  Nothing restates a method's name, its parameter names or its types:

    @psio.interface(name="token")
    class Token(Protocol):
        @psio.method(max_dynamic_data=1024)
        def transfer(self, to: Annotated[str, psio.max(16)], amount: u64,
                     memo: Annotated[str, psio.max(80)]) -> None: ...
        def balance_of(self, owner: Annotated[str, psio.max(16)]) -> u64: ...

    token = psio.interface_of(Token)
    data = token["transfer"].to_pssz("bob", 5, memo="hi")     # the argument tuple
    reply = token.dispatch(ledger, "balance_of", args_bytes)   # validate, call, encode the result
    remote = token.proxy(send)                                 # remote.transfer("bob", 5, "hi")

The arguments of a method are the tuple of its parameters (docs/pssz-spec.md 16): parameters are
annotated like record members, ``max_dynamic_data`` caps the encoded arguments and fixes their
offset width for the method's lifetime, and the only compatible change is an appended
``Optional`` parameter.
"""

from __future__ import annotations

import inspect
import typing
from typing import Any, Callable, Dict, Iterator, List, Optional, TypeVar, Union, overload

from .errors import EncodeError, SchemaError
from .schema import Schema, SchemaType
from .typed import Builder, _wire_name, arguments, codec, shape_of

__all__ = ["interface", "interface_of", "method", "Interface", "Method"]


_C = TypeVar("_C")
_F = TypeVar("_F")


@overload
def method(fn: _F) -> _F: ...
@overload
def method(*, max_dynamic_data: Optional[int] = ..., name: Optional[str] = ...) -> Callable[[_F], _F]: ...
def method(fn: Any = None, *, max_dynamic_data: Optional[int] = None, name: Optional[str] = None) -> Any:
    """Method-level annotations: the cap of the encoded arguments, another wire name."""
    def apply(f: Any) -> Any:
        target = f.__func__ if isinstance(f, (staticmethod, classmethod)) else f
        target.__psio_method__ = {"max_dynamic_data": max_dynamic_data, "name": name}
        return f
    return apply if fn is None else apply(fn)


class Method:
    """One method of an interface."""

    def __init__(self, index: int, name: str, attr: str, params: List[str], attrs: List[str], shape: Any,
                 result_hint: Any, max_dynamic_data: Optional[int], bound: bool) -> None:
        self.index = index
        #: The method's name in the interface.
        self.name = name
        #: The Python attribute that implements it.
        self.attr = attr
        #: The parameter names, in order.
        self.params = params
        self._attrs = attrs
        self._shape = shape
        self.max_dynamic_data = max_dynamic_data
        self._result_hint = result_hint
        self._args: Optional[SchemaType] = None
        self._result: Any = False
        self._bound = bound

    @property
    def args(self) -> SchemaType:
        """The codec of the argument tuple: values are tuples of the arguments."""
        if self._args is None:
            self._args = codec(self._shape)
        return self._args

    @property
    def result(self) -> Optional[SchemaType]:
        """The codec of the result, or ``None`` for a method that returns nothing."""
        if self._result is False:
            self._result = None if self._result_hint in (None, type(None)) else codec(self._result_hint)
        return self._result

    def arguments(self, *args: Any, **kwargs: Any) -> tuple:
        """The argument tuple of a call: positional and keyword arguments in parameter order."""
        count = len(self.params)
        if len(args) > count:
            raise EncodeError(f"psio: {self.name} takes {count} arguments")
        out = list(args) + [None] * (count - len(args))
        for key, value in kwargs.items():
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
        return f"<psio.Method {self.name}({', '.join(self.params)})>"


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
        return b"" if m.result is None else m.result.to_pssz(out)

    def proxy(self, caller: Callable[[Method, bytes], Any]) -> Any:
        """An object with one function per method that hands the encoded arguments to ``caller``.

        ``caller(method, args_bytes)`` returns the encoded result (or ``None``
        / ``b""`` for a method without one); the proxy validates and decodes it.
        """
        return _Proxy(self, caller)

    def schema(self) -> Schema:
        """The Schema IR: the ``Interface`` and ``Func`` nodes, and every type they reach."""
        builder = Builder()
        funcs = []
        for m in self.methods:
            params = [builder._parameter(f) for f in m._shape.fields]
            result = None
            if m._result_hint not in (None, type(None)):
                result = builder._annotated(shape_of(m._result_hint), None)
            attributes = [] if m.max_dynamic_data is None else [
                {"name": "maxDynamicData", "value": str(m.max_dynamic_data)}]
            funcs.append({"name": m.name, "params": params, "result": result, "attributes": attributes})
        type_names = []
        for t in self.types:
            ref = builder.insert(shape_of(t))
            type_names.append(ref["value"].value["type"])
        out = builder.schema()
        out.ir["interfaces"].append({"name": self.name, "type_names": type_names, "funcs": funcs, "attributes": []})
        out.ir["package"]["name"] = self.package
        out.ir["package"]["version"] = self.version
        return out

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
        shape = arguments(params, max_dynamic_data=cap)
        methods.append(Method(len(methods), options.get("name") or _wire_name(attr), attr,
                              [f.name for f in shape.fields], list(params), shape, hints.get("return"), cap,
                              not static))
    if len({m.name for m in methods}) != len(methods):
        raise SchemaError(f"psio.interface: {cls.__name__}: two methods have the same name")
    return Interface(cls, name, methods, package, version, types)
