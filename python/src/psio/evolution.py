"""The schema-upgrade check: may a type be replaced by a revision of it (docs/pssz-spec.md 7.5, 7.6).

    assert psio.evolves_from(AccountV1, AccountV2)
    status = psio.evolves_from(old_schema.type("Account"), new_schema.type("Account"))
    if not status:
        print(status.reason)

``evolves_from(Old, New)`` holds when, in the automatic profile, New's reader reads **every** value
Old's writer can produce, with the same bytes (C++ ``psio::pssz_evolves_from<Old, New>``).  It is
one-directional: Old's reader reads the values of New that are also values of Old's type, reports
appended fields as unknown and rejects a value that uses a bound New widened.

The rules, at every depth:

* a record's (tuple's, argument list's) fields are the old fields in order, followed only by
  ``Optional`` fields; a ``definition_will_not_change`` record gains none, and that status is kept;
* every offset width is unchanged (a widened bound or an appended field can move one: reserve room
  with ``max_dynamic_data`` to evolve);
* a length bound may only **widen**: the new ``[min, max]`` contains the old one, whether the bound
  is in the type or on the member;
* UTF-8 text and raw bytes are different types, in both directions;
* ``sorted`` / ``unique`` / ``unique_keys`` are the same on both sides (typed declarations: the
  Schema IR does not record them);
* ``max_dynamic_data``: the new cap admits everything the old writer can produce (at least the old
  cap; or, where there was none, at least the old inferred bound);
* ``max_fields``: the revision has no more fields than the original's cap, and neither raises nor
  drops it;
* the key type of a map / the element type of a set is unchanged; a variant keeps its alternatives
  in order (new ones are appended); a nested value's inner type evolves by these same rules.

The check works on the compiled types, so it compares two Python declarations, two types of two
Schema IRs, or one of each.  Member *names* are not compared: the wire does not carry them (a
renamed member breaks text readers, not binary ones).
"""

from __future__ import annotations

from typing import Any, List, Optional, Set, Tuple

from ._core import ROOT, Codec, LengthBound, Node, Pos, strip
from .errors import SchemaError
from .schema import SchemaType, Status

__all__ = ["evolves_from"]

_INF = float("inf")


class _No(Exception):
    pass


def _no(path: List[str], why: str) -> Any:
    raise _No(f"{'.'.join(path) or 'the type'}: {why}")


def _length_range(node: Node, bound: Optional[LengthBound]) -> Tuple[float, float]:
    """The lengths a position admits: the bound of its type and the bound of the member that holds it."""
    low: float = 0
    high: float = _INF
    container = node.child if node.kind in ("string", "hex") else node
    if node.kind in ("string", "hex", "list", "array"):
        if container.kind == "array":
            low = high = container.count
        elif container.bounded:
            high = container.count
    elif node.kind == "bitlist":
        high = node.size
    if bound is not None:
        if bound.exact is not None:
            low, high = max(low, bound.exact), min(high, bound.exact)
        if bound.min is not None:
            low = max(low, bound.min)
        if bound.max is not None:
            high = min(high, bound.max)
    return low, high


class _Check:
    def __init__(self, old: Codec, new: Codec) -> None:
        self.old = old
        self.new = new
        self.active: Set[Tuple[int, int, Pos, Pos]] = set()

    def evolves(self, o_ref: Node, n_ref: Node, po: Pos, pn: Pos, path: List[str],
                bo: Optional[LengthBound] = None, bn: Optional[LengthBound] = None) -> None:
        # Wrappers: a box and a custom type keep their identity.
        o, n = o_ref, n_ref
        while o.kind == "custom" or n.kind == "custom":
            if o.kind != n.kind or o.id != n.id:
                _no(path, "a custom type (or a box) on one side only, or under another id")
            inner_o, inner_n = o.inner, n.inner
            if inner_o is None or inner_n is None:
                _no(path, "unresolved custom type")
            o, n = inner_o, inner_n  # type: ignore[assignment]
        key = (id(o), id(n), po, pn)
        if key in self.active:
            return  # a recursive type: the pair is being compared
        self.active.add(key)
        try:
            self._wire(o, n, po, pn, path, bo, bn)
        finally:
            self.active.discard(key)

    def _wire(self, o: Node, n: Node, po: Pos, pn: Pos, path: List[str], bo: Optional[LengthBound],
              bn: Optional[LengthBound]) -> None:
        old, new = self.old, self.new
        k = o.kind
        if k != n.kind:
            _no(path, f"the type changed ({k} -> {n.kind})")
        if k == "option":
            if old.can_be_empty(o.child) != new.can_be_empty(n.child):
                _no(path, "the optional's presence form changed (selector byte or not)")
            return self.evolves(o.text_child, n.text_child, po, pn, path, bo, bn)
        if old.layout(o)[:2] != new.layout(n)[:2]:
            _no(path, "the fixed size changed")
        # A length bound may only widen (in the type or on the member), and
        # the bounds are the member's own: they do not reach below it.
        (lo, ho), (ln, hn) = _length_range(o, bo), _length_range(n, bn)
        if ln > lo or hn < ho:
            _no(path, f"a length bound was tightened ([{lo}, {ho}] -> [{ln}, {hn}])")
        if k in ("int", "float", "bool", "char"):
            if (o.bits, o.signed, o.size) != (n.bits, n.signed, n.size):
                _no(path, "the number type changed")
            if o.time != n.time:
                _no(path, "the time presentation changed")
            if (o.enumerators is None) != (n.enumerators is None):
                _no(path, "an enum on one side only")
            if o.enumerators is not None:
                now = {e["name"]: e["value"] for e in n.enumerators or ()}
                if any(now.get(e["name"]) != e["value"] for e in o.enumerators):
                    _no(path, "an enumerator was removed or renumbered")
            return
        if k in ("string", "hex"):
            if o.raw != n.raw:
                _no(path, "utf8 changed (UTF-8 text and raw bytes are different types)")
            return
        if k == "bitvector":
            if o.size != n.size:
                _no(path, "the bit count changed")
            return
        if k == "bitlist":
            return
        if k == "nested":
            # The member stays a byte list; the inner type is a root of its own.
            return self.evolves(o.text_child, n.text_child, ROOT, ROOT, path + ["nested"])
        if k in ("list", "array"):
            if o.assoc != n.assoc:
                _no(path, "a list, a map and a set are different types")
            if k == "array" and o.count != n.count:
                _no(path, "the array length changed")
            if not old.layout(o.child)[0] and old.width_of(o, po) != new.width_of(n, pn):
                _no(path, f"the offset width changed ({old.width_of(o, po)} -> {new.width_of(n, pn)})")
            below_o, below_n = old.below(po), new.below(pn)
            if o.assoc:
                # A different key type has a different order (spec 5.9.4).
                self.evolves(o.key_ref, n.key_ref, below_o, below_n, path + ["key"])
                try:
                    _Check(new, old).evolves(n.key_ref, o.key_ref, below_n, below_o, path + ["key"])
                except _No:
                    _no(path, "the key type of a map / the element type of a set changed")
                if o.assoc == "set":
                    return
            return self.evolves(o.text_child, n.text_child, below_o, below_n, path + ["element"])
        if k == "variant":
            if len(n.members) < len(o.members):
                _no(path, "an alternative was removed")
            below_o, below_n = old.below(po), new.below(pn)
            for mo, mn in zip(o.members, n.members):
                self.evolves(mo.text, mn.text, below_o, below_n, path + [mo.name])
            return
        if k in ("record", "tuple"):
            return self._record(o, n, po, pn, path)
        _no(path, "unsupported type")

    def _record(self, o: Node, n: Node, po: Pos, pn: Pos, path: List[str]) -> None:
        old, new = self.old, self.new
        record = o.kind == "record"
        if record and o.stable != n.stable:
            _no(path, "definition_will_not_change was added or dropped")
        wo = old.width_of(o, po)
        wn = new.width_of(n, pn)
        if wo != wn:
            _no(path, f"the offset width changed ({wo} -> {wn})")
        count = len(o.members)
        if len(n.members) < count:
            _no(path, "a field was removed")
        if record and o.stable and len(n.members) != count:
            _no(path, "a definition_will_not_change record gained a field")
        if record:
            positions_o, positions_n = old.member_positions(o), new.member_positions(n)
        else:
            positions_o, positions_n = [old.below(po)] * count, [new.below(pn)] * len(n.members)
        orders_o = dict(o.bind.order) if o.bind is not None and o.bind.order else {}
        orders_n = dict(n.bind.order) if n.bind is not None and n.bind.order else {}
        for i in range(count):
            mo, mn = o.members[i], n.members[i]
            at = path + [mo.name]
            self.evolves(mo.text, mn.text, positions_o[i], positions_n[i], at, mo.bound if record else None,
                         mn.bound if record else None)
            a, b = orders_o.get(i), orders_n.get(i)
            if (a is None) != (b is None) or (a is not None and not a.same(b)):
                _no(at, "sorted / unique / unique_keys changed")
        for m in n.members[count:]:
            if m.node.kind != "option":
                _no(path + [m.name], "an appended field must be Optional (old data has no value for it)")
        if not record:
            return
        # Caps (spec 7.5): everything the old writer can produce fits the new cap.
        if n.cap is not None:
            limit = o.cap if o.cap is not None else old.auto.bound_of(o, None, False, [])
            if limit is None or limit > n.cap:
                _no(path, "max_dynamic_data was lowered, or added below what the old type can encode to")
        if o.max_fields is not None:
            if len(n.members) > o.max_fields:
                _no(path, f"more fields than the original's max_fields ({o.max_fields})")
            if n.max_fields is None or n.max_fields > o.max_fields:
                _no(path, "max_fields was raised or dropped (the cap is a promise about every later revision)")


def _compiled(tp: Any) -> SchemaType:
    if type(tp) is SchemaType or isinstance(tp, SchemaType):
        return tp
    from .typed import codec
    return codec(tp)


def evolves_from(old: Any, new: Any) -> Status:
    """Whether ``new`` may replace ``old``: its reader reads every value ``old``'s writer can produce.

    ``old`` and ``new`` are Python types or :class:`psio.SchemaType` values
    (``schema.type(name)``, ``method.args``, ``method.result``) of the
    automatic profile.  The result is truthy when the revision is compatible;
    otherwise ``reason`` names the first rule it breaks and where.
    """
    o, n = _compiled(old), _compiled(new)
    if not (o._codec.automatic and n._codec.automatic):
        raise SchemaError("psio.evolves_from compares types of the automatic profile (width=\"auto\")")
    try:
        check = _Check(o._codec, n._codec)
        if strip(o._ref)[1] != strip(n._ref)[1]:
            _no([], "a box on one side only")
        check.evolves(o._ref, n._ref, o._pos, n._pos, [])
    except _No as e:
        return Status(False, False, SchemaError(f"psio.evolves_from: {e}"))
    except RecursionError:
        return Status(False, False, SchemaError("psio.evolves_from: type nesting too deep"))
    return Status(True)
