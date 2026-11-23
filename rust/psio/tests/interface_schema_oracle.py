#!/usr/bin/env python3
"""The expected schemas of rust/psio/tests/interface_schema.rs.

Not C++ output. They are built by the Python implementation, whose
`Builder` is a port of C++ `psio::SchemaBuilder` and regenerates
`conformance/interface-vectors.json`'s `schema_ir` / `schema_json` byte for
byte (python/tests/test_interface_conformance.py). The declarations below are
the C++ ones in Python's spelling: a C++ `std::string` is `RawString`, with
`psio.utf8` where C++ writes `utf8` (Rust: a `String` under `max`), and a
`str` is C++ `psio::utf8_string<>` (Rust: a `String` with no bound).

    PYTHONPATH=python/src python3 rust/psio/tests/interface_schema_oracle.py

prints, per schema, its name, its Json text and the hex of its PSSZ.
"""
from dataclasses import dataclass
from typing import Annotated, Dict, List, Optional, Tuple

import psio
from psio import RawString, i32, i64, u8, u32, u64
from psio.typed import Builder, _declared, shape_of


@dataclass
class Point:
    x: i32
    y: i32


@dataclass
class Entry:
    key: Annotated[RawString, psio.utf8, psio.max(12)]
    points: List[Point]
    tags: Annotated[List[RawString], psio.max(4), psio.element(psio.utf8, psio.max(8))]
    note: Optional[str]
    blob: bytes


@psio.interface(name="ledger", package="second", version="1.2.0")
class Ledger:
    def put(self, entry: Entry, at: Point) -> Entry: ...

    def title(self, id: u64, lang: str) -> Annotated[RawString, psio.utf8, psio.max(24)]: ...

    @psio.method(max_dynamic_data=4096, max_fields=6)
    def bulk(self, entries: Annotated[List[Entry], psio.max(16)], raw: Annotated[bytes, psio.max(64)],
             flag: bool, /) -> None: ...

    def find(self, filter: Annotated[Dict[RawString, u32], psio.max(3), psio.keys(psio.utf8, psio.max(8))],
             paths: Annotated[List[List[RawString]], psio.element(psio.element(psio.max(5)))]) -> Optional[Entry]: ...

    def pair(self, who: Annotated[Optional[RawString], psio.max(20)]
             ) -> Annotated[Tuple[RawString, i64], psio.at(0, psio.utf8, psio.max(10))]: ...

    def tick(self) -> None: ...


def build(interface, before=(), arguments=True, after=()):
    """`insert<T>(name)` of `before`, `insert_interface`, the argument lists, `insert<T>(name)` of `after`."""
    def run():
        builder = Builder()
        for name, hint in before:
            builder.insert_named(name, shape_of(hint))
        funcs = []
        for m in interface.methods:
            fields = m._shape.fields
            result = m._returns()
            for f in fields:
                builder.insert(f.type)
            if result is not None:
                builder.insert(result.type)
            params = [builder._member(f) for f in fields]
            if not m.named:
                for k, p in enumerate(params):
                    p["name"] = str(k)
            attributes = []
            result_type = None
            if result is not None:
                member = builder._member(result)
                result_type = member["type"]
                attributes += [{"name": "result." + a["name"], "value": a["value"]} for a in member["attributes"]]
            if m.max_fields is not None:
                attributes.append({"name": "maxFields", "value": str(m.max_fields)})
            if m.max_dynamic_data is not None:
                attributes.append({"name": "maxDynamicData", "value": str(m.max_dynamic_data)})
            funcs.append({"name": m.name, "params": params, "result": result_type, "attributes": attributes})
        if arguments:
            for m in interface.methods:
                builder.insert_named(f"{interface.name}.{m.name}", m._shape)
        for name, hint in after:
            builder.insert_named(name, shape_of(hint))
        out = builder.schema()
        out.ir["interfaces"].append({"name": interface.name, "type_names": [], "funcs": funcs, "attributes": []})
        out.ir["package"]["name"] = interface.package
        out.ir["package"]["version"] = interface.version
        return out
    return _declared(run)


def show(name, schema):
    print(name)
    print(schema.to_json("json"))
    print(schema.to_pssz().hex())


ledger = psio.interface_of(Ledger)
# The interface, then every argument list.
show("interface", build(ledger))
assert build(ledger).to_pssz() == ledger.schema(arguments=True).to_pssz()
# A service schema: ordinary types, then the interface, then the argument lists, then more types.
show("service", build(
    ledger,
    before=[("Point", Point), ("Entries", List[Entry]), ("Id", u64), ("Entry", Entry)],
    after=[("Blob", bytes), ("Limits", Tuple[u8, u32])]))
# The interface alone.
show("bare", build(ledger, arguments=False))
