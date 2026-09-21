"""Separate unchanged legacy typing debt from FC1's checked source paths."""
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path('C:/atx')
base = Path(__file__).parent
export = Path(json.loads((base / 'capacity-committed-head.log').read_text().splitlines()[0])['export'])
assert json.loads((base / 'capacity-committed-head.log').read_text().splitlines()[0])['sha'] == '1a3cb68b5f9a2be25a6b9c9a5006fbe5eda74b18'
paths = ['src/atx_db/fundamentals.py', 'src/atx_db/asof/fundamentals.py',
         'src/atx_db/features.py', 'src/atx_db/quality/checks_market_reference.py',
         'src/atx_db/activation.py', 'src/atx_db/fundamental_xbrl_metrics.py',
         'src/atx_db/expected_growth.py', 'src/atx_db/estimates/measure_actuals.py']
env = os.environ.copy()
env['PYTHONPATH'] = str(export / 'src')
env['PYTHONUTF8'] = '1'
run = subprocess.run([sys.executable, '-m', 'mypy', *paths], cwd=export, env=env, capture_output=True, text=True)
assert run.returncode in (0, 1), run.stderr
out = base / 'fundamental-clock-mypy-baseline.log'
with out.open('x', encoding='utf-8') as handle:
    handle.write(run.stdout)
def findings(text):
    return Counter(re.sub(r':\d+(?::\d+)?: error:', ': error:', line).replace('\\', '/')
                   for line in text.splitlines() if ': error:' in line)
before = findings(run.stdout)
after = findings((base / 'fundamental-clock-mypy.log').read_text())
assert before == after, {'added': list((after-before).elements()), 'removed': list((before-after).elements())}
print(json.dumps({'baseline_errors': sum(before.values()), 'current_errors': sum(after.values()), 'new_errors': 0, 'baseline_export': str(export)}))
subprocess.run([sys.executable, '-m', 'mypy', 'src/atx_db/_fundamental_clock.py', 'src/atx_db/migrations/bodies_0319.py'], cwd=root/'atx-db', check=True)
