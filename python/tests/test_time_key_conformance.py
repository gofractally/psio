"""conformance/time-key-vectors.json: time values and the ``key`` format, every row, nothing skipped."""

from __future__ import annotations

import collections
import unittest
from typing import Any

from support import fixture
from test_pssz_conformance import sources

import psio
from psio import PsioError

TYPES = 56
VECTORS = 1164
KINDS = {"value": 359, "key-order": 54, "text-accept": 56, "text-reject": 124, "key-reject": 148,
         "key-prefix": 391, "key-order-bits": 5, "bound": 17, "key-invariant": 10}
FORMATS = ("json", "ijson")


def elements_of(value: Any) -> list:
    """The elements of a tuple / the members of a record, in order."""
    if isinstance(value, dict):
        return list(value.values())
    if isinstance(value, (tuple, list)):
        return list(value)
    return [getattr(value, f) for f in value.__dataclass_fields__]


class TimeKeyFixture(unittest.TestCase):
    maxDiff = None

    def rejects(self, run: Any, name: str) -> PsioError:
        try:
            run()
        except PsioError as e:
            self.assertNotEqual(e.kind, "internal", f"{name}: {e}")
            return e
        self.fail(f"{name}: accepted")

    def replay(self, path: str) -> None:
        fx = fixture("time-key-vectors.json")
        self.assertEqual(fx["version"], 1)
        src = sources(path, bytes.fromhex(fx["schema_ir"]))
        # The rows are the typed codec's: the handlers of the fixture's custom
        # types give the schema their text forms and invariants.  `plain`
        # knows only the wire types.
        types = {row["id"]: src["handled"].type(row["id"]) for row in fx["types"]}
        plain_types = {row["id"]: src["plain"].type(row["id"]) for row in fx["types"]}
        composite = {t["id"]: t["composite"] for t in fx["types"]}
        counts: collections.Counter = collections.Counter()
        seen = set()
        names = set()
        for row in fx["vectors"]:
            name, kind = row["name"], row["kind"]
            self.assertNotIn(name, names)
            names.add(name)
            counts[kind] += 1
            if kind == "bound":
                self.assertEqual(row["type"], "bytes", name)
                bound = psio.key_prefix_upper_bound(bytes.fromhex(row["prefix"]))
                self.assertEqual(None if bound is None else bound.hex(), row["bound"], name)
                continue
            t = types[row["type"]]
            seen.add(row["type"])
            if kind == "value":
                data = bytes.fromhex(row["pssz"])
                value = t.from_pssz(data)
                self.assertEqual(t.to_pssz(value).hex(), row["pssz"], f"{name}: pssz")
                key = t.to_key(value)
                self.assertEqual(key.hex(), row["key"], f"{name}: key")
                self.assertTrue(t.validate_key(key).ok, f"{name}: validate_key")
                back = t.from_key(key)
                self.assertEqual(t.to_pssz(back).hex(), row["pssz"], f"{name}: from_key -> pssz")
                self.assertEqual("json" in row, "ijson" in row, name)
                for fmt in FORMATS:
                    want = row.get(fmt)
                    if want is None:
                        self.rejects(lambda: t.to_json_checked(value, fmt), f"{name}: {fmt} text of a value without text")
                        self.rejects(lambda: t.pssz_to_json(data, fmt), f"{name}: {fmt} pssz_to_json")
                        continue
                    self.assertEqual(t.to_json(value, fmt), want, f"{name}: {fmt}")
                    self.assertEqual(t.json_to_pssz(want, fmt).hex(), row["pssz"], f"{name}: {fmt} read")
                    self.assertEqual(t.to_pssz(t.from_json(want, fmt)).hex(), row["pssz"], f"{name}: {fmt} value")
            elif kind == "text-accept":
                for fmt in FORMATS:
                    self.assertEqual(t.json_to_pssz(row["text"], fmt).hex(), row["pssz"], f"{name}: {fmt}")
                    self.assertEqual(t.json_to_pssz(row["text"].encode("utf-8"), fmt).hex(), row["pssz"], name)
                self.assertEqual(t.pssz_to_json(bytes.fromhex(row["pssz"]), "json"), row["json"], f"{name}: canonical")
            elif kind == "text-reject":
                for fmt in FORMATS:
                    self.rejects(lambda: t.from_json(row["text"], fmt), f"{name}: {fmt}")
            elif kind == "key-reject":
                data = bytes.fromhex(row["key"])
                self.rejects(lambda: t.from_key(data), name)
                self.assertFalse(t.validate_key(data).ok, name)
            elif kind in ("key-order", "key-order-bits"):
                # Bytewise order, strictly ascending; `key-order-bits`: where
                # `<` has no answer (NaNs) each key gives the exact bits back.
                self.assertTrue(row["values"], name)
                last = None
                for hx in row["values"]:
                    value = t.from_pssz(bytes.fromhex(hx))
                    self.assertEqual(t.to_pssz(value).hex(), hx, f"{name}: pssz bits")
                    key = t.to_key(value)
                    if last is not None:
                        self.assertLess(last, key, f"{name}: keys not ascending at {hx}")
                    self.assertTrue(t.validate_key(key).ok, name)
                    self.assertEqual(t.to_key(t.from_key(key)), key, f"{name}: round trip")
                    if kind == "key-order-bits":
                        self.assertEqual(t.to_pssz(t.from_key(key)).hex(), hx, f"{name}: key gives the bits back")
                    last = key
            elif kind == "key-prefix":
                self.assertTrue(composite[row["type"]], f"{name}: composite")
                value = t.from_pssz(bytes.fromhex(row["pssz"]))
                elements = elements_of(value)[:row["count"]]
                self.assertEqual(len(elements), row["count"], name)
                prefix = t.key_prefix(elements)
                self.assertEqual(prefix.hex(), row["prefix"], f"{name}: prefix")
                bound = psio.key_prefix_upper_bound(prefix)
                self.assertEqual(None if bound is None else bound.hex(), row["bound"], f"{name}: bound")
                full = t.to_key(value)
                self.assertTrue(full.startswith(prefix), f"{name}: byte-prefix of the full key")
                if bound is not None:
                    self.assertLess(full, bound, f"{name}: below the bound")
            elif kind == "key-invariant":
                # `key` and `pssz` are those of a value of the wire type that
                # violates an invariant of the type.
                key = bytes.fromhex(row["key"])
                data = bytes.fromhex(row["pssz"])
                read = self.rejects(lambda: t.from_key(key), f"{name}: from_key")
                self.assertEqual((read.reason, read.kind, read.invariant), (row["reason"], "bytes", True), name)
                st = t.validate_key(key)
                self.assertTrue(not st.ok and st.reason == row["reason"], f"{name}: validate_key")
                st = t.validate_pssz(data)
                self.assertTrue(not st.ok and st.reason == row["reason"], f"{name}: validate_pssz")
                plain = plain_types[row["type"]]
                self.assertTrue(plain.validate_key(key).ok, f"{name}: wire type")
                value = plain.from_key(key)
                self.assertEqual(plain.to_pssz(value).hex(), row["pssz"], f"{name}: wire type value")
                self.assertEqual(plain.to_key(plain.from_pssz(data)).hex(), row["key"], f"{name}: wire type key")
                unchecked = t.from_pssz(psio.trusted(data))
                written = self.rejects(lambda: t.to_key_checked(unchecked), f"{name}: to_key")
                self.assertEqual((written.reason, written.kind), (row["reason"], "value"), name)
                # A prefix element is checked as the value it is.
                if composite[row["type"]]:
                    self.assertEqual(plain.key_prefix(elements_of(value)).hex(), row["key"], f"{name}: prefix")
            else:
                self.fail(f"{name}: unknown kind {kind}")

        print(f"time-key-vectors.json ({path}): types={len(fx['types'])} vectors={sum(counts.values())} {dict(counts)}")
        self.assertEqual(sum(counts.values()), len(fx["vectors"]))
        self.assertEqual(len(fx["types"]), TYPES)
        self.assertEqual(len(fx["vectors"]), VECTORS)
        self.assertEqual(dict(counts), KINDS)
        monotonic = ["monotonic<seconds>", "monotonic<micros>", "monotonic<millis,u32>", "Timer",
                     "tuple<monotonic<micros>,string>"]
        self.assertEqual(sum(1 for r in fx["vectors"] if r["type"] in monotonic), 82)
        # Every declared type has rows.
        for row in fx["types"]:
            self.assertIn(row["id"], seen, f"no rows for {row['id']}")

    def test_schema_driven(self) -> None:
        self.replay("schema")

    def test_typed(self) -> None:
        self.replay("typed")

    def test_time_ids(self) -> None:
        """The schema marks time values with their ids; ticks are ints at every width."""
        fx = fixture("time-key-vectors.json")
        raw = bytes.fromhex(fx["schema_ir"])
        schema = psio.Schema.from_pssz(raw)
        by_name = {m["name"]: m["type"] for m in schema.ir["types"]}
        ids = {}
        for m in schema.ir["types"]:
            t = m["type"]["value"]
            for _ in range(8):
                if t.type != "Type":
                    break
                t = by_name[t.value["type"]]["value"]
            if t.type == "Custom":
                ids[m["name"]] = t.value["id"]
        for name, id in (("time_point<seconds>", "time-point-seconds"), ("time_point<millis>", "time-point-millis"),
                         ("time_point<micros>", "time-point-micros"), ("time_point<nanos>", "time-point-nanos"),
                         ("time_point<seconds,u32>", "time-point-seconds"), ("duration<seconds>", "duration-seconds"),
                         ("duration<nanos>", "duration-nanos"), ("duration<60/1,i32>", "duration-60/1"),
                         ("monotonic<seconds>", "monotonic-seconds"), ("monotonic<micros>", "monotonic-micros"),
                         ("monotonic<millis,u32>", "monotonic-millis")):
            self.assertEqual(ids.get(name), id)
        # A time presentation is the codec's own, also when every custom id must have a handler.
        strict = psio.Schema.from_pssz(raw, require_handlers=True)
        self.assertEqual(strict.type("monotonic<millis,u32>").from_pssz(bytes.fromhex("01000000")), 1)
        self.assertEqual(strict.type("monotonic<micros>").to_ijson(9007199254740993), '"9007199254740993"')
        timer = strict.type("Timer")
        text = '{"expires":12,"started":-5,"laps":[7]}'
        self.assertEqual(timer.pssz_to_json(timer.json_to_pssz(text)), text)
        self.assertEqual(schema.type("duration<60/1,i32>").from_pssz(bytes.fromhex("a6ffffff")), -90)


if __name__ == "__main__":
    unittest.main()
