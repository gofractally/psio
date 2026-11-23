"""The consistency round, mirrored from cpp/tests/consistency_tests.cpp.

Name coders (every row of conformance/name-vectors.json), order constraints, ``max_fields``, a
transparent type that bounds its member (every ``transparent-bounds`` row of pssz-vectors.json, on
hand-written declarations), the key writer's bounds, reserved custom ids, enums, text and caps,
missing members.
"""

import collections
import dataclasses
import enum
import unittest
from dataclasses import dataclass
from typing import Annotated, Any, Dict, List, Optional, Tuple

from support import BOUND_REASONS, fixture, same_reason

import psio
from psio import RawString, i16, i32, ir, u8, u32, u64


def refused(case: unittest.TestCase, run: Any, kind: Any = psio.PsioError) -> psio.PsioError:
    with case.assertRaises(kind) as raised:
        run()
    case.assertNotEqual(raised.exception.kind, "internal")
    return raised.exception  # type: ignore[no-any-return]


# ── Name coders ──────────────────────────────────────────────────────────────

class Names(unittest.TestCase):
    def test_every_row(self) -> None:
        fx = fixture("name-vectors.json")
        self.assertEqual(fx["version"], 1)
        hashed = compressed = named = 0
        for text, method_number, method_text, name_number, name_text in fx["names"]:
            n = psio.method_to_number(text)
            self.assertEqual(str(n), method_number, f"method_to_number({text!r})")
            self.assertEqual(psio.number_to_method(n), method_text, text)
            self.assertEqual(psio.method_to_number(text.encode("utf-8")), n, text)
            if n:
                # A method number round-trips through its text.
                self.assertEqual(psio.method_to_number(psio.number_to_method(n)), n, text)
                hashed += psio.is_hash_name(n)
                compressed += not psio.is_hash_name(n)
                self.assertEqual(psio.number_to_method(n).startswith("#"), psio.is_hash_name(n), text)
            m = psio.name_to_number(text)
            self.assertEqual(str(m), name_number, f"name_to_number({text!r})")
            self.assertEqual(psio.number_to_name(m), name_text, text)
            if m:
                # A number is a name's only if it decodes to the name.
                self.assertEqual(psio.number_to_name(m), text)
                named += 1
        for number, method_text, name_text in fx["numbers"]:
            self.assertEqual(psio.number_to_method(int(number)), method_text, number)
            self.assertEqual(psio.number_to_name(int(number)), name_text, number)
        print(f"name-vectors.json: names={len(fx['names'])} numbers={len(fx['numbers'])} "
              f"(method numbers: compressed={compressed} hash={hashed}; identifier names with a number={named})")
        self.assertEqual((len(fx["names"]), len(fx["numbers"])), (1538, 617))
        self.assertGreater(min(hashed, compressed, named), 100)

    def test_the_rules(self) -> None:
        # (cpp/tests/consistency_tests.cpp "method_to_number", "name_to_number")
        n = psio.method_to_number("getSymbolType")
        self.assertEqual((n, psio.number_to_method(n)), (0xD55DF3070B8758EB, "#hmlnslheseoypnnp"))
        self.assertTrue(psio.is_hash_name(n))
        self.assertEqual(psio.method_to_number("#hmlnslheseoypnnp"), n)
        short = psio.method_to_number("transfer")
        self.assertFalse(psio.is_hash_name(short))
        self.assertEqual(psio.number_to_method(short), "transfer")
        self.assertEqual(psio.method_to_number("Transfer"), short)            # case is ignored
        for malformed in ("#", "#abc", "#hmlnslheseoypnn", "#hmlnslheseoypnnpp", "#zzzzzzzzzzzzzzzz", "", "a b", "é"):
            self.assertEqual(psio.method_to_number(malformed), 0, malformed)
        self.assertEqual(psio.number_to_method(0), "")
        self.assertEqual(psio.name_to_number("x--b"), 0)                      # no frequency for - after -
        self.assertEqual(psio.name_to_number("alice"), int(next(r[3] for r in fixture("name-vectors.json")["names"]
                                                               if r[0] == "alice")))
        for bad in ("", "9lives", "-x", "Alice", "a" * 19, "a_b"):
            self.assertEqual(psio.name_to_number(bad), 0, bad)
        self.assertEqual(psio.number_to_name(0), "")
        for bad in (-1, 1 << 64, "1", 1.0):
            with self.assertRaises((ValueError, TypeError)):
                psio.number_to_name(bad)  # type: ignore[arg-type]
            with self.assertRaises((ValueError, TypeError)):
                psio.number_to_method(bad)  # type: ignore[arg-type]


# ── sorted / unique / unique_keys ────────────────────────────────────────────

@dataclass
class Ranked:
    scores: Annotated[List[u32], psio.sorted, psio.max(8)]
    names: Annotated[List[RawString], psio.unique]
    falling: Annotated[List[i32], psio.sorted(descending=True)]
    entries: Annotated[List[Tuple[RawString, u32]], psio.unique_keys]
    rows: Annotated[List[List[u8]], psio.element(psio.sorted)]
    triple: Annotated[List[i16], psio.array(3), psio.sorted]


@dataclass
class Loose:
    scores: Annotated[List[u32], psio.max(8)]
    names: List[RawString]
    falling: List[i32]
    entries: List[Tuple[RawString, u32]]
    rows: List[List[u8]]
    triple: Annotated[List[i16], psio.array(3)]


def good() -> Ranked:
    return Ranked([1, 1, 2, 300], [b"a", b"ab", b"b"], [5, 5, -1], [(b"x", 1), (b"a", 1)], [[1, 2, 2], [], [9]],
                  [-5, 0, 0])


def loose(r: Ranked) -> Loose:
    return Loose(**dataclasses.asdict(r))


ORDER = "sorted: elements are not in order"
STRICT = "sorted: elements are not in strictly ascending order"
KEYS = "unique_keys: two elements have the same key"


class OrderConstraints(unittest.TestCase):
    def test_writers_and_checking_readers(self) -> None:
        ok = good()
        frame, text, key = psio.to_pssz(ok), psio.to_json(ok), psio.to_key(ok)
        self.assertTrue(psio.validate_pssz(Ranked, frame, strict=True).ok)
        self.assertEqual(psio.from_pssz(Ranked, frame), ok)
        self.assertEqual(psio.from_json(Ranked, text), ok)
        self.assertEqual(psio.from_ijson(Ranked, psio.to_ijson(ok)), ok)
        self.assertEqual(psio.from_key(Ranked, key), ok)
        # The constraints change no byte, no width and no schema.
        self.assertEqual(frame, psio.to_pssz(loose(ok)))
        self.assertEqual(psio.codec(Ranked).max_encoded_size, psio.codec(Loose).max_encoded_size)
        self.assertEqual(psio.schema_of(names={"R": Ranked}).ir["types"][-2]["type"],
                         psio.schema_of(names={"R": Loose}).ir["types"][-2]["type"])
        self.assertEqual([m["name"] for m in psio.schema_of(Ranked).ir["types"]][1:],
                         [m["name"] for m in psio.schema_of(Loose).ir["types"]][1:])

        cases = (
            ("scores descend", {"scores": [2, 1]}, ORDER),
            ("names repeat", {"names": [b"a", b"a"]}, STRICT),
            ("names descend", {"names": [b"b", b"a"]}, STRICT),
            ("falling ascends", {"falling": [1, 2]}, ORDER),
            ("key order, not signed order", {"falling": [-1, 1]}, ORDER),
            ("entries repeat a key", {"entries": [(b"a", 1), (b"b", 2), (b"a", 3)]}, KEYS),
            ("a row descends", {"rows": [[1], [3, 2]]}, ORDER),
            ("the array descends", {"triple": [1, 0, 2]}, ORDER),
        )
        for what, change, message in cases:
            bad = dataclasses.replace(good(), **change)
            # Writers.
            for write in (psio.to_pssz, psio.to_json, psio.to_ijson, psio.to_key, psio.pssz_size, psio.key_size):
                e = refused(self, lambda: write(bad), psio.EncodeError)
                self.assertEqual(e.reason, message, f"{what}: {write.__name__}")
            # Readers, on what an unchecked writer produces.
            raw = loose(bad)
            frame = psio.to_pssz(raw)
            st = psio.validate_pssz(Ranked, frame)
            self.assertEqual((st.ok, st.reason, st.error.kind), (False, message, "bytes"), what)
            self.assertFalse(psio.validate_pssz(Ranked, frame, strict=True).ok, what)
            self.assertEqual(refused(self, lambda: psio.from_pssz(Ranked, frame), psio.DecodeError).reason, message)
            for fmt, to_text, from_text, validate in (("json", psio.to_json, psio.from_json, psio.validate_json),
                                                      ("ijson", psio.to_ijson, psio.from_ijson, psio.validate_ijson)):
                raw_text = to_text(raw)
                self.assertFalse(validate(Ranked, raw_text).ok, f"{what}: {fmt}")
                self.assertEqual(refused(self, lambda: from_text(Ranked, raw_text), psio.JsonError).reason, message)
                self.assertEqual(refused(self, lambda: psio.json_to_pssz(Ranked, raw_text, fmt)).reason, message)
                self.assertEqual(refused(self, lambda: psio.pssz_to_json(Ranked, frame, fmt)).reason, message)
            raw_key = psio.to_key(raw)
            self.assertFalse(psio.validate_key(Ranked, raw_key).ok, what)
            self.assertEqual(refused(self, lambda: psio.from_key(Ranked, raw_key), psio.DecodeError).reason, message)
            # The trusted decode and a view do not check; decoding through the view does.
            self.assertEqual(psio.from_pssz(Ranked, psio.trusted(frame)), bad, what)
            view = psio.pssz_view(Ranked, frame)
            self.assertEqual(refused(self, view.to_value, psio.DecodeError).reason, message)

    def test_order_is_by_key_bytes(self) -> None:
        @dataclass
        class Mixed:
            words: Annotated[List[str], psio.sorted]
            maybe: Annotated[Optional[List[i32]], psio.unique]
            by_id: Annotated[Dict[u8, List[u32]], psio.values(psio.unique)]
            pairs: Annotated[Tuple[List[u8], u8], psio.at(0, psio.sorted)]

        ok = Mixed(["", "a", "a\x00", "ab", "b"], None, {1: [1, 2]}, ([1, 1], 0))
        self.assertEqual(psio.from_pssz(Mixed, psio.to_pssz(ok)), ok)
        self.assertEqual(psio.from_json(Mixed, psio.to_json(ok)), ok)
        # Signed integers order by key bytes, which is their numeric order.
        self.assertEqual(psio.from_pssz(Mixed, psio.to_pssz(dataclasses.replace(ok, maybe=[-2, -1, 0, 7]))).maybe,
                         [-2, -1, 0, 7])
        for change in ({"maybe": [0, -1]}, {"maybe": [3, 3]}, {"by_id": {1: [1, 2], 2: [2, 2]}},
                       {"pairs": ([2, 1], 0)}, {"words": ["b", "a"]}):
            refused(self, lambda: psio.to_pssz(dataclasses.replace(ok, **change)), psio.EncodeError)

    def test_method_parameters_and_results(self) -> None:
        @psio.interface
        class Api:
            def put(self, ids: Annotated[List[u32], psio.unique, psio.max(4)]) -> Annotated[List[u32], psio.sorted]:
                return list(reversed(ids))

        put = psio.interface_of(Api)["put"]
        self.assertEqual(put.from_pssz(put.to_pssz([1, 2])), ([1, 2],))
        self.assertEqual(refused(self, lambda: put.to_pssz([2, 2]), psio.EncodeError).reason, STRICT)
        self.assertEqual(refused(self, lambda: put.args.from_json('{"ids":[2,1]}'), psio.JsonError).reason, STRICT)
        self.assertEqual(refused(self, lambda: put.args.from_json('[[2,1]]'), psio.JsonError).reason, STRICT)
        frame = psio.to_pssz(([2, 1],), Tuple[Annotated[List[u32], psio.bounded(4)]])
        self.assertEqual(put.args.validate_pssz(frame).reason, STRICT)
        self.assertEqual(put.result_to_pssz([1, 2]), psio.to_pssz([1, 2], List[u32]))
        self.assertEqual(refused(self, lambda: put.result_to_pssz([2, 1]), psio.EncodeError).reason, ORDER)
        self.assertEqual(put.validate_result(psio.to_pssz([2, 1], List[u32])).reason, ORDER)
        # dispatch encodes the result as its type: an implementation that breaks the promise is refused.
        refused(self, lambda: psio.interface_of(Api).dispatch(Api(), "put", put.to_pssz([1, 2])), psio.EncodeError)

    def test_where_it_may_be_written(self) -> None:
        for hint in (Annotated[str, psio.sorted], Annotated[Dict[u8, u8], psio.unique],
                     Annotated[set[u8], psio.sorted], Annotated[u32, psio.unique_keys]):
            @dataclass
            class Bad:
                member: hint  # type: ignore[valid-type]
            refused(self, lambda: psio.codec(Bad), psio.SchemaError)
        # Not a type's own: a record member, a parameter or a position below one.
        refused(self, lambda: psio.codec(Annotated[List[u32], psio.sorted]), psio.SchemaError)

        @psio.transparent
        @dataclass
        class Wrapper:
            items: Annotated[List[u32], psio.sorted]
        refused(self, lambda: psio.codec(Wrapper), psio.SchemaError)


# ── evolves_from ─────────────────────────────────────────────────────────────

def account(name: str, owner: Any, tags: Any, **options: Any) -> Any:
    """``struct { u64 id; string owner; vector<string> tags; }`` under the given annotations."""
    cls = dataclasses.make_dataclass(name, [
        ("id", u64), ("owner", Annotated[tuple([RawString] + list(owner))]),  # type: ignore[misc]
        ("tags", Annotated[tuple([List[RawString]] + list(tags))])])  # type: ignore[misc]
    return psio.record(cls, **options) if options else cls


def fields3(name: str, count: int, **options: Any) -> Any:
    members = [("a", u32)] + [(chr(ord("b") + i), Optional[u32], None) for i in range(count - 1)]
    return psio.record(dataclasses.make_dataclass(name, members), max_dynamic_data=64, **options)


class Evolution(unittest.TestCase):
    def test_member_annotations_and_caps(self) -> None:
        # (cpp/tests/consistency_tests.cpp "pssz_evolves_from: member annotations and caps")
        utf8, mx, el = psio.utf8, psio.max, psio.element
        cap = {"max_dynamic_data": 1024}
        V1 = account("V1", [utf8, mx(32)], [mx(8), el(mx(16))], **cap)
        Same = account("Same", [utf8, mx(32)], [mx(8), el(mx(16))], **cap)
        Wider = account("Wider", [utf8, mx(64)], [mx(8), el(mx(16))], **cap)
        Tighter = account("Tighter", [utf8, mx(16)], [mx(8), el(mx(16))], **cap)
        Raw = account("Raw", [mx(32)], [mx(8), el(mx(16))], **cap)
        ElemWider = account("ElemWider", [utf8, mx(32)], [mx(8), el(mx(20))], **cap)
        ElemTighter = account("ElemTighter", [utf8, mx(32)], [mx(8), el(mx(8))], **cap)
        ElemUtf8 = account("ElemUtf8", [utf8, mx(32)], [mx(8), el(utf8, mx(16))], **cap)
        CountWider = account("CountWider", [utf8, mx(32)], [mx(12), el(mx(16))], **cap)
        WithMin = account("WithMin", [utf8, mx(32), psio.min(1)], [mx(8), el(mx(16))], **cap)
        Unbounded = account("Unbounded", [utf8], [mx(8), el(mx(16))], **cap)
        Sorted = account("Sorted", [utf8, mx(32)], [mx(8), psio.sorted, el(mx(16))], **cap)
        CapWider = account("CapWider", [utf8, mx(32)], [mx(8), el(mx(16))], max_dynamic_data=4096)
        CapTighter = account("CapTighter", [utf8, mx(32)], [mx(8), el(mx(16))], max_dynamic_data=512)
        CapOtherWidth = account("CapOtherWidth", [utf8, mx(32)], [mx(8), el(mx(16))], max_dynamic_data=100000)
        Open = account("Open", [utf8, mx(300)], [mx(8), el(mx(16))])
        OpenWideList = account("OpenWideList", [utf8, mx(300)], [mx(8), el(mx(40))])
        OpenWider = account("OpenWider", [utf8, mx(400)], [mx(8), el(mx(16))])
        OpenCapped = account("OpenCapped", [utf8, mx(300)], [mx(8), el(mx(16))], max_dynamic_data=600)
        OpenCappedLow = account("OpenCappedLow", [utf8, mx(300)], [mx(8), el(mx(16))], max_dynamic_data=400)
        Fields3 = fields3("Fields3", 2, max_fields=3)
        Fields3Grown = fields3("Fields3Grown", 3, max_fields=3)
        Fields3Raised = fields3("Fields3Raised", 4, max_fields=8)
        Fields3Dropped = fields3("Fields3Dropped", 3)
        Fields3Lowered = fields3("Fields3Lowered", 2, max_fields=2)
        self.assertTrue(400 < psio.codec(Open).max_encoded_size <= 600)
        self.assertEqual(psio.codec(Open).width, psio.codec(OpenWideList).width)
        expected = (
            (V1, Same, True),
            # A bound may only widen, and only while every width stays the same.
            (V1, Wider, True), (V1, Tighter, False), (Wider, V1, False),
            (V1, ElemWider, True), (V1, ElemTighter, False),
            (V1, CountWider, True), (CountWider, V1, False),
            (V1, Unbounded, True), (Unbounded, V1, False),
            (V1, WithMin, False), (WithMin, V1, True),
            # utf8 / raw and sorted are the member's type.
            (V1, Raw, False), (Raw, V1, False), (V1, ElemUtf8, False), (V1, Sorted, False), (Sorted, V1, False),
            # The cap may grow within its width class and may not shrink.
            (V1, CapWider, True), (V1, CapTighter, False), (V1, CapOtherWidth, False),
            # Adding a cap: only one the old type's inferred bound fits.  Dropping it: nothing old is refused.
            (Open, OpenCapped, True), (Open, OpenCappedLow, False), (OpenCapped, Open, True),
            # A widened bound that moves a nested list's offset table into another width class changes the bytes.
            (Open, OpenWideList, False), (Open, OpenWider, True),
            # maxFields: no more fields than the original's cap; not raised, not dropped.
            (Fields3, Fields3Grown, True), (Fields3, Fields3Raised, False), (Fields3, Fields3Dropped, False),
            (Fields3, Fields3Lowered, True), (Fields3Lowered, Fields3Grown, False),
        )
        for old, new, ok in expected:
            status = psio.evolves_from(old, new)
            self.assertEqual(bool(status), ok, f"{old.__name__} -> {new.__name__}: {status.reason}")
            self.assertEqual(status.reason == "", ok)
            # The same over two Schema IRs (which do not record sorted / unique).
            a = psio.Schema.from_pssz(psio.schema_of(old).to_pssz()).type(old.__name__)
            b = psio.Schema.from_pssz(psio.schema_of(new).to_pssz()).type(new.__name__)
            self.assertEqual(bool(psio.evolves_from(a, b)), ok or Sorted in (old, new),
                             f"{old.__name__} -> {new.__name__} (IR)")
        # What the check promises, on bytes.
        full = V1(0, "a" * 32, [b"b" * 16])
        frame = psio.to_pssz(full)
        self.assertTrue(psio.validate_pssz(Wider, frame).ok)
        self.assertTrue(psio.validate_pssz(CapWider, frame).ok)
        self.assertFalse(psio.validate_pssz(Tighter, frame).ok)
        self.assertFalse(psio.validate_pssz(ElemTighter, frame).ok)

    def test_shapes(self) -> None:
        @dataclass
        class P1:
            a: u32
            b: str

        def revision(*members: Any, **options: Any) -> Any:
            cls = dataclasses.make_dataclass("P1", [("a", u32), ("b", str)] + list(members))
            return psio.record(cls, **options) if options else cls

        self.assertTrue(psio.evolves_from(P1, revision(("c", Optional[u64], None))))
        status = psio.evolves_from(P1, revision(("c", u64)))
        self.assertFalse(status)
        self.assertIn("Optional", status.reason)
        self.assertFalse(psio.evolves_from(revision(("c", Optional[u64], None)), P1))        # a field removed
        self.assertFalse(psio.evolves_from(P1, revision(definition_will_not_change=True)))
        self.assertFalse(psio.evolves_from(P1, dataclasses.make_dataclass("P1", [("a", psio.i32), ("b", str)])))
        self.assertFalse(psio.evolves_from(P1, dataclasses.make_dataclass("P1", [("b", str), ("a", u32)])))
        # An appended field that moves the record's width is not compatible: reserve room with a cap.

        @dataclass
        class Small:
            a: Annotated[str, psio.max(200)]
        grown = dataclasses.make_dataclass("Small", [("a", Annotated[str, psio.max(200)]),
                                                     ("b", Optional[Annotated[str, psio.max(200)]], None)])
        self.assertEqual((psio.codec(Small).width, psio.codec(grown).width), (1, 2))
        self.assertIn("offset width", psio.evolves_from(Small, grown).reason)
        # Containers: the same rules below; a map's key type is fixed, its mapped type evolves.
        self.assertTrue(psio.evolves_from(List[P1], List[revision(("c", Optional[u64], None))]))
        self.assertTrue(psio.evolves_from(Dict[u32, P1], Dict[u32, revision(("c", Optional[u64], None))]))
        self.assertFalse(psio.evolves_from(Dict[u32, u8], Dict[u64, u8]))
        self.assertFalse(psio.evolves_from(Dict[Annotated[str, psio.bounded(4)], u8],
                                           Dict[Annotated[str, psio.bounded(8)], u8]))
        self.assertFalse(psio.evolves_from(List[Tuple[u32, u8]], Dict[u32, u8]))
        self.assertFalse(psio.evolves_from(set[u32], List[u32]))
        self.assertFalse(psio.evolves_from(Annotated[List[u8], psio.array(4)], Annotated[List[u8], psio.array(5)]))
        self.assertTrue(psio.evolves_from(Annotated[List[u8], psio.bounded(4)], Annotated[List[u8], psio.bounded(5)]))
        self.assertFalse(psio.evolves_from(Annotated[List[u8], psio.bounded(5)], Annotated[List[u8], psio.bounded(4)]))
        self.assertFalse(psio.evolves_from(str, RawString))
        self.assertFalse(psio.evolves_from(Optional[u32], Optional[str]))
        self.assertTrue(psio.evolves_from(Annotated[P1, psio.nested()],
                                          Annotated[revision(("c", Optional[u64], None)), psio.nested()]))
        # A variant: alternatives in order; new ones are appended (spec 7.5).
        self.assertTrue(psio.evolves_from(Annotated[u8, psio.alt("a")] | Annotated[str, psio.alt("b")],  # type: ignore[arg-type]
                                          Annotated[u8, psio.alt("a")] | Annotated[str, psio.alt("b")]
                                          | Annotated[u64, psio.alt("c")]))
        self.assertFalse(psio.evolves_from(Annotated[u8, psio.alt("a")] | Annotated[str, psio.alt("b")],  # type: ignore[arg-type]
                                           Annotated[str, psio.alt("b")] | Annotated[u8, psio.alt("a")]))
        # Methods: an argument list evolves by an appended Optional parameter, under its cap.

        @psio.interface
        class A1:
            @psio.method(max_dynamic_data=256, max_fields=4)
            def transfer(self, to: Annotated[str, psio.max(16)], amount: u64) -> None: ...

        @psio.interface
        class A2:
            @psio.method(max_dynamic_data=256, max_fields=4)
            def transfer(self, to: Annotated[str, psio.max(16)], amount: u64, memo: Optional[str] = None) -> None: ...

        old, new = psio.interface_of(A1)["transfer"].args, psio.interface_of(A2)["transfer"].args
        self.assertTrue(psio.evolves_from(old, new))
        self.assertFalse(psio.evolves_from(new, old))
        with self.assertRaises(psio.SchemaError):
            psio.evolves_from(psio.codec(P1, width=2), P1)


# ── max_fields ───────────────────────────────────────────────────────────────

class MaxFields(unittest.TestCase):
    def test_declarations(self) -> None:
        @psio.record(max_fields=4)
        @dataclass
        class Capped:
            a: u32
            b: Optional[str] = None

        attributes = psio.schema_of(Capped).ir["types"][0]["type"]["value"].value["attributes"]
        self.assertEqual(attributes, [{"name": "maxFields", "value": "4"}])
        self.assertEqual(psio.from_pssz(Capped, psio.to_pssz(Capped(1, "x"))), Capped(1, "x"))

        @psio.record(max_fields=1)
        @dataclass
        class TooMany:
            a: u32
            b: u32
        self.assertIn("max_fields", refused(self, lambda: psio.codec(TooMany), psio.SchemaError).reason)

        @psio.record(max_fields=256)
        @dataclass
        class TooHigh:
            a: u32
        refused(self, lambda: psio.schema_of(TooHigh), psio.SchemaError)

        wide = dataclasses.make_dataclass("Wide", [(f"m{i}", u8) for i in range(256)])
        self.assertIn("255", refused(self, lambda: psio.codec(wide), psio.SchemaError).reason)
        most = dataclasses.make_dataclass("Most", [(f"m{i}", u8) for i in range(255)])
        self.assertEqual(len(psio.to_pssz(most(*range(255)))), 2 + 255)

        # A method's argument list takes the same cap.
        put = psio.arguments({"id": u32, "text": Annotated[str, psio.max(8)]}, max_fields=4, max_dynamic_data=64)
        node = next(m["type"]["value"].value for m in psio.schema_of(names={"PutArgs": put}).ir["types"]
                    if m["type"]["value"].type == "Object")
        self.assertEqual(node["attributes"], [{"name": "tuple", "value": "named"}, {"name": "maxFields", "value": "4"},
                                              {"name": "maxDynamicData", "value": "64"}])
        refused(self, lambda: psio.arguments({"a": u8, "b": u8}, max_fields=1), psio.SchemaError)
        refused(self, lambda: psio.arguments({"a": u8}, max_fields=256), psio.SchemaError)

        def declare() -> None:
            @psio.interface
            class Service:
                @psio.method(max_fields=1)
                def put(self, id: u32, text: str) -> None: ...
        refused(self, declare, psio.SchemaError)

    def test_schema_validation(self) -> None:
        def record(members: int, max_fields: Optional[str], kind: str = "Object") -> Any:
            return {kind: {"members": [ir.member(f"m{i}", ir.U8) for i in range(members)],
                           "attributes": [] if max_fields is None else [ir.attribute("maxFields", max_fields)]}}

        def reads(root: Any, width: Any = "auto") -> bool:
            try:
                psio.Schema({"R": root}).type("R", width=width)
            except psio.SchemaError:
                return False
            return True

        self.assertTrue(reads(record(3, None)))
        self.assertTrue(reads(record(3, "3")))
        self.assertTrue(reads(record(3, "255")))
        self.assertFalse(reads(record(4, "3")))
        self.assertFalse(reads(record(3, "256")))
        self.assertFalse(reads(record(3, "many")))
        self.assertTrue(reads(record(255, None)))
        self.assertFalse(reads(record(256, None)))
        self.assertFalse(reads(record(256, None, "Struct")))
        self.assertFalse(reads(record(256, None), 4))
        for message, root in (("pssz IR: a record has more than 255 fields", record(256, None)),
                              ("pssz IR: a record has more fields than its maxFields", record(4, "3")),
                              ("pssz IR: malformed maxFields attribute", record(1, "256"))):
            self.assertEqual(refused(self, lambda: psio.Schema({"R": root}).type("R"), psio.SchemaError).reason, message)
        # The `tuple` attribute: named or positional, nothing else.
        for value, ok in (("named", True), ("positional", True), ("both", False), (None, False)):
            root = {"Object": {"members": [ir.member("0", ir.U8)], "attributes": [ir.attribute("tuple", value)]}}
            self.assertEqual(reads(root), ok, value)

        # validate: a function's parameter list is a tuple.
        def with_params(count: int, attributes: List[Any]) -> psio.Schema:
            schema = psio.Schema({"U": ir.U32})
            schema.ir["interfaces"].append({"name": "api", "type_names": [], "attributes": [], "funcs": [{
                "name": "call", "result": None, "attributes": attributes,
                "params": [{"name": f"p{i}", "type": ir.normalize_type(ir.U32), "attributes": []}
                           for i in range(count)]}]})
            return psio.Schema.from_pssz(schema.to_pssz())

        def valid(schema: psio.Schema) -> bool:
            try:
                schema.validate()
            except psio.SchemaError:
                return False
            return True

        self.assertTrue(valid(with_params(255, [])))
        self.assertFalse(valid(with_params(256, [])))
        self.assertTrue(valid(with_params(2, [ir.attribute("maxFields", "2")])))
        self.assertFalse(valid(with_params(3, [ir.attribute("maxFields", "2")])))
        self.assertFalse(valid(with_params(1, [ir.attribute("maxFields", "256")])))
        self.assertFalse(valid(with_params(1, [ir.attribute("maxDynamicData", "x")])))
        self.assertEqual(with_params(2, [ir.attribute("maxFields", "2")]).method("api", "call").max_fields, 2)


# ── A transparent type may bound its member ──────────────────────────────────

@psio.transparent
@dataclass(frozen=True)
class Memo:                                   # is BoundedString(80)
    text: Annotated[str, psio.max(80)]


@psio.transparent
@dataclass(frozen=True)
class WrapMemo:                               # the same, spelled with the wrapper type
    text: Annotated[str, psio.bounded(80)]


@dataclass
class MemoRec:
    id: u32
    memo: Memo
    note: Optional[Memo]
    memos: List[Memo]


@psio.transparent
@dataclass(frozen=True)
class Tags:
    list: Annotated[List[str], psio.max(4), psio.element(psio.max(8))]


class TransparentBounds(unittest.TestCase):
    def test_every_row(self) -> None:
        """The 62 ``transparent-bounds`` rows of pssz-vectors.json, on the declarations above."""
        fx = fixture("pssz-vectors.json")
        declared = {"Memo": Memo, "WrapMemo": WrapMemo, "MemoRec": MemoRec}
        rows = [v for v in fx["vectors"] if v["class"] == "transparent-bounds"]
        self.assertEqual({v["type"] for v in rows}, set(declared))
        counts: collections.Counter = collections.Counter()
        widths = {"auto": "auto", "pssz8": 1, "pssz16": 2, "pssz32": 4}
        for row in rows:
            name = row["name"]
            t = psio.codec(declared[row["type"]], width=widths[row["format"]])
            data = bytes.fromhex(row["hex"])
            st = t.validate_pssz(data)
            counts[row["expect"]] += 1
            if row["expect"] == "reject":
                self.assertFalse(st.ok, name)
                self.assertEqual(st.error.kind, "bytes", name)
                self.assertTrue(same_reason(row["reason"], st.reason), f"{name}: {st.reason!r}")
                refused(self, lambda: t.from_pssz(data), psio.DecodeError)
                # (a view of a leaf is its value: read at once; of a record, when it is walked)
                refused(self, lambda: t.pssz_view(data).to_value(), psio.DecodeError)
                if row["reason"] in BOUND_REASONS:
                    # The value the frame holds: no writer writes it, in any format.
                    outside = t.from_pssz(psio.trusted(data))
                    for write in (t.to_pssz, t.to_key, t.to_json, t.to_ijson):
                        refused(self, lambda: write(outside), psio.EncodeError)
                    counts["writer refused"] += 1
                continue
            self.assertEqual(row["expect"], "canonical", name)
            self.assertTrue(st.ok and not st.has_unknown, f"{name}: {st.reason}")
            value = t.from_pssz(data)
            self.assertIsInstance(value, declared[row["type"]], name)
            self.assertEqual(t.to_pssz(value).hex(), row["hex"], name)
            self.assertEqual(t.from_key(t.to_key(value)), value, name)
            if row["format"] == "auto":
                for fmt in ("json", "ijson"):
                    self.assertEqual(t.json_to_pssz(t.to_json(value, fmt), fmt).hex(), row["hex"], f"{name}: {fmt}")
                counts["text"] += 1
        print(f"pssz-vectors.json transparent-bounds, hand-declared: {dict(counts)}")
        self.assertEqual((counts["canonical"], counts["reject"]), (30, 32))
        self.assertEqual(len(rows), 62)
        self.assertGreater(counts["writer refused"], 0)

    def test_the_wrapper_is_the_bounded_type(self) -> None:
        bounded = psio.codec(Annotated[str, psio.bounded(80)])
        for cls in (Memo, WrapMemo):
            t = psio.codec(cls)
            self.assertEqual((t.max_encoded_size, t.width), (80, bounded.width))
            self.assertEqual(t.to_pssz(cls("hi")), b"hi")
            self.assertEqual(t.to_json(cls("hi")), '"hi"')
            self.assertEqual(t.to_key(cls("hi")), bounded.to_key("hi"))
            for write in (t.to_pssz, t.to_key, t.to_json):
                refused(self, lambda: write(cls("m" * 81)), psio.EncodeError)
            refused(self, lambda: t.from_json('"' + "m" * 81 + '"'), psio.JsonError)
            self.assertFalse(t.validate_pssz(b"m" * 81).ok)
            self.assertFalse(t.validate_pssz(b"\xff").ok)
            self.assertFalse(t.validate_key(bounded.to_key("m" * 80)[:-2] + b"mm\x00\x00").ok)
        # The schema node: a named alias of the bounded type (the IR of the fixture, test_typed_ir.py).
        schema = psio.schema_of(Memo)
        by_name = {m["name"]: m["type"]["value"] for m in schema.ir["types"]}
        target = by_name[by_name["Memo"].value["type"]]
        self.assertEqual((target.type, target.value["id"], target.value["type"]["value"].value["maxCount"]),
                         ("Custom", "utf8", 80))
        self.assertEqual(psio.schema_of(WrapMemo).ir["types"][1]["type"], schema.ir["types"][1]["type"])
        # Held anywhere, it is that type: the holder's bound and width follow.

        @dataclass
        class Small:
            a: Memo
        self.assertEqual((psio.codec(Small).max_encoded_size, psio.codec(Small).width), (82, 1))
        # Bounds below the member.
        t = psio.codec(Tags)
        self.assertEqual(t.max_encoded_size, 4 * 1 + 4 * 8)
        self.assertEqual(t.to_pssz(Tags(["a", "bc"])).hex(), "020361" + "6263")
        refused(self, lambda: t.to_pssz(Tags(["a"] * 5)), psio.EncodeError)
        refused(self, lambda: t.to_pssz(Tags(["x" * 9])), psio.EncodeError)
        self.assertEqual(t.from_pssz(t.to_pssz(Tags(["a", "bc"]))), Tags(["a", "bc"]))

    def test_what_a_type_cannot_say(self) -> None:
        for hint in (Annotated[str, psio.min(1)], Annotated[str, psio.exact(4)], Annotated[str, psio.length(max=4)],
                     Annotated[u64, psio.json_string], Annotated[u32, psio.max(4)], Optional[str]):
            cls = psio.transparent(dataclasses.make_dataclass("W", [("value", hint)]))
            refused(self, lambda: psio.codec(cls), psio.SchemaError)


# ── The key writer checks bounds and UTF-8 ───────────────────────────────────

@dataclass
class KBound:
    shard: u32
    name: Annotated[str, psio.max(4)]
    tags: Annotated[List[RawString], psio.max(2), psio.element(psio.max(3))]


class KeyWriter(unittest.TestCase):
    def test_kbound(self) -> None:
        fx = fixture("time-key-vectors.json")
        rows = [v for v in fx["vectors"] if v["type"] == "KBound"]
        counts: collections.Counter = collections.Counter()
        t = psio.codec(KBound)
        for row in rows:
            kind = row["kind"]
            counts[kind] += 1
            if kind == "value":
                value = t.from_pssz(bytes.fromhex(row["pssz"]))
                self.assertEqual(t.to_key(value).hex(), row["key"], row["name"])
                self.assertEqual(t.from_key(bytes.fromhex(row["key"])), value, row["name"])
                self.assertEqual(t.to_pssz(value).hex(), row["pssz"], row["name"])
                if "json" in row:
                    self.assertEqual((t.to_json(value), t.to_ijson(value)), (row["json"], row["ijson"]), row["name"])
            elif kind == "key-prefix":
                value = t.from_pssz(bytes.fromhex(row["pssz"]))
                elements = [value.shard, value.name, value.tags][:row["count"]]
                prefix = t.key_prefix(elements)
                self.assertEqual(prefix.hex(), row["prefix"], row["name"])
                self.assertTrue(t.to_key(value).startswith(prefix), row["name"])
                bound = psio.key_prefix_upper_bound(prefix)
                self.assertEqual(None if bound is None else bound.hex(), row["bound"], row["name"])
            elif kind == "key-order":
                keys = [t.to_key(t.from_pssz(bytes.fromhex(v))) for v in row["values"]]
                self.assertEqual(keys, sorted(keys), row["name"])
                self.assertEqual(len(set(keys)), len(keys), row["name"])
            elif kind == "key-reject":
                st = t.validate_key(bytes.fromhex(row["key"]))
                self.assertFalse(st.ok, row["name"])
                self.assertEqual(st.error.kind, "bytes", row["name"])
            else:
                self.fail(row["name"])
        self.assertEqual(dict(counts), {"value": 5, "key-prefix": 4, "key-order": 1, "key-reject": 7})

    def test_the_writer_refuses(self) -> None:
        # (cpp/tests/consistency_tests.cpp "key: the writer refuses a value outside a bound or not UTF-8")
        ok = KBound(7, "abcd", [b"xyz", b""])
        self.assertEqual(psio.from_key(KBound, psio.to_key(ok)), ok)
        for change in ({"name": "abcde"}, {"name": "héé"}, {"tags": [b"a", b"b", b"c"]}, {"tags": [b"abcd"]},
                       {"tags": [b"a", b"abcd"]}, {"name": "\ud800"}):
            bad = dataclasses.replace(ok, **change)
            e = refused(self, lambda: psio.to_key(bad), psio.EncodeError)
            self.assertEqual(e.kind, "value")
            refused(self, lambda: psio.to_pssz(bad), psio.EncodeError)
            refused(self, lambda: psio.key_size(bad), psio.EncodeError)
        # A prefix element is checked as the value it is.
        refused(self, lambda: psio.key_prefix(KBound, [7, "abcde"]), psio.EncodeError)
        self.assertEqual(psio.key_prefix(KBound, [7, "abcd"]).hex(), "00000007616263640000")
        # Wrapper types, an optional, a nested record: the same checks at every depth.

        @dataclass
        class Wrapped:
            name: Annotated[str, psio.bounded(4)]
            codes: Annotated[List[psio.u16], psio.bounded(2)]
            alias: Optional[Annotated[str, psio.bounded(4)]]
            rows: List[Tuple[u8, KBound]]

        fine = Wrapped("abcd", [1, 2], "abcd", [(1, ok)])
        self.assertEqual(psio.from_key(Wrapped, psio.to_key(fine)), fine)
        for change in ({"name": "abcde"}, {"codes": [1, 2, 3]}, {"alias": "abcde"},
                       {"rows": [(1, dataclasses.replace(ok, name="abcde"))]}):
            refused(self, lambda: psio.to_key(dataclasses.replace(fine, **change)), psio.EncodeError)


# ── Reserved ids, enums, text and caps, missing members, raw strings ─────────

class Verified(unittest.TestCase):
    def test_reserved_custom_ids(self) -> None:
        reserved = ("bool", "char", "string", "utf8", "hex", "bitvector", "bitlist", "map", "set", "box", "unknown",
                    "time-point", "time-point-seconds", "duration", "duration-1/60", "monotonic-millis")
        free = ("Bool", "AccountNumber", "time", "durations", "time-points", "monotonically", "hexadecimal", "Memo")
        for id in reserved:
            # A handler cannot be registered under one; a declaration cannot use one.
            refused(self, lambda: psio.Schema({"T": ir.U8}, handlers={id: psio.Handler()}), psio.SchemaError)
            refused(self, lambda: psio.codec(Annotated[u64, psio.custom(id)]), psio.SchemaError)
            cls = psio.record(dataclasses.make_dataclass("R", [("a", u8)]), custom=id)
            refused(self, lambda: psio.codec(cls), psio.SchemaError)
        for id in free:
            psio.Schema({"T": ir.U8}, handlers={id: psio.Handler()})
            self.assertEqual(psio.to_pssz(5, Annotated[u8, psio.custom(id)]), b"\x05")
        # `hex_spec` is reserved: nothing reads it.
        self.assertFalse(hasattr(psio, "hex_spec"))
        # A schema in which a reserved id sits on another wire shape is malformed for every reader.
        u8list = ir.list_of(ir.U8)
        for inner, id, text in (
                (u8list, "bool", "true"), (ir.U32, "bool", "true"), (ir.U32, "char", '"a"'), (ir.U64, "string", '"a"'),
                (ir.U64, "utf8", '"a"'), (ir.U64, "hex", '"00"'), (ir.U64, "bitlist", '"0"'),
                (ir.U64, "bitvector", '"0"'), (ir.U64, "map", "{}"), (u8list, "map", "{}"), (ir.U64, "set", "[]"),
                (u8list, "time-point-seconds", '"1970-01-01T00:00:00Z"'), (ir.F64, "duration-millis", "5"),
                (ir.I64, "time-point", "5"), (ir.I64, "duration-bogus", "5"), (ir.U32, "unknown", "5")):
            schema = psio.Schema({"T": ir.custom_of(inner, id)})
            refused(self, lambda: schema.type("T"), psio.SchemaError)
            for run in (lambda: schema.type("T").json_to_pssz(text, "json"),
                        lambda: schema.type("T").from_pssz(bytes(8)), lambda: schema.type("T").from_pssz(b"")):
                refused(self, run, psio.SchemaError)

    def test_an_undeclared_enum_value(self) -> None:
        # docs/pssz-spec.md 4.5: the wire is the underlying integer, any value.  A Python reader carries
        # an undeclared value when the enum says `other=True`, and refuses it otherwise.
        @psio.enum(u8, other=True)
        class Level(enum.Enum):
            low = 0
            high = 2

        @psio.enum(u8)
        class Closed(enum.Enum):
            low = 0
            high = 2

        @dataclass
        class Holder:
            level: Level
            more: List[Level]

        value = Holder(7, [Level.high, 200])  # type: ignore[arg-type, list-item]
        frame = psio.to_pssz(value)
        self.assertEqual(frame.hex(), psio.to_pssz((7, [2, 200]), Tuple[u8, List[u8]]).hex())
        self.assertTrue(psio.validate_pssz(Holder, frame, strict=True).ok)
        self.assertEqual(psio.from_pssz(Holder, frame), value)
        self.assertEqual(psio.from_key(Holder, psio.to_key(value)).more[1], 200)
        # Text: the name for a declared value, the number otherwise; both read.
        self.assertEqual(psio.to_json(value), '{"level":7,"more":["high",200]}')
        self.assertEqual(psio.from_json(Holder, '{"level":7,"more":["high",200]}'), value)
        self.assertEqual(psio.from_ijson(Holder, '{"level":"7","more":[2]}'), Holder(7, [Level.high]))  # type: ignore[arg-type]
        for text in ('{"level":"medium","more":[]}', '{"level":256,"more":[]}'):
            self.assertFalse(psio.validate_json(Holder, text).ok, text)
        # Without `other`, the typed reader refuses what it cannot hold; the bytes are the same.
        self.assertEqual(psio.to_pssz(Closed.high), b"\x02")
        self.assertFalse(psio.validate_pssz(Closed, b"\x07").ok)
        self.assertEqual(psio.from_pssz(Closed, b"\x02"), Closed.high)

    def test_the_order_of_a_union_is_its_wire_format(self) -> None:
        # Python compares unions without regard to order; the codec cache does not.
        ab = Annotated[u8, psio.alt("a")] | Annotated[str, psio.alt("b")]
        ba = Annotated[str, psio.alt("b")] | Annotated[u8, psio.alt("a")]
        self.assertEqual(ab, ba)
        self.assertEqual((psio.to_pssz(7, ab), psio.to_pssz(7, ba)), (b"\x00\x07", b"\x01\x07"))
        self.assertEqual((psio.to_pssz([7], list[ab])[-2:], psio.to_pssz([7], list[ba])[-2:]),
                         (b"\x00\x07", b"\x01\x07"))
        # (`typing.List[...]` / `Optional[...]` are cached by Python itself under that equality: the
        # first spelling wins there.  README "Limits": declare a union once, under a name.)
        self.assertIsNot(psio.schema_of(ab), psio.schema_of(ba))
        self.assertIs(psio.codec(ab), psio.codec(ab))

    def test_a_cap_is_not_applied_to_the_text(self) -> None:
        @psio.record(definition_will_not_change=True, max_dynamic_data=16)
        @dataclass
        class Capped:
            a: u64
            b: u64

        value = Capped((1 << 64) - 1, 2)
        self.assertEqual(len(psio.to_pssz(value)), 16)
        text = psio.to_json(value)
        self.assertGreater(len(text), 16)
        for candidate in (text, "   " + text + " " * 64):
            self.assertTrue(psio.validate_json(Capped, candidate).ok)
            self.assertEqual(psio.from_json(Capped, candidate), value)
            self.assertEqual(psio.json_to_pssz(Capped, candidate, "json"), psio.to_pssz(value))
        self.assertTrue(psio.validate_ijson(Capped, psio.to_ijson(value)).ok)
        # The cap is of the bytes: a value that encodes over it has no encoding, whatever its text.

        @psio.record(max_dynamic_data=8)
        @dataclass
        class Tight:
            s: str
        self.assertTrue(psio.validate_json(Tight, '{"s":"abcdefghij"}').ok)
        refused(self, lambda: psio.json_to_pssz(Tight, '{"s":"abcdefghij"}', "json"), psio.EncodeError)

    def test_a_missing_member(self) -> None:
        @dataclass
        class Inner:
            a: u32
            b: Optional[u32]

        @dataclass
        class Outer:
            id: u32
            inner: Inner
            items: List[Inner]
            maybe: Optional[Inner]

        full = Outer(1, Inner(2, None), [Inner(3, None)], None)
        # A missing optional member is None anywhere; a missing required member is refused anywhere.
        self.assertEqual(psio.from_json(Outer, '{"id":1,"inner":{"a":2},"items":[{"a":3}]}'), full)
        for text in ('{"inner":{"a":2},"items":[]}', '{"id":1,"inner":{},"items":[]}', '{"id":1,"inner":{"a":2}}',
                     '{"id":1,"inner":{"a":2},"items":[{}]}', '{"id":1,"inner":{"a":2},"items":[],"maybe":{"b":1}}'):
            for fmt, read, validate in (("json", psio.from_json, psio.validate_json),
                                        ("ijson", psio.from_ijson, psio.validate_ijson)):
                e = refused(self, lambda: read(Outer, text), psio.JsonError)
                self.assertEqual(e.reason, "json: missing required member", text)
                self.assertFalse(validate(Outer, text).ok, text)
        # The same schema-driven.
        dynamic = psio.Schema.from_pssz(psio.schema_of(Outer).to_pssz()).type("Outer")
        self.assertEqual(dynamic.from_json('{"id":1,"inner":{"a":2},"items":[]}'),
                         {"id": 1, "inner": {"a": 2, "b": None}, "items": [], "maybe": None})
        refused(self, lambda: dynamic.from_json('{"id":1,"items":[]}'), psio.JsonError)

    def test_strict_validation_takes_a_raw_string_that_is_not_utf8(self) -> None:
        @dataclass
        class Raw:
            blob: RawString
            text: str

        frame = psio.to_pssz(Raw(b"\xff\xfe", "ok"))
        for strict in (False, True):
            self.assertTrue(psio.validate_pssz(Raw, frame, strict=strict).ok)
            self.assertTrue(psio.validate_pssz(RawString, b"\xff\xc0\x80", strict=strict).ok)
            self.assertFalse(psio.validate_pssz(str, b"\xff", strict=strict).ok)
        self.assertEqual(psio.from_pssz(Raw, frame, reject_unknown=True).blob, b"\xff\xfe")
        self.assertEqual(psio.from_key(Raw, psio.to_key(Raw(b"\xff\xfe", "ok"))).blob, b"\xff\xfe")
        # (it has no text: a JSON string is UTF-8)
        refused(self, lambda: psio.to_json(Raw(b"\xff", "ok")), psio.EncodeError)
        # The schema-driven strict reader: every such row of the fixture.
        fx = fixture("pssz-vectors.json")
        schema = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        rows = [v for v in fx["vectors"] if v["class"] == "string-raw-non-utf8"]
        self.assertEqual(len(rows), 48)
        widths = {"auto": "auto", "pssz8": 1, "pssz16": 2, "pssz32": 4}
        for row in rows:
            st = schema.type(row["type"], width=widths[row["format"]]).validate_pssz(bytes.fromhex(row["hex"]), strict=True)
            self.assertTrue(st.ok and not st.has_unknown, row["name"])


if __name__ == "__main__":
    unittest.main()
