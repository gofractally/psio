#!/usr/bin/env python3
"""Compare the public C++, Rust and JS pjson validators and canonicalizers on deterministic mutations and
random inputs.

The three validators must be built first; the tool does not build them:

    cmake --build <build-dir> --target psio_pjson_validate
    (cd rust && cargo build -p psio --example pjson_validate --features pjson)
    (cd js && npm ci && npm run build)

    python3 tools/check-pjson-validation.py \
        <build-dir>/cpp/conformance/psio_pjson_validate \
        rust/target/debug/examples/pjson_validate [--cases N]

Each validator reads one hex frame per line and prints one line per frame: `invalid` when the bytes are not
a well-formed frame (psio::pjson_lazy), else `valid <c> <hex>`: whether the frame is the canonical one of its
value (psio::pjson) and the canonical frame of that value (`noform` when it has none). So the three
languages are held to the same verdict on both validators and to the SAME BYTES for the canonical form.

The inputs are every pjson row of conformance/cpp-golden.json (and its canonical frame), single-byte
mutations of each, and N random frames (default 10 000; the seed is fixed, so a run is reproducible). The
tool prints the number of cases on which all three agree, and how many were well-formed and canonical, or
the first disagreements and a non-zero exit.
"""
import argparse
import json
from pathlib import Path
import random
import subprocess

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('cpp_validator', type=Path)
parser.add_argument('rust_validator', type=Path)
parser.add_argument('--cases', type=int, default=10000)
args = parser.parse_args()
for label, path, how in [
    ('C++', args.cpp_validator, 'cmake --build <build-dir> --target psio_pjson_validate'),
    ('Rust', args.rust_validator, '(cd rust && cargo build -p psio --example pjson_validate --features pjson)'),
]:
    if not path.is_file():
        raise SystemExit(f'{label} validator not found at {path}; build it with: {how}')
root = Path(__file__).resolve().parent.parent
if not (root / 'js/dist/pjson.js').is_file():
    raise SystemExit('the JS validator is not built; run: (cd js && npm ci && npm run build)')
rng = random.Random(20260916)
fixtures = json.loads((root / 'conformance/cpp-golden.json').read_text())
inputs = [bytes.fromhex(f[key]) for f in fixtures for key in ('wire_hex', 'canonical_hex')
          if f['format'].startswith('pjson') and f.get(key)]
for value in inputs[:]:
    for _ in range(12):
        altered = bytearray(value)
        altered[rng.randrange(len(value))] = rng.randrange(256)
        inputs.append(bytes(altered))
inputs.extend(rng.randbytes(rng.randrange(1, 100)) for _ in range(args.cases))
wire = '\n'.join(value.hex() for value in inputs) + '\n'
results = {}
commands = {'C++': [str(args.cpp_validator.resolve())],
            'Rust': [str(args.rust_validator.resolve())],
            'JS': ['node', str(root / 'js/tools/pjson-validate.mjs')]}
for lang, command in commands.items():
    p = subprocess.run(command, input=wire, capture_output=True, text=True)
    if p.returncode:
        raise SystemExit(f'{lang} validator crashed: {p.stderr[:4000]}')
    results[lang] = p.stdout.splitlines()
    if len(results[lang]) != len(inputs):
        raise SystemExit(f'{lang}: missing results')
for lang in ['Rust', 'JS']:
    mismatches = [i for i, (a,b) in enumerate(zip(results['C++'], results[lang])) if a != b]
    if mismatches:
        for i in mismatches[:10]:
            print(lang, inputs[i].hex(), 'C++', results['C++'][i], lang, results[lang][i])
        raise SystemExit(f'{lang}: {len(mismatches)} validation disagreements')
valid = sum(r.startswith('valid') for r in results['C++'])
canonical = sum(r.startswith('valid 1') for r in results['C++'])
print(f'{len(inputs)} validation cases agree across C++, Rust and JS '
      f'({valid} well-formed, {canonical} of them canonical; the canonical bytes of every well-formed one agree)')
