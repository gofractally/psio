#!/usr/bin/env python3
"""Check fixtures against the actual public C++ codec; --update regenerates them."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('emitter', type=Path, help='path to psio_cpp_golden executable')
parser.add_argument('--update', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
result = subprocess.run([str(args.emitter.resolve())], capture_output=True, text=True)
if result.returncode:
    raise SystemExit(result.stderr.strip() or "C++ fixture emitter failed")
fixtures = json.loads(result.stdout)
assert fixtures and all(set(f) == {'format', 'id', 'wire_hex'} for f in fixtures)
paths = ['conformance/cpp-golden.json', 'rust/psio/tests/fixtures/cpp-golden.json',
         'js/src/test/cpp-golden.json']
for name in paths:
    path = root / name
    if args.update:
        path.write_text(result.stdout)
    elif json.loads(path.read_text()) != fixtures:
        raise SystemExit(f'{name} differs from C++: regenerate and review the wire change')
print(f'{len(fixtures)} C++ fixtures match all three copies')
