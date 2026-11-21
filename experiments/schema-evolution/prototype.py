#!/usr/bin/env python3
"""Experiments only. No production codec or format policy changes.

Consumes the actual C++ repro output and emits the optional-suffix parser and
two fully specified alternative encodings for the same input values.
"""
import json
from pathlib import Path
import sys


def word(n):
    return n.to_bytes(4, "little")


def read_word(data, start):
    if start < 0 or start + 4 > len(data):
        raise ValueError("offset/header underrun")
    return int.from_bytes(data[start:start + 4], "little")


def read_v1_offset_suffix(data):
    """Valid ONLY when all unknown suffix fields are offset-bearing shapes.

    Unknown Some/None payloads are kept opaque. This cannot establish the writer
    schema's compliance with that contract from bytes alone.
    """
    fixed = read_word(data, 0)
    body = data[4:]
    if fixed < 4 or fixed % 4 or fixed > len(body):
        raise ValueError("invalid variable-only fixed region")
    offsets = [read_word(body, at) for at in range(0, fixed, 4)]
    if offsets[0] != fixed or any(a > b for a, b in zip(offsets, offsets[1:])) or offsets[-1] > len(body):
        raise ValueError("invalid variable-only offsets")
    stop = offsets[1] if len(offsets) > 1 else len(body)
    return body[offsets[0]:stop].decode("utf-8")


def read_v2_optional_suffix(data):
    name = read_v1_offset_suffix(data)
    fixed = read_word(data, 0)
    if fixed == 4:
        return {"name": name, "extra": None}
    body = data[4:]
    begin = read_word(body, 4)
    end = read_word(body, 8) if fixed >= 12 else len(body)
    payload = body[begin:end]
    if payload == b"\0":
        return {"name": name, "extra": None}
    if payload[:1] == b"\1":
        return {"name": name, "extra": payload[1:].decode()}
    raise ValueError("invalid optional string payload")


def mixed_record(fields):
    """Current-style 4-byte header, inline fixed values, offset-bearing others."""
    fixed = sum(len(payload) if inline else 4 for inline, payload in fields)
    slots, tail = bytearray(), bytearray()
    for inline, payload in fields:
        if inline:
            slots.extend(payload)
        else:
            slots.extend(word(fixed + len(tail)))
            tail.extend(payload)
    return word(fixed) + slots + tail


def prefixed(text):
    raw = text.encode()
    return word(len(raw)) + raw


def all_spans(fields):
    """u32 field count + N+1 u32 offsets + all field payloads, no inline slots.

    Offsets are relative to the offset-table start. The final offset is the
    terminal boundary; every field, including a fixed scalar, is delimited.
    """
    table = 4 * (len(fields) + 1)
    offsets, tail = [], bytearray()
    for payload in fields:
        offsets.append(table + len(tail))
        tail.extend(payload)
    offsets.append(table + len(tail))
    return word(len(fields)) + b"".join(word(n) for n in offsets) + tail


def run(rows):
    typed = {
        "v1": ("a", None),
        "v2_string": ("a", ("string", "b")),
        "v2_string_default": ("a", ("string", "")),
        "v2_fixed": ("a", ("fixed", 7)),
        "v2_fixed_ambiguous": ("ab", ("fixed", 9)),
        "v2_fixed_default": ("a", ("fixed", 0)),
        "v2_optional_some": ("a", ("optional", "b")),
        "v2_optional_none": ("a", ("optional", None)),
    }
    for row in rows:
        name, extra = typed[row["case"]]
        actual = bytes.fromhex(row["hex"])
        raw_fields = [(False, name.encode())]
        prefixed_fields = [(False, prefixed(name))]
        if extra:
            kind, value = extra
            if kind == "fixed":
                raw_fields.append((True, word(value)))
                prefixed_fields.append((True, word(value)))
            elif kind == "string":
                raw_fields.append((False, value.encode()))
                prefixed_fields.append((False, prefixed(value)))
            else:
                raw_fields.append((False, b"\0" if value is None else b"\1" + value.encode()))
                prefixed_fields.append((False, b"\0" if value is None else b"\1" + prefixed(value)))
        assert mixed_record(raw_fields) == actual, row["case"]
        try:
            suffix = {"name": read_v1_offset_suffix(actual)}
        except ValueError as error:
            suffix = {"error": str(error)}
        prefix_bytes = mixed_record(prefixed_fields)
        prefix_start = 4 + read_word(prefix_bytes, 4)
        prefix_len = read_word(prefix_bytes, prefix_start)
        assert prefix_bytes[prefix_start + 4:prefix_start + 4 + prefix_len].decode() == name
        span_bytes = all_spans([payload for _, payload in raw_fields])
        first, second = read_word(span_bytes, 4), read_word(span_bytes, 8)
        assert span_bytes[4 + first:4 + second].decode() == name
        try:
            new_reader = read_v2_optional_suffix(actual)
        except ValueError as error:
            new_reader = {"error": str(error)}
        pruned = mixed_record(raw_fields[:-1]) if extra == ("optional", None) else actual
        yield {"case": row["case"], "expected_name": name, "current_bytes": len(actual),
               "offset_only_suffix_v1": suffix,
               "offset_only_suffix_v2_optional": new_reader,
               "offset_only_contract_satisfied": extra is None or extra[0] != "fixed",
               "optional_none_pruning_prototype": {"bytes": len(pruned), "hex": pruned.hex(), "v2_read": read_v2_optional_suffix(pruned)} if extra == ("optional", None) else None,
               "length_prefixed": {"bytes": len(prefix_bytes), "hex": prefix_bytes.hex(), "read_v1_name": name},
               "all_field_spans": {"bytes": len(span_bytes), "hex": span_bytes.hex(), "read_v1_name": name}}


if __name__ == "__main__":
    source = Path(sys.argv[1])
    for result in run([json.loads(line) for line in source.read_text().splitlines()]):
        print(json.dumps(result))
