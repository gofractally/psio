"""Offset widths (docs/pssz-spec.md 3.1, 3.2, 3.6): ``fit``, own widths, narrow list tables, position caps.

The fixtures are replayed in full by ``test_pssz_conformance.py`` on
generated declarations.  Here the new types are declared by hand, the way a
program writes them, and held to the same manifest, Schema IR and bytes;
views are walked one element at a time beneath caps; and the rules that have
no fixture row (the tuple limit, a ``lengthBound`` attribute on a plain list,
what the trusted decode skips) are pinned.
"""

from __future__ import annotations

import collections
import unittest
from dataclasses import dataclass
from typing import Annotated, Any, Dict, List, Optional, Set, Tuple

import support
from support import WIDTHS, fixture

import psio
from psio import Bits, RawString, ir, u8, u32, u64
from psio._core import fit, width_class

Str16 = Annotated[RawString, psio.max(16)]


@dataclass
class ListW1:
    items: Annotated[List[RawString], psio.max(15), psio.element(psio.max(16))]


@dataclass
class ListW2:
    items: Annotated[List[RawString], psio.max(15), psio.element(psio.max(17))]


@dataclass
class ListW2Top:
    items: Annotated[List[RawString], psio.max(255), psio.element(psio.max(255))]


@dataclass
class ListW4:
    items: Annotated[List[RawString], psio.max(256), psio.element(psio.max(255))]


@dataclass
class ListFree:
    items: Annotated[List[RawString], psio.max(8)]


@dataclass
class ListNested:
    rows: Annotated[List[List[RawString]], psio.max(20), psio.element(psio.max(15), psio.element(psio.max(16)))]


@dataclass
class MapW1:
    m: Annotated[Dict[RawString, u32], psio.max(4), psio.keys(psio.max(8))]


@dataclass
class MapW2:
    m: Annotated[Dict[u32, RawString], psio.max(100), psio.values(psio.max(16))]


@dataclass
class SetW1:
    s: Annotated[Set[RawString], psio.max(15), psio.element(psio.max(16))]


@psio.record(max_dynamic_data=255)
@dataclass
class CapTransfer:
    token: u32
    from_: u64
    amount: u64
    memo: str
    data: Optional[bytes] = None


@psio.record(max_dynamic_data=255)
@dataclass
class CapList:
    items: List[RawString]


@psio.record(max_dynamic_data=256)
@dataclass
class CapList2:
    items: List[RawString]


@psio.record(max_dynamic_data=65536)
@dataclass
class CapList4:
    items: List[RawString]


@psio.record(max_dynamic_data=255)
@dataclass
class CapNested:
    rows: List[List[RawString]]


@psio.record(max_dynamic_data=255)
@dataclass
class CapMap:
    m: Dict[RawString, List[u32]]


@psio.record(max_dynamic_data=255)
@dataclass
class CapTuple:
    t: Tuple[RawString, List[RawString]]


@psio.record(max_dynamic_data=60000)
@dataclass
class CapTight:
    items: Annotated[List[RawString], psio.max(8), psio.element(psio.max(16))]


@psio.record(max_dynamic_data=255)
@dataclass
class CapClamp:
    items: Annotated[List[RawString], psio.max(1000), psio.element(psio.max(1000))]


@dataclass
class R0:
    a: u32
    s: RawString


@psio.record(max_dynamic_data=255)
@dataclass
class CapChild:
    child: R0
    kids: List[R0]


@dataclass
class CapOuter:
    inner: CapList
    items: List[RawString]


@dataclass
class BitRec:
    id: u8
    flags: Annotated[Bits, psio.bitvector(12)]
    seen: Annotated[Bits, psio.bitlist(12)]


@dataclass
class BitNarrow:
    seen: Annotated[Bits, psio.bitlist(2016)]


@dataclass
class BitWide:
    seen: Annotated[Bits, psio.bitlist(2024)]


@dataclass
class S253:
    s: Annotated[RawString, psio.max(253)]


@dataclass
class S254:
    s: Annotated[RawString, psio.max(254)]


@dataclass
class S65531:
    s: Annotated[RawString, psio.max(65531)]


@dataclass
class S65532:
    s: Annotated[RawString, psio.max(65532)]


@dataclass
class WidthThreshold254:
    value: Annotated[bytes, psio.array(254)]


@dataclass
class WidthThreshold255:
    value: Annotated[bytes, psio.array(255)]


def _strings(element: int, count: int) -> Any:
    return Annotated[List[Annotated[str, psio.max(element)]], psio.bounded(count)]


#: The 34 types the list-width change added to pssz-vectors.json, declared by hand.
DECLARED: Dict[str, Any] = {
    "WidthThreshold254": WidthThreshold254, "WidthThreshold255": WidthThreshold255,
    "S253": S253, "S254": S254, "S65531": S65531, "S65532": S65532,
    "list<string<=16><=15": _strings(16, 15), "list<string<=17><=15": _strings(17, 15),
    "list<string<=255><=255": _strings(255, 255), "list<string<=255><=256": _strings(255, 256),
    "list<string><=8": Annotated[List[str], psio.bounded(8)],
    "ListW1": ListW1, "ListW2": ListW2, "ListW2Top": ListW2Top, "ListW4": ListW4, "ListFree": ListFree,
    "ListNested": ListNested, "MapW1": MapW1, "MapW2": MapW2, "SetW1": SetW1,
    "CapTransfer": CapTransfer, "CapList": CapList, "CapList2": CapList2, "CapList4": CapList4,
    "CapNested": CapNested, "CapMap": CapMap, "CapTuple": CapTuple, "CapTight": CapTight, "CapClamp": CapClamp,
    "CapChild": CapChild, "CapOuter": CapOuter, "BitRec": BitRec, "BitNarrow": BitNarrow, "BitWide": BitWide,
}


def expanded(schema_ir: Any, t: Any, active: Tuple[str, ...] = ()) -> Any:
    """An IR type with every reference replaced by what it names: two schemas that intern differently compare equal."""
    by_name = {m["name"]: m["type"] for m in schema_ir["types"]}

    def walk(x: Any, active: Tuple[str, ...]) -> Any:
        v = x["value"]
        k, b = v.type, v.value
        if k == "Type":
            name = b["type"]
            if name in active:
                return ("recursive", name)
            return walk(by_name[name], active + (name,))
        if k == "Tuple":
            return (k, [walk(m, active) for m in b["members"]])
        if k in ("Struct", "Object", "Variant"):
            return (k, [(m["name"], walk(m["type"], active), m["attributes"]) for m in b["members"]], b["attributes"])
        if k in ("List", "Option", "Nested"):
            return (k, walk(b["type"], active))
        if k == "BoundedList":
            return (k, walk(b["type"], active), b["maxCount"])
        if k == "Array":
            return (k, walk(b["type"], active), b["len"])
        if k == "Custom":
            return (k, walk(b["type"], active), b["id"])
        if k == "Enum":
            return (k, walk(b["type"], active), b["members"])
        return (k, b)
    return walk(t, active)


def walked(view: Any) -> Any:
    """The value a view holds, read through the view one member / element at a time."""
    if isinstance(view, psio.RecordView):
        return {name: walked(view[name]) for name in view.view_fields()}
    if isinstance(view, psio.TupleView):
        return tuple(walked(view[i]) for i in range(len(view)))
    if isinstance(view, psio.ListView):
        return [walked(view[i]) for i in range(len(view))]
    if isinstance(view, psio.VariantView):
        return psio.Variant(view.type, walked(view.value))
    if isinstance(view, psio.Some):
        return psio.Some(walked(view.value))
    if isinstance(view, memoryview):
        return bytes(view)
    return view


class Widths(unittest.TestCase):
    maxDiff = None

    def test_fit(self) -> None:
        """The examples of spec 3.2."""
        self.assertEqual(fit(8, 8 * 16), 136)
        self.assertEqual(fit(15, 15 * 16), 255)
        self.assertEqual(fit(15, 15 * 17), 285)
        self.assertEqual(fit(255, 255 * 255), 65535)
        self.assertEqual(fit(256, 256 * 255), 66304)
        self.assertEqual(fit(1 + 1, 4 + 136), 142)
        self.assertIsNone(fit(3, None))
        self.assertIsNone(fit(1, (1 << 64) - 1))
        self.assertEqual(fit(0, 0), 0)
        # The width class of the result is the W of the branch that returned it.
        for k in (0, 1, 2, 15, 127, 255, 256, 30000, 70000):
            for r in (0, 1, 100, 128, 254, 255, 256, 30000, 65000, 65535, 65536, 1 << 40):
                n = fit(k, r)
                w = width_class(n)
                self.assertEqual(n, k * w + r, (k, r))
                for narrower in (1, 2):
                    if narrower < w:
                        self.assertGreater(k * narrower + r, (1 << (8 * narrower)) - 1, (k, r))

    def test_hand_declared_types(self) -> None:
        """The 34 new fixture types as a program declares them: manifest, Schema IR, and every vector."""
        fx = fixture("pssz-vectors.json")
        schema = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        by_name = {m["name"]: m["type"] for m in schema.ir["types"]}
        manifest = {t["id"]: t for t in fx["types"]}
        self.assertEqual(len(DECLARED), 34)
        counts: collections.Counter = collections.Counter()
        for id, hint in DECLARED.items():
            row = manifest[id]
            t = psio.codec(hint)
            self.assertEqual(t.width, row["auto_width"], f"width of {id}")
            self.assertEqual(t.max_encoded_size, row["max_encoded_size"], f"max_encoded_size of {id}")
            self.assertEqual(t.max_dynamic_data, row["max_dynamic_data"], f"cap of {id}")
            own = psio.schema_of(names={id: hint}) if not isinstance(hint, type) else psio.schema_of(hint)
            mine = {m["name"]: m["type"] for m in own.ir["types"]}
            self.assertEqual(expanded(own.ir, mine[id]), expanded(schema.ir, by_name[id]), f"Schema IR of {id}")
        for row in fx["vectors"]:
            hint = DECLARED.get(row["type"])
            if hint is None:
                continue
            name = row["name"]
            t = psio.codec(hint, width=WIDTHS[row["format"]])
            data = bytes.fromhex(row["hex"])
            st = t.validate_pssz(data)
            counts[row["class"]] += 1
            if row["expect"] == "reject":
                self.assertFalse(st.ok, f"{name}: accepted")
                self.assertTrue(support.same_reason(row["reason"], st.reason), f"{name}: {st.reason}")
                counts["reject"] += 1
                continue
            self.assertTrue(st.ok, f"{name}: {st.reason}")
            self.assertEqual(row["expect"], "canonical", name)
            value = t.from_pssz(data)
            self.assertEqual(t.to_pssz(value).hex(), row["hex"], name)
            self.assertEqual(t.from_pssz(psio.trusted(data)), value, f"{name}: trusted")
            self.assertEqual(walked_typed(t.pssz_view(data)), walked_typed(value), f"{name}: view")
            counts["canonical"] += 1
        print(f"pssz-vectors.json, 34 hand-declared types: {dict(sorted(counts.items()))}")
        # (the 89th list-width row is of `list<string>`, a type the fixture already had)
        self.assertEqual(counts["list-width"], 88)
        self.assertEqual(counts["cap-implicit"], 40)
        self.assertEqual((counts["canonical"], counts["reject"]), (150, 27))

    def test_views_walk_every_frame(self) -> None:
        """Every canonical frame, read through a view one element at a time, is the value (schema-driven path)."""
        fx = fixture("pssz-vectors.json")
        schema = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        types: Dict[Any, Any] = {}
        walks = 0
        for row in fx["vectors"]:
            if row["expect"] != "canonical" or row["class"] in ("invariant", "nested-deferred"):
                continue
            key = (row["type"], row["format"])
            t = types.get(key)
            if t is None:
                t = types[key] = schema.type(row["type"], width=WIDTHS[row["format"]])
            data = bytes.fromhex(row["hex"])
            self.assertEqual(t.to_pssz(walked(t.pssz_view(data))).hex(), row["hex"], row["name"])
            walks += 1
        print(f"pssz-vectors.json views walked element by element: {walks}")
        self.assertEqual(walks, 4724)

    def test_position_cap(self) -> None:
        """Spec 3.6: the cap of the nearest enclosing record selects the widths beneath it, and nothing else."""
        two = [b"ab", b"c"]
        self.assertEqual(psio.to_pssz(CapList(two)).hex(), "01010204616263")
        self.assertEqual(psio.to_pssz(CapList2(two)).hex(), "0200020004000600616263")
        self.assertEqual(psio.to_pssz(CapList4(two)).hex(), "0400000004000000080000000a000000616263")
        # The cap changes no bound and no schema: the member is still a plain list.
        self.assertIsNone(psio.codec(CapList).max_encoded_size)
        own = {m["name"]: m["type"] for m in psio.schema_of(CapList).ir["types"]}
        member = own["CapList"]["value"].value["members"][0]
        self.assertEqual(member["attributes"], [])
        # The list alone, at the root, is 4 wide; a record keeps its own width beneath a cap.
        self.assertEqual(psio.to_pssz(two, List[RawString]).hex(), "080000000a000000616263")
        self.assertEqual(psio.codec(R0).width, 4)
        child = psio.to_pssz(CapChild(R0(7, b"hi"), [R0(7, b"hi")]))
        self.assertEqual(child.count(psio.to_pssz(R0(7, b"hi"))), 2)
        # A cap does not reach out of its record, nor into a nested value.
        outer = psio.to_pssz(CapOuter(CapList(two), two))
        self.assertTrue(outer.endswith(bytes.fromhex("080000000a000000616263")))

        @psio.record(max_dynamic_data=255)
        @dataclass
        class Envelope:
            inner: Annotated[List[RawString], psio.nested()]
            direct: List[RawString]

        data = psio.to_pssz(Envelope(two, two))
        self.assertEqual(data.hex(), "02020d" + "080000000a000000616263" + "0204616263")
        self.assertEqual(psio.from_pssz(Envelope, data), Envelope(two, two))
        view = psio.pssz_view(Envelope, data)
        self.assertEqual([bytes(x) for x in view.inner], two)
        self.assertEqual([bytes(x) for x in view.direct], two)
        # Through an optional, a variant, a tuple, a map and a list of lists.

        @psio.record(max_dynamic_data=255)
        @dataclass
        class Deep:
            maybe: Optional[List[RawString]]
            either: Annotated[u32, psio.alt("n")] | Annotated[List[RawString], psio.alt("list")]
            pair: Tuple[u8, List[RawString]]
            table: Dict[u8, List[RawString]]

        deep = Deep(two, two, (1, two), {1: two})
        data = psio.to_pssz(deep)
        self.assertEqual(data.count(bytes.fromhex("0204616263")), 4)
        self.assertNotIn(bytes.fromhex("080000000a000000"), data)
        self.assertEqual(psio.from_pssz(Deep, data), deep)
        self.assertEqual(psio.pssz_view(Deep, data).to_value(), deep)
        self.assertEqual([bytes(x) for x in psio.pssz_view(Deep, data).table[0][1]], two)
        # The same type beneath two caps and beneath none, in one program.
        self.assertEqual(psio.from_pssz(CapList2, psio.to_pssz(CapList2(two))), CapList2(two))
        self.assertEqual(psio.from_pssz(List[RawString], psio.to_pssz(two, List[RawString])), two)
        # An explicit profile ignores it: every table is W wide.
        self.assertEqual(psio.codec(CapList, width=2).to_pssz(CapList([])).hex(), "02000200")

    def test_method_arguments_cap(self) -> None:
        """A method's argument list with a cap is such a record: the positions beneath it use the cap."""
        @psio.interface(name="notes")
        class Notes:
            @psio.method(max_dynamic_data=255)
            def put(self, tags: List[RawString], rows: List[List[RawString]]) -> None: ...

            @psio.method(max_dynamic_data=256)
            def put2(self, tags: List[RawString]) -> None: ...

            def put4(self, tags: List[RawString]) -> None: ...

        notes = psio.interface_of(Notes)
        two = [b"ab", b"c"]
        self.assertEqual(notes["put"].to_pssz(two, [two]).hex(), "020207" + "0204616263" + "01" + "0204616263")
        self.assertEqual(notes["put"].from_pssz(notes["put"].to_pssz(two, [two])), (two, [two]))
        self.assertEqual(notes["put2"].to_pssz(two).hex(), "02000200" + "04000600616263")
        self.assertEqual(notes["put4"].to_pssz(two).hex(), "0400000004000000" + "080000000a000000616263")
        # The schema alone gives the same bytes.
        schema = psio.Schema.from_pssz(notes.schema().to_pssz())
        self.assertEqual(schema.method("notes", "put").args.to_pssz({"tags": two, "rows": [two]}).hex(),
                         "0202070204616263010204616263")

    def test_member_length_bound_on_a_plain_list(self) -> None:
        """A schema may bound a plain List by a member attribute: the list is then that bounded list (spec 3.5)."""
        string16 = ir.custom_of(ir.bounded_list_of(ir.U8, 16), "string")
        bound = ir.attribute("lengthBound", "max=15")

        def one(member: Any) -> psio.SchemaType:
            return psio.Schema({"R": {"Object": {"members": [member], "attributes": []}}}).type("R")

        fx = fixture("pssz-vectors.json")
        by_type = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"])).type("ListW1")
        value = {"items": [b"ab", b"c"]}
        expected = by_type.to_pssz(value)
        # The record: bound fit(2, 255) = 259, 2-byte header and slot.  The list: 255, a 1-byte table.
        self.assertEqual(expected.hex(), "0200" "0200" "0204616263")
        by_attribute = one(ir.member("items", ir.list_of(string16), bound))
        self.assertEqual((by_attribute.width, by_attribute.max_encoded_size), (2, 259))
        self.assertEqual(by_attribute.to_pssz(value), expected)
        self.assertEqual(by_attribute.from_pssz(expected), value)
        self.assertEqual(walked(by_attribute.pssz_view(expected)), value)
        sixteen = bytes.fromhex("02000200") + bytes(range(16, 32)) + b"x" * 16
        self.assertIn(by_attribute.validate_pssz(sixteen).reason, support.BOUND_REASONS)
        self.assertIn(by_type.validate_pssz(sixteen).reason, support.BOUND_REASONS)
        self.assertFalse(by_attribute.validate_pssz(sixteen))
        self.assertTrue(by_attribute.validate_pssz(bytes.fromhex("02000200") + bytes(range(15, 30)) + b"x" * 15))
        # The bound reaches the list through an optional ...
        optional = one(ir.member("items", ir.option_of(ir.list_of(string16)), bound))
        self.assertEqual(optional.to_pssz(value).hex(), "0200" "0200" "010204616263")
        self.assertEqual(optional.from_pssz(optional.to_pssz(value)), value)
        self.assertEqual(walked(optional.pssz_view(optional.to_pssz(value))), value)
        # ... a set or a map the same way, and nothing else: a list inside the member's list has no count.
        as_set = one(ir.member("items", ir.custom_of(ir.list_of(string16), "set"), bound))
        self.assertEqual(as_set.to_pssz({"items": [b"ab", b"c"]}).hex(), "0200" "0200" "0204616263")
        rows = one(ir.member("items", ir.list_of(ir.list_of(string16)), bound))
        self.assertEqual(rows.to_pssz({"items": [[b"ab", b"c"]]}).hex(),
                         "04000000" "04000000" "04000000" "080000000a000000616263")
        # Without the attribute the list is unbounded: 4 wide.
        free = one(ir.member("items", ir.list_of(string16)))
        self.assertEqual(free.to_pssz(value).hex(), "04000000" "04000000" "080000000a000000616263")

    def test_tuple_limit(self) -> None:
        """A tuple has at most 255 elements (spec 8.4): typed and schema-driven, before any value."""
        reason = "pssz IR: a tuple has more than 255 elements"
        ok = psio.Schema({"T": ir.tuple_of(*[ir.U8] * 255)}).type("T")
        self.assertEqual(len(ok.to_pssz(tuple(range(255)))), 2 + 255)
        self.assertEqual(ok.width, 2)
        big = psio.Schema({"T": ir.tuple_of(*[ir.U8] * 256), "L": ir.list_of("T"),
                           "R": {"Object": {"t": ir.option_of("T")}}, "U": ir.U8})
        for root in ("T", "L", "R"):
            for width in ("auto", 1, 2, 4):
                with self.assertRaises(psio.SchemaError) as raised:
                    big.type(root, width=width)
                self.assertEqual(raised.exception.reason, reason, root)
        self.assertEqual(big.type("U").to_pssz(7), b"\x07")
        # The schema itself is a value of the IR: it is its meaning that is refused.
        self.assertEqual(psio.Schema.from_pssz(big.to_pssz()).to_pssz(), big.to_pssz())
        self.assertEqual(psio.codec(Tuple[tuple([u8] * 255)]).width, 2)  # type: ignore[misc]
        with self.assertRaises(psio.SchemaError):
            psio.codec(Tuple[tuple([u8] * 256)])  # type: ignore[misc]
        with self.assertRaises(psio.SchemaError):
            psio.schema_of(Tuple[tuple([u8] * 256)], name="T")  # type: ignore[misc]
        # A method's argument list is a tuple.
        self.assertEqual(len(psio.arguments({f"p{i}": u8 for i in range(255)}).fields), 255)
        with self.assertRaises(psio.SchemaError):
            psio.arguments({f"p{i}": u8 for i in range(256)})
        params = [{"name": f"p{i}", "type": ir.normalize_type(ir.U8), "attributes": []} for i in range(256)]

        @psio.interface(name="i")
        class Two:
            def f(self, p: u8) -> None: ...

            def g(self, p: u8) -> None: ...

        wide = psio.Schema.from_pssz(psio.interface_of(Two).schema().to_pssz()).ir     # a copy
        wide["interfaces"][0]["funcs"][0]["params"] = params
        wide["interfaces"][0]["funcs"][1]["params"] = params[:255]
        funcs = psio.Schema.from_pssz(psio.Schema(wide).to_pssz())
        with self.assertRaises(psio.SchemaError) as raised:
            funcs.method("i", "f")
        self.assertEqual(raised.exception.reason, "pssz IR: a function has more than 255 parameters")
        with self.assertRaises(psio.SchemaError):
            funcs.validate()
        self.assertEqual(len(funcs.method("i", "g").args.to_pssz({f"p{i}": i for i in range(255)})), 2 + 255)
        # A variant keeps its own limit: 256 alternatives.
        self.assertEqual(psio.Schema({"V": {"Variant": {f"a{i}": ir.U8 for i in range(256)}}}).type("V").width, 1)

    def test_bit_types_are_bounded(self) -> None:
        self.assertEqual(psio.codec(Annotated[Bits, psio.bitvector(12)]).max_encoded_size, 2)
        self.assertEqual(psio.codec(Annotated[Bits, psio.bitlist(12)]).max_encoded_size, 3)
        self.assertEqual(psio.codec(Annotated[Bits, psio.bitlist(16)]).max_encoded_size, 3)
        # id 1 + flags 2 inline + header 1 + seen (slot 1 + 3) = 8.
        self.assertEqual((psio.codec(BitRec).width, psio.codec(BitRec).max_encoded_size), (1, 8))
        self.assertEqual((psio.codec(BitNarrow).width, psio.codec(BitNarrow).max_encoded_size), (1, 255))
        self.assertEqual((psio.codec(BitWide).width, psio.codec(BitWide).max_encoded_size), (2, 258))

    def test_utf8_is_always_enforced(self) -> None:
        """There is no mode in which a UTF-8 position holds other bytes: every reader, trusted included."""
        bad = psio.to_pssz(CapTransfer(1, 2, 3, "hi"))[:-1] + b"\xff"
        self.assertEqual(psio.validate_pssz(CapTransfer, bad).reason, "utf8_spec: invalid leading byte")
        for run in (lambda: psio.from_pssz(CapTransfer, bad), lambda: psio.from_pssz(CapTransfer, psio.trusted(bad)),
                    lambda: psio.pssz_view(CapTransfer, bad).memo, lambda: psio.pssz_to_json(CapTransfer, bad)):
            with self.assertRaises(psio.DecodeError):
                run()
        with self.assertRaises(psio.EncodeError):
            psio.to_pssz(CapTransfer(1, 2, 3, "\ud800"))

    def test_trusted_skips_what_trust_makes_unnecessary(self) -> None:
        @psio.record(max_dynamic_data=16)
        @dataclass
        class Small:
            names: Annotated[Dict[str, u8], psio.max(2), psio.keys(psio.max(3))]
            extra: Optional[u8] = None

        loose = psio.codec(dataclass(type("Small", (), {"__annotations__": {
            "names": Dict[str, u8], "extra": Optional[u8]}, "extra": None})), width=1)
        t = psio.codec(Small)
        # Over the cap, over two bounds, entries out of order, and an unpruned trailing optional.
        value = {"names": {"zzzz": 1, "b": 2, "a": 3}, "extra": None}
        frame = loose.to_pssz(loose.python_type(**value))
        for wrong in (frame, frame[:1] + frame[1:]):
            self.assertFalse(t.validate_pssz(wrong))
            self.assertEqual(t.from_pssz(psio.trusted(wrong)), Small(value["names"], None))
        # What cannot be parsed is still a decode error, never a read outside the input.
        fx = fixture("pssz-vectors.json")
        schema = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        frames = errors = 0
        for row in fx["vectors"]:
            if row["format"] != "auto" or row["class"] not in ("list-width", "cap-implicit", "malformed", "map-set"):
                continue
            t2 = schema.type(row["type"])
            data = bytes.fromhex(row["hex"])
            frames += 1
            for candidate in [data] + [data[:cut] for cut in range(len(data))] + [
                    data[:i] + bytes((data[i] ^ 0xFF,)) + data[i + 1:] for i in range(min(len(data), 24))]:
                try:
                    t2.from_pssz(psio.trusted(candidate))
                except psio.PsioError as e:
                    self.assertEqual(e.kind, "bytes", f"{row['name']}: {e}")
                    errors += 1
        self.assertGreater(frames, 200)
        self.assertGreater(errors, 1000)


def walked_typed(value: Any) -> Any:
    """A typed value or a view of one, as plain data (records by member name; ``from`` for ``from_``)."""
    if isinstance(value, psio.RecordView):
        return {name: walked_typed(value[name]) for name in value.view_fields()}
    if hasattr(value, "__dataclass_fields__"):
        return {name.rstrip("_"): walked_typed(getattr(value, name)) for name in value.__dataclass_fields__}
    if isinstance(value, (psio.TupleView, tuple)):
        return tuple(walked_typed(value[i]) for i in range(len(value)))
    if isinstance(value, dict):
        return sorted((walked_typed(k), walked_typed(v)) for k, v in value.items())
    if isinstance(value, (set, frozenset)):
        return sorted(walked_typed(v) for v in value)
    if isinstance(value, (psio.ListView, list)):
        return [walked_typed(value[i]) for i in range(len(value))]
    if isinstance(value, memoryview):
        return bytes(value)
    return value


if __name__ == "__main__":
    unittest.main()
