"""A lazy view on bytes that are not a frame of its type (gofractally/psio#15; docs/annotations.md 7.2
"On bytes that are not a frame").

A member that cannot be placed is a ``DecodeError`` when it is read - never an empty or zero member,
never a read outside the bytes.  These are the cases of the C++ test
(``cpp/tests/trusted_structure_tests.cpp``).
"""

import unittest
from dataclasses import dataclass
from typing import Annotated, List, Optional

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


@dataclass
class Header:
    num: u32
    producer: str
    xs: List[u64]


@dataclass
class Block:
    header: Header
    trx: List[str]
    sig: Optional[str]
    final: bool


@dataclass
class InNested:
    kind: u32
    packed: Annotated[Block, psio.nested()]
    after: str


@dataclass
class InOptional:
    kind: u32
    packed: Optional[Annotated[Block, psio.nested()]]
    after: str


@dataclass
class InList:
    kind: u32
    packed: List[Annotated[Block, psio.nested()]]
    after: str


@dataclass
class InDeferred:
    kind: u32
    packed: Annotated[bytes, psio.nested(Block, deferred=True)]
    after: str


def block(n: int) -> Block:
    return Block(Header(n, "prod", [1, 2, 3]), ["a", "bb"], "sig", True)


class PackedMembers(unittest.TestCase):
    """What a node relies on when it reads a message off the network: the validating read of the
    outer value validates a nested member as its type.  A deferred nested member is the documented
    exception."""

    def mutations(self, outer_type, good: bytes, validated: bool):
        inner = psio.to_pssz(block(7))
        pos = good.index(inner)
        self.assertTrue(psio.validate_pssz(outer_type, good).ok)
        tried = accepted = member_refused = 0
        seed = pos * 1000003 + len(inner)
        for i in range(len(inner)):
            values = [0xFF, 0x00, 0x7F]
            for _ in range(3):
                seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
                values.append((seed >> 16) & 0xFF)
            for v in values:
                if good[pos + i] == v:
                    continue
                bad = edited(good, (pos + i, v))
                tried += 1
                inner_valid = psio.validate_pssz(Block, bad[pos:pos + len(inner)]).ok
                outer_valid = psio.validate_pssz(outer_type, bad).ok
                try:
                    psio.from_pssz(outer_type, bad)
                    decoded = True
                except psio.DecodeError:
                    decoded = False
                if validated:
                    self.assertEqual(outer_valid, inner_valid, f"byte {i} set to {v}")
                else:
                    self.assertTrue(outer_valid, f"deferred: byte {i} set to {v}")
                self.assertEqual(decoded, outer_valid, f"byte {i} set to {v}")
                accepted += outer_valid
                member_refused += not inner_valid
        return tried, accepted, member_refused

    def test_validating_an_outer_value_validates_a_nested_member_as_its_type(self) -> None:
        for outer_type, value in ((InNested, InNested(1, block(7), "z")),
                                  (InOptional, InOptional(1, block(7), "z")),
                                  (InList, InList(1, [block(1), block(7), block(3)], "z"))):
            with self.subTest(outer_type.__name__):
                tried, accepted, refused = self.mutations(outer_type, psio.to_pssz(value), True)
                self.assertGreater(tried, 150)
                self.assertGreater(accepted, 0)
                self.assertGreater(refused, 0)
                self.assertEqual(accepted + refused, tried)
        # The exception: a deferred member's bytes are not looked at.
        deferred = InDeferred(1, psio.to_pssz(block(7)), "z")
        tried, accepted, refused = self.mutations(InDeferred, psio.to_pssz(deferred), False)
        self.assertEqual(accepted, tried)
        self.assertGreater(refused, 0)


if __name__ == "__main__":
    unittest.main()
