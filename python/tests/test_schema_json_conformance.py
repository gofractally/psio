"""conformance/schema-json-vectors.json: the three serialized forms of the Schema IR."""

from __future__ import annotations

import unittest

from support import fixture

import psio
from psio import PsioError

FX = fixture("schema-json-vectors.json")
FORMATS = ("json", "ijson")
SCHEMAS, ACCEPTS, REJECTS = len(FX["schemas"]), len(FX["accepts"]), len(FX["rejects"])


class SchemaJson(unittest.TestCase):
    maxDiff = None

    def test_every_schema(self) -> None:
        self.assertEqual(FX["version"], 1)
        self.assertGreaterEqual(len(FX["schemas"]), 7)
        names = set()
        for row in FX["schemas"]:
            name = row["name"]
            self.assertNotIn(name, names)
            names.add(name)
            schema = psio.Schema.from_pssz(bytes.fromhex(row["ir_hex"]))
            self.assertEqual(schema.to_pssz().hex(), row["ir_hex"], f"{name}: PSSZ round trip")
            for fmt in FORMATS:
                text = row[fmt]
                self.assertEqual(schema.to_json(fmt), text, f"{name}: {fmt} text")
                # Text as a string and as its UTF-8 bytes.
                for given in (text, text.encode("utf-8")):
                    back = psio.Schema.from_json(given, fmt)
                    self.assertEqual(back.to_pssz().hex(), row["ir_hex"], f"{name}: bytes from {fmt} text")
                    self.assertEqual(back.ir, schema.ir, f"{name}: IR from {fmt} text")
        for name in ("pssz-vectors", "json-vectors", "time-key-vectors", "schema-of-schema", "every-alternative"):
            self.assertIn(name, names)

    def test_every_alternative(self) -> None:
        row = next(r for r in FX["schemas"] if r["name"] == "every-alternative")
        schema = psio.Schema.from_json(row["ijson"])
        any_type = next(m for m in psio.Schema.meta().ir["types"] if m["name"] == "AnyType")["type"]["value"]
        self.assertEqual(any_type.type, "Object")
        variant = any_type.value["members"][0]["type"]["value"]
        self.assertEqual(variant.type, "Variant")
        self.assertEqual(len({m["type"]["value"].type for m in schema.ir["types"]}), len(variant.value["members"]))
        self.assertEqual(len(variant.value["members"]), 15)
        self.assertIsNone(schema.ir["ir_version"])
        self.assertTrue(schema.ir["interfaces"] and schema.ir["worlds"] and schema.ir["uses"])
        self.assertTrue(schema.ir["package"]["attributes"])

        def length(name: str) -> int:
            t = next(m for m in schema.ir["types"] if m["name"] == name)["type"]["value"]
            self.assertEqual(t.type, "Array")
            return t.value["len"]
        # 64-bit integers are exact on both sides of 2^53 - 1.
        self.assertEqual(length("array-safe"), 9007199254740991)
        self.assertEqual(length("array-beyond-safe"), 9007199254740992)
        self.assertEqual(length("array-max"), 18446744073709551615)
        self.assertIn('"len":18446744073709551615', row["json"])
        self.assertIn('"len":"18446744073709551615"', row["ijson"])
        self.assertIn('"len":9007199254740991', row["ijson"])
        self.assertIn('"len":"9007199254740992"', row["ijson"])

    def test_accepts(self) -> None:
        self.assertGreaterEqual(len(FX["accepts"]), 18)
        for row in FX["accepts"]:
            self.assertEqual(psio.Schema.from_json(row["text"], row["format"]).to_pssz().hex(), row["ir_hex"], row["name"])

    def test_rejects(self) -> None:
        self.assertGreaterEqual(len(FX["rejects"]), 40)
        for row in FX["rejects"]:
            for given in (row["text"], row["text"].encode("utf-8")):
                with self.assertRaises(PsioError, msg=row["name"]) as caught:
                    psio.Schema.from_json(given, row["format"])
                self.assertEqual(caught.exception.kind, "json", f"{row['name']}: {caught.exception}")

    def test_counts(self) -> None:
        print(f"schema-json-vectors.json: schemas={SCHEMAS} accepts={ACCEPTS} rejects={REJECTS}")
        self.assertEqual((SCHEMAS, ACCEPTS, REJECTS), (7, 18, 48))


if __name__ == "__main__":
    unittest.main()
