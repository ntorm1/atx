"""Run the bounded native fixture through the worktree wrapper and preserve its producer."""
from pathlib import Path
import hashlib
import json
import subprocess
import time
import zipfile

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
DESTINATION = AUDITS / 'iteration12-native-measurement.json'
SNAPSHOT = AUDITS / 'iteration12-native-snapshot.zip'
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
    assert not DESTINATION.exists() and not SNAPSHOT.exists(), 'immutable measurement'
    paths = [ROOT / name for name in SOURCES] + [EXECUTABLE]
    before = {str(path.relative_to(ROOT)): pin(path) for path in paths}
    with zipfile.ZipFile(SNAPSHOT, 'x', zipfile.ZIP_DEFLATED) as archive:
        for path in paths:
            archive.write(path, str(path.relative_to(ROOT)).replace('\\', '/'))
        archive.writestr('snapshot.json', json.dumps({
            'scope': 'Selected transition sources and test producer; not a portable rebuild bundle.',
            'files': before}, indent=2))
    with zipfile.ZipFile(SNAPSHOT) as archive:
        for name, entry in before.items():
            assert hashlib.sha256(archive.read(name.replace('\\', '/'))).hexdigest() == entry['sha256']
    command = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT / 'scripts/atx-build.ps1'), '-Ctest', '-Preset', 'equity-dev',
        '-Jobs', '1', '-R', '^SecurityTransition\\.', '--verbose',
        '--output-junit', str(ROOT / 'build-equity/iteration12-tests.xml'),
        '-O', str(AUDITS / 'iteration12-tests.log')]
    started = time.perf_counter()
    with (AUDITS / 'iteration12-wrapper-test-output.log').open('wb') as output:
        completed = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                   check=False)
    elapsed = time.perf_counter() - started
    after = {str(path.relative_to(ROOT)): pin(path) for path in paths}
    result = {'schema': 'atx-security-transition-native-measurement-v1',
        'exit_code': completed.returncode, 'wrapper_wall_seconds': elapsed,
        'command': command, 'source_and_executable_before': before,
        'source_and_executable_after': after, 'pins_unchanged': before == after,
        'snapshot': pin(SNAPSHOT), 'snapshot_readback_verified': True,
        'snapshot_member_count_excluding_manifest': len(paths),
        'qualification': 'Focused synthetic correctness run, not throughput or investment evidence.'}
    DESTINATION.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'exit_code': completed.returncode, 'seconds': elapsed,
                      'pins_unchanged': before == after, 'measurement': str(DESTINATION)}))
    assert completed.returncode == 0 and before == after


if __name__ == '__main__':
    main()
