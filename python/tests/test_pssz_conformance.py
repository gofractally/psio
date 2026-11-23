"""conformance/pssz-vectors.json: every type and every vector, nothing skipped.

Replayed on the schema-driven path (the Schema IR recorded in the fixture and
nothing else) and on the typed path (Python declarations of the fixture
types: ``typed_fixture.py``).  The totals at the end must add up to the
fixture's size.
"""

from __future__ import annotations

import collections
import unittest
from typing import Any, Dict

import support
from support import BOUND_REASONS, FIXTURE_HANDLERS, WIDTHS, fixture, same_reason, without_bounds

import psio
from psio import PsioError

# The classes of pssz-vectors.json and their sizes, as conformance/pssz-vectors.md
# lists them: every one is replayed in full, and a fixture that changes must be
# looked at.
PSSZ_CLASSES = {
    "corpus": 956, "evolution-forward": 660, "evolution-backward": 1380, "pruning": 212, "malformed": 419,
    "optional-presence": 400, "width": 335, "cap-overflow-explicit-nested": 15, "tuple": 60, "variant": 48,
    "enum": 76, "enum-unknown-value": 36, "string": 308, "string-raw-non-utf8": 48,
    "length-bound-overflow": 32, "box": 100, "bytes": 103, "zero-fixed": 100, "depth": 28, "nested": 252,
    "nested-unknown": 20, "nested-deferred": 24, "map-set": 266, "map-set-order": 352, "transparent": 160,
    "custom": 116, "invariant": 128, "annotated-bounds": 148, "method-args": 62, "list-width": 89,
    "cap-implicit": 40, "transparent-bounds": 62,
}
TYPES = 360
VECTORS = 7035
NO_HANDLER = "pssz IR: no handler registered for a custom type"
CAP = "pssz: encoded size exceeds maxDynamicData cap"
KEY_BOUND = ("key: list exceeds its bound", "key: string exceeds its bound", "key: bytes exceed their bound")
TEXT_BOUND = ("json: array exceeds its bound", "json: string exceeds its bound", "key: string exceeds its bound")


def has_some_none(value: Any) -> bool:
    """JSON has one null: Some(None) of a nested optional reads back as None."""
    if isinstance(value, psio.Some):
        return value.value is None or has_some_none(value.value)
    if isinstance(value, psio.Variant):
        return has_some_none(value.value)
    if isinstance(value, dict):
        return any(has_some_none(k) or has_some_none(v) for k, v in value.items())
    if isinstance(value, (list, tuple, frozenset, set)):
        return any(has_some_none(v) for v in value)
    if hasattr(value, "__dataclass_fields__"):
        return any(has_some_none(getattr(value, f)) for f in value.__dataclass_fields__)
    return False


class SchemaSource:
    """The schema-driven path: the fixture's IR under one registry."""

    def __init__(self, schema: psio.Schema) -> None:
        self.schema = schema

    def type(self, id: str, **options: Any) -> psio.SchemaType:
        return self.schema.type(id, **options)


def sources(path: str, raw_ir: bytes) -> Dict[str, Any]:
    """Three registries (conformance/custom-types.md): none, the fixture's handlers, a strict one lacking them."""
    schema = psio.Schema.from_pssz(raw_ir)
    if path == "schema":
        return {
            "schema": schema,
            "plain": SchemaSource(schema),
            "handled": SchemaSource(schema.with_handlers(handlers=FIXTURE_HANDLERS)),
            "strict": SchemaSource(psio.Schema.from_pssz(raw_ir, require_handlers=True)),
        }
    import typed_fixture
    return typed_fixture.sources(schema)


class PsszFixture(unittest.TestCase):
    maxDiff = None

    def rejects(self, run: Any, name: str) -> PsioError:
        try:
            run()
        except PsioError as e:
            self.assertNotEqual(e.kind, "internal", f"{name}: {e}")
            return e
        self.fail(f"{name}: accepted")

    def replay(self, path: str) -> None:
        fx = fixture("pssz-vectors.json")
        raw_ir = bytes.fromhex(fx["schema_ir"])
        src = sources(path, raw_ir)
        schema: psio.Schema = src["schema"]
        source, handled, strict = src["plain"], src["handled"], src["strict"]
        # The IR travels as its own PSSZ encoding and re-encodes to the same bytes.
        self.assertEqual(schema.to_pssz().hex(), fx["schema_ir"])

        # The manifest: for EVERY type the derived automatic width, size bound,
        # cap and fixedness are the typed C++ codec's.
        self.assertEqual([t["id"] for t in fx["types"] if not t["ir"]], [])
        by_name = {m["name"]: m["type"] for m in schema.ir["types"]}
        manifest = {t["id"]: t for t in fx["types"]}
        # max_encoded_size is an upper bound of the encoding (spec 3.7): no
        # canonical frame of the automatic profile, of any fixture type, is
        # longer than the number computed for its type (which is the
        # fixture's, below).  Some vectors sit exactly on it.
        largest_of = {row["id"]: source.type(row["id"]).max_encoded_size for row in fx["types"]}
        under = at_maximum = 0
        for v in fx["vectors"]:
            if v["format"] != "auto" or v["expect"] != "canonical":
                continue
            largest = largest_of[v["type"]]
            if largest is None:
                continue
            size = len(v["hex"]) // 2
            self.assertLessEqual(size, largest, f"{v['name']}: longer than max_encoded_size")
            under += 1
            at_maximum += size == largest
        self.assertGreater(under, 300)
        self.assertGreater(at_maximum, 0)
        self.assertGreater(sum(1 for n in largest_of.values() if n is not None), 140)

        custom_ids = 0
        for row in fx["types"]:
            for label, s in (("plain", source), ("handlers", handled)):
                t = s.type(row["id"])
                self.assertEqual(t.width, row["auto_width"], f"auto width of {row['id']} ({label})")
            t = source.type(row["id"])
            self.assertEqual(t.max_dynamic_data, row["max_dynamic_data"], row["id"])
            # (the inferred bound of spec 3.2: a cap, also an argument list's, is not part of it)
            self.assertEqual(t.inferred_bound, row["inferred_bound"], f"inferred_bound of {row['id']}")
            # The largest encoding (spec 3.7): a capped type never exceeds its
            # cap, a type with no cap never exceeds its inferred bound.
            largest = t.max_encoded_size
            self.assertEqual(largest, row["max_encoded_size"], f"max_encoded_size of {row['id']}")
            self.assertEqual((handled.type(row["id"]).inferred_bound, handled.type(row["id"]).max_encoded_size),
                             (t.inferred_bound, largest), row["id"])
            if t.max_dynamic_data is not None:
                self.assertTrue(largest is not None and largest <= t.max_dynamic_data, row["id"])
            else:
                self.assertEqual(largest is None, t.inferred_bound is None, row["id"])
                self.assertTrue(largest is None or largest <= t.inferred_bound, row["id"])
            self.assertEqual(t.fixed_size is not None, row["fixed"], row["id"])
            self.assertEqual(t.fixed_size or 0, row["fixed_size"], row["id"])
            node = by_name[row["id"]]["value"]
            for _ in range(64):
                if node.type != "Type":
                    break
                node = by_name[node.value["type"]]["value"]
            is_custom = node.type == "Custom" and node.value["id"] in FIXTURE_HANDLERS
            self.assertEqual(is_custom, "custom_id" in row, f"custom_id of {row['id']}")
            if "custom_id" in row:
                self.assertEqual(node.value["id"], row["custom_id"])
                custom_ids += 1
        self.assertEqual(custom_ids, 8)
        # A box gives its container no inferred bound; an explicit cap still does.
        self.assertEqual(
            [source.type(i).width for i in ("BDirect", "BoxRec", "BoxCapRec", "BoxVecCapRec", "box<BLeaf>",
                                            "optional<BoxCapRec>")],
            [1, 4, 1, 2, 4, 1])

        types: Dict[Any, Any] = {}

        def type_of(tag: str, id: str, width: Any, nested_bytes: str = "typed") -> Any:
            key = (tag, nested_bytes, width, id)
            t = types.get(key)
            if t is None:
                options = {"width": width}
                if nested_bytes != "typed":
                    options["nested_bytes"] = nested_bytes
                t = types[key] = {"plain": source, "handled": handled, "strict": strict}[tag].type(id, **options)
            return t

        unbounded = without_bounds(schema.ir, False)
        unbounded_raw = without_bounds(schema.ir, True)
        count: Dict[str, collections.Counter] = {"auto": collections.Counter(), "explicit": collections.Counter()}
        classes: collections.Counter = collections.Counter()
        row_by_name = {v["name"]: v for v in fx["vectors"]}
        self.assertEqual(len(row_by_name), len(fx["vectors"]), "vector names are unique")
        reasons_exact = reasons_family = 0

        for row in fx["vectors"]:
            name = row["name"]
            data = bytes.fromhex(row["hex"])
            cls = name[:name.index("/")]
            self.assertEqual(row["class"], cls, name)
            automatic = row["format"] == "auto"
            width = WIDTHS[row["format"]]
            c = count["auto" if automatic else "explicit"]
            c["vectors"] += 1
            classes[cls] += 1
            self.assertEqual(row["reason"] != "", row["expect"] == "reject", f"{name}: reason")

            # ── Class `invariant`: a structurally valid frame of a value that
            # violates an invariant.  `reject` is the typed outcome, and that
            # of a schema-driven codec given the handlers, under every width
            # profile; with no handler the frame is its wire type; a strict
            # registry that lacks the handler refuses the type.
            if cls == "invariant":
                self.assertEqual(row["expect"], "reject", name)
                self.assertIs(manifest[row["type"]].get("invariant"), True, f"{name}: manifest invariant")
                plain = type_of("plain", row["type"], width)
                st = plain.validate_pssz(data)
                self.assertTrue(st.ok and not st.has_unknown, f"{name}: no handler: {st.reason}")
                value = plain.from_pssz(data)
                self.assertEqual(plain.to_pssz(value).hex(), row["hex"], f"{name}: no handler, re-encode")
                checked = type_of("handled", row["type"], width)
                refused = checked.validate_pssz(data)
                self.assertFalse(refused.ok, f"{name}: accepted with the handlers")
                self.assertEqual(refused.reason, row["reason"], name)
                self.assertEqual(refused.error.kind, "bytes", name)
                self.assertTrue(refused.error.invariant, name)
                self.assertEqual(self.rejects(lambda: checked.from_pssz(data), name).reason, row["reason"], name)
                # The trusted decode reads the value.
                trusted_value = checked.from_pssz(psio.trusted(data))
                self.assertEqual(plain.to_pssz(plain.from_pssz(data)), data, name)
                # A view does not check; decoding through it does.
                view = checked.pssz_view(data)
                if isinstance(view, psio.View):
                    self.assertEqual(self.rejects(view.to_value, name).reason, row["reason"], f"{name}: view")
                # No writer writes the value back: binary, key and text.
                written = self.rejects(lambda: checked.to_pssz(trusted_value), f"{name}: to_pssz")
                self.assertEqual(written.reason, row["reason"], name)
                self.assertEqual(written.kind, "value", name)
                self.assertTrue(written.invariant, name)
                if automatic:
                    for fmt in ("json", "ijson"):
                        self.assertEqual(self.rejects(lambda: checked.pssz_to_json(data, fmt), name).reason,
                                         row["reason"], f"{name}: {fmt}")
                        self.assertEqual(self.rejects(lambda: checked.to_json(trusted_value, fmt), name).reason,
                                         row["reason"], f"{name}: {fmt} value")
                        # The wire type's text reads back to the frame.
                        text = plain.pssz_to_json(data, fmt)
                        self.assertEqual(plain.json_to_pssz(text, fmt).hex(), row["hex"], f"{name}: plain {fmt}")
                required = type_of("strict", row["type"], width).validate_pssz(data)
                self.assertFalse(required.ok, f"{name}: accepted by a strict registry")
                self.assertEqual(required.reason, NO_HANDLER, name)
                self.assertFalse(required.error.invariant, name)
                c["invariant"] += 1
                continue

            # A typed nested<T,deferred> does not look inside its bytes.  The
            # schema does not record that policy, so the schema-driven reader
            # gives the recorded outcome when told the nested bytes are
            # opaque; the typed declarations say `deferred` themselves.
            deferred = cls == "nested-deferred"
            mode = "opaque" if deferred and path == "schema" else "typed"
            t = type_of("plain", row["type"], width, mode)
            result = t.validate_pssz(data)
            if not result.ok:
                self.assertNotEqual(result.error.kind, "internal", f"{name}: {result.reason}")
            # The handlers change nothing for a value that satisfies its
            # invariants, or for a type that has none.
            with_handlers = type_of("handled", row["type"], width, mode)
            self.assertEqual(with_handlers.validate_pssz(data).ok, result.ok, f"{name}: with handlers")

            if deferred:
                # Any inner bytes (valid, garbage, empty, unknown fields) are
                # canonical, and kept as they are.
                self.assertEqual(row["expect"], "canonical", name)
                self.assertTrue(result.ok and not result.has_unknown, name)
                self.assertEqual(t.to_pssz(t.from_pssz(data)).hex(), row["hex"], name)
                self.assertEqual(t.to_pssz(t.from_pssz(data, reject_unknown=True)).hex(), row["hex"], name)
                # The same through the option alone, on both paths.
                opaque = type_of("plain", row["type"], width, "opaque")
                st = opaque.validate_pssz(data)
                self.assertTrue(st.ok and not st.has_unknown, name)
                self.assertEqual(opaque.to_pssz(opaque.from_pssz(data)).hex(), row["hex"], name)
                c["nestedDeferred"] += 1
                c["validateAccept"] += 1
                c["canonicalReencode"] += 1
                continue

            if cls == "nested-unknown" and row["expect"] == "unknown":
                # Unknown fields inside nested bytes.  The value of a nested
                # node is the inner value, so the default re-encoding is
                # canonical: the frame of the `nested` vector holding the same
                # logical value for this reader.  With the bytes opaque the
                # frame is kept, which is what the typed C++ reader's
                # re-encoding (`reencoded`) is.
                self.assertTrue(result.ok and result.has_unknown, name)
                self.assertFalse(t.validate_pssz(data, strict=True).ok, name)
                self.rejects(lambda: t.from_pssz(data, reject_unknown=True), name)
                again = t.to_pssz(t.from_pssz(data))
                self.assertNotEqual(again.hex(), row["hex"], name)
                st = t.validate_pssz(again)
                self.assertTrue(st.ok and not st.has_unknown, name)
                self.assertEqual(t.to_pssz(t.from_pssz(again, reject_unknown=True)), again, name)
                if row["value"] != "":
                    same = row_by_name.get(f"nested/{row['value']}/as/{row['type']}/{row['format']}")
                    self.assertTrue(same and same["expect"] == "canonical", f"{name}: no nested vector of the value")
                    self.assertEqual(again.hex(), same["hex"], name)
                opaque = type_of("plain", row["type"], width, "opaque")
                st = opaque.validate_pssz(data)
                self.assertTrue(st.ok and not st.has_unknown, name)
                kept = opaque.to_pssz(opaque.from_pssz(data)).hex()
                self.assertEqual(kept, row["reencoded"], name)
                self.assertEqual(kept, row["hex"], name)
                c["nestedUnknown"] += 1
                c["validateAccept"] += 1
                c["unknownReencode"] += 1
                continue

            # Non-UTF-8 bytes in a raw string are valid PSSZ.
            if cls == "string-raw-non-utf8":
                self.assertEqual(row["expect"], "canonical", name)
            if row["expect"] == "reject":
                self.assertFalse(result.ok, f"{name}: accepted")
                self.assertEqual(result.error.kind, "bytes", name)
                # The reason is the fixture's (the C++ validator's own words).
                self.assertTrue(same_reason(row["reason"], result.reason), f"{name}: {result.reason!r}")
                if row["reason"] == result.reason:
                    reasons_exact += 1
                else:
                    reasons_family += 1
                self.assertEqual(self.rejects(lambda: t.from_pssz(data), name).reason, result.reason, name)
                self.assertFalse(t.validate_pssz(data, strict=True).ok, name)

                def every_reader(expected: Any) -> None:
                    for label, reader in (("plain", t), ("handlers", with_handlers)):
                        n = f"{name}: {label}"
                        refused = reader.validate_pssz(data)
                        self.assertTrue(not refused.ok and refused.error.kind == "bytes", n)
                        self.assertIn(refused.reason, expected, n)
                        self.assertIn(self.rejects(lambda: reader.from_pssz(data), n).reason, expected, n)
                        self.assertIn(self.rejects(lambda: reader.from_pssz(data, reject_unknown=True), n).reason,
                                      expected, n)
                        # The trusted decode does not check bounds or caps
                        # (the caller vouches for the bytes): it reads the
                        # value the frame holds, or fails as a decode error.
                        try:
                            reader.from_pssz(psio.trusted(data))
                            c["trustedReadOutside"] += 1 if label == "plain" else 0
                        except PsioError as e:
                            self.assertEqual(e.kind, "bytes", f"{n}: trusted: {e}")
                        for fmt in ("json", "ijson"):
                            self.assertIn(self.rejects(lambda: reader.pssz_to_json(data, fmt), n).reason, expected, n)
                        view = reader.pssz_view(data)
                        self.assertIsInstance(view, psio.View, n)
                        self.assertIn(self.rejects(view.to_value, n).reason, expected, f"{n}: view")

                if cls == "length-bound-overflow":
                    self.assertEqual(row["reason"], "pssz: member length_bound violated", name)
                    every_reader(BOUND_REASONS)
                    c["lengthBoundReject"] += 1
                # A value outside a bound stated below the member, a method
                # argument outside its parameter's bounds, and arguments over
                # the cap.  Every reader refuses the frame for the reason
                # recorded; then every writer and every text / key reader
                # refuses the value the frame holds.
                if cls in ("annotated-bounds", "method-args"):
                    not_utf8 = row["reason"].startswith("utf8_spec:")
                    every_reader(BOUND_REASONS if row["reason"] in BOUND_REASONS else (row["reason"],))
                    c["treeReject"] += 1
                    wide = row_by_name.get(name[:name.rindex("/") + 1] + "pssz32")
                    if row["reason"] == CAP:
                        self.assertEqual(name, "method-args/note_over_cap/as/tuple<raw_string>@cap=64/auto")
                        c["treeOverCap"] += 1
                    else:
                        # The value, read by the twin that has no bounds.
                        self.assertTrue(wide and wide["expect"] == "reject" and wide["reason"] == row["reason"], name)
                        twin = unbounded_raw if not_utf8 else unbounded
                        loose = twin.type(row["type"], width=4)
                        outside = loose.from_pssz(bytes.fromhex(wide["hex"]))
                        key_bytes = twin.type(row["type"]).to_key(outside)
                        # (the typed readers take typed values: the outside
                        # value exists only in the dynamic model)
                        for reader in ((t, with_handlers) if path == "schema" else ()):
                            key = reader.validate_key(key_bytes)
                            self.assertTrue(not key.ok and key.error.kind == "bytes", f"{name}: key accepted")
                            if not_utf8:
                                self.assertEqual(key.reason, "key: string is not valid UTF-8", name)
                                # A Python str is always text: there is no value
                                # of a UTF-8 position that is not UTF-8 to write.
                                continue
                            self.assertIn(key.reason, KEY_BOUND, name)
                            written = self.rejects(lambda: reader.to_pssz(outside), f"{name}: to_pssz")
                            self.assertEqual(written.kind, "value", name)
                            self.assertIn(written.reason, BOUND_REASONS | set(KEY_BOUND), name)
                            kw = self.rejects(lambda: reader.to_key(outside), f"{name}: to_key")
                            self.assertEqual(kw.kind, "value", name)
                            self.assertIn(kw.reason, BOUND_REASONS | set(KEY_BOUND) | {"key: member length bound violated"},
                                          name)
                            for fmt in ("json", "ijson"):
                                text = loose.to_json(outside, fmt)
                                for run in (lambda: reader.to_json(outside, fmt), lambda: reader.from_json(text, fmt),
                                            lambda: reader.json_to_pssz(text, fmt)):
                                    e = self.rejects(run, f"{name}: {fmt}")
                                    self.assertIn(e.kind, ("value", "json"), name)
                                    self.assertIn(e.reason, set(TEXT_BOUND) | BOUND_REASONS
                                                  | {"json: member length bound violated"}, f"{name}: {fmt}")
                        c["treeNotUtf8" if not_utf8 else "treeOutsideValue"] += 1
                        if path == "schema" and not not_utf8:
                            c["treeWriterRefused"] += 1
                c["validateReject"] += 1
                continue

            self.assertTrue(result.ok, f"{name}: {result.reason}")
            self.assertEqual(result.has_unknown, row["expect"] == "unknown", name)
            c["validateAccept"] += 1
            value = t.from_pssz(data)
            again = t.to_pssz(value).hex()
            self.assertEqual(t.pssz_size(value), len(again) // 2, name)
            if row["expect"] == "unknown":
                self.assertEqual(again, row["reencoded"], name)
                self.assertFalse(t.validate_pssz(data, strict=True).ok, name)
                self.rejects(lambda: t.from_pssz(data, reject_unknown=True), name)
                c["unknownReencode"] += 1
                continue
            self.assertEqual(again, row["hex"], name)
            self.assertTrue(t.validate_pssz(data, strict=True).ok, name)
            self.assertEqual(t.to_pssz(t.from_pssz(data, reject_unknown=True)).hex(), row["hex"], name)
            self.assertEqual(t.to_pssz(t.from_pssz(psio.trusted(data))).hex(), row["hex"], f"{name}: trusted")
            self.assertEqual(with_handlers.to_pssz(with_handlers.from_pssz(data)).hex(), row["hex"],
                             f"{name}: with handlers")
            # A view reads the same value.
            view = t.pssz_view(data)
            if isinstance(view, psio.View):
                self.assertEqual(t.to_pssz(view.to_value()).hex(), row["hex"], f"{name}: view")
                c["viewed"] += 1
            c["canonicalReencode"] += 1
            if not automatic:
                continue

            # Bytes -> text -> bytes, in both text formats.
            lossy = has_some_none(value)
            no_text = False
            for fmt in ("ijson", "json"):
                try:
                    text = t.pssz_to_json(data, fmt)
                except PsioError as e:
                    self.assertEqual(e.kind, "value", f"{name}: {e}")
                    if "no JSON" in e.reason:
                        # Only bit types and non-finite floats have no text.
                        self.assertIn(e.reason, ("json: bit types have no JSON form",
                                                 f"{fmt}: non-finite float has no JSON text"), name)
                        no_text = True
                        break
                    # A raw string holding bytes that are not UTF-8 has no text.
                    if e.reason == "json: string is not valid UTF-8":
                        no_text = True
                        if fmt == "ijson":
                            count["auto"]["rawNonUtf8NoText"] += 1
                        break
                    # A string holding a Unicode noncharacter is JSON but not I-JSON.
                    self.assertEqual(fmt, "ijson", name)
                    self.assertEqual(e.reason, "ijson: string holds a Unicode noncharacter", name)
                    count["auto"]["ijsonNoncharacter"] += 1
                    continue
                self.assertEqual(t.json_to_pssz(text, fmt).hex() == row["hex"], not lossy, f"{name} via {fmt}: {text}")
                if not lossy:
                    c["jsonRoundTrip" if fmt == "json" else "ijsonRoundTrip"] += 1
                    # The same through the handlers' text forms.
                    typed_text = with_handlers.pssz_to_json(data, fmt)
                    self.assertEqual(with_handlers.json_to_pssz(typed_text, fmt).hex(), row["hex"],
                                     f"{name} via {fmt}, handlers: {typed_text}")
                    c["handlerTextRoundTrip"] += 1
            if no_text:
                c["noJsonForm"] += 1
            elif lossy:
                c["someNoneLossy"] += 1
            # Every raw non-UTF-8 value is refused by the text writers.
            if cls == "string-raw-non-utf8":
                self.assertTrue(no_text, name)

        a, e = count["auto"], count["explicit"]
        print(f"pssz-vectors.json ({path}): types={len(fx['types'])} vectors={len(fx['vectors'])} "
              f"reasons: exact={reasons_exact} bound-family={reasons_family} auto={dict(a)} explicit={dict(e)}")
        # Every vector was replayed: nothing is skipped.
        self.assertEqual(a["vectors"] + e["vectors"], len(fx["vectors"]))
        self.assertEqual(a["validateAccept"] + a["validateReject"] + a["invariant"], a["vectors"])
        self.assertEqual(a["canonicalReencode"] + a["unknownReencode"], a["validateAccept"])
        self.assertEqual(a["jsonRoundTrip"] + a["someNoneLossy"] + a["noJsonForm"],
                         a["canonicalReencode"] - a["nestedDeferred"])
        self.assertEqual(e["validateAccept"] + e["validateReject"] + e["invariant"], e["vectors"])
        self.assertEqual(e["canonicalReencode"] + e["unknownReencode"], e["validateAccept"])
        # The fixture as checked in (a change to it must be looked at).
        self.assertEqual(dict(sorted(classes.items())), dict(sorted(PSSZ_CLASSES.items())))
        self.assertEqual(a["invariant"] + e["invariant"], PSSZ_CLASSES["invariant"])
        self.assertEqual(a["nestedDeferred"] + e["nestedDeferred"], PSSZ_CLASSES["nested-deferred"])
        self.assertEqual(a["lengthBoundReject"] + e["lengthBoundReject"], PSSZ_CLASSES["length-bound-overflow"])
        self.assertEqual(a["nestedUnknown"] + e["nestedUnknown"], 16)
        tree = [v for v in fx["vectors"] if v["class"] in ("annotated-bounds", "method-args") and v["expect"] == "reject"]
        self.assertEqual(len(tree), 109)
        self.assertEqual(a["treeReject"] + e["treeReject"], len(tree))
        self.assertEqual([a["treeOutsideValue"] + e["treeOutsideValue"], a["treeNotUtf8"] + e["treeNotUtf8"],
                          a["treeOverCap"] + e["treeOverCap"]], [96, 12, 1])
        # Every writer refuses the value outside its bounds.  (Schema-driven:
        # the value is read by a twin schema without bounds, which exists
        # only in the dynamic model.  Typed writers: test_typed.py.)
        self.assertEqual(a["treeWriterRefused"] + e["treeWriterRefused"], 96 if path == "schema" else 0)
        self.assertGreaterEqual(a["rawNonUtf8NoText"], 11)
        rejects = sum(1 for v in fx["vectors"] if v["expect"] == "reject" and v["class"] != "invariant")
        self.assertEqual(reasons_exact + reasons_family, rejects)
        # (140, and the 16 length rejects of `transparent-bounds` whose bound is a member annotation in C++)
        self.assertEqual(reasons_family, 156)
        self.assertEqual(len(fx["types"]), TYPES)
        self.assertEqual(len(fx["vectors"]), VECTORS)

    def test_schema_driven(self) -> None:
        self.replay("schema")

    def test_typed(self) -> None:
        self.replay("typed")

    def test_corruption_sweep(self) -> None:
        """Truncations and single-byte corruptions of the valid frames never escape as a non-psio error."""
        fx = fixture("pssz-vectors.json")
        schema = psio.Schema.from_pssz(bytes.fromhex(fx["schema_ir"]))
        frames = 0
        for row in fx["vectors"]:
            if row["expect"] == "reject" or row["format"] != "auto" or row["class"] == "nested-deferred":
                continue
            t = schema.type(row["type"])
            data = bytes.fromhex(row["hex"])
            if len(data) > 96:
                continue
            frames += 1
            for cut in range(len(data)):
                st = t.validate_pssz(data[:cut])
                if st.ok:  # a shorter valid frame re-encodes to itself or reports unknown fields
                    self.assertTrue(st.has_unknown or t.to_pssz(t.from_pssz(data[:cut])) == data[:cut], row["name"])
            for i in range(len(data)):
                for flip in (0x01, 0x80, 0xFF):
                    bad = data[:i] + bytes((data[i] ^ flip,)) + data[i + 1:]
                    st = t.validate_pssz(bad)
                    if st.ok and not st.has_unknown:
                        self.assertEqual(t.to_pssz(t.from_pssz(bad)), bad, f"{row['name']}: byte {i} ^ {flip:#x}")
                    elif not st.ok:
                        self.assertEqual(st.error.kind, "bytes", row["name"])
        self.assertGreater(frames, 1000)


if __name__ == "__main__":
    unittest.main()
