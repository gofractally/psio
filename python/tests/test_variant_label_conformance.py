"""conformance/variant-label-vectors.json: the label of every alternative of a variant.

An alternative has one label — its name in the Schema IR and the key of its I-JSON form — and every
language derives it by one rule (docs/annotations.md "Variant alternative labels").  The five variants
are declared here in Python; the schema they build is the fixture's ``schema_ir`` byte for byte, and
every row holds on the typed path and on the schema-driven one (the fixture's schema and nothing else).
"""

from __future__ import annotations

import unittest
from dataclasses import dataclass
from typing import Annotated, Any, List, Optional, Tuple, Union

from support import fixture

import psio
from psio import RawString, f32, f64, i16, i64, u8, u16, u32, u64

FORMATS = ("json", "ijson")


@dataclass
class Circle:
    radius: u32


@dataclass
class Square:
    side: u32


# Rule 2, classes: the class's name.
Records = Union[Circle, Square]
# Rule 2, types without a declared name: the table.  (A C++ std::string is a raw string.)
Scalars = Union[bool, u8, i16, u32, i64, u64, f32, f64, RawString]
# Rule 2, generic types: built from the names of what they hold.  (typing flattens an Optional written
# directly in a Union: Annotated keeps it one alternative.)
Generic = Union[List[u32], Annotated[Optional[u32], "one alternative"], Tuple[u32, RawString],
                Annotated[List[u16], psio.array(2)], List[Circle], bytes]
# Rule 3: a type name two alternatives share is no label; they are labelled by position.  (typing drops
# a repeated member of a Union: Annotated keeps the second.)
Repeated = Union[u32, RawString, Annotated[RawString, "the second string"], Circle]
# Stated labels.
Stated = Union[Annotated[Circle, psio.alt("circle")], Annotated[Square, psio.alt("square")],
               Annotated[u32, psio.alt("side")]]

DECLARED = {"Records": Records, "Scalars": Scalars, "Generic": Generic, "Repeated": Repeated, "Stated": Stated}


def alternative_names(schema: psio.Schema, name: str) -> List[Any]:
    node = next(t for t in schema.ir["types"] if t["name"] == name)["type"]
    while node["value"].type == "Type":
        target = node["value"].value["type"]
        node = next(t for t in schema.ir["types"] if t["name"] == target)["type"]
    return [(m["name"], m["attributes"]) for m in node["value"].value["members"]]


class VariantLabelFixture(unittest.TestCase):
    maxDiff = None

    def test_declarations_build_the_fixture_schema(self) -> None:
        fx = fixture("variant-label-vectors.json")
        self.assertEqual(fx["version"], 1)
        builder = psio.SchemaBuilder()
        for name, hint in DECLARED.items():
            builder.insert_named(name, psio.typed.shape_of(hint))
        schema = builder.schema()
        self.assertEqual(schema.to_pssz().hex(), fx["schema_ir"])
        self.assertEqual(schema.to_json("json"), fx["schema_json"])
        recorded = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        self.assertEqual(schema.ir, recorded.ir)
        recorded.validate()
        self.assertEqual([v["name"] for v in fx["variants"]], list(DECLARED))
        for row in fx["variants"]:
            # The schema names each alternative by its label and says nothing else about its name.
            self.assertEqual(alternative_names(schema, row["name"]), [(label, []) for label in row["labels"]])

    def test_every_row_typed_and_schema_driven(self) -> None:
        fx = fixture("variant-label-vectors.json")
        recorded = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        labels = {v["name"]: v["labels"] for v in fx["variants"]}
        count = 0
        for row in fx["vectors"]:
            name, data = row["name"], bytes.fromhex(row["pssz"])
            self.assertEqual(labels[row["type"]][row["index"]], row["label"], name)
            # The tagged text form is keyed by the label; binary is the position.
            self.assertTrue(row["ijson"].startswith('{' + psio.to_json(row["label"], str) + ':'), name)
            self.assertEqual(data[0], row["index"], name)
            for path, t in (("typed", psio.codec(DECLARED[row["type"]])), ("schema", recorded.type(row["type"]))):
                where = f"{name} ({path})"
                st = t.validate_pssz(data, strict=True)
                self.assertTrue(st.ok and not st.has_unknown, f"{where}: {st.reason}")
                value = t.from_pssz(data)
                self.assertEqual(t.to_pssz(value).hex(), row["pssz"], where)
                for fmt in FORMATS:
                    self.assertEqual(t.to_json(value, fmt), row[fmt], f"{where}: {fmt}")
                    self.assertEqual(t.pssz_to_json(data, fmt), row[fmt], f"{where}: {fmt}")
                    self.assertEqual(t.json_to_pssz(row[fmt], fmt).hex(), row["pssz"], f"{where}: {fmt}")
                    self.assertEqual(t.to_pssz(t.from_json(row[fmt], fmt)).hex(), row["pssz"], f"{where}: {fmt}")
                if path == "schema":
                    self.assertEqual((value.type, ), (row["label"], ), where)
            count += 1
        self.assertEqual(count, 25)
        print(f"variant-label-vectors.json: variants={len(fx['variants'])} vectors={count} (typed and schema-driven)")

    def test_the_rule(self) -> None:
        # Alternatives of one Python class are told apart by label.
        repeated = psio.codec(Repeated)
        self.assertEqual(repeated.to_pssz(psio.Variant("2", "b")).hex(), "0262")
        self.assertEqual(repeated.to_json(psio.Variant("1", "a"), "ijson"), '{"1":"a"}')
        self.assertEqual(repeated.from_json('{"uint32":1}', "ijson"), 1)
        with self.assertRaises(psio.JsonError):
            repeated.from_json('{"string":"a"}', "ijson")       # a shared type name is no label
        # A stated label is the key of the text and the name in the schema; the type's name is not read.
        stated = psio.codec(Stated)
        self.assertEqual(stated.to_json(Square(4), "ijson"), '{"square":{"side":4}}')
        self.assertEqual(stated.from_json('{"side":5}', "ijson"), 5)
        with self.assertRaises(psio.JsonError):
            stated.from_json('{"Square":{"side":4}}', "ijson")
        # Stated labels are nonempty and distinct; a stated label that a default one has too is refused.
        for bad in (lambda: psio.alt(""),
                    lambda: psio.codec(Union[Annotated[u32, psio.alt("a")], Annotated[u64, psio.alt("a")]]),
                    lambda: psio.codec(Union[Annotated[u32, psio.alt("uint64")], u64])):
            with self.assertRaises(psio.SchemaError):
                bad()
        # A type with no name is labelled by its position, as is every alternative of a repeated name.
        nameless = psio.schema_of(Union[Circle, psio.JsonValue])
        self.assertEqual([m["name"] for m in nameless.ir["types"][0]["type"]["value"].value["members"]]
                         if nameless.ir["types"][0]["type"]["value"].type == "Variant" else None,
                         ["Circle", "1"])
        # There is no second name: a schema that states one is not a schema.
        import json
        doc = json.loads(fixture("variant-label-vectors.json")["schema_json"])
        variant = next(t["type"]["value"] for t in doc["types"] if t["type"]["value"][0] == 6)
        variant[1]["members"][0]["attributes"].append({"name": "ijsonName", "value": "x"})
        with self.assertRaises(psio.SchemaError):
            psio.Schema.from_json(json.dumps(doc), "json").type("Records")

if __name__ == "__main__":
    unittest.main()
