"""conformance/json-vectors.json: every type and every row, nothing skipped; and the schema rows."""

from __future__ import annotations

import collections
import unittest
from typing import Any

from support import fixture
from test_pssz_conformance import NO_HANDLER, sources
from typed_fixture import TypedFixture

import psio
from psio import PsioError

TYPES = 96
VECTORS = 1759
KINDS = {"value": 818, "accept": 194, "reject": 697, "invariant": 50}
SCHEMA_REJECTS = 77
SCHEMA_ACCEPTS = 28


class JsonFixture(unittest.TestCase):
    maxDiff = None

    def rejects(self, run: Any, name: str) -> PsioError:
        try:
            run()
        except PsioError as e:
            self.assertNotEqual(e.kind, "internal", f"{name}: {e}")
            return e
        self.fail(f"{name}: accepted")

    def replay(self, path: str) -> None:
        fx = fixture("json-vectors.json")
        src = sources(path, bytes.fromhex(fx["schema_ir"]))
        self.assertEqual(src["schema"].to_pssz().hex(), fx["schema_ir"])
        schema, with_handlers, strict = src["plain"], src["handled"], src["strict"]
        rows = {t["id"]: t for t in fx["types"]}
        types = {row["id"]: with_handlers.type(row["id"]) for row in fx["types"] if row["ir"]}
        # Width fidelity: every type the typed PSSZ codec encodes records its
        # automatic width, and the schema derives the same (handlers or not).
        widths = 0
        for row in fx["types"]:
            self.assertEqual("auto_width" in row, row["pssz"], row["id"])
            if "auto_width" not in row:
                continue
            self.assertEqual(types[row["id"]].width, row["auto_width"], f"auto width of {row['id']}")
            self.assertEqual(schema.type(row["id"]).width, row["auto_width"], f"auto width of {row['id']}")
            widths += 1
        self.assertEqual(widths, sum(1 for t in fx["types"] if t["pssz"]))

        count: collections.Counter = collections.Counter()
        covered = set()

        def input_of(row: Any) -> Any:
            return row["text"] if "text" in row else bytes.fromhex(row["text_hex"])

        for row in fx["vectors"]:
            name = row["name"]
            info = rows[row["type"]]
            self.assertTrue(info["ir"], name)
            covered.add(row["type"])
            t = types[row["type"]]
            kind = row["kind"]
            count[kind] += 1
            self.assertEqual("pssz" in row, info["pssz"] and kind != "reject", name)
            self.assertEqual("reason" in row, kind == "invariant", f"{name}: reason")
            if kind == "value":
                data = t.json_to_pssz(row["json"], "json")
                again = t.json_to_pssz(row["ijson"], "ijson")
                self.assertEqual(data.hex(), again.hex(), name)
                count["valueTextToBytes"] += 2
                if "pssz" in row:
                    self.assertEqual(data.hex(), row["pssz"], name)
                    count["valueBytesMatchFixture"] += 1
                st = t.validate_pssz(data)
                self.assertTrue(st.ok and not st.has_unknown, name)
                self.assertEqual(t.pssz_to_json(data, "json"), row["json"], name)
                self.assertEqual(t.pssz_to_json(data, "ijson"), row["ijson"], name)
                count["valueBytesToText"] += 2
                # The value <-> text functions agree with the byte path.
                value = t.from_pssz(data)
                self.assertEqual(t.to_pssz(t.from_json(row["json"])), data, name)
                self.assertEqual(t.to_pssz(t.from_ijson(row["ijson"])), data, name)
                self.assertEqual(t.to_ijson(value), row["ijson"], name)
                self.assertEqual(t.to_json(value), row["json"], name)
                self.assertTrue(t.validate_json(row["json"]).ok and t.validate_ijson(row["ijson"]).ok, name)
                # Text given as UTF-8 bytes reads the same.
                self.assertEqual(t.json_to_pssz(row["ijson"].encode("utf-8"), "ijson"), data, name)

                # A type holding a custom text form.  With NO handler the
                # schema path writes the wire type's own text (the row's
                # plain_*) and reads it back to the same bytes; with handlers
                # required it refuses.
                self.assertEqual("plain_json" in row, info.get("custom") is True, name)
                self.assertEqual("plain_ijson" in row, info.get("custom") is True, name)
                if info.get("custom"):
                    plain = schema.type(row["type"])
                    self.assertEqual(plain.pssz_to_json(data, "json"), row["plain_json"], name)
                    self.assertEqual(plain.pssz_to_json(data, "ijson"), row["plain_ijson"], name)
                    self.assertEqual(plain.json_to_pssz(row["plain_json"], "json"), data, name)
                    self.assertEqual(plain.json_to_pssz(row["plain_ijson"], "ijson"), data, name)
                    # The binary side does not depend on handlers.
                    self.assertEqual(plain.to_pssz(plain.from_pssz(data)), data, name)
                    count["plainValue"] += 1
                    required = strict.type(row["type"])
                    for fmt in ("json", "ijson"):
                        # (The bytes are refused first; then the value's text.)
                        self.assertEqual(self.rejects(lambda: required.pssz_to_json(data, fmt), name).reason,
                                         NO_HANDLER, name)
                        write = self.rejects(lambda: required.to_json_checked(plain.from_pssz(data), fmt), name)
                        self.assertEqual((write.kind, write.reason), ("value", NO_HANDLER), name)
                        read = self.rejects(lambda: required.json_to_pssz(row[fmt], fmt), name)
                        self.assertEqual((read.kind, read.reason), ("json", NO_HANDLER), name)
                    # A strict registry applies on the binary entry points as
                    # well: wherever a value of a custom type occurs (an empty
                    # map keyed by one holds none).
                    refused = required.validate_pssz(data)
                    if refused.ok:
                        self.assertEqual(required.to_pssz(required.from_pssz(data)), data, name)
                    else:
                        self.assertEqual((refused.error.kind, refused.reason), ("bytes", NO_HANDLER), name)
                        self.assertEqual(self.rejects(lambda: required.from_pssz(data), name).kind, "bytes", name)
                        unwritten = self.rejects(lambda: required.to_pssz_checked(plain.from_pssz(data)), name)
                        self.assertEqual((unwritten.kind, unwritten.reason), ("value", NO_HANDLER), name)
                        count["strictBinaryRefused"] += 1
                    count["strictRefused"] += 1
            elif kind == "accept":
                data = t.json_to_pssz(input_of(row), row["format"])
                if "pssz" in row:
                    self.assertEqual(data.hex(), row["pssz"], name)
                self.assertEqual(t.pssz_to_json(data, row["format"]), row["canonical"], name)
                self.assertTrue(t.validate_json(input_of(row), row["format"]).ok, name)
            elif kind == "invariant":
                # Well-formed text of the type whose value violates an
                # invariant.  With the handlers every entry point, text and
                # binary, reports the invariant's own message; with none the
                # value is its wire type; a strict registry refuses the type.
                self.assertIs(info.get("invariant"), True, name)
                fmt = row["format"]
                data = bytes.fromhex(row["pssz"])
                read = self.rejects(lambda: t.json_to_pssz(row["text"], fmt), f"{name}: text")
                self.assertEqual((read.reason, read.kind, read.invariant), (row["reason"], "json", True), name)
                self.assertEqual(self.rejects(lambda: t.from_json(row["text"], fmt), name).reason, row["reason"], name)
                st = t.validate_json(row["text"], fmt)
                self.assertTrue(not st.ok and st.reason == row["reason"], name)
                for f in ("json", "ijson"):
                    self.assertEqual(self.rejects(lambda: t.pssz_to_json(data, f), name).reason, row["reason"], name)
                refused = t.validate_pssz(data)
                self.assertFalse(refused.ok, name)
                self.assertEqual((refused.reason, refused.error.invariant), (row["reason"], True), name)
                self.assertEqual(self.rejects(lambda: t.from_pssz(data), name).reason, row["reason"], name)
                # No handler: valid bytes, the wire type's texts, each reading back.
                plain = schema.type(row["type"])
                st = plain.validate_pssz(data)
                self.assertTrue(st.ok and not st.has_unknown, name)
                self.assertEqual(plain.pssz_to_json(data, "json"), row["plain_json"], name)
                self.assertEqual(plain.pssz_to_json(data, "ijson"), row["plain_ijson"], name)
                self.assertEqual(plain.json_to_pssz(row["plain_json"], "json").hex(), row["pssz"], name)
                self.assertEqual(plain.json_to_pssz(row["plain_ijson"], "ijson").hex(), row["pssz"], name)
                # Writers refuse the value too: binary and text.  (The value
                # exists only unchecked: the trusted decode gives it.)
                value = t.from_pssz(psio.trusted(data))
                written = self.rejects(lambda: t.to_pssz_checked(value), f"{name}: to_pssz")
                self.assertEqual((written.reason, written.kind), (row["reason"], "value"), name)
                for f in ("json", "ijson"):
                    text = self.rejects(lambda: t.to_json_checked(value, f), f"{name}: value to {f}")
                    self.assertEqual((text.reason, text.kind), (row["reason"], "value"), name)
                required = strict.type(row["type"]).validate_pssz(data)
                self.assertTrue(not required.ok and required.reason == NO_HANDLER, name)
            else:
                self.assertEqual(kind, "reject", name)
                error = self.rejects(lambda: t.json_to_pssz(input_of(row), row["format"]), name)
                self.assertIn(error.kind, ("json", "value"), f"{name}: {error.kind} {error}")
                self.assertFalse(t.validate_json(input_of(row), row["format"]).ok, name)

        print(f"json-vectors.json ({path}): types={len(fx['types'])} vectors={len(fx['vectors'])} {dict(count)}")
        # Every expressible type is exercised, and every vector is accounted for.
        for row in fx["types"]:
            self.assertTrue(row["ir"] and row["id"] in covered, row["id"])
        self.assertEqual(sum(count[k] for k in KINDS), len(fx["vectors"]))
        # Every type an invariant constrains has `invariant` rows, and only those.
        for row in fx["types"]:
            self.assertEqual(any(v["type"] == row["id"] and v["kind"] == "invariant" for v in fx["vectors"]),
                             row.get("invariant") is True, row["id"])
        self.assertEqual({k: count[k] for k in KINDS}, KINDS)
        self.assertTrue(count["plainValue"] >= 14 and count["strictRefused"] == count["plainValue"])
        self.assertTrue(30 <= count["strictBinaryRefused"] <= count["strictRefused"])
        self.assertEqual(len(fx["types"]), TYPES)
        self.assertEqual(len(fx["vectors"]), VECTORS)

    def test_schema_driven(self) -> None:
        self.replay("schema")

    def test_typed(self) -> None:
        self.replay("typed")

    def test_malformed_schemas(self) -> None:
        """Malformed schemas are refused at every schema-driven entry point; their neighbours work."""
        fx = fixture("json-vectors.json")
        names = set()
        for row in fx["schema_rejects"]:
            name = row["name"]
            self.assertNotIn(name, names)
            names.add(name)
            # The IR itself is a valid Schema value: it is its meaning that is refused.
            schema = psio.Schema.from_pssz(bytes.fromhex(row["schema_ir"]))
            self.assertEqual(schema.to_pssz().hex(), row["schema_ir"], name)
            for width in ("auto", 1, 2, 4):
                e = self.rejects(lambda: schema.type(row["root"], width=width), f"{name}: type({width})")
                self.assertEqual(e.kind, "schema", f"{name}: {e}")
            self.rejects(lambda: schema.with_handlers(handlers={}, require_handlers=True).type(row["root"]), name)
        typed_accepts = 0
        for row in fx["schema_accepts"]:
            name = row["name"]
            self.assertNotIn(name, names)
            names.add(name)
            t = psio.Schema.from_pssz(bytes.fromhex(row["schema_ir"])).type(row["root"])
            data = bytes.fromhex(row["pssz"])
            st = t.validate_pssz(data)
            self.assertTrue(st.ok and not st.has_unknown, f"{name}: {st.reason}")
            self.assertEqual(t.pssz_to_json(data, "json"), row["json"], f"{name}: json")
            self.assertEqual(t.pssz_to_json(data, "ijson"), row["ijson"], f"{name}: ijson")
            self.assertEqual(t.json_to_pssz(row["json"], "json").hex(), row["pssz"], f"{name}: json read")
            self.assertEqual(t.json_to_pssz(row["ijson"], "ijson").hex(), row["pssz"], f"{name}: ijson read")
            self.assertEqual(t.to_pssz(t.from_pssz(data)).hex(), row["pssz"], f"{name}: value")
            # The same row through Python declarations of the row's types.
            typed = TypedFixture(psio.Schema.from_pssz(bytes.fromhex(row["schema_ir"])).ir).type(row["root"])
            st = typed.validate_pssz(data)
            self.assertTrue(st.ok and not st.has_unknown, f"{name}: typed: {st.reason}")
            self.assertEqual(typed.pssz_to_json(data, "json"), row["json"], f"{name}: typed json")
            self.assertEqual(typed.pssz_to_json(data, "ijson"), row["ijson"], f"{name}: typed ijson")
            self.assertEqual(typed.json_to_pssz(row["json"], "json").hex(), row["pssz"], f"{name}: typed json read")
            self.assertEqual(typed.json_to_pssz(row["ijson"], "ijson").hex(), row["pssz"], f"{name}: typed ijson read")
            value = typed.from_pssz(data)
            self.assertEqual(typed.to_pssz(value).hex(), row["pssz"], f"{name}: typed value")
            self.assertEqual(typed.to_pssz(typed.from_pssz(psio.trusted(data))).hex(), row["pssz"], f"{name}: trusted")
            self.assertEqual(typed.from_json(typed.to_json(value)), value, f"{name}: typed text")
            typed_accepts += 1
        print(f"json-vectors.json schema_accepts: rows={len(fx['schema_accepts'])} schema-driven={len(fx['schema_accepts'])} "
              f"typed={typed_accepts}")
        self.assertEqual(typed_accepts, SCHEMA_ACCEPTS)
        self.assertEqual(len(fx["schema_rejects"]), SCHEMA_REJECTS)
        self.assertEqual(len(fx["schema_accepts"]), SCHEMA_ACCEPTS)

    def test_ir_version(self) -> None:
        """The ir_version marker is required and backward compatible."""
        fx = fixture("json-vectors.json")
        current = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        self.assertEqual(current.ir["ir_version"], psio.IR_VERSION)
        self.assertEqual(psio.IR_VERSION, 1)
        id = next(t["id"] for t in fx["types"] if t["ir"] and t["pssz"])
        current.type(id)
        u8 = {"Int": {"bits": 8, "isSigned": False}}
        self.assertEqual(psio.Schema({"T": u8}).ir["ir_version"], 1)
        members = [{"name": "T", "type": u8}]
        self.assertEqual(psio.Schema({"types": members, "ir_version": 1}).ir["ir_version"], 1)
        self.assertIsNone(psio.Schema({"types": members}).ir["ir_version"])
        for bad in (-1, 1.5, "1"):
            self.rejects(lambda: psio.Schema({"types": members, "ir_version": bad}), repr(bad))
        # The same schema as it was written before the marker existed.
        old = psio.Schema({**current.ir, "ir_version": None})
        old_bytes = old.to_pssz()
        self.assertNotEqual(old_bytes.hex(), fx["schema_ir"])
        decoded = psio.Schema.from_pssz(old_bytes)
        self.assertIsNone(decoded.ir["ir_version"])
        self.assertEqual(decoded.ir["types"], current.ir["types"])
        self.assertEqual(decoded.to_pssz(), old_bytes)
        with_null = old.to_json("json")
        self.assertTrue(with_null.endswith(',"ir_version":null}'))
        self.assertIsNone(psio.Schema.from_json(with_null, "json").ir["ir_version"])
        text = with_null.replace(',"ir_version":null}', "}")
        self.assertIsNone(psio.Schema.from_json(text, "json").ir["ir_version"])
        self.assertEqual(psio.Schema.from_json(current.to_json("ijson")).ir["ir_version"], 1)
        for schema, what in ((decoded, "no ir_version marker"),
                             (psio.Schema({**current.ir, "ir_version": 2}), "newer than this library"),
                             (psio.Schema({**current.ir, "ir_version": 0}), "newer than this library")):
            for width in ("auto", 1, 2, 4):
                e = self.rejects(lambda: schema.type(id, width=width), what)
                self.assertEqual(e.kind, "schema")
                self.assertIn(what, e.reason)
            self.rejects(lambda: schema.with_handlers(handlers={}).type(id), "with_handlers")
            self.rejects(lambda: schema.type(u8), "inline type")


if __name__ == "__main__":
    unittest.main()
