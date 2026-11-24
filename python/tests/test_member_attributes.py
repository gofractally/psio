"""The record member attributes of the numbered formats: ``field``, ``default``, ``deprecated``.

protobuf, Cap'n Proto and FlatBuffers read them; the schema-driven codec accepts them on a record member
and does not read them; the typed layer writes them from ``psio.number``, ``psio.default`` and
``psio.deprecated``.
"""

from __future__ import annotations

import enum
import math
import unittest
from dataclasses import dataclass
from typing import Annotated, Any, Dict, List, Optional

import support  # noqa: F401

import psio
from psio import f32, f64, i16, i32, ir, u8, u32, u64, u128

UNSUPPORTED = "pssz IR: unsupported schema constraint or annotation"


def row(id: List[Any], name: List[Any]) -> Any:
    return {"Object": {"members": [ir.member("id", ir.I32, *id), ir.member("name", ir.STRING, *name)],
                       "attributes": []}}


@psio.enum(i32)
class Status(enum.IntEnum):
    pending = -1
    idle = 0
    active = 2


@psio.record
@dataclass
class Stated:
    id: Annotated[u32, psio.number(3)]
    old: Annotated[u8, psio.deprecated]
    count: Annotated[u64, psio.default(7), psio.deprecated("v2"), psio.number(1), psio.json_string]
    tags: Annotated[List[u8], psio.length(min=1), psio.number(9)]
    on: Annotated[bool, psio.default(True)]
    low: Annotated[i16, psio.default(-40)]
    status: Annotated[Status, psio.default(Status.pending)]
    half: Annotated[f32, psio.default(0.5)]
    bottom: Annotated[f64, psio.default(-math.inf)]
    # A zero default is the absence of one.
    zero: Annotated[u32, psio.default(0)]
    off: Annotated[bool, psio.default(False)]
    idle: Annotated[Status, psio.default(0)]
    nothing: Annotated[f64, psio.default(0.0)]
    plain: str


@psio.record(name="Stated")
@dataclass
class Plain:
    id: u32
    old: u8
    count: Annotated[u64, psio.json_string]
    tags: Annotated[List[u8], psio.length(min=1)]
    on: bool
    low: i16
    status: Status
    half: f32
    bottom: f64
    zero: u32
    off: bool
    idle: Status
    nothing: f64
    plain: str


def attributes_of(cls: Any, name: str) -> Dict[str, List[Any]]:
    schema = psio.schema_of(cls)
    record = next(t for t in schema.ir["types"] if t["name"] == name)["type"]["value"].value
    return {m["name"]: [(a["name"], a["value"]) for a in m["attributes"]] for m in record["members"]}


class SchemaDriven(unittest.TestCase):
    def test_a_record_member_with_them_is_the_same_type(self) -> None:
        numbered = [
            ir.attribute("field", "7"),
            ir.attribute("deprecated", ""),
            ir.attribute("default", "-1"),
            ir.attribute("protobuf", "sint"),
        ]
        bound = ir.attribute("lengthBound", "max=9")
        stated = row(numbered, [bound, ir.attribute("field", "2"), ir.attribute("deprecated", "v2")])
        bounded = row([], [bound])
        plain = row([], [])
        value = {"id": -5, "name": "abc"}
        for other, width in ((bounded, "auto"), (plain, 1), (plain, 2), (plain, 4)):
            a = psio.Schema({"R": stated}).type("R", width=width)
            b = psio.Schema({"R": other}).type("R", width=width)
            data = a.to_pssz(value)
            self.assertEqual(data, b.to_pssz(value))
            self.assertEqual(a.from_pssz(data), value)
            self.assertTrue(a.validate_pssz(data).ok)
            self.assertEqual(a.validate_pssz(data).has_unknown, b.validate_pssz(data).has_unknown)
            if width != "auto":
                continue
            for format in ("json", "ijson"):
                self.assertEqual(a.to_json(value, format), b.to_json(value, format))
                self.assertEqual(a.from_json(a.to_json(value, format), format), value)
            self.assertEqual(a.to_key(value), b.to_key(value))
            # The same type to a comparison, either way round.
            for x, y in ((a, b), (b, a)):
                self.assertTrue(psio.equivalent(x, y), psio.equivalent(x, y).reason)
                self.assertTrue(psio.evolves_from(x, y), psio.evolves_from(x, y).reason)

    def test_they_belong_to_record_members_only(self) -> None:
        unit = {"Object": {"members": [], "attributes": []}}
        for name in ("field", "default", "deprecated", "protobuf"):
            stated = [ir.attribute(name, "1")]
            cases = {
                "record": {"Object": {"members": [], "attributes": stated}},
                "fixed record": {"Struct": {"members": [], "attributes": stated}},
                "variant": {"Variant": {"members": [ir.member("a", unit)], "attributes": stated}},
                "variant member": {"Variant": {"members": [ir.member("a", unit, *stated)], "attributes": []}},
            }
            for what, root in cases.items():
                for width in ("auto", 4):
                    with self.subTest(name=name, site=what, width=width):
                        with self.assertRaises(psio.SchemaError) as raised:
                            psio.Schema({"R": root}).type("R", width=width)
                        self.assertEqual(raised.exception.reason, UNSUPPORTED)
        # An unknown name on a record member is rejected as before.
        with self.assertRaises(psio.SchemaError) as raised:
            psio.Schema({"R": row([ir.attribute("fieldNumber", "1")], [])}).type("R")
        self.assertEqual(raised.exception.reason, UNSUPPORTED)


class Typed(unittest.TestCase):
    def test_the_builder_writes_them(self) -> None:
        self.assertEqual(attributes_of(Stated, "Stated"), {
            "id": [("field", "3")],
            "old": [("deprecated", "")],
            "count": [("jsonInt", "string"), ("field", "1"), ("deprecated", "v2"), ("default", "7")],
            "tags": [("lengthBound", "min=1"), ("field", "9")],
            "on": [("default", "true")],
            "low": [("default", "-40")],
            "status": [("default", "-1")],
            "half": [("default", "0.5")],
            "bottom": [("default", "-inf")],
            "zero": [], "off": [], "idle": [], "nothing": [], "plain": [],
        })

    def test_the_codecs_do_not_read_them(self) -> None:
        fields = dict(id=1, old=2, count=3, tags=[4], on=True, low=-5, status=Status.active, half=1.5, bottom=2.5,
                      zero=6, off=False, idle=Status.pending, nothing=0.0, plain="p")
        value, plain = Stated(**fields), Plain(**fields)
        data = psio.to_pssz(value)
        self.assertEqual(data, psio.to_pssz(plain))
        self.assertEqual(psio.from_pssz(Stated, data), value)
        self.assertEqual(psio.to_json(value), psio.to_json(plain))
        self.assertEqual(psio.to_key(value), psio.to_key(plain))
        self.assertEqual(psio.schema_of(Stated).type("Stated").to_pssz(value), data)
        # The schema travels and is read back with them.
        fetched = psio.Schema.from_pssz(psio.schema_of(Stated).to_pssz())
        self.assertEqual(fetched.ir, psio.schema_of(Stated).ir)
        self.assertTrue(fetched.type("Stated").validate_pssz(data).ok)
        self.assertTrue(psio.equivalent(Stated, Plain))
        self.assertTrue(psio.evolves_from(Plain, Stated))

    def test_the_text_of_a_float_default_is_the_shortest(self) -> None:
        def text(tp: Any, value: Any) -> str:
            cls = psio.record(dataclass(type("F", (), {"__annotations__": {"x": Annotated[tp, psio.default(value)]}})))
            found = attributes_of(cls, "F")["x"]
            return found[0][1] if found else ""

        for value, expected in (
                (0.0, ""), (-0.0, "-0"), (math.nan, "nan"), (math.inf, "inf"), (-math.inf, "-inf"), (1.0, "1"),
                (1, "1"), (-2.5, "-2.5"), (0.1, "0.1"), (1234.5, "1234.5"), (10000.0, "10000"), (100000.0, "1e+05"),
                (123456.0, "123456"), (1e22, "1e+22"), (1.5e300, "1.5e+300"), (0.001, "0.001"), (0.0001, "1e-04"),
                (0.00015, "0.00015"), (1.25e-7, "1.25e-07"), (-3e-310, "-3e-310"),
                (12345678901234567890.0, "12345678901234567168")):
            self.assertEqual(text(f64, value), expected, repr(value))
        self.assertEqual(text(f32, 0.1), "0.1")
        self.assertEqual(text(f32, 16777216.0), "16777216")
        self.assertEqual(text(f32, 3.4028234663852886e38), "3.4028235e+38")
        self.assertEqual(text(f32, 1e-46), "")

    def test_they_are_checked_and_are_a_record_members(self) -> None:
        def declare(hint: Any) -> Any:
            cls = psio.record(dataclass(type("R", (), {"__annotations__": {"a": hint}})))
            return psio.schema_of(cls)

        for hint, reason in (
                (Annotated[str, psio.default(1)], "psio.default applies to a scalar member"),
                (Annotated[Optional[u32], psio.default(1)], "psio.default applies to a scalar member"),
                (Annotated[u128, psio.default(1)], "psio.default applies to a scalar member"),
                (Annotated[u8, psio.default(256)], "out of the range"),
                (Annotated[u8, psio.default(True)], "the default of an integer or an enum is an integer"),
                (Annotated[u32, psio.default(1.5)], "the default of an integer or an enum is an integer"),
                (Annotated[bool, psio.default(1)], "the default of a bool is True or False"),
                (Annotated[f64, psio.default("1")], "the default of a float is a number"),
                (Annotated[u32, psio.number(1), psio.number(2)], "stated once")):
            with self.subTest(hint=hint):
                with self.assertRaises(psio.SchemaError) as raised:
                    declare(hint)
                self.assertIn(reason, raised.exception.reason)
        for bad in (-1, 1 << 32, True, "1"):
            with self.assertRaises(TypeError):
                psio.number(bad)  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            psio.deprecated(2)  # type: ignore[arg-type]
        for spec in (psio.number(1), psio.default(1), psio.deprecated):
            with self.subTest(spec=spec):
                with self.assertRaises(psio.SchemaError) as raised:
                    psio.schema_of(names={"A": psio.arguments({"a": Annotated[u32, spec]})})
                self.assertIn("belong to record members", raised.exception.reason)


if __name__ == "__main__":
    unittest.main()
