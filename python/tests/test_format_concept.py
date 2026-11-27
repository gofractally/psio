"""One generic function, every format (gofractally/psio#18).

``exercise(fmt, T, value)`` is written once against ``psio.formats.Format`` and run for every format value of
the package over the shared record of conformance/cpp-golden.json (C++ cpp/tests/format_concept_tests.cpp,
Rust tests/format_concept.rs, JS src/test/format-concept.test.ts).  What a format lacks is stated in EXPECTED.
"""

from __future__ import annotations

import unittest
from dataclasses import dataclass, field
from typing import List

from support import fixture

import psio
from psio import formats

FX = fixture("cpp-golden.json")


@dataclass
class Record:
    id: psio.u32 = 0
    name: psio.RawString = b""
    values: List[psio.u32] = field(default_factory=list)
    active: bool = False


VALUES = {
    "record": Record(42, b"Alice", [1, 256, 65536], True),
    "empty_record": Record(),
}

# What each format has today: (text, trusted read, view, rows in cpp-golden.json).  Gaps against the concept
# (psio#18, B7): a trusted read and a view for pssz only; no buffer type.
EXPECTED = {
    "pssz": (False, True, True, True),
    "key": (False, False, False, True),
    "json": (True, False, False, True),
    "ijson": (True, False, False, False),
}


class FormatConcept(unittest.TestCase):
    def exercise(self, fmt: formats.Format, T, value, golden=None) -> None:
        data = fmt.encode(value)
        raw = data.encode("utf-8") if fmt.text else data
        self.assertIsInstance(data, str if fmt.text else bytes, fmt.name)
        if golden is not None:
            self.assertEqual(raw, golden, f"{fmt.name}: the C++ bytes")
        self.assertEqual(fmt.encode(value, T), data, fmt.name)
        self.assertEqual(fmt.encode_checked(value), data, fmt.name)
        self.assertEqual(fmt.size(value), len(raw), fmt.name)
        self.assertTrue(fmt.validate(T, data).ok, fmt.name)
        self.assertEqual(fmt.decode(T, data), value, fmt.name)
        self.assertEqual(fmt.decode(T, raw), value, fmt.name)
        if fmt.decode_trusted is not None:
            self.assertEqual(fmt.decode_trusted(T, data), value, fmt.name)
        if fmt.view is not None:
            self.assertEqual(fmt.view(T, data).id, value.id, fmt.name)
        # Bytes that are not a frame.
        if len(raw) > 8:
            cut = raw[:3]
            self.assertFalse(fmt.validate(T, cut).ok, fmt.name)
            with self.assertRaises(psio.PsioError):
                fmt.decode(T, cut)

    def test_the_format_values_and_their_capabilities(self) -> None:
        # pjson has no Python implementation yet (gofractally/psio#17, S4): a declared gap.
        self.assertFalse(hasattr(formats, "pjson"))
        self.assertEqual(sorted(f.name for f in formats.ALL), sorted(EXPECTED))
        for fmt in formats.ALL:
            self.assertEqual((fmt.text, fmt.decode_trusted is not None, fmt.view is not None),
                             EXPECTED[fmt.name][:3], fmt.name)

    def test_every_format_through_the_same_function(self) -> None:
        for fmt in formats.ALL:
            golden_rows = 0
            for row_id, value in VALUES.items():
                rows = [f for f in FX if f["format"] == fmt.name and f["id"] == row_id]
                golden = bytes.fromhex(rows[0]["wire_hex"]) if rows else None
                golden_rows += len(rows)
                self.exercise(fmt, Record, value, golden)
            self.assertEqual(golden_rows > 0, EXPECTED[fmt.name][3], fmt.name)


if __name__ == "__main__":
    unittest.main()
