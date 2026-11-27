"""conformance/cpp-golden.json: the rows of the formats this package implements (pssz, key, json).

Every row is run or accounted for: the other formats of the C++ library have
no Python implementation (pjson among them).
"""

from __future__ import annotations

import collections
import unittest
from dataclasses import dataclass, field
from typing import List

from support import fixture

import psio
from psio import ir

FX = fixture("cpp-golden.json")
RUN = ("pssz", "key", "json")
NOT_IN_PYTHON = (
    "pjson", "pjson_f64", "pjson_extra", "pjson_typed", "pjson_invalid", "pjson_to_json", "pjson_f32",
    "pjson_list_i64", "pjson_list_f64", "pjson_list_f32", "ssz", "frac32", "frac16",
    "frac32_invalid_strings", "frac32_invalid_optionals", "bin", "borsh", "bincode", "avro", "capnp", "wit",
    "bson", "msgpack", "protobuf", "flatbuf",
)

# The C++ record { u32 id; std::string name; std::vector<u32> values; bool active; }.
FROM_IR = psio.Schema({"Record": {"Object": {
    "id": ir.U32, "name": ir.RAW_STRING, "values": ir.list_of(ir.U32), "active": ir.BOOL}}}).type("Record")
VALUES = {
    "record": {"id": 42, "name": b"Alice", "values": [1, 256, 65536], "active": True},
    "empty_record": {"id": 0, "name": b"", "values": [], "active": False},
}


class CppGolden(unittest.TestCase):
    def test_every_row_is_classified(self) -> None:
        counts = collections.Counter(f["format"] for f in FX)
        for fmt in counts:
            self.assertTrue(fmt in RUN or fmt in NOT_IN_PYTHON, f"unclassified format {fmt}")
        run = sum(counts[f] for f in RUN)
        print(f"cpp-golden.json: rows={len(FX)} run={run} not implemented in Python={len(FX) - run}")
        self.assertEqual(len(FX), 599)
        self.assertEqual(run, 6)

    def check(self, t: psio.SchemaType, values: dict) -> int:
        ran = 0
        for f in FX:
            if f["format"] not in RUN:
                continue
            value = values[f["id"]]
            expected = bytes.fromhex(f["wire_hex"])
            if f["format"] == "pssz":
                self.assertEqual(t.to_pssz(value), expected)
                self.assertEqual(t.from_pssz(expected), value)
                st = t.validate_pssz(expected)
                self.assertTrue(st.ok and not st.has_unknown)
                view = t.pssz_view(expected)
                self.assertEqual(view.id, values[f["id"]]["id"] if isinstance(value, dict) else value.id)
                self.assertEqual(len(view.values), len(view.to_value()["values"] if isinstance(value, dict)
                                                        else value.values))
            elif f["format"] == "key":
                self.assertEqual(t.to_key(value), expected)
                self.assertEqual(t.from_key(expected), value)
                self.assertTrue(t.validate_key(expected).ok)
            else:
                text = expected.decode("utf-8")
                self.assertEqual(t.to_json(value), text)
                self.assertEqual(t.from_json(text), value)
                self.assertEqual(t.from_ijson(expected), value)
            ran += 1
        return ran

    def test_schema_driven(self) -> None:
        self.assertEqual(self.check(FROM_IR, VALUES), 6)

    def test_typed(self) -> None:
        @dataclass
        class Record:
            id: psio.u32 = 0
            name: psio.RawString = b""
            values: List[psio.u32] = field(default_factory=list)
            active: bool = False

        t = psio.codec(Record)
        self.assertEqual(self.check(t, {k: Record(**v) for k, v in VALUES.items()}), 6)


if __name__ == "__main__":
    unittest.main()
