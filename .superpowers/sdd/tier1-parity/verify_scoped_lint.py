"""Check exact task paths; expose new Ruff findings without hiding baseline debt."""
from collections import Counter
import json
import subprocess
import sys

failures = []
summary = []
for path in sys.argv[1:]:
    baseline = subprocess.run(['git', 'show', 'HEAD:atx-db/' + path], capture_output=True)
    command = [sys.executable, '-m', 'ruff', 'check', '--output-format', 'json']
    current = subprocess.run([*command, path], capture_output=True)
    assert current.returncode in (0, 1), current.stderr
    after = json.loads(current.stdout)
    before = []
    if baseline.returncode == 0:
        old = subprocess.run([*command, '--stdin-filename', path, '-'], input=baseline.stdout, capture_output=True)
        assert old.returncode in (0, 1), old.stderr
        before = json.loads(old.stdout)
    signature = lambda rows: Counter((r['code'], r['message']) for r in rows)
    added = signature(after) - signature(before)
    summary.append({'path': path, 'baseline': len(before), 'current': len(after), 'added': sum(added.values())})
    if added:
        failures.extend({'path': path, **row} for row in after if (row['code'], row['message']) in added)
print(json.dumps({'summary': summary, 'new_findings': failures}, indent=2))
raise SystemExit(bool(failures))
