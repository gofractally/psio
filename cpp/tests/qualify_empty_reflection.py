#!/usr/bin/env python3
"""Retain the focused empty-record repair's native and compile-only probes."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--cc', required=True)
p.add_argument('--cxx', required=True)
p.add_argument('--boost-include', required=True)
p.add_argument('--build-dir', default='build/empty-reflection')
p.add_argument('--guest-cxx')
p.add_argument('--catch-no-posix-signals', action='store_true',
               help='Disable the bundled Catch2 v2 crash reporter on modern glibc; assertions and process-failure detection remain enabled')
p.add_argument('--label', default='native')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
build = root / a.build_dir
out = root / 'logs' / ('empty-reflection-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + a.label)
out.mkdir(parents=True, exist_ok=False)
targets = ['psio_reflect_tests', 'psio_pssz_tests', 'psio_empty_action_consumer',
           'psio_view_access_tests', 'psio_native_view_tests', 'psio_conformance_tests',
           'psio_wit_resource_tests']
report = {'baseline': 'f6b10ec7d7bba4022cd1c9394f88ad1440b4b2b7',
          'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
          'platform': platform.platform(), 'load_before': list(os.getloadavg()),
          'free_bytes_before': shutil.disk_usage(root).free, 'jobs': 2,
          'scope': 'Existing reflection/PSSZ/view/native-view/cross-format tests plus zero-argument action-data consumer; no codec/IR/width or Rust changes.',
          'source_sha256': {}, 'commands': [], 'passed': False}
for name in ['cpp/include/psio/reflect.hpp', 'cpp/include/psio/view.hpp', 'cpp/tests/pssz_tests.cpp',
             'cpp/tests/CMakeLists.txt', 'cpp/tests/empty_action_consumer.cpp',
             'cpp/tests/qualify_empty_reflection.py']:
    report['source_sha256'][name] = hashlib.sha256((root / name).read_bytes()).hexdigest()

def save():
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

def run(name, argv, expected=0):
    start = time.monotonic()
    result = subprocess.run(list(map(str, argv)), cwd=root, capture_output=True, text=True, timeout=900)
    (out / (name + '.stdout')).write_text(result.stdout)
    (out / (name + '.stderr')).write_text(result.stderr)
    report['commands'].append({'name': name, 'argv': list(map(str, argv)),
                               'exit_code': result.returncode, 'expected': expected,
                               'seconds': time.monotonic() - start})
    save()
    if (result.returncode != expected if expected is not None else result.returncode == 0):
        raise RuntimeError(name + ' had unexpected exit code')
    return result

try:
    if report['free_bytes_before'] < 15 * 1024**3:
        raise RuntimeError('less than 15GiB free; no build started')
    run('compiler', [a.cxx, '--version'])
    run('cmake-version', ['cmake', '--version'])
    run('hardware', ['lscpu'] if platform.system() == 'Linux' else ['sysctl', 'machdep.cpu.brand_string', 'hw.memsize', 'hw.ncpu'])
    source = root / 'cpp/tests/empty_action_consumer.cpp'
    probe = [a.cxx, '-std=c++23', '-DPSIO_EXCEPTIONS_ENABLED=1', '-I' + str(root / 'cpp/include'),
             '-idirafter', a.boost_include, '-fsyntax-only', str(source)]
    run('codec-probe', [*probe, '-DPSIO_EMPTY_CODEC_ONLY'])
    run('view-probe', [*probe, '-DPSIO_EMPTY_VIEW_ONLY'])
    rejected = run('invalid-index-probe', [*probe, '-DPSIO_EMPTY_VIEW_ONLY', '-DPSIO_EMPTY_INVALID_INDEX'], expected=None)
    if 'empty_reflected_member<0>' not in rejected.stderr:
        raise RuntimeError('get<0> did not fail for the expected missing empty member')
    compatibility = ['-DCMAKE_CXX_FLAGS=-DCATCH_CONFIG_NO_POSIX_SIGNALS'] if a.catch_no_posix_signals else []
    run('configure', ['cmake', '-S', root, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                       '-DCMAKE_C_COMPILER=' + a.cc, '-DCMAKE_CXX_COMPILER=' + a.cxx,
                       '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DPSIO_ENABLE_TESTS=ON',
                       '-DPSIO_ENABLE_BENCHMARKS=OFF', '-DPSIO_ENABLE_TOOLS=OFF', *compatibility])
    run('build', ['cmake', '--build', build, '--target', *targets, '--parallel', '2'])
    run('build-commands', ['ninja', '-C', build, '-t', 'commands', *targets])
    run('tests', ['ctest', '--test-dir', build, '--verbose', '--output-on-failure', '-R', '^(' + '|'.join(targets) + ')$'])
    report['binary_sha256'] = {target: hashlib.sha256((build / 'cpp/tests' / target).read_bytes()).hexdigest() for target in targets}
    if a.guest_cxx:
        run('guest-compiler', [a.guest_cxx, '--version'])
        obj = out / 'empty-action-wasi.o'
        run('guest-compile', [a.guest_cxx, '-std=c++23', '-DPSIO_EXCEPTIONS_ENABLED=1',
                              '-I' + str(root / 'cpp/include'), '-idirafter', a.boost_include,
                              '-c', source, '-o', obj])
        run('guest-object', ['file', obj])
        report['guest_scope'] = 'Compile-only existing WASI SDK with exceptions enabled; no guest link or runtime qualification.'
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
