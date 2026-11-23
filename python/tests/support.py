"""Shared by the conformance tests: the fixtures and the handlers of their custom types.

The text forms and invariants are written by hand from conformance/custom-types.md
(a schema cannot carry them).
"""

from __future__ import annotations

import base64
import binascii
import json
import os
import re
import sys
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

import psio  # noqa: E402
from psio import Handler, Variant  # noqa: E402

CONFORMANCE = os.path.join(HERE, "..", "..", "conformance")


def fixture(name: str) -> Any:
    with open(os.path.join(CONFORMANCE, name), encoding="utf-8") as f:
        return json.load(f)


WIDTHS = {"auto": "auto", "pssz8": 1, "pssz16": 2, "pssz32": 4}

_DECIMAL = re.compile(r"^(0|[1-9][0-9]*)$")


def _decimal(text: str, maximum: int) -> int:
    if not _DECIMAL.match(text) or len(text) > 20 or not text.isascii():
        raise ValueError("not a canonical decimal")
    value = int(text)
    if value > maximum:
        raise ValueError("decimal out of range")
    return value


def _version_from_text(text: str) -> Any:
    parts = text.split(".")
    if len(parts) != 2:
        raise ValueError("version: expected major.minor")
    major, minor = (_decimal(p, 65535) if len(p) <= 5 else _decimal("x", 0) for p in parts)
    return {"major": major, "minor": minor}


ALPHABET = ".12345abcdefghijklmnopqrstuvwxyz"


def _account_to_text(v: int) -> str:
    return "".join(ALPHABET[v >> (5 * (11 - i)) & 31] for i in range(12)).rstrip(".")


def _account_from_text(text: str) -> int:
    if len(text) > 12 or text.endswith("."):
        raise ValueError("account: too long or trailing dot")
    v = 0
    for i in range(12):
        digit = 0
        if i < len(text):
            digit = ALPHABET.find(text[i])
            if digit < 0:
                raise ValueError("account: bad character")
        v = v << 5 | digit
    return v


def _blob_from_text(text: str) -> bytes:
    if len(text) % 4 or not text.isascii():
        raise ValueError("base64: length is not a multiple of 4")
    try:
        out = base64.b64decode(text, validate=True)
    except binascii.Error as e:
        raise ValueError(f"base64: {e}") from None
    # Exact: the bits a padded group does not use are zero, padding only at the end.
    if base64.b64encode(out).decode("ascii") != text:
        raise ValueError("base64: not the canonical text")
    return out


def _endpoint_to_text(v: Variant) -> str:
    a, port = v.value["address"], v.value["port"]
    if v.type == "V4":
        return f"{a[0]}.{a[1]}.{a[2]}.{a[3]}:{port}"
    groups = ":".join(f"{a[2 * i] << 8 | a[2 * i + 1]:04x}" for i in range(8))
    return f"[{groups}]:{port}"


_V6 = re.compile(r"^\[([0-9a-f]{4}(?::[0-9a-f]{4}){7})\]:(.*)$", re.S)


def _endpoint_from_text(text: str) -> Variant:
    if text.startswith("["):
        m = _V6.match(text)
        if not m:
            raise ValueError("endpoint: bad V6")
        address = bytes.fromhex(m.group(1).replace(":", ""))
        return Variant("V6", {"address": address, "port": _decimal(m.group(2), 65535)})
    host, colon, port = text.rpartition(":")
    if not colon:
        raise ValueError("endpoint: no port")
    octets = host.split(".")
    if len(octets) != 4:
        raise ValueError("endpoint: bad V4")
    return Variant("V4", {"address": bytes(_decimal(o, 255) for o in octets), "port": _decimal(port, 65535)})


def _ratio_from_text(text: str) -> Any:
    n, slash, d = text.partition("/")
    if not slash:
        raise ValueError("ratio: no slash")
    negative = n.startswith("-")
    magnitude = _decimal(n[1:] if negative else n, 32768 if negative else 32767)
    if negative and magnitude == 0:
        raise ValueError("ratio: -0")
    return (-magnitude if negative else magnitude, _decimal(d, 65535))


def _ref_to_text(v: Variant) -> str:
    if v.type == "uint64":
        return f"#{v.value}"
    return "@" + v.value.decode("utf-8")


def _ref_from_text(text: str) -> Variant:
    if text.startswith("#"):
        return Variant("uint64", _decimal(text[1:], (1 << 64) - 1))
    if text.startswith("@"):
        return Variant("string", text[1:].encode("utf-8"))
    raise ValueError("ref: neither #n nor @name")


def _der_check(b: bytes) -> None:
    # Raised rather than returned: both spellings are the invariant's message.
    if len(b) == 0 or b[0] != 0x30:
        raise ValueError("DerKey: not a DER sequence")


CUSTOM_TYPE_HANDLERS = {
    "Blob": Handler(to_text=lambda b: base64.b64encode(b).decode("ascii"), from_text=_blob_from_text),
    "Endpoint": Handler(to_text=_endpoint_to_text, from_text=_endpoint_from_text),
    "Ratio": Handler(to_text=lambda v: f"{v[0]}/{v[1]}", from_text=_ratio_from_text),
    "Ref": Handler(to_text=_ref_to_text, from_text=_ref_from_text,
                   check=lambda v: "Ref: a name is not empty" if v.type == "string" and len(v.value) == 0 else None),
    "Name": Handler(check=lambda v: None if v["value"] >> 60 == 0 else "Name: value is not a canonical name"),
    "DerKey": Handler(check=_der_check),
    "Range": Handler(check=lambda r: None if r["lo"] <= r["hi"] else "Range: lo is greater than hi"),
    "Level": Handler(check=lambda v: None if v <= 3 else "Level: not a listed level"),
}

#: The fixtures' registry: every custom id their Schema IRs use.
FIXTURE_HANDLERS = {
    "Version": Handler(to_text=lambda v: f"{v['major']}.{v['minor']}", from_text=_version_from_text),
    "AccountName": Handler(to_text=_account_to_text, from_text=_account_from_text),
    **CUSTOM_TYPE_HANDLERS,
}

#: Reasons that say "a length bound was exceeded".  The Schema IR records a
#: bound, not how it was declared (a member annotation, `psio::bounded`,
#: `psio::utf8_string<N>`), so a schema-driven reader cannot tell these apart.
BOUND_REASONS = frozenset((
    "pssz: member length_bound violated", "pssz: bounded list exceeds its bound",
    "pssz: utf8_string exceeds its bound",
))


def same_reason(expected: str, actual: str) -> bool:
    return expected == actual or (expected in BOUND_REASONS and actual in BOUND_REASONS)


def without_bounds(ir: Any, raw: bool) -> "psio.Schema":
    """The schema with every bound taken out (bit types aside); ``raw``: UTF-8 strings are raw strings too."""
    def walk(x: Any) -> Any:
        if isinstance(x, list):
            return [walk(i) for i in x if not (isinstance(i, dict) and i.get("name") == "lengthBound"
                                               and isinstance(i.get("value"), str))]
        if isinstance(x, Variant):
            if x.type == "Custom" and x.value["id"] in ("bitlist", "bitvector"):
                return x
            if x.type == "BoundedList":
                return Variant("List", {"type": walk(x.value["type"])})
            body = walk(x.value)
            if raw and x.type == "Custom" and x.value["id"] == "utf8":
                body["id"] = "string"
            return Variant(x.type, body)
        if isinstance(x, dict):
            return {k: walk(v) for k, v in x.items()}
        return x
    return psio.Schema(walk(ir))
