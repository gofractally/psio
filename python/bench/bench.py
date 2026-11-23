"""A small benchmark, reported and never failing on timing (the counterpart of js/src/test/bench.test.ts).

Encode, decode, validate and view of a typical record and of a list of 1,000
records, through the typed layer (dataclasses) and through the schema-driven
path (dicts, a schema that travelled as bytes).

    PYTHONPATH=src python3 bench/bench.py
"""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass
from typing import Annotated, Any, Callable, Dict, List, Optional

import psio
from psio import u32, u64


@dataclass
class Account:
    id: u64
    name: str
    email: Optional[str]
    balance: u64
    active: bool
    tags: List[str]
    logins: u32


@psio.record(max_dynamic_data=200)
@dataclass
class Bounded:
    id: u64
    name: Annotated[str, psio.max(32)]
    tags: Annotated[List[str], psio.max(8), psio.element(psio.max(16))]
    counts: Annotated[Dict[str, u32], psio.max(4), psio.keys(psio.max(8))]


def one(i: int) -> Account:
    return Account(1000 + i, f"account-{i}", f"user{i}@example.com" if i % 3 else None, i * 1000003, i % 2 == 0,
                   ["alpha", "beta", f"t{i % 7}"], i * 3)


def measure(run: Callable[[], Any], iterations: int) -> float:
    """Best ns per operation over several batches."""
    for _ in range(max(3, iterations >> 3)):
        run()
    samples = []
    for _ in range(5):
        start = time.perf_counter_ns()
        for _ in range(iterations):
            run()
        samples.append((time.perf_counter_ns() - start) / iterations)
    return min(samples)  # the least disturbed batch (the machine may be busy)


def fmt(ns: float) -> str:
    return f"{ns / 1e6:.2f} ms" if ns >= 1e6 else f"{ns / 1e3:.2f} us" if ns >= 1e3 else f"{ns:.0f} ns"


def main() -> None:
    single = one(7)
    many = [one(i) for i in range(1000)]
    typed_one, typed_many = psio.codec(Account), psio.codec(List[Account])
    fetched = psio.Schema.from_pssz(psio.schema_of(List[Account]).to_pssz())
    names = [m["name"] for m in fetched.ir["types"]]
    dyn_one, dyn_many = fetched.type("Account"), fetched.type(names[0])
    one_bytes = typed_one.to_pssz(single)
    many_bytes = typed_many.to_pssz(many)
    assert dyn_many.to_pssz(dyn_many.from_pssz(many_bytes)) == many_bytes
    rows = []
    for path, rec, recs, value, values in (
            ("typed", typed_one, typed_many, single, many),
            ("schema-driven", dyn_one, dyn_many, dyn_one.from_pssz(one_bytes), dyn_many.from_pssz(many_bytes))):
        assert rec.to_pssz(value) == one_bytes
        results = [
            ("record encode", measure(lambda: rec.to_pssz(value), 2000)),
            ("record decode (validating)", measure(lambda: rec.from_pssz(one_bytes), 2000)),
            ("record decode (trusted)", measure(lambda t=psio.trusted(one_bytes): rec.from_pssz(t), 2000)),
            ("record validate", measure(lambda: rec.validate_pssz(one_bytes), 2000)),
            ("record view, 1 field", measure(lambda: rec.pssz_view(one_bytes).balance, 2000)),
            ("record to I-JSON", measure(lambda: rec.pssz_to_json(one_bytes), 1000)),
            ("record from I-JSON", measure(lambda t=rec.pssz_to_json(one_bytes): rec.from_ijson(t), 1000)),
            ("record to key", measure(lambda: rec.to_key(value), 1000)),
            ("list[1000] encode", measure(lambda: recs.to_pssz(values), 5)),
            ("list[1000] decode (validating)", measure(lambda: recs.from_pssz(many_bytes), 5)),
            ("list[1000] decode (trusted)", measure(lambda t=psio.trusted(many_bytes): recs.from_pssz(t), 5)),
            ("list[1000] view, 1 field of 1 element", measure(lambda: recs.pssz_view(many_bytes)[500].balance, 2000)),
        ]
        rows += [f"{path:14} {name:42} {fmt(ns):>10}" for name, ns in results]
    # What the trusted decode leaves out shows where there is something to leave out:
    # bounds and a cap on every member, and the order of a map's entries.
    bounded = psio.codec(List[Bounded])
    bounded_bytes = bounded.to_pssz([Bounded(i, f"account-{i}", ["alpha", "beta"], {f"k{i}": i, "a": 1}) for i in range(1000)])
    table = psio.codec(Dict[str, u64])
    table_bytes = table.to_pssz({f"key-{i:05}": i for i in range(1000)})
    for name, t, data in (("list[1000] of a bounded, capped record", bounded, bounded_bytes),
                          ("map[1000] str -> u64", table, table_bytes)):
        checked = measure(lambda: t.from_pssz(data), 5)
        trusted = measure(lambda d=psio.trusted(data): t.from_pssz(d), 5)
        assert t.from_pssz(psio.trusted(data)) == t.from_pssz(data)
        rows.append(f"{'typed':14} {name + ' decode':42} {fmt(checked):>10}   trusted {fmt(trusted):>10}"
                    f"   ({checked / trusted:.2f}x)")
    print(f"python {sys.version.split()[0]}; record {len(one_bytes)} bytes, list of 1000 = {len(many_bytes)} bytes; "
          "best of 5 batches, per operation:")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
