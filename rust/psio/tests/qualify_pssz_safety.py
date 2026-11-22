#!/usr/bin/env python3
"""Retain native existing-codec Rust safety checks and exact f6 reference bytes."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--reference-source', required=True)
p.add_argument('--label', default='native')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
reference = Path(a.reference_source).resolve()
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out = root / 'logs' / ('rust-safety-' + stamp + '-' + a.label)
out.mkdir(parents=True, exist_ok=False)
artifacts = root / 'rust/target/safety-evidence' / out.name
artifacts.mkdir(parents=True)
report = {'baseline': 'f6b10ec7d7bba4022cd1c9394f88ad1440b4b2b7',
          'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
          'reference_source': str(reference), 'platform': platform.platform(),
          'load_before': list(os.getloadavg()), 'free_bytes_before': shutil.disk_usage(root).free,
          'jobs': 2, 'commands': [], 'passed': False, 'source_sha256': {},
          'scope': 'Existing Rust explicit PSSZ input/memory safety; safe trait metadata never proves POD. Not full strictness, append evolution, automatic widths, derive expansion or SDK activation.'}
for name in ['rust/psio/src/pssz.rs', 'rust/psio-macros/src/pssz_macro.rs', 'rust/psio/src/pssz_view.rs',
             'rust/psio/src/lib.rs', 'rust/psio/tests/pssz_safety.rs',
             'rust/psio/tests/probes/pssz_safety_before.rs',
             'rust/psio/tests/probes/pssz_safety_reference_before.rs',
             'rust/psio/tests/qualify_pssz_safety.py',
             'rust/psio/tests/fixtures/pssz-safety-explicit.json',
             'rust/psio/examples/pssz_safety_reference.rs', 'rust/Cargo.lock',
             'rust/Cargo.toml', 'rust/psio/Cargo.toml', 'rust/psio-macros/Cargo.toml']:
    report['source_sha256'][name] = hashlib.sha256((root / name).read_bytes()).hexdigest()

def save():
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

def run(name, argv, expected=0):
    argv = list(map(str, argv))
    start = time.monotonic()
    result = subprocess.run(argv, cwd=root, capture_output=True, text=True, timeout=900)
    (out / (name + '.stdout')).write_text(result.stdout)
    (out / (name + '.stderr')).write_text(result.stderr)
    report['commands'].append({'name': name, 'argv': argv, 'exit_code': result.returncode,
                               'expected': expected, 'seconds': time.monotonic() - start})
    save()
    if result.returncode != expected:
        raise RuntimeError(name + ' had unexpected exit code')
    return result

try:
    if report['free_bytes_before'] < 18 * 1024**3:
        raise RuntimeError('less than 18GiB free; no build started')
    ref_head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=reference, text=True).strip()
    if ref_head != report['baseline']:
        raise RuntimeError('reference must be exact clean f6')
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=reference, text=True):
        raise RuntimeError('reference tracked files are dirty')
    report['reference_head'] = ref_head
    report['reference_source_sha256'] = {name: hashlib.sha256((reference / name).read_bytes()).hexdigest()
                                         for name in ['rust/psio/src/pssz.rs', 'rust/psio/src/pssz_derive.rs',
                                                      'rust/psio/src/pssz_view.rs', 'rust/Cargo.lock']}
    run('rustc', ['rustc', '-vV'])
    run('cargo', ['cargo', '--version'])
    run('toolchains', ['rustup', 'toolchain', 'list'])
    run('hardware', ['lscpu'] if platform.system() == 'Linux' else
        ['sysctl', 'machdep.cpu.brand_string', 'hw.memsize', 'hw.ncpu'])
    run('reference-build', ['cargo', 'build', '--manifest-path', reference / 'rust/Cargo.toml',
                             '-p', 'psio', '--lib', '--locked', '-j', '2'])
    ref_args = ['rustc', '--edition=2021', '--extern',
                'psio=' + str(reference / 'rust/target/debug/libpsio.rlib'),
                '-L', 'dependency=' + str(reference / 'rust/target/debug/deps')]
    before = artifacts / 'reference-safe-failures'
    run('reference-safe-compile', [*ref_args, '--test', root / 'rust/psio/tests/probes/pssz_safety_before.rs', '-o', before])
    before_run = run('reference-safe-failures', [before, '--nocapture'], expected=101)
    if '0 passed; 4 failed' not in before_run.stdout:
        raise RuntimeError('safe baseline probe did not demonstrate the four expected failures')
    emitter = artifacts / 'reference-emitter'
    run('reference-emitter-compile', [*ref_args, root / 'rust/psio/tests/probes/pssz_safety_reference_before.rs', '-o', emitter])
    emitted = run('reference-goldens', [emitter])
    pinned = json.loads((root / 'rust/psio/tests/fixtures/pssz-safety-explicit.json').read_text())
    if json.loads(emitted.stdout) != pinned or len(pinned) != 40:
        raise RuntimeError('39 valid explicit profile encodings differ from clean f6')
    report['reference_golden_count'] = 39
    base = ['cargo', 'test', '--manifest-path', root / 'rust/Cargo.toml', '-p', 'psio', '--locked', '-j', '2']
    debug = run('debug-tests', [*base, '--lib', '--tests'])
    release = run('release-safety', [*base, '--release', '--test', 'pssz_safety'])
    current = run('current-goldens', ['cargo', 'run', '--manifest-path', root / 'rust/Cargo.toml',
                                      '-p', 'psio', '--locked', '-j', '2', '--example', 'pssz_safety_reference'])
    if json.loads(current.stdout) != pinned:
        raise RuntimeError('current explicit encodings differ from f6 reference')
    report['debug_test_counts'] = list(map(int, re.findall(r'test result: ok\. (\d+) passed;', debug.stdout)))
    report['release_test_counts'] = list(map(int, re.findall(r'test result: ok\. (\d+) passed;', release.stdout)))
    report['artifact_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in artifacts.iterdir() if path.is_file()}
    report['binary_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                               for profile in ['debug', 'release']
                               for name in ['psio-*', 'pssz_safety-*', 'conformance_corpus-*']
                               for path in (root / 'rust/target' / profile / 'deps').glob(name)
                               if path.is_file() and os.access(path, os.X_OK)}
    report['passed'] = True
except Exception as error:
    report['error'] = str(error)
finally:
    report['load_after'] = list(os.getloadavg())
    report['free_bytes_after'] = shutil.disk_usage(root).free
    save()
print(json.dumps({'evidence': str(out), 'passed': report['passed'], 'error': report.get('error')}))
raise SystemExit(0 if report['passed'] else 1)
