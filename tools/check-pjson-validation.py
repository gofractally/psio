#!/usr/bin/env python3
"""Compare public C++, Rust and JS validators on deterministic mutations and random inputs."""
import argparse
import json
from pathlib import Path
import random
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('cpp_validator', type=Path)
parser.add_argument('rust_validator', type=Path)
parser.add_argument('--cases', type=int, default=10000)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
rng = random.Random(20260916)
fixtures = json.loads((root / 'conformance/cpp-golden.json').read_text())
inputs = [bytes.fromhex(f['wire_hex']) for f in fixtures
          if f['format'].startswith('pjson') and f['wire_hex']]
for value in inputs[:]:
    for _ in range(3):
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
print(f'{len(inputs)} validation cases agree across C++, Rust and JS')
