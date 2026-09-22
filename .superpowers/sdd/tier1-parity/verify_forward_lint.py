"""Verify scoped new files and distinguish pre-existing delisting lint findings."""
import json
import subprocess
import sys

paths = ['src/atx_db/_forward_return_publication.py', 'src/atx_db/migrations/bodies_0317.py',
         'src/atx_db/migrations/registry.py', 'src/atx_db/migrations/__init__.py',
         'tests/test_forward_return_publication.py']
subprocess.run([sys.executable, '-m', 'ruff', 'check', *paths], check=True)
path = 'src/atx_db/delisting.py'
base = subprocess.run(['git', 'show', 'HEAD:atx-db/' + path], check=True, capture_output=True).stdout
def diagnostics(args, source=None):
    run = subprocess.run([sys.executable, '-m', 'ruff', 'check', '--output-format', 'json', *args], input=source, capture_output=True)
    assert run.returncode in (0, 1), run.stderr
    return json.loads(run.stdout)
before = diagnostics(['--stdin-filename', path, '-'], base)
after = diagnostics([path])
signature = lambda rows: sorted((r['code'], r['message']) for r in rows)
assert signature(before) == signature(after), (before, after)
print(json.dumps({'delisting_baseline_findings': len(before), 'current_findings': len(after), 'new_findings': 0, 'codes': signature(after)}))
