"""Any JSON value (the Custom id ``json``) and untagged variants (the Variant attribute ``untagged``).

The cases of ``cpp/tests/json_api_tests.cpp`` ("json_value: ...", "json: an untagged variant ...",
"json: untagged variants in a record, in PSSZ and through the schema"), typed and schema-driven
from hand-written IR; then the two wire rules the spec restates (4.1: a bool is 0 or 1; 7.1: only
optional fields are appended).
"""

from __future__ import annotations

import unittest
from dataclasses import dataclass
from typing import Annotated, Any, Dict, List, Optional, Union

from support import fixture  # noqa: F401  (puts src/ on the path)

import psio
from psio import JsonNumber, JsonValue, i32, ir, u8, u32, u64

FORMATS = ("json", "ijson")


def write(value: Any, T: Any, fmt: str) -> str:
    return psio.to_ijson(value, T) if fmt == "ijson" else psio.to_json(value, T)


def read(T: Any, text: Any, fmt: str) -> Any:
    return psio.from_ijson(T, text) if fmt == "ijson" else psio.from_json(T, text)


def refused(case: unittest.TestCase, run: Any, kind: Any = psio.PsioError) -> psio.PsioError:
    with case.assertRaises(kind) as raised:
        run()
    case.assertNotEqual(raised.exception.kind, "internal")
    return raised.exception  # type: ignore[no-any-return]


# ── japi:: of cpp/tests/json_api_tests.cpp ──────────────────────────────────

Extra = Annotated[Union[str, Annotated[List[str], psio.alt("vector")]], psio.untagged]


@dataclass
class Point:
    x: i32
    y: i32


Mixed = Annotated[Union[bool, i32, str, Annotated[List[i32], psio.alt("vector")], Point], psio.untagged]


@dataclass
class Config:
    extra: Dict[str, Extra]
    one: Optional[Extra]
    mixed: List[Mixed]


@dataclass
class Pretty:
    service: str
    data: JsonValue
    more: Optional[JsonValue]
    list: List[JsonValue]


@dataclass
class PrettyText:
    service: str
    data: str
    more: Optional[str]
    list: List[str]


# The same types as hand-written IR (what C++ SchemaBuilder builds: docs/schema-builder.md).
UNTAGGED = ir.attribute("untagged", "")
EXTRA_IR = {"Variant": {"members": [ir.member("string", ir.STRING), ir.member("vector", ir.list_of(ir.STRING))],
                        "attributes": [UNTAGGED]}}
SCHEMA = psio.Schema({
    "Point": {"Object": {"x": ir.I32, "y": ir.I32}},
    "Extra": EXTRA_IR,
    "Tagged": {"Variant": {"string": ir.STRING, "vector": ir.list_of(ir.STRING)}},
    "Mixed": {"Variant": {"members": [
        ir.member("bool", ir.BOOL), ir.member("i32", ir.I32), ir.member("string", ir.STRING),
        ir.member("vector", ir.list_of(ir.I32)), ir.member("Point", "Point")], "attributes": [UNTAGGED]}},
    "Config": {"Object": {"extra": ir.custom_of(ir.list_of(ir.tuple_of(ir.STRING, "Extra")), "map"),
                          "one": ir.option_of("Extra"), "mixed": ir.list_of("Mixed")}},
    "Pretty": {"Object": {"service": ir.STRING, "data": ir.JSON, "more": ir.option_of(ir.JSON),
                          "list": ir.list_of(ir.JSON)}},
    "Any": ir.JSON,
    "Ambiguous": {"Variant": {"members": [ir.member("0", ir.U32), ir.member("1", ir.F64)],
                              "attributes": [UNTAGGED]}},
})


class JsonValues(unittest.TestCase):
    def test_a_document_read_and_written_again_is_the_same_text(self) -> None:
        # Numbers keep their text: nothing is rounded, renormalised or widened.
        text = r'{"a":1.50,"b":"x\n","c":[1,2,{"d":null}],"big":18446744073709551616,"e":1e3,"neg":-0,"z":true,"o":{}}'
        dynamic = SCHEMA.type("Any")
        for fmt in FORMATS:
            for get, put in ((lambda t: read(JsonValue, t, fmt), lambda v: write(v, JsonValue, fmt)),
                             (lambda t: dynamic.from_json(t, fmt), lambda v: dynamic.to_json(v, fmt))):
                v = get(text)
                self.assertEqual(put(v), text)
                self.assertEqual(len(v), 8)
                self.assertEqual((type(v["a"]), v["a"].text, v["a"]), (JsonNumber, "1.50", 1.5))
                self.assertEqual((type(v["big"]), v["big"]), (int, 18446744073709551616))
                self.assertEqual((v["e"], v["e"].text, v["neg"].text), (1000.0, "1e3", "-0"))
                self.assertEqual((v["b"], v["c"][1], v["c"][2], v["z"], v["o"]), ("x\n", 2, {"d": None}, True, {}))
        self.assertEqual(psio.from_ijson(JsonValue, ' [ 1 , "a" ] '), [1, "a"])
        # Scalars at the root.
        self.assertEqual(psio.to_ijson(psio.from_ijson(JsonValue, "null"), JsonValue), "null")
        self.assertEqual(psio.to_ijson(psio.from_ijson(JsonValue, "-12.5e-3"), JsonValue), "-12.5e-3")
        # Not JSON: an error, on every reading entry point.
        for bad in ('{"a":1,}', "[1 2]", "", "01", '{"a":1}x', "nul"):
            refused(self, lambda: psio.from_ijson(JsonValue, bad), psio.JsonError)
            self.assertFalse(psio.validate_json(JsonValue, bad).ok, bad)
            self.assertFalse(dynamic.validate_ijson(bad).ok, bad)
        # Nesting is bounded like every reader's.
        refused(self, lambda: psio.from_json(JsonValue, "[" * 65), psio.JsonError)
        self.assertEqual(psio.from_json(JsonValue, "[" * 64 + "]" * 64), eval("[" * 64 + "]" * 64))

    def test_a_repeated_key_keeps_its_last_value_at_the_first_position(self) -> None:
        v = psio.from_ijson(JsonValue, '{"a":1,"b":2,"a":{"x":[]},"b":3}')
        self.assertEqual(psio.to_ijson(v, JsonValue), '{"a":{"x":[]},"b":3}')
        self.assertEqual(list(v), ["a", "b"])

    def test_built_and_edited_in_code(self) -> None:
        v: Dict[str, Any] = {}
        v["service"] = "tokens"
        v["amount"] = 18446744073709551615
        v["ratio"] = 0.25
        v["ok"] = True
        v["none"] = None
        v["list"] = [1, -2, "three"]
        v["amount"] = 5        # replaced where it is
        self.assertEqual(psio.to_ijson(v, JsonValue),
                         '{"service":"tokens","amount":5,"ratio":0.25,"ok":true,"none":null,"list":[1,-2,"three"]}')
        # A number is its text; a JSON value does not know its width, so ijson writes it as it is.
        self.assertEqual(psio.to_ijson(18446744073709551615, JsonValue), "18446744073709551615")
        self.assertEqual(psio.to_json((1, 2.5, JsonNumber("1.50")), JsonValue), "[1,2.5,1.50]")
        # A value with no JSON text is an error, not output.
        e = refused(self, lambda: psio.to_ijson(JsonNumber("0x10"), JsonValue), psio.EncodeError)
        self.assertEqual(e.reason, "json: a json_value number is not a JSON number")
        for bad in (float("inf"), float("nan"), "\udcff", {1: 2}, b"bytes", {"a": object()}):
            refused(self, lambda: psio.to_json(bad, JsonValue), psio.EncodeError)
            refused(self, lambda: psio.to_pssz(bad, JsonValue), psio.EncodeError)
        deep: Any = []
        for _ in range(64):
            deep = [deep]
        refused(self, lambda: psio.to_json(deep, JsonValue), psio.EncodeError)
        # The I-JSON string rule is the only difference between the formats.
        nonchar = "￿"
        self.assertEqual(psio.to_json(nonchar, JsonValue), '"￿"')
        refused(self, lambda: psio.to_ijson(nonchar, JsonValue), psio.EncodeError)
        refused(self, lambda: psio.from_ijson(JsonValue, '"￿"'), psio.JsonError)
        self.assertEqual(psio.from_json(JsonValue, '"￿"'), nonchar)

    def test_a_member_of_a_record_in_text_and_in_pssz(self) -> None:
        text = '{"service":"tokens","data":{"amount":"5","memo":[1.0,null]},"more":7,"list":[true,"x",{}]}'
        p = psio.from_ijson(Pretty, text)
        self.assertEqual(psio.to_ijson(p), text)
        self.assertEqual(psio.to_json(p), text)
        self.assertEqual(p.data["amount"], "5")
        self.assertEqual((p.more, p.list), (7, [True, "x", {}]))
        # A missing optional member is none; a missing JSON value is an error (null is a value: it
        # has to be written).
        q = psio.from_ijson(Pretty, '{"service":"s","data":null,"list":[]}')
        self.assertEqual((q.data, q.more), (None, None))
        e = refused(self, lambda: psio.from_ijson(Pretty, '{"service":"s","list":[]}'), psio.JsonError)
        self.assertEqual(e.reason, 'json: missing required member "data" of Pretty')

        # PSSZ: each JSON value is a UTF-8 string holding its compact text.
        twin = PrettyText("tokens", '{"amount":"5","memo":[1.0,null]}', "7", ["true", '"x"', "{}"])
        data = psio.to_pssz(p)
        self.assertEqual(data, psio.to_pssz(twin))
        self.assertTrue(psio.validate_pssz(Pretty, data))
        self.assertEqual(psio.to_ijson(psio.from_pssz(Pretty, data)), text)
        self.assertEqual(psio.from_pssz(Pretty, psio.trusted(data)), p)
        self.assertEqual(psio.pssz_view(Pretty, data).data, {"amount": "5", "memo": [1.0, None]})
        # Bytes that are not JSON text are not a value; text that is JSON but not compact is a value
        # with another, canonical, encoding.
        bad = psio.to_pssz(PrettyText("tokens", "{not json", "7", []))
        status = psio.validate_pssz(Pretty, bad)
        self.assertEqual((status.ok, status.reason), (False, "pssz IR: the text of a json value is not JSON"))
        self.assertFalse(psio.validate_pssz(Pretty, psio.to_pssz(PrettyText("tokens", "", None, []))))
        loose = psio.to_pssz(PrettyText("tokens", '{ "amount" : "5" }', "7", ["true", '"x"', "{}"]))
        self.assertTrue(psio.validate_pssz(Pretty, loose))
        self.assertNotEqual(psio.to_pssz(psio.from_pssz(Pretty, loose)), loose)

        # The schema: Custom "json" over a UTF-8 string; and the schema-driven codecs agree with the
        # typed ones on bytes and on text.
        built = psio.schema_of(Pretty)
        member = built.ir["types"][[m["name"] for m in built.ir["types"]].index("Pretty")]["type"]["value"].value[
            "members"][1]["type"]
        while member["value"].type == "Type":
            member = next(t["type"] for t in built.ir["types"] if t["name"] == member["value"].value["type"])
        self.assertEqual((member["value"].type, member["value"].value["id"]), ("Custom", "json"))
        inner = member["value"].value["type"]["value"]
        self.assertEqual((inner.type, inner.value["id"], inner.value["type"]["value"].type), ("Custom", "utf8", "List"))
        for schema, root in ((SCHEMA, "Pretty"), (psio.Schema.from_pssz(built.to_pssz()), "Pretty")):
            t = schema.type(root)
            for fmt in FORMATS:
                self.assertEqual(t.pssz_to_json(data, fmt), text)
                self.assertEqual(t.json_to_pssz(text, fmt), data)
            self.assertTrue(t.validate_pssz(data))
            self.assertFalse(t.validate_pssz(bad))
            # Loose text in the bytes is presented compact.
            self.assertEqual(t.pssz_to_json(loose),
                             '{"service":"tokens","data":{"amount":"5"},"more":7,"list":[true,"x",{}]}')
            # The dynamic value is the JSON value.
            self.assertEqual(t.from_pssz(data)["data"], {"amount": "5", "memo": [1.0, None]})
        # Number tokens survive bytes -> text -> bytes.
        t = SCHEMA.type("Any")
        for token in ("1.50", "1e3", "-0", "18446744073709551616", "-12.5e-3", "0.1e-999", "1E400"):
            frame = psio.to_pssz(token)
            self.assertEqual(t.pssz_to_json(frame), token)
            self.assertEqual(t.json_to_pssz(f" {token} "), frame)
            self.assertEqual(t.to_key(t.from_pssz(frame)), psio.to_key(token))
            self.assertEqual(t.from_key(psio.to_key(token)), t.from_pssz(frame))

    def test_json_is_a_reserved_id(self) -> None:
        # No handler may claim the id; no custom type may take it; it sits on a UTF-8 string only.
        e = refused(self, lambda: psio.Schema({"T": ir.JSON}, handlers={"json": psio.Handler(to_text=str)}),
                    psio.SchemaError)
        self.assertIn('"json"', e.reason)
        refused(self, lambda: psio.codec(Annotated[str, psio.custom("json")]), psio.SchemaError)

        def named() -> Any:
            @psio.record(custom="json")
            @dataclass
            class Claims:
                a: u8
            return psio.codec(Claims)
        refused(self, named, psio.SchemaError)
        for wrong in (ir.custom_of(ir.RAW_STRING, "json"), ir.custom_of(ir.BYTES, "json"), ir.custom_of(ir.U8, "json"),
                      ir.custom_of(ir.custom_of(ir.STRING, "Name"), "json")):
            e = refused(self, lambda: psio.Schema({"T": wrong}).type("T"), psio.SchemaError)
            self.assertEqual(e.reason, "pssz IR: a json value must sit directly on a UTF-8 string")
        # Through a named alias, and bounded.
        schema = psio.Schema({"Text": ir.custom_of(ir.bounded_list_of(ir.U8, 8), "utf8"), "T": ir.custom_of("Text", "json")})
        t = schema.type("T")
        self.assertEqual(t.json_to_pssz('{"a": 1}'), b'{"a":1}')
        refused(self, lambda: t.json_to_pssz('{"a": 100}'), psio.PsioError)
        # A JSON value in a contract: the id is compared (the bytes are a UTF-8 string's).
        self.assertTrue(psio.equivalent(JsonValue, str, mode="wire"))
        self.assertEqual(psio.equivalent(JsonValue, str).code, "custom-id-changed")


class Untagged(unittest.TestCase):
    def test_an_untagged_variant_is_the_bare_value_of_its_alternative(self) -> None:
        dynamic = SCHEMA.type("Extra")
        self.assertEqual(psio.to_ijson(["a"], Extra), '["a"]')
        self.assertEqual(psio.to_ijson("s", Extra), '"s"')
        self.assertEqual(psio.to_json(["a", "b"], Extra), '["a","b"]')
        self.assertEqual(psio.to_json("s", Extra), '"s"')
        self.assertEqual(psio.from_ijson(Extra, '["a"]'), ["a"])
        self.assertEqual(psio.from_json(Extra, ' "x" '), "x")
        self.assertEqual(psio.from_ijson(Extra, "[]"), [])
        self.assertEqual(dynamic.from_ijson('["a"]'), psio.Variant("vector", ["a"]))
        self.assertEqual(dynamic.to_json(psio.Variant("string", "s")), '"s"')
        # A value of no alternative's kind; a value of the right kind that is not the alternative.
        for get in (lambda text, fmt: read(Extra, text, fmt), lambda text, fmt: dynamic.from_json(text, fmt)):
            e = refused(self, lambda: get("7", "ijson"), psio.JsonError)
            self.assertEqual(e.reason, "json: the value is none of the untagged variant's alternatives")
            refused(self, lambda: get("[1]", "ijson"), psio.JsonError)
            refused(self, lambda: get('{"string":"x"}', "ijson"), psio.JsonError)
            refused(self, lambda: get('[0,"x"]', "json"), psio.JsonError)
        # Five kinds, five alternatives.
        everything = [True, 7, "s", [1, 2], Point(3, 4)]
        text = '[true,7,"s",[1,2],{"x":3,"y":4}]'
        for fmt in FORMATS:
            self.assertEqual(write(everything, List[Mixed], fmt), text)
            self.assertEqual(read(List[Mixed], text, fmt), everything)
            self.assertEqual(SCHEMA.type(ir.list_of("Mixed")).json_to_pssz(text, fmt), psio.to_pssz(everything, List[Mixed]))
        refused(self, lambda: psio.from_ijson(Mixed, "null"), psio.JsonError)
        refused(self, lambda: psio.from_ijson(Mixed, "1.5"), psio.JsonError)     # a number: the i32

    def test_in_a_record_in_pssz_and_through_the_schema(self) -> None:
        value = Config({"hosts": ["a", "b"], "name": "n"}, "one", [Point(1, 2), False])
        text = '{"extra":{"hosts":["a","b"],"name":"n"},"one":"one","mixed":[{"x":1,"y":2},false]}'
        for fmt in FORMATS:
            self.assertEqual(write(value, Config, fmt), text)
            self.assertEqual(read(Config, text, fmt), value)
        # An optional untagged variant: null is none.
        self.assertIsNone(psio.from_ijson(Config, '{"extra":{},"one":null,"mixed":[]}').one)
        # Binary forms are untouched: the selector byte, then the alternative.
        one = psio.to_pssz(["a"], Extra)
        self.assertEqual(one[0], 1)
        self.assertEqual(one, psio.to_pssz(["a"], Union[str, Annotated[List[str], psio.alt("vector")]]))  # type: ignore[arg-type]
        # The schema says `untagged`, and the schema-driven codecs follow.
        built = psio.schema_of(Config)
        untagged = sum(1 for m in built.ir["types"] if m["type"]["value"].type == "Variant"
                       and m["type"]["value"].value["attributes"] == [{"name": "untagged", "value": ""}])
        self.assertEqual(untagged, 2)
        data = psio.to_pssz(value)
        for schema in (psio.Schema.from_pssz(built.to_pssz()), SCHEMA):
            t = schema.type("Config")
            for fmt in FORMATS:
                self.assertEqual(t.pssz_to_json(data, fmt), text)
                self.assertEqual(t.json_to_pssz(text, fmt), data)
            e = refused(self, lambda: t.json_to_pssz('{"extra":{"k":7},"one":null,"mixed":[]}'), psio.JsonError)
            self.assertEqual(e.reason, "json: the value is none of the untagged variant's alternatives")
        # A schema whose alternatives cannot be told apart is refused where a value is read.
        ambiguous = SCHEMA.type("Ambiguous")
        self.assertEqual(ambiguous.to_pssz(psio.Variant("0", 1)), b"\x00\x01\x00\x00\x00")
        e = refused(self, lambda: ambiguous.json_to_pssz("1"), psio.JsonError)
        self.assertEqual(e.reason,
                         "json: the alternatives of an untagged variant are not told apart by the kind of JSON value")
        # Whether a variant is untagged is part of its text contract.
        self.assertTrue(psio.equivalent(SCHEMA.type("Tagged"), SCHEMA.type("Extra"), mode="wire"))
        status = psio.equivalent(SCHEMA.type("Tagged"), SCHEMA.type("Extra"))
        self.assertEqual((status.code, status.path), ("text-form-changed", "(root)"))
        self.assertEqual(psio.evolves_from(SCHEMA.type("Extra"), SCHEMA.type("Tagged")).code, "text-form-changed")

    def test_a_declaration_whose_alternatives_overlap_is_refused(self) -> None:
        # (C++: these do not compile - cpp/tests/json_untagged_ambiguous_fail.cpp)
        for a, b in ((i32, psio.f64), (u64, str), (str, bytes), (Optional[u8], Optional[str]),
                     (Point, Dict[str, u8]), (str, psio.TimePointSec), (str, JsonValue)):
            hint = Annotated[Union[Annotated[a, psio.alt("a")], Annotated[b, psio.alt("b")]], psio.untagged]  # type: ignore[valid-type]
            e = refused(self, lambda: psio.codec(hint), psio.SchemaError)
            self.assertIn("must be told apart by the kind of JSON value", e.reason)
        # A custom type with a text form is a string; without one it is its wire type.
        Name = Annotated[u32, psio.custom("Name", to_text=str, from_text=int)]
        Count = Annotated[u32, psio.custom("Count", check=lambda v: None)]
        named = Annotated[Union[Annotated[Name, psio.alt("name")], Annotated[u8, psio.alt("n")]], psio.untagged]
        self.assertEqual(psio.to_json(psio.Variant("name", 7), named), '"7"')
        self.assertEqual(psio.from_json(named, '"7"'), psio.Variant("name", 7))
        self.assertEqual(psio.from_json(named, "7"), psio.Variant("n", 7))
        refused(self, lambda: psio.codec(Annotated[Union[Annotated[Count, psio.alt("c")], Annotated[u8, psio.alt("n")]],
                                                   psio.untagged]), psio.SchemaError)
        refused(self, lambda: psio.codec(Annotated[Optional[u8], psio.untagged]), psio.SchemaError)
        # Nested untagged unions are the union of their alternatives' kinds.
        inner = Annotated[Union[bool, str], psio.untagged]
        outer = Annotated[Union[Annotated[inner, psio.alt("inner")], Annotated[List[u8], psio.alt("list")]], psio.untagged]
        self.assertEqual(psio.to_json(psio.from_json(outer, "true"), outer), "true")
        self.assertEqual(psio.to_json(psio.from_json(outer, "[1]"), outer), "[1]")


class SpecFixes(unittest.TestCase):
    def test_a_bool_is_exactly_0_or_1(self) -> None:
        """docs/pssz-spec.md 4.1."""
        @dataclass
        class Flag:
            on: bool
            n: u8
        self.assertEqual((psio.to_pssz(False, bool), psio.to_pssz(True, bool)), (b"\x00", b"\x01"))
        dynamic = psio.Schema.from_pssz(psio.schema_of(Flag).to_pssz()).type("Flag")
        for byte in range(2, 256):
            self.assertFalse(psio.validate_pssz(bool, bytes([byte])), byte)
        for byte in (2, 3, 0x80, 0xFF):
            frame = psio.to_pssz(Flag(True, 7)).replace(b"\x01", bytes([byte]), 1)
            for status in (psio.validate_pssz(Flag, frame), dynamic.validate_pssz(frame)):
                self.assertFalse(status, byte)
            refused(self, lambda: psio.from_pssz(Flag, frame), psio.DecodeError)
            refused(self, lambda: psio.from_pssz(List[bool], bytes([1, byte])), psio.DecodeError)
        # A value that is not a bool is not written as one.
        for value in (1, 0, 2, "true", None):
            refused(self, lambda: psio.to_pssz(value, bool), psio.EncodeError)

    def test_only_optional_fields_may_be_appended(self) -> None:
        """docs/pssz-spec.md 7.1: a field an old writer did not write is an absent optional."""
        @dataclass
        class V1:
            a: u32

        @dataclass
        class V2:
            a: u32
            b: Optional[u32] = None

        V2Required = dataclass(type("V2", (), {"__annotations__": {"a": u32, "b": u32}}))
        old = psio.to_pssz(V1(7))
        self.assertEqual(psio.from_pssz(V2, old), V2(7, None))
        refused(self, lambda: psio.from_pssz(V2Required, old), psio.DecodeError)
        self.assertFalse(psio.validate_pssz(V2Required, old))
        self.assertTrue(psio.evolves_from(V1, V2))
        for mode in ("wire", "contract"):
            status = psio.evolves_from(V1, V2Required, mode=mode)
            self.assertEqual((status.ok, status.code), (False, "appended-field-not-optional"))
        # An old reader reads the new value it knows and reports the rest as unknown.
        status = psio.validate_pssz(V1, psio.to_pssz(V2(7, 9)))
        self.assertTrue(status.ok and status.has_unknown)
        self.assertEqual(psio.from_pssz(V1, psio.to_pssz(V2(7, None))), V1(7))


if __name__ == "__main__":
    unittest.main()
