"""Python declarations of a fixture's types, rebuilt from the Schema IR it records.

Each IR type is turned into the declaration a programmer would write for it —
a frozen ``@dataclass`` with ``Annotated`` members, an ``enum.Enum``, a
``NewType``, ``list[...]`` / ``dict[...]`` / ``Optional[...]`` / unions — so
the fixtures can be replayed through the typed layer (type-hint reflection,
bindings and all).  The round trip is checked: ``psio.schema_of`` of the
declarations, inserted in the emitter's order, must give back the fixture's
IR byte for byte (``test_typed_ir.py``).

A type the typed layer cannot declare raises ``Unsupported`` with the reason;
nothing is skipped silently.  (The counterpart of js/src/test/support/typed-from-ir.ts.)
"""

from __future__ import annotations

import dataclasses
import enum
import keyword
import re
import sys
import types
from typing import Annotated, Any, Dict, List, NewType, Optional, Tuple, Union

from support import FIXTURE_HANDLERS

import psio
from psio import Handler
from psio._time import parse_schema_id
from psio.typed import _class_shape, _enum_shape


class Unsupported(Exception):
    pass


def spelling(t: Any) -> str:
    """The spelling of a type as the IR writes it: two entries with one spelling are one node, two identities."""
    v = t["value"]
    k, b = v.type, v.value
    if k == "Type":
        return b["type"]
    if k == "Tuple":
        return "Tuple[" + ",".join(spelling(m) for m in b["members"]) + "]"
    if k in ("List", "Option", "Nested"):
        return f"{k}<{spelling(b['type'])}>"
    if k == "BoundedList":
        return f"BoundedList<{spelling(b['type'])},{b['maxCount']}>"
    if k == "Custom":
        return f"Custom{{{spelling(b['type'])},{b['id']!r}}}"
    return ""


def references(t: Any, out: set) -> None:
    v = t["value"]
    k, b = v.type, v.value
    if k == "Type":
        out.add(b["type"])
    elif k == "Tuple":
        for m in b["members"]:
            references(m, out)
    elif k in ("Struct", "Object", "Variant"):
        for m in b["members"]:
            references(m["type"], out)
    elif k in ("List", "Option", "Nested", "BoundedList", "Array", "Custom", "Enum"):
        references(b["type"], out)


INTS = {"u8": psio.u8, "u16": psio.u16, "u32": psio.u32, "u64": psio.u64, "u128": psio.u128, "u256": psio.u256,
        "i8": psio.i8, "i16": psio.i16, "i32": psio.i32, "i64": psio.i64, "i128": psio.i128}

_counter = iter(range(1 << 62))


class TypedFixture:
    def __init__(self, ir: Dict[str, Any], handlers: Optional[Dict[str, Handler]] = None) -> None:
        self.ir = ir
        self.handlers = handlers or {}
        self.by_name = {m["name"]: m for m in ir["types"]}
        self.hints: Dict[str, Any] = {}
        self.building: set = set()
        self.plain = psio.Schema(ir, _normalized=True)
        # The generated classes live in a module of their own: forward references resolve there.
        self.module = types.ModuleType(f"psio_fixture_{next(_counter)}")
        sys.modules[self.module.__name__] = self.module
        self.class_names: Dict[str, str] = {}
        # What the IR does not say about an interned (`@N`) node, because C++
        # gives two types the same node (docs/schema-builder.md): a second
        # 2-tuple of the same spelling is a std::pair, a second map / set an
        # unordered container; a nested value whose id says `,deferred>` is deferred.
        self.alternate: set = set()
        self.deferred: set = set()
        # A named alias that other types refer to (or that precedes its
        # target) is a transparent type, not a name given to insert<T>(name).
        self.transparents: set = set()
        types_ = ir["types"]
        index = {m["name"]: i for i, m in enumerate(types_)}
        referenced: set = set()
        map_entries: set = set()

        def visit(t: Any) -> None:
            v = t["value"]
            if v.type == "Custom" and v.value["id"] == "map":
                lst = v.value["type"]["value"]
                if lst.type in ("List", "BoundedList") and lst.value["type"]["value"].type == "Type":
                    map_entries.add(lst.value["type"]["value"].value["type"])
            if v.type in ("Object", "Struct", "Variant"):
                for x in v.value["members"]:
                    visit(x["type"])

        for m in types_:
            if m["type"]["value"].type != "Type":
                references(m["type"], referenced)
            visit(m["type"])
        groups: Dict[str, List[str]] = {}
        # `@N` is the N-th interned type: the count of the interned entries
        # before it says how many of the named aliases since the previous `@N`
        # are transparent types (interned) and not names given to
        # insert<T>(name) (not interned).
        interned = 0
        pending: List[str] = []
        for m in types_:
            t = m["type"]["value"]
            name = m["name"]
            if name.startswith("@"):
                marked = sum(1 for x in pending if x in self.transparents)
                need = int(name[1:]) - interned - marked

                def recent(x: str) -> bool:
                    target = self.by_name[x]["type"]["value"].value["type"]
                    named = index[x] - 1
                    while named >= 0 and types_[named]["name"].startswith("@"):
                        named -= 1
                    return index.get(target, -1) > named
                for x in [p for p in pending if not recent(p)] + [p for p in pending if recent(p)]:
                    if need <= 0:
                        break
                    if x in self.transparents:
                        continue
                    self.transparents.add(x)
                    need -= 1
                interned = int(name[1:]) + 1
                pending = []
            elif t.type != "Type":
                interned += 1
            if t.type == "Type":
                if name.startswith("@"):
                    continue
                target = t.value["type"]
                if re.match(r"^nested<.*,deferred>$", name):
                    self.deferred.add(target)
                else:
                    if name in referenced or index.get(target, -1) > index[name]:
                        self.transparents.add(name)
                    pending.append(name)
                continue
            if not name.startswith("@"):
                continue
            two_tuple = t.type == "Tuple" and len(t.value["members"]) == 2
            assoc = t.type == "Custom" and t.value["id"] in ("map", "set")
            if two_tuple or assoc:
                groups.setdefault(spelling(m["type"]), []).append(name)
        for names in groups.values():
            # The entry of a map is the tuple; otherwise the first is.
            plain = next((n for n in names if n in map_entries), names[0])
            self.alternate.update(n for n in names if n != plain)

    # ── Codecs ───────────────────────────────────────────────────────────────

    def type(self, id: str, **options: Any) -> psio.SchemaType:
        return psio.codec(self.hint(id), **options)

    # ── Declarations ─────────────────────────────────────────────────────────

    def hint(self, name: str) -> Any:
        """The Python type hint of a named type."""
        known = self.hints.get(name)
        if known is not None:
            return known
        if name not in self.by_name:
            raise KeyError(f'fixture has no type "{name}"')
        # A record reached again while it is being declared: a recursive type.
        if name in self.building:
            return self.class_names[name]
        self.building.add(name)
        try:
            out = self.of(self.by_name[name]["type"], name)
            self.hints[name] = out
            return out
        finally:
            self.building.discard(name)

    def resolved(self, t: Any) -> Any:
        for _ in range(64):
            if t["value"].type != "Type":
                break
            t = self.by_name[t["value"].value["type"]]["type"]
        return t

    def is_u8(self, t: Any) -> bool:
        v = self.resolved(t)["value"]
        return v.type == "Int" and v.value["bits"] == 8 and not v.value["isSigned"]

    def class_name(self, slot: str) -> str:
        name = self.class_names.get(slot)
        if name is None:
            name = slot if slot.isidentifier() and not keyword.iskeyword(slot) else f"T{len(self.class_names)}"
            self.class_names[slot] = name
        return name

    def publish(self, slot: str, cls: Any) -> Any:
        cls.__module__ = self.module.__name__
        setattr(self.module, self.class_name(slot), cls)
        return cls

    def optional(self, inner: Any) -> Any:
        # Optional[Optional[T]] is Optional[T] to Python: the outer level is explicit.
        import typing
        if typing.get_origin(inner) in (Union, getattr(types, "UnionType", Union)) \
                and type(None) in typing.get_args(inner):
            return Optional[psio.Some[inner]]
        return Optional[inner]

    def of(self, t: Any, slot: Optional[str] = None) -> Any:
        v = t["value"]
        k, b = v.type, v.value
        if k == "Type":
            # A transparent type: a named alias of its member's type.
            if slot is not None and slot in self.transparents:
                return NewType(slot, self.hint(b["type"]))
            return self.hint(b["type"])
        if k == "Nested":
            if slot is not None and slot in self.deferred:
                return Annotated[bytes, psio.nested(self.of(b["type"]), deferred=True)]
            return Annotated[self.of(b["type"]), psio.nested()]
        if k == "Int":
            out = INTS.get(("i" if b["isSigned"] else "u") + str(b["bits"]))
            if out is None:
                raise Unsupported(f"Int {b['bits']}")
            return out
        if k == "Float":
            if (b["exp"], b["mantissa"]) == (8, 24):
                return psio.f32
            if (b["exp"], b["mantissa"]) == (11, 53):
                return psio.f64
            raise Unsupported("a float other than the C++ f32 / f64 spelling")
        if k == "Option":
            return self.optional(self.of(b["type"]))
        if k == "List":
            return List[self.of(b["type"])]  # type: ignore[misc]
        if k == "BoundedList":
            return Annotated[List[self.of(b["type"])], psio.bounded(b["maxCount"])]  # type: ignore[misc]
        if k == "Array":
            return Annotated[List[self.of(b["type"])], psio.array(b["len"])]  # type: ignore[misc]
        if k == "Tuple":
            items = tuple(self.of(m) for m in b["members"])
            out = Tuple[items] if items else Tuple[()]  # type: ignore[misc]
            if slot is not None and slot in self.alternate:
                return Annotated[out, psio.identity("pair")]
            return out
        if k == "Variant":
            if b["attributes"]:
                raise Unsupported("variant attributes")
            return self.union([(m, self.of(m["type"])) for m in b["members"]])
        if k == "Enum":
            # An enum sits directly on its integer (through named aliases).
            under_ir = self.resolved(b["type"])
            under = self.of(under_ir)
            signed = under_ir["value"].value["isSigned"]
            members = {m["name"]: (m["value"] - (1 << 64) if signed and m["value"] >> 63 else m["value"])
                       for m in b["members"]}
            cls = enum.Enum(f"E{next(_counter)}", members)  # type: ignore[misc]
            # (C++ enums hold any value of their underlying type: `other`)
            return psio.enum(under, other=True)(cls)
        if k == "Object":
            # An interned Object is a method's argument tuple.
            if slot is not None and slot.startswith("@"):
                return self.argument_tuple(b["members"], b["attributes"])
            if slot is None:
                raise Unsupported("an unnamed record")
            return self.record(slot, b["members"], b["attributes"])
        if k == "Struct":
            raise Unsupported("a Struct node (C++ and the typed layer spell a fixed record as Object + "
                              "definitionWillNotChange)")
        if k == "Custom":
            return self.custom(b["type"], b["id"], slot)
        raise Unsupported(f"{k} node")

    def union(self, alts: List[Tuple[Any, Any]]) -> Any:
        members = []
        for m, hint in alts:
            names = [a for a in m["attributes"] if a["name"] == "ijsonName"]
            if len(names) != len(m["attributes"]):
                raise Unsupported("variant member attributes")
            members.append(Annotated[hint, psio.alt(m["name"], ijson_name=names[0]["value"] if names else None)])
        if len(members) < 2:
            raise Unsupported("a variant of one alternative (a Python union of one type is that type)")
        return Union[tuple(members)]  # type: ignore[return-value]

    def adapt(self, id: str, inner_ir: Any, typed: Any) -> Optional[Handler]:
        """The fixture's handler of a custom id (written for the dynamic values) on the typed value.

        ``typed()`` gives the codec of the wire type as Python declares it;
        the conversion goes through the bytes, with no handler consulted.
        """
        h = self.handlers.get(id)
        if h is None:
            return None
        dyn = self.plain.type(inner_ir)

        def to_dyn(value: Any) -> Any:
            return dyn.from_pssz(typed().to_pssz(value))

        def to_typed(value: Any) -> Any:
            return typed().from_pssz(dyn.to_pssz(value))
        return Handler(
            to_text=(lambda v: h.to_text(to_dyn(v))) if h.to_text is not None else None,
            from_text=(lambda s: to_typed(h.from_text(s))) if h.from_text is not None else None,
            check=(lambda v: h.check(to_dyn(v))) if h.check is not None else None)

    def custom(self, inner: Any, id: str, slot: Optional[str]) -> Any:
        # An adapter may sit on its container through a named alias.
        w = self.resolved(inner)["value"]
        if id == "bool":
            return bool
        if id == "char":
            return psio.char
        if id == "box":
            return Annotated[self.of(inner), psio.boxed]
        if id in ("map", "set"):
            if w.type not in ("List", "BoundedList"):
                raise Unsupported(f"a {id} over {w.type}")
            return self.assoc(id, w.value["type"], slot is not None and slot in self.alternate,
                              w.value["maxCount"] if w.type == "BoundedList" else None)
        if id == "bitvector":
            if w.type != "Array":
                raise Unsupported("bitvector shape")
            return Annotated[psio.Bits, psio.bitvector(w.value["len"])]
        if id == "bitlist":
            if w.type != "BoundedList":
                raise Unsupported("bitlist shape")
            return Annotated[psio.Bits, psio.bitlist(w.value["maxCount"])]
        if id in ("utf8", "string", "hex"):
            if w.type not in ("List", "BoundedList", "Array") or not self.is_u8(w.value["type"]):
                raise Unsupported("a byte adapter over something other than u8")
            base = {"utf8": str, "string": psio.RawString, "hex": bytes}[id]
            if w.type == "List":
                return base
            if w.type == "BoundedList":
                return Annotated[base, psio.bounded(w.value["maxCount"])]
            if id == "hex":
                return Annotated[bytes, psio.array(w.value["len"])]
            raise Unsupported(f"a {id} adapter over an Array")
        time = parse_schema_id(id)
        if time is not None:
            make = psio.time_point if time.point else psio.monotonic if time.monotonic else psio.duration
            # (int ticks: every tick count is a value; datetime covers years 1..9999 at microseconds)
            return Annotated[int, make((time.num, time.den), ticks=self.of(inner))]
        # A custom type over a transparent type: the Custom wraps the member's
        # type, under the transparent type's name.
        if inner["value"].type == "Type":
            if slot is None or slot.startswith("@"):
                raise Unsupported("a custom type over a reference, not named")
            member = self.of(inner)
            cls = dataclasses.make_dataclass(self.class_name(slot), [("value", member)], frozen=True)
            self.publish(slot, cls)
            handler = self.adapt(id, inner, lambda: psio.codec(member))
            options = psio.record(cls, name=slot, transparent=True, custom=id).__psio_options__
            if handler is not None:
                if handler.to_text is not None:
                    options.to_text = lambda self_: handler.to_text(self_.value)
                if handler.from_text is not None:
                    options.from_text = lambda text: cls(handler.from_text(text))
                if handler.check is not None:
                    options.invariant = lambda self_: handler.check(self_.value)
            return cls
        iv = inner["value"]
        if iv.type == "Object" and slot is not None and not slot.startswith("@"):
            # A record with a text form / an invariant: the class says so.
            cls = self.record(slot, iv.value["members"], iv.value["attributes"], custom=id)
            handler = self.adapt(id, inner, lambda: psio.codec(_class_shape(cls).inner))
            options = cls.__psio_options__
            if handler is not None:
                options.to_text, options.from_text, options.invariant = (
                    handler.to_text, handler.from_text, handler.check)
            return cls
        if iv.type == "Enum":
            cls = self.of(inner)
            handler = self.adapt(id, inner, lambda: psio.codec(_enum_shape(cls).inner))
            under, other, name, _, _ = cls.__psio_enum__
            cls.__psio_enum__ = (under, other, name, handler.check if handler is not None else None, id)
            return cls
        # A custom type over any other type: by annotation.
        base = self.of(inner, slot)
        handler = self.adapt(id, inner, lambda: psio.codec(base))
        if handler is None:
            return Annotated[base, psio.custom(id)]
        return Annotated[base, psio.custom(id, to_text=handler.to_text, from_text=handler.from_text,
                                           check=handler.check)]

    def assoc(self, id: str, element: Any, unordered: bool, limit: Optional[int] = None) -> Any:
        if id == "set":
            out: Any = set[self.of(element)]  # type: ignore[misc]
        else:
            entry = self.resolved(element)["value"]
            if entry.type != "Tuple" or len(entry.value["members"]) != 2:
                raise Unsupported("a map over something other than 2-tuples")
            out = Dict[self.of(entry.value["members"][0]), self.of(entry.value["members"][1])]  # type: ignore[misc]
        specs: List[Any] = []
        if limit is not None:
            specs.append(psio.bounded(limit))
        if unordered:
            specs.append(psio.identity("unordered"))
        return Annotated[tuple([out] + specs)] if specs else out  # type: ignore[misc]

    def record(self, slot: str, members: List[Any], attributes: List[Any], custom: Optional[str] = None) -> Any:
        options: Dict[str, Any] = {"name": slot}
        for a in attributes:
            if a["name"] == "definitionWillNotChange":
                options["definition_will_not_change"] = True
            elif a["name"] == "maxDynamicData":
                options["max_dynamic_data"] = int(a["value"])
            elif a["name"] == "maxFields":
                options["max_fields"] = int(a["value"])
            else:
                raise Unsupported(f"record attribute {a['name']}")
        if custom is not None:
            options["custom"] = custom
        name = self.class_name(slot)
        cls = dataclasses.make_dataclass(name, self.members(members), frozen=True)
        return psio.record(self.publish(slot, cls), **options)

    def argument_tuple(self, members: List[Any], attributes: List[Any]) -> Any:
        cap = None
        for a in attributes:
            if a["name"] != "maxDynamicData":
                raise Unsupported(f"argument tuple attribute {a['name']}")
            cap = int(a["value"])
        if any(m["attributes"] for m in members):
            raise Unsupported("argument tuple member attributes")
        return psio.arguments({attr: hint for attr, hint in self.members(members)}, max_dynamic_data=cap)

    def members(self, members: List[Any]) -> List[Tuple[str, Any]]:
        fields = []
        for i, m in enumerate(members):
            specs: List[Any] = []
            # A member type that is not a reference is the type its bounds
            # describe (member annotations): declared as the plain type and the bounds.
            bounded = None if m["type"]["value"].type == "Type" else self.bounded(m["type"])
            if bounded is not None:
                hint, below = bounded
                specs.extend(below or [])
            else:
                hint = self.of(m["type"])
            for a in m["attributes"]:
                if a["name"] == "jsonInt":
                    specs.append(psio.json_string if a["value"] == "string" else psio.json_number)
                elif a["name"] == "ijsonInt":
                    specs.append(psio.ijson_string if a["value"] == "string" else psio.ijson_number)
                elif a["name"] == "lengthBound":
                    bound = {item.split("=")[0]: int(item.split("=")[1]) for item in a["value"].split(",")}
                    # A bounded UTF-8 string states its own bound: nothing to declare.
                    direct = self.resolved(m["type"])["value"]
                    inherent = (direct.type == "Custom" and direct.value["id"] == "utf8"
                                and direct.value["type"]["value"].type == "BoundedList"
                                and a["value"] == f"max={direct.value['type']['value'].value['maxCount']}")
                    if not inherent:
                        specs.append(psio.length(**bound))
                else:
                    raise Unsupported(f"member attribute {a['name']}")
            name = m["name"]
            if name.isidentifier() and not keyword.iskeyword(name) and not name.startswith("_"):
                attr = name
            elif keyword.iskeyword(name):
                attr = name + "_"  # (the trailing underscore is dropped on the wire)
            else:
                attr = f"f{i}"
                specs.append(psio.name(name))
            fields.append((attr, Annotated[tuple([hint] + specs)] if specs else hint))  # type: ignore[misc]
        return fields

    def bounded(self, t: Any) -> Optional[Tuple[Any, Optional[List[Any]]]]:
        """The plain type and the annotations of a type written in place (the inverse of ``annotated_type``)."""
        v = t["value"]
        k, b = v.type, v.value
        if k == "Type":
            return self.of(t), None
        if k == "Option":
            child = self.bounded(b["type"])
            return None if child is None else (self.optional(child[0]), child[1])
        if k in ("List", "BoundedList", "Array"):
            child = self.bounded(b["type"])
            if child is None:
                return None
            specs: List[Any] = []
            if k == "BoundedList":
                specs.append(psio.max(b["maxCount"]))
            if child[1]:
                specs.append(psio.element(*child[1]))
            hint: Any = List[child[0]]  # type: ignore[valid-type]
            if k == "Array":
                hint = Annotated[hint, psio.array(b["len"])]
            return hint, specs or None
        if k == "Tuple":
            items = [self.bounded(m) for m in b["members"]]
            if any(x is None for x in items):
                return None
            specs = [psio.at(i, *x[1]) for i, x in enumerate(items) if x[1]]  # type: ignore[index]
            return Tuple[tuple(x[0] for x in items)], specs or None  # type: ignore[index, misc]
        if k == "Variant":
            if b["attributes"]:
                return None
            alts = []
            specs = []
            for m in b["members"]:
                x = self.bounded(m["type"])
                if x is None:
                    return None
                alts.append((m, x[0]))
                if x[1]:
                    specs.append(psio.at(m["name"], *x[1]))
            return self.union(alts), specs or None
        if k == "Custom":
            w = b["type"]["value"]
            if w.type not in ("List", "BoundedList"):
                return None
            limit = [psio.max(w.value["maxCount"])] if w.type == "BoundedList" else []
            id = b["id"]
            if id in ("utf8", "string", "hex"):
                if w.value["type"]["value"].type != "Type" or not self.is_u8(w.value["type"]):
                    return None
                if id == "hex":
                    return (bytes, limit) if limit else None
                if id == "string":
                    return (psio.RawString, limit) if limit else None
                # (C++ `utf8` on a std::string: the raw string, said to be text)
                return psio.RawString, [psio.utf8] + limit
            if id == "set":
                element = self.bounded(w.value["type"])
                if element is None:
                    return None
                return set[element[0]], (limit + ([psio.element(*element[1])] if element[1] else [])) or None  # type: ignore[misc, valid-type]
            if id == "map":
                entry = w.value["type"]["value"]
                if entry.type == "Type":
                    if w.type != "BoundedList":
                        return None
                    return self.assoc("map", w.value["type"], False), limit
                if entry.type != "Tuple" or len(entry.value["members"]) != 2:
                    return None
                key = self.bounded(entry.value["members"][0])
                value = self.bounded(entry.value["members"][1])
                if key is None or value is None:
                    return None
                specs = limit + ([psio.keys(*key[1])] if key[1] else []) + ([psio.values(*value[1])] if value[1] else [])
                return Dict[key[0], value[0]], specs or None  # type: ignore[misc]
        return None

    # ── The generated schema ─────────────────────────────────────────────────

    def insertion_order(self) -> List[str]:
        """The names C++ passed to ``SchemaBuilder::insert<T>(name)``, in order.

        The next one is found from the first entry of the IR that the names so
        far did not bring in: that entry itself when it is a named type, and
        the name given to it (the alias entry that follows its children) when
        it is an interned ``@N``.
        """
        names: List[str] = []
        present: set = set()
        types_ = self.ir["types"]
        while True:
            missing = next((m for m in types_ if m["name"] not in present), None)
            if missing is None:
                return names
            root = missing["name"]
            if root.startswith("@"):
                alias = next((m for m in types_ if not m["name"].startswith("@") and m["name"] not in present
                              and m["type"]["value"].type == "Type" and m["type"]["value"].value["type"] == root), None)
                if alias is None:
                    raise AssertionError(f"no inserted name brings in {root}")
                root = alias["name"]
            names.append(root)
            present = {m["name"] for m in self.generate(names).ir["types"]}
            if missing["name"] not in present:
                raise AssertionError(f"inserting {root} does not bring in {missing['name']}")

    def generate(self, names: List[str]) -> psio.Schema:
        """``psio.schema_of`` of the named declarations, in the given order."""
        return psio.schema_of(names={name: self.hint(name) for name in names})


class _Strict:
    def __init__(self, plain: TypedFixture) -> None:
        self.plain = plain

    def type(self, id: str, **options: Any) -> psio.SchemaType:
        return self.plain.type(id, require_handlers=True, **options)


def sources(schema: psio.Schema) -> Dict[str, Any]:
    """The typed path's three registries: no handler, the fixture's handlers, a strict one lacking them."""
    plain = TypedFixture(schema.ir)
    return {"schema": schema, "plain": plain, "handled": TypedFixture(schema.ir, FIXTURE_HANDLERS),
            "strict": _Strict(plain)}
