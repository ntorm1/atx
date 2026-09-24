"""Attempt 3 of the bounded native fixture run through the worktree wrapper.

Attempt 1 (iteration12_run_transition_checks.py): wrapper rejected the ctest
short flag ``-O`` as an ambiguous PowerShell common parameter; CTest never ran.
Attempt 2 (iteration12_run_transition_checks_attempt2.py): CTest ran and all
three SecurityTransition tests passed (exit 0), but PowerShell 5.1 silently bound
``--verbose`` to its own common parameter, so CTest ran non-verbose and captured
zero ``SECURITY_TRANSITION_MEASUREMENT`` stdout lines. This attempt passes
``-VV`` (verified to survive wrapper binding via -DryRun) and otherwise repeats
attempt 2. Prior evidence is left untouched; the attempt-1 snapshot is reused
after readback verification.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import time
import zipfile

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
ATTEMPT = 3
DESTINATION = AUDITS / f'iteration12-native-measurement-attempt{ATTEMPT}.json'
SNAPSHOT = AUDITS / 'iteration12-native-snapshot.zip'
PRIOR = [AUDITS / 'iteration12-native-measurement.json',
         AUDITS / 'iteration12-native-measurement-attempt2.json']
TESTS_LOG = AUDITS / f'iteration12-tests-attempt{ATTEMPT}.log'
WRAPPER_LOG = AUDITS / f'iteration12-wrapper-test-output-attempt{ATTEMPT}.log'
JUNIT = ROOT / f'build-equity/iteration12-tests-attempt{ATTEMPT}.xml'
EXECUTABLE = ROOT / 'build-equity/bin/atx-engine-book-tests.exe'
MARKER = 'SECURITY_TRANSITION_MEASUREMENT '
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
        assert not out.exists(), f'immutable attempt-{ATTEMPT} output exists: {out}'
    assert SNAPSHOT.exists() and all(p.exists() for p in PRIOR), 'prior evidence missing'
    paths = [ROOT / name for name in SOURCES] + [EXECUTABLE]
    before = {str(path.relative_to(ROOT)): pin(path) for path in paths}
    with zipfile.ZipFile(SNAPSHOT) as archive:
        for name, entry in before.items():
            member = hashlib.sha256(archive.read(name.replace(chr(92), '/'))).hexdigest()
            assert member == entry['sha256'], f'snapshot drift: {name}'
    command = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT / 'scripts/atx-build.ps1'), '-Ctest', '-Preset', 'equity-dev',
        '-Jobs', '1', '-R', '^SecurityTransition' + chr(92) + '.', '-VV',
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
    marker_lines = {}
    for name, path in (('tests_log', TESTS_LOG), ('wrapper_log', WRAPPER_LOG)):
        if path.exists():
            text = path.read_text(encoding='utf-8', errors='replace')
            marker_lines[name] = sum(1 for line in text.splitlines() if MARKER in line)
    result = {'schema': 'atx-security-transition-native-measurement-v1', 'attempt': ATTEMPT,
        'prior_attempts': [pin(p) for p in PRIOR],
        'prior_attempt_failures': [
            'attempt 1: wrapper rejected ctest -O as ambiguous PowerShell parameter; CTest never ran',
            'attempt 2: PowerShell bound --verbose as common parameter; tests passed but no '
            'measurement stdout captured'],
        'exit_code': completed.returncode, 'wrapper_wall_seconds': elapsed,
        'command': command, 'source_and_executable_before': before,
        'source_and_executable_after': after, 'pins_unchanged': before == after,
        'snapshot': pin(SNAPSHOT), 'snapshot_readback_verified': True,
        'snapshot_member_count_excluding_manifest': len(paths), 'outputs': outputs,
        'measurement_marker_lines': marker_lines,
        'qualification': 'Focused synthetic correctness run, not throughput or investment evidence.'}
    DESTINATION.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'exit_code': completed.returncode, 'seconds': elapsed,
                      'pins_unchanged': before == after, 'marker_lines': marker_lines,
                      'measurement': str(DESTINATION)}))
    assert completed.returncode == 0 and before == after


if __name__ == '__main__':
    main()
