"""Untimed runner qualification: original/batch scalar/batch SIMD, all 150 cases."""
import json
from pathlib import Path
import subprocess
import sys

import audit


def main():
    binary, reference_path, output = map(Path, sys.argv[1:])
    reference = audit.load_reference(reference_path)
    output.mkdir(parents=True, exist_ok=False)
    failures = [
        ['--mode', 'unknown', '--grid', '400'],
        ['--mode', 'original', '--grid', '0'],
        ['--mode', 'original', '--grid', '400junk'],
        ['--mode', 'original', '--grid', '400', '--mode', 'batch-scalar'],
        ['--mode', 'original', '--grid', '400', '--require-pcore'],
        ['--mode', 'original', '--grid', '400', '--unknown'],
    ]
    for args in failures:
        failed = subprocess.run([str(binary.resolve()), *args], capture_output=True, text=True)
        assert failed.returncode == 2, (args, failed.stderr)
        assert not failed.stdout.strip(), 'invalid CLI emitted a report'
    reports = []
    for grid in (400, 800, 1600):
        for mode in audit.MODES:
            run = subprocess.run([str(binary.resolve()), '--grid', str(grid), '--mode', mode,
                                  '--untimed'], capture_output=True, text=True, check=True)
            path = output / f'{grid}-{mode}.json'
            path.write_text(run.stdout, encoding='utf-8')
            native = json.loads(run.stdout)
            assert native['timed'] is False and native['inclusive_seconds'] is None
            audit.verify(reference, native)
            reports.append(native)
    summary = audit.summarize(reference, reports)
    assert summary['native_quiet_all'] is False
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print('Six strict CLI refusals; all nine untimed reports, all 150 unique cases, exact parity.')


if __name__ == '__main__':
    main()
