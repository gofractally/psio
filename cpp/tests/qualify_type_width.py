#!/usr/bin/env python3
"""Qualify existing PSSZ automatic per-type widths; retain commands and raw output."""
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
p.add_argument('--cc', required=True)
p.add_argument('--cxx', required=True)
p.add_argument('--boost-include', required=True)
p.add_argument('--build-dir', default='build/type-width')
p.add_argument('--reference-source', help='Read-only exact 552ca source used for failure-before and explicit reference bytes')
p.add_argument('--guest-cxx', help='Optional existing WASI compiler; object compile only')
p.add_argument('--catch-no-posix-signals', action='store_true',
               help='Bundled Catch2 v2 crash-reporter compatibility on modern glibc; assertions remain enabled')
p.add_argument('--label', default='native')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
build = root / a.build_dir
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out = root / 'logs' / ('type-width-' + stamp + '-' + a.label)
out.mkdir(parents=True, exist_ok=False)
targets = ['psio_pssz_type_width_tests', 'psio_pssz_type_width_consumer', 'psio_pssz_tests',
           'psio_reflect_tests', 'psio_empty_action_consumer', 'psio_view_access_tests',
           'psio_native_view_tests', 'psio_conformance_tests', 'psio_caps_parity_tests',
           'psio_annotate_tests', 'psio_adapter_tests', 'psio_validate_structural_tests',
           'psio_validate_fuzz_tests', 'psio_wit_resource_tests', 'psio_schema_pssz_tests',
           'psio_static_dynamic_parity_tests']
report = {'baseline': '552ca3744fc5a0cd3c69530cfd4f1e466eadd8f7',
          'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
          'platform': platform.platform(), 'machine': platform.machine(),
          'load_before': list(os.getloadavg()), 'free_bytes_before': shutil.disk_usage(root).free,
          'jobs': 2, 'targets': targets, 'commands': [], 'passed': False,
          'scope': 'Automatic per-logical-type width recursion and bounds; existing explicit profiles retain bytes. Not global strict-semantic PSSZ or complete malformed-input qualification.',
          'source_sha256': {}}
sources = list((root / 'cpp/include/psio').rglob('*.hpp'))
sources += list((root / 'cpp/tests').glob('*type_width*'))
sources += [root / 'cpp/tests/CMakeLists.txt', root / 'CMakeLists.txt',
            root / 'cpp/external/catch2/catch.hpp']
for path in sorted(set(sources)):
    report['source_sha256'][str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()

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
    if report['free_bytes_before'] < 15 * 1024**3:
        raise RuntimeError('less than 15GiB free; no build started')
    run('compiler', [a.cxx, '--version'])
    run('cmake-version', ['cmake', '--version'])
    run('hardware', ['lscpu'] if platform.system() == 'Linux' else
        ['sysctl', 'machdep.cpu.brand_string', 'hw.memsize', 'hw.ncpu'])
    common = [a.cxx, '-std=c++23', '-DPSIO_EXCEPTIONS_ENABLED=1', '-idirafter', a.boost_include]
    probe = root / 'cpp/tests/pssz_type_width_reproducer.cpp'
    if a.reference_source:
        ref = Path(a.reference_source).resolve()
        ref_head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ref, text=True).strip()
        if ref_head != report['baseline']:
            raise RuntimeError('reference HEAD must be exact 552ca')
        status = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=ref, text=True)
        if status:
            raise RuntimeError('reference tracked source is dirty')
        report['reference_source'] = {'path': str(ref), 'head': ref_head,
                                     'header_sha256': {str(path.relative_to(ref)): hashlib.sha256(path.read_bytes()).hexdigest()
                                                      for path in (ref / 'cpp/include/psio').rglob('*.hpp')}}
        ref_probe = out / 'reference-probe'
        run('reference-compile', [*common, '-I' + str(ref / 'cpp/include'), probe, '-o', ref_probe])
        run('reference-failure', [ref_probe], expected=1)
        emitter = out / 'reference-goldens'
        run('reference-golden-compile', [*common, '-I' + str(ref / 'cpp/include'),
                                         root / 'cpp/tests/pssz_type_width_golden.cpp', '-o', emitter])
        goldens = run('reference-goldens', [emitter])
        pinned = dict(re.findall(r'\{"([^"\n]+)",\s*"([0-9a-f]*)"\}',
                                (root / 'cpp/tests/pssz_type_width_explicit_goldens.hpp').read_text()))
        if json.loads(goldens.stdout) != pinned or len(pinned) != 24:
            raise RuntimeError('pinned explicit bytes differ from exact reference')
    fixed_probe = out / 'fixed-probe'
    run('fixed-compile', [*common, '-I' + str(root / 'cpp/include'), probe, '-o', fixed_probe])
    run('fixed-probe', [fixed_probe])
    compatibility = ['-DCMAKE_CXX_FLAGS=-DCATCH_CONFIG_NO_POSIX_SIGNALS'] if a.catch_no_posix_signals else []
    run('configure', ['cmake', '-S', root, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                      '-DCMAKE_C_COMPILER=' + a.cc, '-DCMAKE_CXX_COMPILER=' + a.cxx,
                      '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DPSIO_ENABLE_TESTS=ON',
                      '-DPSIO_ENABLE_BENCHMARKS=OFF', '-DPSIO_ENABLE_TOOLS=OFF', *compatibility])
    run('build', ['cmake', '--build', build, '--target', *targets, '--parallel', '2'])
    run('build-commands', ['ninja', '-C', build, '-t', 'commands', *targets])
    tests = run('tests', ['ctest', '--test-dir', build, '--verbose', '--output-on-failure',
                           '-R', '^(' + '|'.join(targets) + ')$'])
    report['catch_cases'] = sum(map(int, re.findall(r'All tests passed \([^\n]+ in (\d+) test cases?\)', tests.stdout)))
    report['catch_assertions'] = sum(map(int, re.findall(r'All tests passed \((\d+) assertions?', tests.stdout)))
    report['binary_sha256'] = {name: hashlib.sha256((build / 'cpp/tests' / name).read_bytes()).hexdigest()
                               for name in targets}
    manifest = run('width-manifest', [build / 'cpp/tests/psio_pssz_type_width_consumer'])
    report['fixture_width_manifest'] = json.loads(manifest.stdout)
    if a.guest_cxx:
        run('guest-compiler', [a.guest_cxx, '--version'])
        obj = out / 'type-width-wasi.o'
        run('guest-compile', [a.guest_cxx, '-std=c++23', '-DPSIO_EXCEPTIONS_ENABLED=1',
                              '-I' + str(root / 'cpp/include'), '-idirafter', a.boost_include,
                              '-c', root / 'cpp/tests/pssz_type_width_consumer.cpp', '-o', obj])
        run('guest-object', ['file', obj])
        report['guest_scope'] = 'Existing WASI SDK compile-only, exceptions enabled; no guest link/runtime guarantee.'
        report['guest_object_sha256'] = hashlib.sha256(obj.read_bytes()).hexdigest()
    report['passed'] = True
except Exception as error:
    report['error'] = str(error)
finally:
    report['load_after'] = list(os.getloadavg())
    report['free_bytes_after'] = shutil.disk_usage(root).free
    save()
print(json.dumps({'evidence': str(out), 'passed': report['passed'], 'error': report.get('error')}))
raise SystemExit(0 if report['passed'] else 1)
