"""Compare scoped Ruff diagnostics to the committed source baseline."""

import collections
import json
import subprocess
import sys
from pathlib import Path

root = Path('C:/atx')
files = [
    'src/atx_db/jobs.py', 'src/atx_db/press_release.py',
    'tests/test_sec_submissions_bulk.py',
]
result = {'baseline': '92cf42c4', 'files': {}}
for path in files:
    baseline = subprocess.run(
        ['git', 'show', f'92cf42c4:atx-db/{path}'], cwd=root,
        check=True, capture_output=True,
    ).stdout
    versions = {}
    for label, payload in [('baseline', baseline), ('current', (root / 'atx-db' / path).read_bytes())]:
        run = subprocess.run(
            [sys.executable, '-m', 'ruff', 'check', '--output-format=json', '--stdin-filename', path, '-'],
            input=payload, cwd=root / 'atx-db', capture_output=True,
        )
        assert run.returncode in (0, 1), run.stderr
        versions[label] = json.loads(run.stdout)
    old = collections.Counter((row['code'], row['message']) for row in versions['baseline'])
    now = collections.Counter((row['code'], row['message']) for row in versions['current'])
    result['files'][path] = {
        'baseline_count': sum(old.values()), 'current_count': sum(now.values()),
        'introduced': [{'code': key[0], 'message': key[1], 'count': count} for key, count in (now-old).items()],
        'baseline_diagnostics': [{'code': key[0], 'message': key[1], 'count': count} for key, count in old.items()],
    }
print(json.dumps(result, indent=2))
