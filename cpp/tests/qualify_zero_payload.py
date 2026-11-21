#!/usr/bin/env python3
"""Retain focused zero-payload native and ASan/UBSan qualification."""
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
p.add_argument('--label', default='native')
p.add_argument('--stage', choices=['optional', 'sequence'], required=True)
p.add_argument('--catch-no-posix-signals', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = root / 'logs' / ('zero-' + a.stage + '-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + a.label)
out.mkdir(parents=True)
targets = ['psio_pssz_zero_tests', 'psio_pssz_type_width_tests', 'psio_pssz_tests', 'psio_empty_action_consumer']
report = {'baseline': '7dff9e327e23cfc4307f5cdce2af7cc96f357e1c',
          'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
          'stage': a.stage, 'platform': platform.platform(), 'jobs': 2,
          'load_before': os.getloadavg(), 'free_bytes_before': shutil.disk_usage(root).free,
          'commands': [], 'passed': False, 'source_sha256': {},
          'scope': 'Existing zero-size optional/sequence handling only. No typed length-bound or nonzero fixed-payload semantic validation repair.'}
for name in ['cpp/include/psio/pssz.hpp', 'cpp/include/psio/pssz_view.hpp',
             'cpp/tests/pssz_zero_tests.cpp', 'cpp/tests/pssz_zero_reproducer.cpp',
             'cpp/tests/CMakeLists.txt', 'cpp/tests/qualify_zero_payload.py']:
    report['source_sha256'][name] = hashlib.sha256((root / name).read_bytes()).hexdigest()

def save():
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

def run(name, argv):
    argv = list(map(str, argv)); start = time.monotonic()
    result = subprocess.run(argv, cwd=root, capture_output=True, text=True, timeout=900)
    (out / (name + '.stdout')).write_text(result.stdout)
    (out / (name + '.stderr')).write_text(result.stderr)
    report['commands'].append({'name': name, 'argv': argv, 'exit_code': result.returncode,
                               'seconds': time.monotonic() - start})
    save()
    if result.returncode:
        raise RuntimeError(name + ' failed')
    return result

try:
    if report['free_bytes_before'] < 18 * 1024**3:
        raise RuntimeError('less than 18GiB free; no build started')
    run('compiler', [a.cxx, '--version'])
    run('hardware', ['lscpu'] if platform.system() == 'Linux' else
        ['sysctl', 'machdep.cpu.brand_string', 'hw.memsize', 'hw.ncpu'])
    for profile in ['native', 'sanitize']:
        build = root / 'build' / ('zero-' + profile)
        flags = ['-DCATCH_CONFIG_NO_POSIX_SIGNALS'] if a.catch_no_posix_signals else []
        if profile == 'sanitize':
            flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-g']
        configure = ['cmake', '-S', root, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                     '-DCMAKE_C_COMPILER=' + a.cc, '-DCMAKE_CXX_COMPILER=' + a.cxx,
                     '-DPSIO_ENABLE_TESTS=ON', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                     '-DCMAKE_CXX_FLAGS=' + ' '.join(flags)]
        run(profile + '-configure', configure)
        selected = targets if profile == 'native' else targets[:2]
        run(profile + '-build', ['cmake', '--build', build, '--target', *selected, '--parallel', '2'])
        run(profile + '-build-commands', ['ninja', '-C', build, '-t', 'commands', *selected])
        tested = run(profile + '-tests', ['ctest', '--test-dir', build, '--verbose', '--output-on-failure',
                                          '-R', '^(' + '|'.join(selected) + ')$'])
        report[profile + '_cases'] = sum(map(int, re.findall(r'All tests passed \([^\n]+ in (\d+) test cases?\)', tested.stdout)))
        report[profile + '_assertions'] = sum(map(int, re.findall(r'All tests passed \((\d+) assertions?', tested.stdout)))
        report[profile + '_binary_sha256'] = {name: hashlib.sha256((build / 'cpp/tests' / name).read_bytes()).hexdigest()
                                               for name in selected}
    report['passed'] = True
except Exception as error:
    report['error'] = str(error)
finally:
    report['load_after'] = os.getloadavg()
    report['free_bytes_after'] = shutil.disk_usage(root).free
    save()
print(json.dumps({'evidence': str(out), 'passed': report['passed'], 'error': report.get('error')}))
raise SystemExit(0 if report['passed'] else 1)
