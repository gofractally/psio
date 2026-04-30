#!/usr/bin/env python3
"""
Decompresses consensus-spec-tests ssz_generic fixtures and emits a plain-text
manifest that the C++ test harness can consume without a YAML/snappy
dependency.

Input layout (extracted from general.tar.gz):
    <root>/tests/general/<phase>/ssz_generic/<category>/<valid|invalid>/<name>/
        meta.yaml
        serialized.ssz_snappy
        value.yaml

Output layout (sibling directory `_decoded`):
    <out>/<category>/<valid_or_invalid>/<name>/raw.ssz           (decompressed)
    <out>/manifest.txt                                            (one line per test)

Manifest line format (tab-separated):
    <category>\t<valid|invalid>\t<name>\t<type_tag>\t<value_literal>

value_literal depends on category:
  uints/boolean: a decimal string
  basic_vector: comma-separated elements
  (containers, bitlists, bitvectors are skipped for now — the MVP psio
   SSZ doesn't support those types yet)
"""
import os
import sys
import cramjam
import yaml
import pathlib

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(
    "build/spec-tests/tests/general")
OUT = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else pathlib.Path(
    "build/spec-tests/_decoded")

OUT.mkdir(parents=True, exist_ok=True)
manifest_lines = []


def decompress_snappy(path: pathlib.Path) -> bytes:
    raw = path.read_bytes()
    return bytes(cramjam.snappy.decompress_raw(raw))


def handle_uint(cat: str, valid_or_invalid: str, name: str,
                case_dir: pathlib.Path, out_dir: pathlib.Path):
    ssz = decompress_snappy(case_dir / "serialized.ssz_snappy")
    (out_dir / "raw.ssz").write_bytes(ssz)

    # Parse the type tag out of the test name — uint_N_foo
    parts = name.split("_")
    if len(parts) < 2 or parts[0] != "uint":
        return
    try:
        nbits = int(parts[1])
    except ValueError:
        return
    if nbits not in (8, 16, 32, 64):  # MVP scope
        return

    value = (case_dir / "value.yaml").read_text().strip()
    # value.yaml contains a scalar int (possibly with "..." sentinel).
    # yaml.safe_load handles it.
    parsed = yaml.safe_load(value)
    if not isinstance(parsed, int):
        return
    manifest_lines.append(
        f"uints\t{valid_or_invalid}\t{name}\tuint{nbits}\t{parsed}")


def handle_boolean(cat: str, valid_or_invalid: str, name: str,
                   case_dir: pathlib.Path, out_dir: pathlib.Path):
    ssz = decompress_snappy(case_dir / "serialized.ssz_snappy")
    (out_dir / "raw.ssz").write_bytes(ssz)
    value = (case_dir / "value.yaml").read_text().strip()
    parsed = yaml.safe_load(value)
    if not isinstance(parsed, bool):
        return
    manifest_lines.append(
        f"boolean\t{valid_or_invalid}\t{name}\tbool\t{'1' if parsed else '0'}")


def handle_basic_vector(cat: str, valid_or_invalid: str, name: str,
                        case_dir: pathlib.Path, out_dir: pathlib.Path):
    # Names are `vec_<elem>_<N>_<suffix>`; elem is bool/uintXX.
    parts = name.split("_")
    if len(parts) < 3 or parts[0] != "vec":
        return
    elem = parts[1]
    try:
        n = int(parts[2])
    except ValueError:
        return
    if elem not in ("bool", "uint8", "uint16", "uint32", "uint64"):
        return

    ssz = decompress_snappy(case_dir / "serialized.ssz_snappy")
    (out_dir / "raw.ssz").write_bytes(ssz)

    value = (case_dir / "value.yaml").read_text()
    parsed = yaml.safe_load(value)
    if not isinstance(parsed, list):
        return

    if elem == "bool":
        values_csv = ",".join("1" if v else "0" for v in parsed)
    else:
        values_csv = ",".join(str(v) for v in parsed)

    manifest_lines.append(
        f"basic_vector\t{valid_or_invalid}\t{name}\tvec_{elem}_{n}\t{values_csv}")


HANDLERS = {
    "uints":        handle_uint,
    "boolean":      handle_boolean,
    "basic_vector": handle_basic_vector,
}

for phase_dir in sorted(ROOT.iterdir()):
    if not phase_dir.is_dir():
        continue
    ssz_root = phase_dir / "ssz_generic"
    if not ssz_root.exists():
        continue
    for cat_dir in sorted(ssz_root.iterdir()):
        if not cat_dir.is_dir():
            continue
        cat = cat_dir.name
        handler = HANDLERS.get(cat)
        if handler is None:
            continue
        for status_dir in sorted(cat_dir.iterdir()):
            if not status_dir.is_dir():
                continue
            status = status_dir.name
            for case_dir in sorted(status_dir.iterdir()):
                if not case_dir.is_dir():
                    continue
                name = case_dir.name
                out_case = OUT / cat / status / name
                out_case.mkdir(parents=True, exist_ok=True)
                try:
                    handler(cat, status, name, case_dir, out_case)
                except Exception as e:
                    print(f"SKIP {cat}/{status}/{name}: {e}", file=sys.stderr)

(OUT / "manifest.txt").write_text("\n".join(manifest_lines) + "\n")
print(f"Wrote {len(manifest_lines)} manifest entries to {OUT / 'manifest.txt'}",
      file=sys.stderr)
