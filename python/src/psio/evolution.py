"""The schema comparison: may a type be replaced by a revision of it, and are two types the same
(docs/pssz-spec.md 7.5, 7.6, 16).

    assert psio.evolves_from(AccountV1, AccountV2)
    status = psio.evolves_from(old_schema.type("Account"), new_schema.type("Account"))
    if not status:
        print(status.code, status.path, status.reason)      # "bound-tightened", ".tags", "the length ..."
    psio.equivalent(a.type("Key"), b.type("Key"), mode="wire")
    psio.evolves_from(old_schema.interface("token"), new_schema.interface("token"))
    psio.evolves_from(old_schema, new_schema)               # every interface, by name

``evolves_from(Old, New)`` holds when, in the automatic profile, New's reader reads **every** value
Old's writer can produce, with the same bytes (C++ ``psio::pssz_evolves_from<Old, New>`` and its
run-time twin ``psio::pssz_ir::evolves_from``).  It is one-directional: Old's reader reads the
values of New that are also values of Old's type, reports appended fields as unknown and rejects a
value that uses a bound New widened.  ``equivalent(A, B)`` is the symmetric relation: the same
values, the same bytes, the same permitted revisions (C++ ``psio::pssz_ir::equivalent``).

Two things can be asked for (``mode``):

* ``"wire"``: the bytes, and which bytes are values - the kind of every node, length bounds
  (wherever written: in the type or on the member), every offset width,
  ``definition_will_not_change``, ``max_dynamic_data``, ``max_fields``, boxes, the key type of a
  map / element type of a set, and (typed declarations only: the Schema IR does not record them)
  ``sorted`` / ``unique`` / ``unique_keys``;
* ``"contract"`` (the default, as in C++): also what a consumer of the text forms and of the type's
  identity sees - the names of record members and variant alternatives, custom ids (a time
  presentation, a custom type, ``json``), enums (a revision may add enumerators), the text shape of
  a record (an object, a named or a positional argument list), ``json_number`` / ``json_string``
  and their I-JSON twins, an alternative's I-JSON name, and whether a variant is ``untagged``.

The rules of ``evolves_from``, at every depth:

* a record's (tuple's, argument list's) fields are the old fields in order, followed only by
  ``Optional`` fields; a ``definition_will_not_change`` record gains none, and that status is kept;
* every offset width is unchanged (a widened bound or an appended field can move one: reserve room
  with ``max_dynamic_data`` to evolve);
* a length bound may only **widen**: the new ``[min, max]`` contains the old one;
* UTF-8 text and raw bytes are different types, in both directions;
* ``max_dynamic_data``: the new cap admits everything the old writer can produce (at least the old
  cap; or, where there was none, at least the old inferred bound);
* ``max_fields``: the revision has no more fields than the original's cap, and neither raises nor
  drops it;
* the key type of a map / the element type of a set is equivalent; a variant keeps its alternatives
  in order (new ones are appended); a nested value's inner type evolves by these same rules as a
  root of its own.

Functions (``Schema.method`` / ``Interface[...]``) compare their argument list and their result
(present on both sides or on neither); interfaces (``Schema.interface`` / ``psio.interface_of``)
match functions and listed types by name; whole schemas match interfaces by name.

The result is a :class:`Compatibility`: truthy, or the first rule broken as a stable ``code``
(shared with the other languages through ``conformance/evolution-vectors.json``), a ``reason`` for
a person and the ``path`` from the root: ``.field``, ``[]`` (an element; a map entry's key and
value are ``[].0`` / ``[].1``), ``.2`` (a tuple element), ``<Alt>`` (an alternative), ``?`` (an
optional's payload), ``(nested)``; for a function ``(args)`` / ``(result)``, preceded by its name
in an interface (``transfer(args).amount``) or ``type:Name`` for a listed type; for a whole schema
that is preceded by the interface name and ``/``.  The root itself is ``(root)``.
"""

from __future__ import annotations

from typing import Any, Callable, List, Mapping, Optional, Set, Tuple

from ._core import Codec, LengthBound, Member, Node, strip
from .errors import PsioError, SchemaError
from .schema import ResultType, Schema, SchemaInterface, SchemaMethod, SchemaType, Status

__all__ = ["evolves_from", "equivalent", "Compatibility"]

MODES = ("wire", "contract")


class Compatibility(Status):
    """The outcome of :func:`evolves_from` / :func:`equivalent`: truthy, or the first rule broken.

    ``code`` is the stable identifier of the rule (``conformance/evolution-vectors.md``; ``""`` when
    compatible), ``reason`` a sentence for a person and ``path`` where, from the root.
    """

    __slots__ = ("code", "path", "_reason")

    def __init__(self, ok: bool, code: str = "", reason: str = "", path: str = "") -> None:
        super().__init__(ok, False, None if ok else SchemaError(f"psio: {path}: {reason}"))
        self.code = code
        self.path = path
        self._reason = reason

    @property
    def reason(self) -> str:
        return self._reason

    def __repr__(self) -> str:
        if self.ok:
            return "Compatibility(ok=True)"
        return f"Compatibility(ok=False, code={self.code!r}, path={self.path!r}, reason={self.reason!r})"


class _No(Exception):
    def __init__(self, code: str, reason: str) -> None:
        super().__init__(reason)
        self.code = code
        self.reason = reason


def _no(code: str, reason: str) -> Any:
    raise _No(code, reason)


#: Where a value sits: the width class of the cap of the nearest enclosing record that has one
#: (4: none) and the ``lengthBound`` of the member holding it (it reaches through optionals only).
_Pos = Tuple[int, Optional[LengthBound]]
_ROOT: _Pos = (4, None)

_Range = Tuple[int, Optional[int]]


def _number(n: Optional[int]) -> str:
    return "unbounded" if n is None else str(n)


def _range_of(container: Optional[Node], bound: Optional[LengthBound]) -> _Range:
    """The lengths a position admits: the bound of its container type and of the member that holds it."""
    low, high = 0, None
    if container is not None and container.bounded:
        high = container.count
        if container.exact:
            low = container.count
    if bound is not None:
        lower = bound.exact if bound.exact is not None else bound.min
        upper = bound.exact if bound.exact is not None else bound.max
        if lower is not None and lower > low:
            low = lower
        if upper is not None and (high is None or upper < high):
            high = upper
    return low, high


def _range_text(r: _Range) -> str:
    return f"[{r[0]}, {_number(r[1])}]"


def _bound_key(b: Optional[LengthBound]) -> Any:
    return None if b is None else (b.exact, b.max, b.min)


_KIND = {"int": "integer", "char": "integer", "float": "float", "bool": "bool", "string": "string", "hex": "bytes",
         "bitvector": "bit vector", "bitlist": "bit list", "option": "optional", "list": "list", "array": "array",
         "variant": "variant", "record": "record", "tuple": "record", "nested": "nested value"}


def _kind(node: Node) -> str:
    if node.assoc:
        return node.assoc
    return _KIND.get(node.kind, "unsupported type")


class _Peeled:
    """A type with the nodes that add nothing to its bytes taken off."""

    __slots__ = ("wire", "boxed", "ids", "enumerators", "kind")

    def __init__(self, ref: Node) -> None:
        self.boxed = False
        self.ids: List[str] = []
        hops = 0
        while ref.kind == "custom":
            if ref.inner is None or hops > 4096:
                _no("malformed-schema", "cyclic named aliases")
            if ref.boxed:
                self.boxed = True
            else:
                self.ids.append(ref.id)
            ref = ref.inner  # type: ignore[assignment]
            hops += 1
        if ref.time is not None:
            self.ids.append(ref.extra.get("time_id", ""))
        elif ref.json:
            self.ids.append("json")
        self.wire = ref
        self.enumerators = ref.enumerators
        self.kind = _kind(ref)


def _text_shape(node: Node) -> str:
    if node.kind == "tuple":
        return "positional"
    if node.tuple is None:
        return "object"
    return "named" if node.tuple == "named" else "positional"


class _Compare:
    """One comparison: the port of ``psio::pssz_ir::detail::Comparer`` (pssz_ir_compare.hpp) to compiled nodes."""

    def __init__(self, a: Codec, b: Codec, names: bool) -> None:
        self.a = a
        self.b = b
        self.names = names
        self.path = ""
        self.seen: Set[Any] = set()

    # (the path is left as it is when a rule fails: it says where)
    def _at(self, step: str, run: Callable[[], None]) -> None:
        size = len(self.path)
        self.path += step
        run()
        self.path = self.path[:size]

    def _ranges(self, was: _Range, now: _Range, exact: bool, what: str) -> None:
        if exact:
            if was != now:
                _no("bound-changed", f"the length bound of {what} differs ({_range_text(was)} / {_range_text(now)})")
            return
        if now[0] > was[0] or (now[1] is not None and (was[1] is None or now[1] < was[1])):
            _no("bound-tightened",
                f"the length bound of {what} was tightened ({_range_text(was)} -> {_range_text(now)})")

    @staticmethod
    def _width(codec: Codec, node: Node, pos: _Pos) -> int:
        return codec.width_of(node, (pos[0], pos[1].limit if pos[1] is not None else None, False))

    def _widths(self, a: Node, b: Node, pa: _Pos, pb: _Pos, what: str) -> None:
        was, now = self._width(self.a, a, pa), self._width(self.b, b, pb)
        if was != now:
            _no("width-changed", f"the offset width of {what} differs ({was} / {now})")

    def _enums(self, a: _Peeled, b: _Peeled, exact: bool) -> None:
        if (a.enumerators is None) != (b.enumerators is None):
            _no("enum-changed", "an enum on one side only")
        if a.enumerators is None or b.enumerators is None:
            return

        def has(members: Any, e: Mapping[str, Any]) -> bool:
            for other in members:
                if other["name"] == e["name"]:
                    return bool(other["value"] == e["value"])
            return False

        for e in a.enumerators:
            if not has(b.enumerators, e):
                _no("enum-changed", f'the enumerator "{e["name"]}" was removed or renumbered')
        if exact:
            for e in b.enumerators:
                if not has(a.enumerators, e):
                    _no("enum-changed", f'the enumerator "{e["name"]}" is on one side only')

    def walk(self, ta: Node, tb: Node, pa: _Pos, pb: _Pos, exact: bool) -> None:
        a, b = _Peeled(ta), _Peeled(tb)
        if a.boxed != b.boxed:
            _no("boxed-changed", "a boxed value on one side only")
        if self.names:
            if a.ids != b.ids:
                first_a, first_b = (a.ids[0] if a.ids else ""), (b.ids[0] if b.ids else "")
                _no("custom-id-changed",
                    f'a custom type on one side only, or under another id ("{first_a}" / "{first_b}")')
            self._enums(a, b, exact)
        # (a recursive type: the pair is being, or has been, compared)
        key = (id(a.wire), id(b.wire), exact, pa[0], pb[0], _bound_key(pa[1]), _bound_key(pb[1]))
        if key in self.seen:
            return
        self.seen.add(key)
        if a.kind != b.kind:
            _no("type-changed", f"the type differs ({a.kind} / {b.kind})")
        if a.kind == "unsupported type":
            _no("malformed-schema", "a type the codec does not support")
        o, n = a.wire, b.wire
        old, new = self.a, self.b
        la, lb = old.layout(o), new.layout(n)
        below_a: _Pos = (pa[0], None)
        below_b: _Pos = (pb[0], None)

        def fixed_size() -> None:
            if la[0] != lb[0] or (la[0] and la[1] != lb[1]):
                _no("fixed-size-changed",
                    "a fixed-size type on one side and a variable-size type on the other, or another fixed size")

        k = a.kind
        if k == "integer":
            def integer(x: Node) -> Tuple[int, bool]:
                return (8, False) if x.kind == "char" else (x.bits, x.signed)
            if integer(o) != integer(n):
                _no("integer-changed", "the integer type differs (bits or signedness)")
        elif k == "float":
            if la[1] != lb[1]:
                _no("float-changed", "the float type differs")
        elif k == "bool":
            pass
        elif k == "bit vector":
            if o.size != n.size:
                _no("bit-count-changed", f"the bit count differs ({o.size} / {n.size})")
        elif k == "bit list":
            self._ranges((0, o.size), (0, n.size), exact, "the bit list")
        elif k in ("string", "bytes"):
            if o.raw != n.raw:
                _no("utf8-changed", "utf8 on one side only (a raw string and a UTF-8 string are different types)")
            fixed_size()
            self._ranges(_range_of(o.child, pa[1]), _range_of(n.child, pb[1]), exact,
                         "the string" if k == "string" else "the bytes")
        elif k == "nested value":
            self._ranges(_range_of(None, pa[1]), _range_of(None, pb[1]), exact, "the nested value")
            # The inner value is a root of its own: no cap or bound reaches it.
            self._at("(nested)", lambda: self.walk(o.text_child, n.text_child, _ROOT, _ROOT, exact))
        elif k == "optional":
            if old.can_be_empty(o.child) != new.can_be_empty(n.child):
                _no("optional-form-changed", "the optional's presence form differs (a selector byte or not)")
            # The member's bound and the cap reach the payload.
            self._at("?", lambda: self.walk(o.text_child, n.text_child, pa, pb, exact))
        elif k in ("list", "array"):
            if k == "array":
                if o.count != n.count:
                    _no("array-length-changed", f"the array length differs ({o.count} / {n.count})")
            else:
                self._ranges(_range_of(o, pa[1]), _range_of(n, pb[1]), exact, "the list")
            if not (old.layout(o.child)[0] and new.layout(n.child)[0]):
                self._widths(o, n, pa, pb, "the array" if k == "array" else "the list")
            self._at("[]", lambda: self.walk(o.text_child, n.text_child, below_a, below_b, exact))
        elif k in ("map", "set"):
            what = "the map" if k == "map" else "the set"
            self._ranges(_range_of(o, pa[1]), _range_of(n, pb[1]), exact, what)
            if not (old.layout(o.child)[0] and new.layout(n.child)[0]):
                self._widths(o, n, pa, pb, what)
            # Another key type is another order (docs/pssz-spec.md 5.9.4): the keys are the same type,
            # in both relations.
            try:
                self._at("[].0" if k == "map" else "[]",
                         lambda: self.walk(o.key_ref, n.key_ref, below_a, below_b, True))
            except _No as e:
                raise _No("key-type-changed", f"the key type of {what} differs: {e.reason}") from None
            if k == "set":
                return
            # The entry is a tuple (key, value): its width, then the value.
            self._at("[]", lambda: self._widths(o.child, n.child, below_a, below_b, "the map entry"))
            self._at("[].1", lambda: self.walk(o.value_ref, n.value_ref, below_a, below_b, exact))
        elif k == "variant":
            count_a, count_b = len(o.members), len(n.members)
            if exact and count_a != count_b:
                _no("alternative-count-changed", f"the number of alternatives differs ({count_a} / {count_b})")
            if count_b < count_a:
                _no("alternatives-removed", f"variant alternatives were removed ({count_a} -> {count_b})")
            if self.names and o.untagged != n.untagged:
                _no("text-form-changed", "an untagged variant on one side only")
            for i in range(count_a):
                ma, mb = o.members[i], n.members[i]
                if self.names and ma.name != mb.name:
                    _no("alternative-renamed",
                        f'alternative {i} was renamed or reordered ("{ma.name}" -> "{mb.name}")')

                def alternative(ma: Member = ma, mb: Member = mb) -> None:
                    self.walk(ma.text, mb.text, below_a, below_b, exact)
                self._at(f"<{ma.name}>", alternative)
        elif k == "record":
            fixed_size()
            self._record(o, n, pa, pb, exact)
        else:
            _no("malformed-schema", "a type the codec does not support")

    def _record(self, o: Node, n: Node, pa: _Pos, pb: _Pos, exact: bool) -> None:
        old, new = self.a, self.b
        tuple_a, tuple_b = o.kind == "tuple", n.kind == "tuple"
        text_a, text_b = _text_shape(o), _text_shape(n)
        if o.stable != n.stable:
            _no("dwnc-changed", "definitionWillNotChange on one side only")
        if self.names and text_a != text_b:
            _no("text-form-changed",
                "the text shape of the record differs (an object, a named argument list or positional)")
        count, count_b = len(o.members), len(n.members)
        if exact and count_b != count:
            _no("field-count-changed", f"the number of fields differs ({count} / {count_b})")
        if count_b < count:
            _no("fields-removed", f"fields were removed ({count} -> {count_b})")
        if o.stable and count_b != count:
            _no("dwnc-gained-fields", "a definitionWillNotChange record gained fields")
        # A record's width is its own wherever it appears; a tuple's is also bounded by the cap around it.
        if not o.stable:
            self._widths(o, n, pa, pb, "the record")
        # The members lie beneath the record's own cap, or beneath none; a Tuple node has none of its own
        # and passes on the one around it.
        from ._core import width_class
        cap_a = pa[0] if tuple_a else (width_class(o.cap) if o.cap is not None else 4)
        cap_b = pb[0] if tuple_b else (width_class(n.cap) if n.cap is not None else 4)
        orders_a = dict(o.bind.order) if o.bind is not None and o.bind.order else {}
        orders_b = dict(n.bind.order) if n.bind is not None and n.bind.order else {}
        for i in range(count):
            ma, mb = o.members[i], n.members[i]
            step = f".{ma.name}" if not tuple_a and text_a != "positional" else f".{i}"
            if self.names and not tuple_a and not tuple_b and text_a != "positional" and ma.name != mb.name:
                # (reported at the record: the member has two names)
                _no("field-renamed", f'field {i} was renamed or reordered ("{ma.name}" -> "{mb.name}")')

            def member(ma: Member = ma, mb: Member = mb, i: int = i) -> None:
                if self.names and not tuple_a and not tuple_b:
                    for form, x, y in (("jsonInt", ma.json_int, mb.json_int), ("ijsonInt", ma.ijson_int, mb.ijson_int)):
                        if x != y:
                            _no("text-form-changed", f"{form} differs (an integer as a number or a string)")
                self.walk(ma.text, mb.text, (cap_a, None if tuple_a else ma.bound),
                          (cap_b, None if tuple_b else mb.bound), exact)
                # (typed declarations: the Schema IR does not record these)
                order_a, order_b = orders_a.get(i), orders_b.get(i)
                if (order_a is None) != (order_b is None) or (order_a is not None and not order_a.same(order_b)):
                    _no("order-changed", "sorted / unique / unique_keys changed")
            self._at(step, member)
        # An appended member must be able to be absent.
        for i in range(count, count_b):
            m = n.members[i]
            if not new.layout(m.node)[2]:
                name = str(i) if tuple_b or text_b == "positional" else f'"{m.name}"'
                _no("appended-field-not-optional",
                    f"the appended field {name} is not optional (old data has no value for it)")
        # Caps (docs/pssz-spec.md 7.5, 16).
        if exact:
            if o.cap != n.cap:
                _no("max-dynamic-data",
                    f"maxDynamicData differs ({_number(o.cap)} / {_number(n.cap)}; unbounded: none)")
            if o.max_fields != n.max_fields:
                _no("max-fields",
                    f"maxFields differs ({_number(o.max_fields)} / {_number(n.max_fields)}; unbounded: none)")
            return
        if n.cap is not None:
            limit = o.cap
            if limit is None:
                limit = old.auto.bound_of(o, None, False, [])
                if limit is None:
                    _no("max-dynamic-data", f"maxDynamicData ({n.cap}) was added to a record of unbounded size")
            if limit is not None and limit > n.cap:
                _no("max-dynamic-data",
                    f"maxDynamicData was lowered ({o.cap} -> {n.cap})" if o.cap is not None else
                    f"maxDynamicData ({n.cap}) is below the bound inferred for the old record ({limit})")
        if o.max_fields is not None:
            if count_b > o.max_fields:
                _no("max-fields", f"more fields ({count_b}) than the original's maxFields ({o.max_fields})")
            if n.max_fields is None:
                _no("max-fields", f"maxFields ({o.max_fields}) was dropped")
            elif n.max_fields > o.max_fields:
                _no("max-fields", f"maxFields was raised ({o.max_fields} -> {n.max_fields})")

    # ── Types, functions, interfaces, schemas ────────────────────────────────

    def types(self, a: SchemaType, b: SchemaType, exact: bool) -> None:
        def root(t: SchemaType) -> _Pos:
            if isinstance(t, ResultType):
                return (4, t._bound)
            if t._pos[1] is None:
                return _ROOT
            bound = LengthBound()
            bound.max = t._pos[1]
            return (4, bound)
        # (typed declarations: the order constraints of the root)
        x, y = getattr(a, "_order", None), getattr(b, "_order", None)
        if (x is None) != (y is None) or (x is not None and not x.same(y)):
            _no("order-changed", "sorted / unique / unique_keys changed")
        self.walk(a._ref, b._ref, root(a), root(b), exact)

    def function(self, fa: Callable[[], Any], fb: Callable[[], Any], exact: bool) -> None:
        """``fa`` / ``fb`` give the two methods (anything with ``args`` and ``result``)."""
        made: List[Any] = []

        def args() -> None:
            made.append(self._first(fa))
            made.append(self._second(fb))
            self.types(made[0].args, made[1].args, exact)
        self._at("(args)", args)

        def result() -> None:
            ra, rb = made[0].result, made[1].result
            if (ra is None) != (rb is None):
                _no("result-changed", "a result on one side only")
            if ra is not None:
                self.types(ra, rb, exact)
        self._at("(result)", result)

    @staticmethod
    def _first(make: Callable[[], Any]) -> Any:
        try:
            return make()
        except PsioError as e:
            raise _No("malformed-schema", f"the first schema: {e.reason}") from None

    @staticmethod
    def _second(make: Callable[[], Any]) -> Any:
        try:
            return make()
        except PsioError as e:
            raise _No("malformed-schema", f"the second schema: {e.reason}") from None

    def interface(self, ia: SchemaInterface, ib: SchemaInterface, exact: bool) -> None:
        names_a, names_b = ia.names(), ib.names()
        for name in names_a:
            def one(name: str = name) -> None:
                if name not in names_b:
                    _no("function-removed", "the function was removed")
                self.function(lambda: ia.method(name), lambda: ib.method(name), exact)
            self._at(name, one)
        if exact:
            for name in names_b:
                if name not in names_a:
                    self.path += name
                    _no("function-added", "the function is on one side only")
        types_a, types_b = ia.type_names, ib.type_names
        for name in types_a:
            def listed(name: str = name) -> None:
                if name not in types_b:
                    _no("type-removed", "the type was removed from the interface")
                ta = self._first(lambda: ia.schema.type(name))
                tb = self._second(lambda: ib.schema.type(name))
                self.types(ta, tb, exact)
            self._at("type:" + name, listed)
        if exact:
            for name in types_b:
                if name not in types_a:
                    self.path += "type:" + name
                    _no("type-added", "the type is on one side only")

    def schemas(self, a: Schema, b: Schema, exact: bool) -> None:
        names_a, names_b = a.interfaces(), b.interfaces()
        for name in names_a:
            def one(name: str = name) -> None:
                if name not in names_b:
                    _no("interface-removed", "the interface was removed")
                self.interface(a.interface(name), b.interface(name), exact)
            self._at(name + "/", one)
        if exact:
            for name in names_b:
                if name not in names_a:
                    self.path += name + "/"
                    _no("interface-added", "the interface is on one side only")


def _compiled(tp: Any) -> SchemaType:
    if isinstance(tp, SchemaType):
        return tp
    from .typed import codec
    return codec(tp)


def _automatic(what: str, *types: Optional[SchemaType]) -> None:
    for t in types:
        if t is not None and not t._codec.automatic:
            raise SchemaError(f'psio.{what} compares types of the automatic profile (width="auto")')


def _run(what: str, old: Any, new: Any, mode: str, exact: bool) -> Compatibility:
    from .interface import Interface, Method
    if mode not in MODES:
        raise SchemaError(f'psio.{what}: mode must be "wire" or "contract"')
    names = mode == "contract"
    kinds = ((Schema,), (SchemaInterface, Interface), (SchemaMethod, Method))
    for group in kinds:
        if isinstance(old, group) != isinstance(new, group):
            raise SchemaError(f"psio.{what}: the two sides must be of one kind: two types, two methods, "
                              "two interfaces or two schemas")
    check: Optional[_Compare] = None
    try:
        if isinstance(old, Schema):
            check = _Compare(old._codec(0), new._codec(0), names)
            check.schemas(old, new, exact)
        elif isinstance(old, kinds[1]):
            ia = old.schema().interface(old.name) if isinstance(old, Interface) else old
            ib = new.schema().interface(new.name) if isinstance(new, Interface) else new
            check = _Compare(ia.schema._codec(0), ib.schema._codec(0), names)
            check.interface(ia, ib, exact)
        elif isinstance(old, kinds[2]):
            _automatic(what, old.args, old.result, new.args, new.result)
            check = _Compare(old.args._codec, new.args._codec, names)
            check.function(lambda: old, lambda: new, exact)
        else:
            o, n = _compiled(old), _compiled(new)
            _automatic(what, o, n)
            check = _Compare(o._codec, n._codec, names)
            check.types(o, n, exact)
    except _No as e:
        return Compatibility(False, e.code, e.reason, (check.path if check is not None else "") or "(root)")
    except RecursionError:
        return Compatibility(False, "malformed-schema", "type nesting too deep",
                             (check.path if check is not None else "") or "(root)")
    except SchemaError as e:
        if check is None:
            raise
        return Compatibility(False, "malformed-schema", e.reason, check.path or "(root)")
    return Compatibility(True)


def evolves_from(old: Any, new: Any, *, mode: str = "contract") -> Compatibility:
    """Whether ``new`` may replace ``old``: its reader reads every value ``old``'s writer can produce.

    ``old`` and ``new`` are two types - Python types or :class:`psio.SchemaType` values
    (``schema.type(name)``, ``method.args``, ``method.result``) of the automatic profile -, two
    methods (``schema.method(...)``, ``psio.interface_of(C)[name]``: the argument list and the
    result), two interfaces (``schema.interface(name)``, ``psio.interface_of(C)``: every function
    and listed type of ``old``, by name) or two :class:`psio.Schema` values (every interface of
    ``old``, by name).  ``mode``: ``"contract"`` (the default) or ``"wire"`` (the bytes only: names,
    custom ids, enums and text forms are not compared).

    The result is truthy when the revision is compatible; otherwise ``code`` is the first rule it
    breaks, ``path`` where and ``reason`` a sentence.
    """
    return _run("evolves_from", old, new, mode, False)


def equivalent(a: Any, b: Any, *, mode: str = "contract") -> Compatibility:
    """Whether ``a`` and ``b`` are the same type: the same values, the same bytes, the same permitted
    revisions (and, under ``mode="contract"``, the same text).  Arguments and result as :func:`evolves_from`."""
    return _run("equivalent", a, b, mode, True)
