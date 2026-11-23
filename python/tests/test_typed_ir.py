"""The typed layer produces the Schema IR of C++ ``psio::SchemaBuilder``.

For each shared fixture, every type the fixture's IR records is declared in
Python (``typed_fixture.py``), and ``psio.schema_of`` of those declarations,
inserted in the emitter's order, must encode to the fixture's ``schema_ir``
byte for byte.
"""

from __future__ import annotations

import unittest

from support import FIXTURE_HANDLERS, fixture
from typed_fixture import TypedFixture

import psio

FIXTURES = (("pssz-vectors.json", 638), ("json-vectors.json", 223), ("time-key-vectors.json", 140))


class TypedIr(unittest.TestCase):
    maxDiff = 2000

    def check(self, file: str, ir_types: int) -> None:
        fx = fixture(file)
        schema = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        typed = TypedFixture(schema.ir, FIXTURE_HANDLERS)
        # Every type of the fixture is declared in the typed layer.
        for row in fx["types"]:
            typed.hint(row["id"])
        names = typed.insertion_order()
        generated = typed.generate(names)
        for want, got in zip(schema.ir["types"], generated.ir["types"]):
            self.assertEqual(got, want, f"{file}: {want['name']}")
        self.assertEqual(generated.ir, schema.ir)
        self.assertEqual(generated.to_pssz().hex(), fx["schema_ir"])
        self.assertEqual(generated.to_json("ijson"), schema.to_json("ijson"))
        self.assertEqual(generated.to_json("json"), schema.to_json("json"))
        print(f"{file} typed IR: irTypes={len(schema.ir['types'])} inserted={len(names)} "
              f"fixtureTypeIds={len(fx['types'])} notDeclarable=0 identicalToCpp=True")
        self.assertEqual(len(schema.ir["types"]), ir_types)

    def test_pssz_fixture(self) -> None:
        self.check(*FIXTURES[0])

    def test_json_fixture(self) -> None:
        self.check(*FIXTURES[1])

    def test_time_key_fixture(self) -> None:
        self.check(*FIXTURES[2])


if __name__ == "__main__":
    unittest.main()
