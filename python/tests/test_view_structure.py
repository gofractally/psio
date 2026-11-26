"""A lazy view on bytes that are not a frame of its type (gofractally/psio#15; docs/annotations.md 7.2
"On bytes that are not a frame").

A member that cannot be placed is a ``DecodeError`` when it is read - never an empty or zero member,
never a read outside the bytes.  These are the cases of the C++ test
(``cpp/tests/trusted_structure_tests.cpp``).
"""

import unittest
from dataclasses import dataclass
from typing import List, Optional

import psio
from psio import u32, u64


@dataclass
class Wide:
    id: u32
    a: str
    b: List[u64]
    c: Optional[str]


@dataclass
class Old:
    id: u32
    a: str


@dataclass
class Fixed:
    x: u64
    y: u64
    z: u32


@dataclass
class Rows:
    id: u32
    rows: List[Wide]
    names: List[str]


def walk(view) -> int:
    total = view.id + len(view.a) + len(view.b)
    for x in view.b:
        total += x
    if view.c is not None:
        total += len(view.c)
    return total


def edited(data: bytes, *edits) -> bytes:
    out = bytearray(data)
    for at, value in edits:
        out[at] = value
    return bytes(out)


class ViewStructure(unittest.TestCase):
    def test_a_member_that_cannot_be_placed_is_an_error_not_an_empty_member(self) -> None:
        wide = psio.to_pssz(Wide(7, "hello", [1, 2, 3], "opt"))
        self.assertEqual(walk(psio.pssz_view(Wide, wide)), 7 + 5 + 3 + 6 + 3)
        cases = {
            "cut to 10 bytes": wide[:10],
            "the first offset 0xffff": edited(wide, (8, 0xFF), (9, 0xFF)),
            "no bytes": b"",
            "a header larger than the buffer": edited(wide, (0, 0xFF)),
            "offsets out of order": edited(wide, (12, 5)),
            "the payload cut": wide[:-9],
            "a list span that is not a multiple of its element": edited(wide, (16, 0x2A)),
            "a row of an older type: a required member is missing": psio.to_pssz(Old(7, "hello")),
        }
        for name, data in cases.items():
            with self.subTest(name):
                self.assertFalse(psio.validate_pssz(Wide, data).ok)
                with self.assertRaises(psio.DecodeError):
                    walk(psio.pssz_view(Wide, data))
                with self.assertRaises(psio.DecodeError):
                    psio.from_pssz(Wide, data)
                # The trusted read is handed the same bytes: it fails too, it does not return a
                # value with an empty member.
                with self.assertRaises(psio.DecodeError):
                    psio.from_pssz_trusted(Wide, data)
        fixed = psio.to_pssz(Fixed(1, 2, 3))
        with self.assertRaises(psio.DecodeError):
            psio.pssz_view(Fixed, fixed[:10]).y

    def test_every_truncation_and_corruption_reads_inside_the_bytes_or_fails(self) -> None:
        good = psio.to_pssz(Rows(1, [Wide(7, "hello", [1, 2, 3], "opt"), Wide(8, "", [], None)], ["one", "", "three"]))

        def walk_rows(data: bytes) -> None:
            view = psio.pssz_view(Rows, data)
            _ = view.id
            # A list never announces more elements than its bytes can hold.
            self.assertLessEqual(len(view.rows), len(data))
            for row in view.rows:
                walk(row)
            self.assertLessEqual(len(view.names), len(data))
            for name in view.names:
                len(name)

        walk_rows(good)
        failed = read = 0
        candidates = [good[:n] for n in range(len(good))]
        for i in range(len(good)):
            for value in (0, 0xFF, (good[i] + 1) & 0xFF, (good[i] - 1) & 0xFF):
                candidates.append(edited(good, (i, value)))
        for i in range(len(good) - 1):
            candidates.append(edited(good, (i, 0xFF), (i + 1, 0xFF)))
        for data in candidates:
            try:
                walk_rows(data)
                read += 1
            except psio.DecodeError:
                failed += 1
                # What a view refuses, validation refuses.
                self.assertFalse(psio.validate_pssz(Rows, data).ok)
            # (Any other exception - an IndexError, a struct.error - is a read the view did not
            # compare with the bytes: it fails the test.)
            for trusted in (True, False):
                try:
                    psio.from_pssz_trusted(Rows, data) if trusted else psio.from_pssz(Rows, data)
                except psio.DecodeError:
                    pass
        self.assertGreater(failed, 100)
        self.assertGreater(read, 0)


if __name__ == "__main__":
    unittest.main()
