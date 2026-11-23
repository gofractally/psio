"""The typed layer on declarations written the natural way (beyond the fixtures and the README)."""

from __future__ import annotations

import dataclasses
import enum
import typing
import unittest
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Annotated, Dict, FrozenSet, List, NamedTuple, NewType, Optional, Set, Tuple, TypedDict, Union

import support  # noqa: F401

import psio
from psio import PsioError, RawString, i32, u8, u16, u32, u64


@psio.enum(u8)
class Level(enum.Enum):
    LOW = 0
    HIGH = 1


@psio.enum(i32, other=True)
class Code(enum.IntEnum):
    NEG = -2
    POS = 7


class Point(NamedTuple):
    x: i32
    y: i32


class Config(TypedDict):
    name: str
    retries: Optional[u8]


@psio.record(definition_will_not_change=True)
@dataclass(frozen=True)
class Pair:
    a: u8
    b: u8


@dataclass
class Tree:
    value: u32
    children: List["Tree"] = field(default_factory=list)


@dataclass
class Everything:
    level: Level
    code: Code
    point: Point
    config: Config
    pairs: Dict[Pair, List[u16]]
    names: Set[str]
    frozen: FrozenSet[u32]
    many: Tuple[u8, ...]
    either: Union[u32, str, Pair]
    twins: Union[Annotated[u32, psio.alt("a")], Annotated[u32, psio.alt("b")]]
    maybe: Optional[psio.Some[Optional[u8]]]
    when: psio.TimePointMs
    took: Annotated[timedelta, psio.duration("micros")]
    digest: Annotated[bytes, psio.array(2)]
    flags: Annotated[List[bool], psio.bitlist(8)]
    tree: Tree
    inner: Annotated[Pair, psio.nested()]
    raw: RawString = b""
    extra: Optional[u32] = None


def sample() -> Everything:
    return Everything(
        level=Level.HIGH, code=Code.NEG, point=Point(1, -1), config={"name": "n", "retries": None},
        pairs={Pair(2, 1): [3], Pair(1, 9): []}, names={"b", "a"}, frozen=frozenset({5, 1}), many=(1, 2, 3),
        either=Pair(1, 2), twins=psio.Variant("b", 4), maybe=psio.Some(None),
        when=datetime(2026, 1, 2, 3, 4, 5, 678000, tzinfo=timezone.utc), took=timedelta(seconds=1, microseconds=5),
        digest=b"\x01\x02", flags=[True, False, True], tree=Tree(1, [Tree(2), Tree(3, [Tree(4)])]),
        inner=Pair(7, 8), raw=b"\xff", extra=None)


class Typed(unittest.TestCase):
    maxDiff = None

    def rejects(self, run: typing.Any, kind: typing.Any = PsioError) -> PsioError:
        with self.assertRaises(kind) as caught:
            run()
        return caught.exception  # type: ignore[return-value]

    def test_round_trips_in_every_format(self) -> None:
        v = sample()
        data = psio.to_pssz(v)
        self.assertEqual(psio.from_pssz(Everything, data), v)
        self.assertEqual(psio.pssz_size(v), len(data))
        self.assertTrue(psio.validate_pssz(Everything, data, strict=True))
        self.assertEqual(psio.from_key(Everything.__dataclass_fields__ and Tree, psio.to_key(v.tree)), v.tree)
        for fmt, to_text, from_text in (("json", psio.to_json, psio.from_json), ("ijson", psio.to_ijson, psio.from_ijson)):
            # (Some(None) reads back as None: JSON has one null; bit types have no text.)
            w = dataclasses.replace(v, maybe=psio.Some(3))
            self.assertEqual(self.rejects(lambda: to_text(w)).reason, "json: bit types have no JSON form")
        text = psio.to_ijson(v.tree)
        self.assertEqual(text, '{"value":1,"children":[{"value":2,"children":[]},'
                               '{"value":3,"children":[{"value":4,"children":[]}]}]}')
        self.assertEqual(psio.from_ijson(Tree, text), v.tree)
        # The schema-driven reader of the same schema sees the same bytes as plain values.
        dyn = psio.Schema.from_pssz(psio.schema_of(Everything).to_pssz()).type("Everything")
        plain = dyn.from_pssz(data)
        self.assertEqual(plain["level"], 1)
        self.assertEqual(plain["point"], {"x": 1, "y": -1})
        self.assertEqual(plain["pairs"], [({"a": 1, "b": 9}, []), ({"a": 2, "b": 1}, [3])])
        self.assertEqual(plain["either"], psio.Variant("Pair", {"a": 1, "b": 2}))
        self.assertEqual(plain["twins"], psio.Variant("b", 4))
        self.assertEqual(dyn.to_pssz(plain), data)

    def test_natural_values(self) -> None:
        self.assertEqual(psio.to_pssz(Point(1, -1)).hex(), "0801000000ffffffff")
        self.assertEqual(psio.from_pssz(Point, bytes.fromhex("0801000000ffffffff")), Point(1, -1))
        self.assertEqual(psio.to_json({"name": "n", "retries": 3}, Config), '{"name":"n","retries":3}')
        self.assertEqual(psio.from_json(Config, '{"name":"n"}'), {"name": "n", "retries": None})
        self.assertEqual(psio.to_pssz(Pair(1, 2)), b"\x01\x02")
        self.assertEqual(psio.to_pssz([1, 2], List[u16]).hex(), "01000200")
        self.assertEqual(psio.to_pssz({"b": 1, "a": 2}, Dict[str, u8]),
                         psio.to_pssz({"a": 2, "b": 1}, Dict[str, u8]))
        self.assertEqual(psio.to_ijson({"b": 1, "a": 2}, Dict[str, u8]), '{"a":2,"b":1}')
        self.assertEqual(psio.to_ijson({2: "x", 1: "y"}, Dict[u8, str]), '[[1,"y"],[2,"x"]]')
        self.assertEqual(psio.from_pssz(Set[u16], bytes.fromhex("02000102")), {2, 513})
        self.assertFalse(psio.validate_pssz(Set[u16], bytes.fromhex("01020200")))
        self.assertEqual(psio.to_pssz(None, Optional[u32]), b"")
        self.assertEqual(psio.to_pssz(5, Optional[u32]).hex(), "05000000")
        self.assertEqual(psio.to_pssz(None, Optional[str]), b"\x00")
        self.assertEqual(psio.to_pssz("", Optional[str]), b"\x01")

    def test_enums(self) -> None:
        self.assertEqual(psio.to_pssz(Level.HIGH), b"\x01")
        self.assertIs(psio.from_pssz(Level, b"\x00"), Level.LOW)
        self.assertEqual(psio.to_json(Level.HIGH), '"HIGH"')
        self.assertIs(psio.from_json(Level, '"LOW"'), Level.LOW)
        self.assertIs(psio.from_json(Level, "1"), Level.HIGH)
        # Without `other` an undeclared value is not a value of the Python type.
        self.assertIn("not a declared enumerator", self.rejects(lambda: psio.from_pssz(Level, b"\x09")).reason)
        self.rejects(lambda: psio.to_pssz(3, Level), psio.EncodeError)
        # With it the value is carried as an int, and a declared value has one representation.
        self.assertIs(psio.from_pssz(Code, (-2).to_bytes(4, "little", signed=True)), Code.NEG)
        self.assertEqual(psio.from_pssz(Code, (9).to_bytes(4, "little")), 9)
        self.assertEqual(psio.to_pssz(9, Code), (9).to_bytes(4, "little"))
        self.assertEqual(psio.to_json(9, Code), "9")
        ir = psio.schema_of(Code).ir["types"][0]["type"]["value"]
        self.assertEqual(ir.type, "Enum")
        self.assertEqual(ir.value["members"], [{"name": "NEG", "value": (1 << 64) - 2}, {"name": "POS", "value": 7}])

        class NoWidth(enum.Enum):
            A = 1
        self.assertIn("@psio.enum", self.rejects(lambda: psio.codec(NoWidth), psio.SchemaError).reason)

    def test_time(self) -> None:
        T = psio.TimePointMs
        when = datetime(2026, 11, 22, 10, 15, 30, 123000, tzinfo=timezone.utc)
        data = psio.to_pssz(when, T)
        self.assertEqual(int.from_bytes(data, "little", signed=True), 1795342530123)
        self.assertEqual(psio.from_pssz(T, data), when)
        self.assertEqual(psio.to_json(when, T), '"2026-11-22T10:15:30.123Z"')
        self.assertEqual(psio.from_json(T, '"2026-11-22T11:15:30.123+01:00"'), when)
        # A naive datetime is UTC; a value the unit cannot hold exactly is refused, never rounded.
        self.assertEqual(psio.to_pssz(when.replace(tzinfo=None), T), data)
        self.rejects(lambda: psio.to_pssz(when.replace(microsecond=123456), T), psio.EncodeError)
        self.assertEqual(psio.to_pssz(timedelta(milliseconds=1500), psio.DurationMs), (1500).to_bytes(8, "little"))
        self.assertEqual(psio.to_json(timedelta(seconds=2), psio.DurationSec), "2")
        # Nanoseconds are int ticks (datetime holds microseconds).
        self.assertEqual(psio.to_json(1795342530123456789, psio.TimePointNs), '"2026-11-22T10:15:30.123456789Z"')
        narrow = Annotated[datetime, psio.time_point("seconds", ticks=u32)]
        self.assertEqual(len(psio.to_pssz(when.replace(microsecond=0), narrow)), 4)
        self.assertEqual(psio.schema_of(narrow).ir["types"][0]["type"]["value"].value["id"], "time-point-seconds")
        self.assertIn("unit", self.rejects(lambda: psio.codec(datetime), psio.SchemaError).reason)

    def test_declaration_errors(self) -> None:
        @dataclass
        class Bare:
            n: int
        self.assertIn("int needs a width", self.rejects(lambda: psio.codec(Bare), psio.SchemaError).reason)

        @dataclass
        class BadBound:
            n: Annotated[u32, psio.max(3)]
        self.assertIn("length bound applies", self.rejects(lambda: psio.codec(BadBound), psio.SchemaError).reason)

        @dataclass
        class BadPosition:
            n: Annotated[List[u32], psio.keys(psio.max(3))]
        self.assertIn("keys / values", self.rejects(lambda: psio.codec(BadPosition), psio.SchemaError).reason)
        self.rejects(lambda: psio.codec(Union[Annotated[u32, psio.alt("a")], Annotated[u64, psio.alt("a")]]),
                     psio.SchemaError)   # alternatives need distinct names
        # Alternatives of one Python class are told apart by name.
        both = Union[u32, u64]
        self.assertEqual(psio.to_pssz(psio.Variant("u64", 1), both).hex(), "010100000000000000")
        self.assertEqual(psio.from_pssz(both, bytes.fromhex("0001000000")), psio.Variant("u32", 1))
        self.rejects(lambda: psio.to_pssz(1, both), psio.EncodeError)
        self.rejects(lambda: psio.codec(typing.Any), psio.SchemaError)
        self.rejects(lambda: psio.to_pssz(object()), psio.SchemaError)

    def test_wrong_values_are_encode_errors(self) -> None:
        for value in (Tree("x"), Tree(-1), Tree(1, [5]), Tree(1 << 32), "tree", None):  # type: ignore[arg-type, list-item]
            self.rejects(lambda: psio.to_pssz(value, Tree), psio.EncodeError)
        self.rejects(lambda: psio.to_pssz("\ud800", str), psio.EncodeError)

    def test_bounds_at_every_depth(self) -> None:
        @dataclass
        class Book:
            index: Annotated[Dict[str, List[u32]], psio.max(2), psio.keys(psio.max(4)), psio.values(psio.max(2))]
            pair: Annotated[Tuple[str, Optional[bytes]], psio.at(0, psio.max(3)), psio.at(1, psio.max(2))]
            names: Annotated[Set[str], psio.max(2), psio.element(psio.max(3))]

        t = psio.codec(Book)
        self.assertIsNotNone(t.inferred_bound)
        ok = Book({"ab": [1, 2]}, ("abc", b"xy"), {"a", "b"})
        self.assertEqual(psio.from_pssz(Book, psio.to_pssz(ok)), ok)
        loose = psio.codec(dataclasses.make_dataclass("Book", [
            ("index", Dict[str, List[u32]]), ("pair", Tuple[str, Optional[bytes]]), ("names", Set[str])]), width=4)
        for bad in (dataclasses.replace(ok, index={"a": [], "b": [], "c": []}),
                    dataclasses.replace(ok, index={"abcde": []}),
                    dataclasses.replace(ok, index={"a": [1, 2, 3]}),
                    dataclasses.replace(ok, pair=("abcd", None)),
                    dataclasses.replace(ok, pair=("a", b"xyz")),
                    dataclasses.replace(ok, names={"a", "b", "c"}),
                    dataclasses.replace(ok, names={"abcd"})):
            # Every writer refuses the value ...
            for write in (psio.to_pssz, psio.to_json, psio.to_ijson, psio.to_key):
                self.assertEqual(self.rejects(lambda: write(bad)).kind, "value", f"{write.__name__}: {bad}")
            # ... and every reader a frame / text / key that holds it.
            frame = loose.to_pssz(bad)
            wide = psio.codec(Book, width=4)
            self.assertFalse(wide.validate_pssz(frame), bad)
            # (the trusted decode does not check bounds: it reads the value the frame holds)
            self.assertEqual(wide.from_pssz(psio.trusted(frame)), bad)
            self.assertFalse(psio.validate_json(Book, loose.to_json(bad)), bad)
            self.assertFalse(psio.validate_key(Book, loose.to_key(bad)), bad)

    def test_field_metadata_and_names(self) -> None:
        @dataclass
        class Row:
            class_: u8
            tags: List[str] = field(default_factory=list, metadata={"psio": [psio.max(2), psio.element(psio.max(4))]})
            odd: Annotated[u8, psio.name("odd name")] = 0

        self.assertEqual(psio.to_json(Row(1, ["a"], 2)), '{"class":1,"tags":["a"],"odd name":2}')
        self.assertEqual(psio.from_json(Row, '{"class":1,"tags":["a"],"odd name":2}'), Row(1, ["a"], 2))
        self.assertIsNotNone(psio.codec(Row).inferred_bound)
        self.rejects(lambda: psio.to_pssz(Row(1, ["a", "b", "c"])), psio.EncodeError)

    def test_custom_by_annotation(self) -> None:
        Even = Annotated[u32, psio.custom("Even", check=lambda v: None if v % 2 == 0 else "Even: odd")]
        self.assertEqual(psio.to_pssz(4, Even), psio.to_pssz(4, u32))
        self.assertEqual(self.rejects(lambda: psio.to_pssz(3, Even)).reason, "Even: odd")
        st = psio.validate_pssz(Even, psio.to_pssz(3, u32))
        self.assertTrue(not st.ok and st.error.invariant and st.reason == "Even: odd")
        self.assertEqual(psio.from_pssz(Even, psio.trusted(psio.to_pssz(3, u32))), 3)
        node = psio.schema_of(Even).ir["types"][0]["type"]["value"]
        self.assertEqual((node.type, node.value["id"]), ("Custom", "Even"))

    def test_nested_and_deferred(self) -> None:
        @dataclass
        class Action:
            method: u32
            args: Annotated[Pair, psio.nested()]

        @dataclass
        class Relay:
            method: u32
            args: Annotated[bytes, psio.nested(Pair, deferred=True)]

        data = psio.to_pssz(Action(1, Pair(2, 3)))
        self.assertEqual(psio.from_pssz(Relay, data), Relay(1, b"\x02\x03"))
        self.assertEqual(psio.to_pssz(Relay(1, b"\x02\x03")), data)
        self.assertEqual(psio.schema_of(Action).to_json(), psio.schema_of(Relay).to_json().replace("Relay", "Action"))
        # The typed member is validated as its type; the deferred one is not looked at.
        garbage = psio.to_pssz(Relay(1, b"\x02\x03\x04"))
        self.assertFalse(psio.validate_pssz(Action, garbage))
        self.assertTrue(psio.validate_pssz(Relay, garbage))
        self.assertEqual(psio.to_json(Action(1, Pair(2, 3))), '{"method":1,"args":{"a":2,"b":3}}')

    def test_schema_of_matches_a_schema_written_by_hand(self) -> None:
        @dataclass
        class Record:
            id: u32
            name: RawString
            values: List[u32]
            active: bool

        from psio import ir
        by_hand = psio.Schema({"Record": {"Object": {"id": ir.U32, "name": ir.RAW_STRING,
                                                     "values": ir.list_of(ir.U32), "active": ir.BOOL}}})
        value = Record(42, b"Alice", [1, 256, 65536], True)
        self.assertEqual(psio.to_pssz(value),
                         by_hand.type("Record").to_pssz(dataclasses.asdict(value)))
        self.assertEqual(psio.schema_of(Record).names(), ["Record", "@1", "@2", "@3", "@4", "@5"])


if __name__ == "__main__":
    unittest.main()
