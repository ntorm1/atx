"""Attempt 2 of the bounded native fixture run through the worktree wrapper.

Attempt 1 (iteration12_run_transition_checks.py) preserved its snapshot and
measurement but the wrapper never reached CTest: PowerShell 5.1 rejected the
ctest short flag ``-O`` as an ambiguous common parameter (-OutVariable /
-OutBuffer). This attempt passes the long form ``--output-log`` and reuses the
attempt-1 snapshot after verifying its readback hashes still match the current
sources and producer. Attempt-1 evidence is left untouched.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import time
import zipfile

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
DESTINATION = AUDITS / 'iteration12-native-measurement-attempt2.json'
SNAPSHOT = AUDITS / 'iteration12-native-snapshot.zip'
PRIOR = AUDITS / 'iteration12-native-measurement.json'
TESTS_LOG = AUDITS / 'iteration12-tests-attempt2.log'
WRAPPER_LOG = AUDITS / 'iteration12-wrapper-test-output-attempt2.log'
JUNIT = ROOT / 'build-equity/iteration12-tests-attempt2.xml'
EXECUTABLE = ROOT / 'build-equity/bin/atx-engine-book-tests.exe'
SOURCES = [
    'atx-engine/include/atx/engine/data/security_transition.hpp',
    'atx-engine/include/atx/engine/book/security_transition.hpp',
    'atx-engine/src/book/security_transition.cpp',
    'atx-engine/tests/book/security_transition_test.cpp',
    'atx-engine/CMakeLists.txt', 'atx-engine/tests/CMakeLists.txt',
    'atx-core/include/atx/core/error.hpp', 'atx-core/include/atx/core/types.hpp',
    'scripts/atx-build.ps1', 'CMakeLists.txt', 'CMakePresets.json',
    'build-equity/audits/iteration12-security-transition-plan.json',
    'build-equity/audits/iteration12_run_transition_checks.py',
]


def pin(path):
    content = path.read_bytes()
    return {'path': str(path), 'sha256': hashlib.sha256(content).hexdigest(),
            'bytes': len(content)}


def main():
    for out in (DESTINATION, TESTS_LOG, WRAPPER_LOG, JUNIT):
        assert not out.exists(), f'immutable attempt-2 output exists: {out}'
    assert SNAPSHOT.exists() and PRIOR.exists(), 'attempt-1 evidence missing'
    paths = [ROOT / name for name in SOURCES] + [EXECUTABLE]
    before = {str(path.relative_to(ROOT)): pin(path) for path in paths}
    with zipfile.ZipFile(SNAPSHOT) as archive:
        for name, entry in before.items():
            member = hashlib.sha256(archive.read(name.replace(chr(92), '/'))).hexdigest()
            assert member == entry['sha256'], f'snapshot drift: {name}'
    command = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT / 'scripts/atx-build.ps1'), '-Ctest', '-Preset', 'equity-dev',
        '-Jobs', '1', '-R', '^SecurityTransition' + chr(92) + '.', '--verbose',
        '--output-junit', str(JUNIT), '--output-log', str(TESTS_LOG)]
    started = time.perf_counter()
    with WRAPPER_LOG.open('wb') as output:
        completed = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                   check=False)
    elapsed = time.perf_counter() - started
    after = {str(path.relative_to(ROOT)): pin(path) for path in paths}
    outputs = {name: pin(path) for name, path in
               (('tests_log', TESTS_LOG), ('wrapper_log', WRAPPER_LOG), ('junit', JUNIT))
               if path.exists()}
    result = {'schema': 'atx-security-transition-native-measurement-v1', 'attempt': 2,
        'prior_attempt': pin(PRIOR),
        'prior_attempt_failure': 'wrapper rejected ctest -O as ambiguous PowerShell parameter; '
                                 'CTest never ran',
        'exit_code': completed.returncode, 'wrapper_wall_seconds': elapsed,
        'command': command, 'source_and_executable_before': before,
        'source_and_executable_after': after, 'pins_unchanged': before == after,
        'snapshot': pin(SNAPSHOT), 'snapshot_readback_verified': True,
        'snapshot_member_count_excluding_manifest': len(paths), 'outputs': outputs,
        'qualification': 'Focused synthetic correctness run, not throughput or investment evidence.'}
    DESTINATION.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'exit_code': completed.returncode, 'seconds': elapsed,
                      'pins_unchanged': before == after, 'measurement': str(DESTINATION)}))
    assert completed.returncode == 0 and before == after


if __name__ == '__main__':
    main()
