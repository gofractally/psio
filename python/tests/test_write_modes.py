"""The two write modes (docs/annotations.md 7.2; gofractally/psio#14).

``psio.to_pssz`` / ``to_key`` / ``to_json`` / ``to_ijson`` are the FAST write and hold nothing against
what a type declares; ``to_pssz_checked`` / ``to_key_checked`` / ``to_json_checked`` /
``to_ijson_checked`` are the VALIDATED write and refuse exactly what the validating readers refuse.
A row written fast is read back by the trusted pssz read.
"""

import unittest
from dataclasses import dataclass
from typing import Annotated, List

import psio
from psio import u32


@dataclass
class Item:
    code: Annotated[str, psio.max(3)]
    qty: u32


@dataclass
class Even:
    v: u32

    @psio.invariant
    def even(self) -> "str | None":
        return None if self.v % 2 == 0 else "Even: v is odd"


@dataclass
class Prop:
    ids: Annotated[List[u32], psio.max(3), psio.sorted]
    name: Annotated[str, psio.max(6)]
    tags: Annotated[List[Annotated[str, psio.bounded(3)]], psio.max(2)]
    items: Annotated[List[Item], psio.max(2)]
    even: Even


@dataclass
class Pair:
    a: Annotated[str, psio.max(100)]
    b: Annotated[str, psio.max(100)]


class Rng:
    def __init__(self, seed: int) -> None:
        self.state = seed

    def next(self) -> int:
        mask = (1 << 64) - 1
        self.state = (self.state + 0x9E3779B97F4A7C15) & mask
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
        return z ^ (z >> 31)

    def below(self, n: int) -> int:
        return self.next() % n

    def text(self, limit: int) -> str:
        return "".join(chr(97 + self.below(26)) for _ in range(self.below(limit + 1)))


def draw(r: Rng) -> Prop:
    ids = [r.below(4) for _ in range(r.below(6))]
    if r.below(2):
        ids.sort()
    return Prop(
        ids=ids,
        name=r.text(9),
        tags=[r.text(5) for _ in range(r.below(4))],
        items=[Item(r.text(5), r.below(100)) for _ in range(r.below(4))],
        even=Even(r.below(6)),
    )


def attempt(run):  # type: ignore[no-untyped-def]
    try:
        return run()
    except psio.PsioError:
        return None


class WriteModes(unittest.TestCase):
    def test_the_validated_write_refuses_exactly_when_the_validating_read_refuses(self) -> None:
        r = Rng(1)
        valid = invalid = 0
        for _ in range(1500):
            value = draw(r)
            # pssz: fast write, trusted read; validated write <=> validating read.
            fast = psio.to_pssz(value)
            self.assertEqual(psio.from_pssz_trusted(Prop, fast), value)
            checked = attempt(lambda: psio.to_pssz_checked(value))
            self.assertEqual(checked is not None, bool(psio.validate_pssz(Prop, fast)), value)
            if checked is not None:
                self.assertEqual(checked, fast)
            # key
            key = psio.to_key(value)
            key_checked = attempt(lambda: psio.to_key_checked(value))
            self.assertEqual(key_checked is not None, bool(psio.validate_key(Prop, key)), value)
            self.assertEqual(key_checked is not None, checked is not None)
            # json, ijson
            for to_text, to_checked, read in (
                (psio.to_json, psio.to_json_checked, psio.from_json),
                (psio.to_ijson, psio.to_ijson_checked, psio.from_ijson),
            ):
                text = to_text(value)
                text_checked = attempt(lambda: to_checked(value))
                self.assertEqual(text_checked is not None, attempt(lambda: read(Prop, text)) is not None, value)
                self.assertEqual(text_checked is not None, checked is not None)
                if text_checked is not None:
                    self.assertEqual(text_checked, text)
            if checked is None:
                invalid += 1
            else:
                valid += 1
        self.assertGreater(valid, 5)
        self.assertGreater(invalid, 800)

    def test_the_fast_write_still_refuses_what_the_format_cannot_represent(self) -> None:
        self.assertEqual(psio.codec(Pair).width, 1)
        # The last member ends where the record ends: far over its bound, and a frame.
        last_long = Pair("a", "b" * 300)
        with self.assertRaises(psio.EncodeError):
            psio.to_pssz_checked(last_long)
        data = psio.to_pssz(last_long)
        self.assertFalse(psio.validate_pssz(Pair, data))
        self.assertEqual(psio.from_pssz_trusted(Pair, data), last_long)
        # An offset that does not fit: no frame, in either mode.
        first_long = Pair("a" * 300, "b")
        with self.assertRaises(psio.EncodeError):
            psio.to_pssz_checked(first_long)
        with self.assertRaises(psio.EncodeError):
            psio.to_pssz(first_long)
        # A wrong Python type is not a value in any mode.
        with self.assertRaises(psio.EncodeError):
            psio.to_pssz(Pair(1, ""))  # type: ignore[arg-type]

    def test_a_nested_members_bound_is_held_by_key_and_json_too(self) -> None:
        @dataclass
        class Inner:
            x: List[psio.u8]

        @dataclass
        class Holder:
            f: Annotated[Inner, psio.nested(), psio.max(10)]

        over = Holder(Inner([1, 2, 3, 4, 5, 6]))   # the inner encoding is longer than 10 bytes
        fits = Holder(Inner([1]))
        for checked in (psio.to_pssz_checked, psio.to_key_checked, psio.to_json_checked, psio.to_ijson_checked):
            with self.assertRaises(psio.EncodeError):
                checked(over)
            checked(fits)
        # The fast writes hold nothing against it; the validating readers refuse what they wrote.
        self.assertFalse(psio.validate_pssz(Holder, psio.to_pssz(over)))
        self.assertFalse(psio.validate_key(Holder, psio.to_key(over)))
        self.assertFalse(psio.validate_json(Holder, psio.to_json(over)))
        self.assertFalse(psio.validate_ijson(Holder, psio.to_ijson(over)))
        with self.assertRaises(psio.PsioError):
            psio.from_json(Holder, psio.to_json(over))
        with self.assertRaises(psio.PsioError):
            psio.from_key(Holder, psio.to_key(over))
        self.assertEqual(psio.from_key(Holder, psio.to_key(fits)), fits)
        self.assertEqual(psio.from_json(Holder, psio.to_json(fits)), fits)

    def test_a_validating_call_inside_a_fast_write_still_validates(self) -> None:
        # The mode belongs to the call.  Code of the value that runs in the middle of a fast write
        # (here: iterating a member) and asks for a validating read or a validated write gets one.
        bad = Prop([3, 1, 2, 9], "much too long", ["toolong", "b", "c"], [], Even(1))
        fast = {"pssz": psio.to_pssz(bad), "key": psio.to_key(bad), "json": psio.to_json(bad)}
        seen: list = []

        def refuses(run) -> bool:
            try:
                run()
            except psio.PsioError:
                return True
            return False

        class Probe(list):
            def __iter__(inner):
                seen.append({
                    "from_pssz": refuses(lambda: psio.from_pssz(Prop, fast["pssz"])),
                    "validate_pssz": not psio.validate_pssz(Prop, fast["pssz"]).ok,
                    "to_pssz_checked": refuses(lambda: psio.to_pssz_checked(bad)),
                    "from_key": refuses(lambda: psio.from_key(Prop, fast["key"])),
                    "validate_key": not psio.validate_key(Prop, fast["key"]).ok,
                    "to_key_checked": refuses(lambda: psio.to_key_checked(bad)),
                    "from_json": refuses(lambda: psio.from_json(Prop, fast["json"])),
                    "validate_json": not psio.validate_json(Prop, fast["json"]).ok,
                    "to_json_checked": refuses(lambda: psio.to_json_checked(bad)),
                    "validate_value": not psio.schema_of(Prop).validate_value(bad).ok
                    if hasattr(psio.schema_of(Prop), "validate_value") else True,
                })
                return super().__iter__()

        @dataclass
        class Outer:
            ids: Annotated[List[u32], psio.max(2)]

        for write in (psio.to_pssz, psio.to_key, psio.to_json, psio.to_ijson):
            seen.clear()
            write(Outer(Probe([1, 2, 3])))  # itself a fast write of a value over its bound
            self.assertTrue(seen, write.__name__)
            for checks in seen:
                self.assertEqual([name for name, held in checks.items() if not held], [], write.__name__)
        # ... and the fast write goes on as a fast write after the validation inside it.
        self.assertEqual(psio.from_pssz_trusted(Outer, psio.to_pssz(Outer(Probe([1, 2, 3])))).ids, [1, 2, 3])
        # The validated write is not made fast by a fast write inside it either.
        class Inner(list):
            def __iter__(inner):
                psio.to_pssz(bad)
                return super().__iter__()
        self.assertTrue(refuses(lambda: psio.to_pssz_checked(Outer(Inner([1, 2, 3])))))


if __name__ == "__main__":
    unittest.main()
