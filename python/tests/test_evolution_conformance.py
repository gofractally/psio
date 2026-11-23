"""conformance/evolution-vectors.json: the verdicts of the run-time schema comparison.

Every row, nothing skipped: 50 pairs of types and 6 pairs of interfaces, each in both directions,
``evolves_from`` and ``equivalent`` under ``mode="wire"`` and ``mode="contract"`` - the verdict, the
stable code and the path (``conformance/evolution-vectors.md``).
"""

from __future__ import annotations

import collections
import unittest
from typing import Any, Dict

from support import fixture

import psio

RELATIONS = (("evolves", psio.evolves_from), ("equivalent", psio.equivalent))
MODES = ("wire", "contract")


class EvolutionFixture(unittest.TestCase):
    maxDiff = None

    def setUp(self) -> None:
        self.fx = fixture("evolution-vectors.json")
        self.assertEqual(self.fx["version"], 1)
        self.schemas: Dict[str, psio.Schema] = {}
        for s in self.fx["schemas"]:
            self.assertNotIn(s["id"], self.schemas)
            self.schemas[s["id"]] = psio.Schema.from_pssz(bytes.fromhex(s["ir_hex"]))

    def check(self, row: Any, old: Any, new: Any, counts: Any) -> None:
        for relation, run in RELATIONS:
            for mode in MODES:
                want = row[relation][mode]
                got = run(old, new, mode=mode)
                where = f'{row["name"]}: {relation}/{mode}: {got!r}'
                self.assertIsInstance(got, psio.Compatibility, where)
                self.assertEqual({"ok": got.ok, "code": got.code, "path": got.path}, want, where)
                self.assertEqual(bool(got), want["ok"], where)
                self.assertEqual(got.reason == "", want["ok"], where)
                counts[relation, mode, want["code"] or "ok"] += 1

    def test_types(self) -> None:
        counts: Any = collections.Counter()
        rows = self.fx["types"]
        for row in rows:
            old = self.schemas[row["old_schema"]].type(row["old_type"])
            new = self.schemas[row["new_schema"]].type(row["new_type"])
            self.check(row, old, new, counts)
        self.assertEqual(len(rows), 100)       # 50 pairs, each in both directions
        self.assertEqual(len({r["name"].split("~")[0] for r in rows}), 50)
        self.assertEqual(sum(counts.values()), 400)
        print(f"evolution-vectors.json types: rows={len(rows)} verdicts={sum(counts.values())} "
              f"ok={sum(n for k, n in counts.items() if k[2] == 'ok')} codes={len({k[2] for k in counts}) - 1}")

    def test_interfaces(self) -> None:
        counts: Any = collections.Counter()
        rows = self.fx["interfaces"]
        for row in rows:
            a, b = self.schemas[row["old_schema"]], self.schemas[row["new_schema"]]
            self.check(row, a.interface(row["old_interface"]), b.interface(row["new_interface"]), counts)
        self.assertEqual(len(rows), 12)        # 6 pairs, each in both directions
        self.assertEqual(sum(counts.values()), 48)
        print(f"evolution-vectors.json interfaces: rows={len(rows)} verdicts={sum(counts.values())}")

    def test_whole_schemas_and_functions(self) -> None:
        # The same interface rows as whole schemas (the path gains "<interface>/") and, function by
        # function, as methods (the path loses the function's name).
        for row in self.fx["interfaces"]:
            a, b = self.schemas[row["old_schema"]], self.schemas[row["new_schema"]]
            if row["old_interface"] != row["new_interface"]:
                continue
            for relation, run in RELATIONS:
                for mode in MODES:
                    want = row[relation][mode]
                    got = run(a, b, mode=mode)
                    path = row["old_interface"] + "/" + want["path"] if not want["ok"] else ""
                    self.assertEqual((got.ok, got.code, got.path), (want["ok"], want["code"], path), row["name"])
                    if want["ok"]:
                        ia, ib = a.interface(row["old_interface"]), b.interface(row["new_interface"])
                        for name in ia.names():
                            self.assertTrue(run(ia.method(name), ib.method(name), mode=mode), name)


if __name__ == "__main__":
    unittest.main()
