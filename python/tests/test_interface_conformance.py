"""conformance/interface-vectors.json: a method declaration, its Schema IR, its argument lists and results.

Every row, on both paths, nothing skipped: typed (the ``@psio.interface`` declaration below) and
schema-driven (the fixture's ``schema_ir`` and nothing else: the roots ``token.<method>`` and
``Schema.method``).  ``psio.interface_of(Token).schema(arguments=True)`` is the fixture's
``schema_ir`` byte for byte.
"""

from __future__ import annotations

import collections
import unittest
from dataclasses import dataclass
from typing import Annotated, Any, List, Optional

from support import fixture, same_reason

import psio
from psio import RawString, u8, u32, u64

KINDS = {"args": 13, "args-accept": 11, "args-reject": 17, "args-pssz-reject": 4, "result": 7,
         "result-pssz-reject": 3}
FORMATS = ("json", "ijson")


# The declaration of conformance/interface-vectors.md.  (A C++ std::string is a raw string that the
# annotation says is text: declared so here, to give the schema the same interned types.  A Python
# program writes `str`.)
@dataclass
class Receipt:
    id: u64
    note: Annotated[RawString, psio.utf8, psio.max(32)]


@psio.interface(name="token", package="iface_conf")
class Token:
    @psio.method(max_dynamic_data=256, max_fields=8)
    def transfer(self, to: Annotated[RawString, psio.utf8, psio.max(16)], amount: u64,
                 memo: Annotated[Optional[RawString], psio.utf8, psio.max(80)]) -> Receipt:
        return Receipt(7, "ok")

    def names(self, limit: u32) -> Annotated[List[RawString], psio.max(4), psio.element(psio.utf8, psio.max(16))]:
        return ["alice", "bob"][:limit]

    @psio.method(max_dynamic_data=64)
    def note(self, text: Annotated[RawString, psio.max(62)]) -> None:
        return None

    def add(self, a: u32, b: u32, /) -> u64:       # no parameter names: positional-only
        return a + b

    def ping(self) -> None:
        return None

    def batch(self, names: Annotated[List[RawString], psio.max(4), psio.element(psio.max(8))], tip: u8) -> bool:
        return True


class InterfaceFixture(unittest.TestCase):
    maxDiff = None

    def rejects(self, run: Any, name: str, kind: str) -> psio.PsioError:
        try:
            run()
        except psio.PsioError as e:
            self.assertEqual(e.kind, kind, f"{name}: {e}")
            return e
        self.fail(f"{name}: accepted")

    def test_declaration_is_the_fixture_schema(self) -> None:
        fx = fixture("interface-vectors.json")
        self.assertEqual((fx["version"], fx["interface"]), (1, "token"))
        token = psio.interface_of(Token)
        schema = token.schema(arguments=True)
        self.assertEqual(schema.to_pssz().hex(), fx["schema_ir"])
        self.assertEqual(schema.to_json("json"), fx["schema_json"])
        recorded = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        self.assertEqual(schema.ir, recorded.ir)
        self.assertEqual(psio.Schema.from_json(fx["schema_json"], "json").to_pssz().hex(), fx["schema_ir"])
        recorded.validate()
        # psio.schema_of of the class is the same Interface, without the named argument lists.
        plain = psio.schema_of(Token)
        self.assertEqual(plain.ir["interfaces"], recorded.ir["interfaces"])
        self.assertEqual([m["name"] for m in plain.ir["types"]],
                         [m["name"] for m in recorded.ir["types"] if not m["name"].startswith("token.")][:8])
        # No function of this declaration has a bound without a type form.
        self.assertFalse(any(a["name"].startswith("result.") for f in schema.ir["interfaces"][0]["funcs"]
                             for a in f["attributes"]))

        # The manifest: what a typed implementation derives, and what a schema-only reader does.
        self.assertEqual(len(fx["methods"]), 6)
        for row in fx["methods"]:
            m = token[row["name"]]
            self.assertEqual({
                "name": m.name, "params": m.params, "named": m.named, "args": f"token.{m.name}",
                "result": m.result is not None, "max_dynamic_data": m.max_dynamic_data, "max_fields": m.max_fields,
                "width": m.args.width, "max_encoded_size": m.args.max_encoded_size}, row)
            self.assertEqual(m.args.max_dynamic_data, row["max_dynamic_data"])
            for args in (recorded.type(row["args"]), recorded.method("token", row["name"]).args):
                self.assertEqual((args.width, args.max_encoded_size, args.max_dynamic_data),
                                 (row["width"], row["max_encoded_size"], row["max_dynamic_data"]), row["name"])
            looked_up = recorded.method("token", row["name"])
            self.assertEqual((looked_up.params, looked_up.named, looked_up.result is not None,
                              looked_up.max_dynamic_data, looked_up.max_fields),
                             (row["params"], row["named"], row["result"], row["max_dynamic_data"],
                              row["max_fields"]))
        with self.assertRaises(psio.SchemaError):
            recorded.method("token", "nope")

    def replay(self, path: str) -> None:
        fx = fixture("interface-vectors.json")
        token = psio.interface_of(Token)
        recorded = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        counts: collections.Counter = collections.Counter()
        names = set()
        for row in fx["vectors"]:
            name, kind, method = row["name"], row["kind"], row["method"]
            self.assertNotIn(name, names)
            names.add(name)
            self.assertTrue(name.startswith(f"{kind}/{method}/"), name)
            counts[kind] += 1
            if path == "typed":
                m = token[method]
                arguments = [m.args]
                result = m.result
            else:
                looked_up = recorded.method("token", method)
                # (two roots of one type: the named argument list, and the list a Func describes)
                arguments = [recorded.type(f"token.{method}"), looked_up.args]
                result = looked_up.result
            if kind == "args":
                data = bytes.fromhex(row["pssz"])
                for t in arguments:
                    st = t.validate_pssz(data, strict=True)
                    self.assertTrue(st.ok and not st.has_unknown, f"{name}: {st.reason}")
                    value = t.from_pssz(data)
                    self.assertEqual(t.to_pssz(value).hex(), row["pssz"], name)
                    for fmt in FORMATS:
                        self.assertEqual(t.to_json(value, fmt), row[fmt], f"{name}: {fmt}")
                        self.assertEqual(t.pssz_to_json(data, fmt), row[fmt], f"{name}: {fmt}")
                        self.assertEqual(t.json_to_pssz(row[fmt], fmt).hex(), row["pssz"], f"{name}: {fmt}")
                        self.assertEqual(t.from_json(row[fmt], fmt), value, f"{name}: {fmt}")
                        self.assertTrue(t.validate_json(row[fmt], fmt).ok, f"{name}: {fmt}")
                    # Binary is the positional tuple whatever the text form: the value read without names.
                    plain = t.from_pssz(psio.trusted(data))
                    self.assertEqual(t.to_pssz(tuple(plain.values()) if isinstance(plain, dict) else plain).hex(),
                                     row["pssz"], name)
                if path == "typed":
                    value = m.from_pssz(data)
                    self.assertIsInstance(value, tuple, name)
                    self.assertEqual(m.to_pssz(*value).hex(), row["pssz"], name)
                    if m.named:
                        by_name = dict(zip(m._attrs, value))
                        self.assertEqual(m.to_pssz(**by_name).hex(), row["pssz"], name)
                        self.assertEqual(m.args.to_pssz(dict(zip(m.params, value))).hex(), row["pssz"], name)
                    # The call itself: decode, call, encode the result.
                    reply = token.dispatch(Token(), method, data)
                    self.assertEqual(reply == b"" and m.result is None or m.validate_result(reply).ok, True, name)
            elif kind == "args-accept":
                for t in arguments:
                    for fmt in FORMATS:
                        self.assertEqual(t.json_to_pssz(row["text"], fmt).hex(), row["pssz"], f"{name}: {fmt}")
                        self.assertTrue(t.validate_json(row["text"], fmt).ok, f"{name}: {fmt}")
                        value = t.from_json(row["text"], fmt)
                        self.assertEqual(t.to_pssz(value).hex(), row["pssz"], f"{name}: {fmt}")
                    self.assertEqual(t.to_json(t.from_pssz(bytes.fromhex(row["pssz"])), "json"), row["json"], name)
                    self.assertIsNone(row["ijson"], name)
            elif kind == "args-reject":
                for t in arguments:
                    for fmt in FORMATS:
                        self.rejects(lambda: t.from_json(row["text"], fmt), f"{name}: {fmt}", "json")
                        self.rejects(lambda: t.json_to_pssz(row["text"], fmt), f"{name}: {fmt}", "json")
                        self.assertFalse(t.validate_json(row["text"], fmt).ok, f"{name}: {fmt}")
            elif kind == "args-pssz-reject":
                data = bytes.fromhex(row["pssz"])
                for t in arguments:
                    st = t.validate_pssz(data)
                    self.assertFalse(st.ok, name)
                    self.assertEqual(st.error.kind, "bytes", name)
                    self.assertTrue(same_reason(row["reason"], st.reason), f"{name}: {st.reason!r}")
                    self.rejects(lambda: t.from_pssz(data), name, "bytes")
                    for fmt in FORMATS:
                        self.rejects(lambda: t.pssz_to_json(data, fmt), name, "bytes")
                if path == "typed":
                    self.rejects(lambda: token.dispatch(Token(), method, data), name, "bytes")
            elif kind == "result":
                self.assertIsNotNone(result, name)
                data = bytes.fromhex(row["pssz"])
                st = result.validate_pssz(data, strict=True)
                self.assertTrue(st.ok and not st.has_unknown, f"{name}: {st.reason}")
                value = result.from_pssz(data)
                self.assertEqual(result.to_pssz(value).hex(), row["pssz"], name)
                for fmt in FORMATS:
                    self.assertEqual(result.to_json(value, fmt), row[fmt], f"{name}: {fmt}")
                    self.assertEqual(result.json_to_pssz(row[fmt], fmt).hex(), row["pssz"], f"{name}: {fmt}")
                    self.assertEqual(result.pssz_to_json(data, fmt), row[fmt], f"{name}: {fmt}")
                if path == "typed":
                    m = token[method]
                    self.assertEqual(m.result_to_pssz(value).hex(), row["pssz"], name)
                    self.assertEqual(m.result_from_pssz(data), value, name)
                    self.assertTrue(m.validate_result(data).ok, name)
            elif kind == "result-pssz-reject":
                self.assertIsNotNone(result, name)
                data = bytes.fromhex(row["pssz"])
                st = result.validate_pssz(data)
                self.assertFalse(st.ok, name)
                self.assertEqual(st.error.kind, "bytes", name)
                self.assertTrue(same_reason(row["reason"], st.reason), f"{name}: {st.reason!r}")
                self.rejects(lambda: result.from_pssz(data), name, "bytes")
                if path == "typed":
                    self.assertFalse(token[method].validate_result(data).ok, name)
                    self.rejects(lambda: token[method].result_from_pssz(data), name, "bytes")
            else:
                self.fail(f"{name}: unknown kind {kind}")
        print(f"interface-vectors.json ({path}): methods={len(fx['methods'])} vectors={sum(counts.values())} "
              f"{dict(counts)}")
        self.assertEqual(dict(counts), KINDS)
        self.assertEqual(sum(counts.values()), len(fx["vectors"]))
        self.assertEqual(len(fx["vectors"]), 55)

    def test_typed(self) -> None:
        self.replay("typed")

    def test_schema_driven(self) -> None:
        self.replay("schema")

    def test_what_the_rows_pin(self) -> None:
        token = psio.interface_of(Token)
        transfer, add, names = token["transfer"], token["add"], token["names"]
        # Named: the object is written; the object in any order and the array are read.
        self.assertEqual(transfer.args.to_json(("bob", 5, "hi")), '{"to":"bob","amount":5,"memo":"hi"}')
        self.assertEqual(transfer.args.from_json('["bob",5,null]'), ("bob", 5, None))
        self.assertEqual(transfer.args.from_json('{"amount":5,"to":"bob"}'), ("bob", 5, None))
        # No names: the array only, written and read.
        self.assertEqual(add.args.to_json((1, 2)), "[1,2]")
        self.assertEqual(add.args.from_json("[1,2]"), (1, 2))
        self.assertEqual(add.to_pssz(1, 2), psio.to_pssz((1, 2), tuple[u32, u32]))
        for text in ('{"0":1,"1":2}', '{"a":1,"b":2}'):
            with self.assertRaises(psio.JsonError):
                add.args.from_json(text)
        with self.assertRaises(psio.EncodeError):
            add.to_pssz(a=1, b=2)
        with self.assertRaises(psio.EncodeError):
            add.args.to_pssz({"0": 1, "1": 2})
        # Schema-driven, a positional list is a tuple in the dynamic model; a named one a dict.
        recorded = psio.Schema.from_pssz(token.schema().to_pssz())
        dynamic = recorded.method("token", "add").args
        self.assertEqual(dynamic.from_pssz(add.to_pssz(1, 2)), (1, 2))
        self.assertEqual(dynamic.to_pssz((1, 2)), add.to_pssz(1, 2))
        named = recorded.method("token", "transfer").args
        self.assertEqual(named.from_pssz(transfer.to_pssz("bob", 5, None)), {"to": "bob", "amount": 5, "memo": None})
        self.assertEqual(named.to_pssz(("bob", 5)), transfer.to_pssz("bob", 5))
        # The result is the type the return annotation makes of it: a 1-byte table, not 4.
        self.assertEqual(names.result_to_pssz(["alice", "bob"]).hex(), "0207616c696365626f62")
        self.assertEqual(names.result.width, 1)
        self.assertEqual(psio.to_pssz(["alice", "bob"], List[str]).hex(), "080000000d000000616c696365626f62")
        for bad in (["a"] * 5, ["x" * 17]):
            with self.assertRaises(psio.EncodeError):
                names.result_to_pssz(bad)
        with self.assertRaises(psio.SchemaError):
            token["ping"].result_to_pssz(None)


if __name__ == "__main__":
    unittest.main()
