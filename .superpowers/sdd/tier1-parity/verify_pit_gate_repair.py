"""Controller check of PG1 candidate tests against an unchanged committed source."""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

root = Path('C:/atx')
python = root / 'atx-db/.venv/Scripts/python.exe'
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
archive = subprocess.check_output(['git', 'archive', '--format=zip', sha, 'atx-db'], cwd=root)
target = Path(tempfile.mkdtemp(prefix='atx-pit-gate-'))
with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
    for name in bundle.namelist():
        if not (target / name).resolve().is_relative_to(target.resolve()):
            raise RuntimeError('Unsafe archive member')
    bundle.extractall(target)
project = target / 'atx-db'
test_path = Path('tests/test_derived_pit_revisions.py')
candidate = (root / 'atx-db' / test_path).read_bytes()
(project / test_path).write_bytes(candidate)
env = os.environ.copy()
env.update(PYTHONPATH=str(project / 'src'), PYTHONUTF8='1')
key = subprocess.check_output(
    [str(python), '-c', "import runpy; print(runpy.run_path('tests/conftest.py')['_schema_fingerprint']())"],
    cwd=project, env=env, text=True,
).strip()
origin = Path('C:/Users/natha/AppData/Local/Temp/atx-tier1-head-6bzfpb4d/atx-db/.pytest_cache/db_schema_templates') / key
assert (origin / 'warehouse_template.duckdb.ready').read_text().strip() == key
destination = project / '.pytest_cache/db_schema_templates' / key
destination.mkdir(parents=True, exist_ok=False)
for name in ('warehouse_template.duckdb', 'warehouse_template.duckdb.ready'):
    shutil.copy2(origin / name, destination / name)
selectors = {
    'chunk': 'test_chunk_equivalence_ties_and_event_aware_stub_period',
    'upgrade': 'test_populated_0314_upgrade_preserves_legacy_contract_and_reentry',
}
selector = selectors[sys.argv[1]]
print(json.dumps({'committed_sha': sha, 'candidate_test_sha256': hashlib.sha256(candidate).hexdigest(),
                  'schema_fingerprint': key, 'export': str(project), 'selector': selector}), flush=True)
result = subprocess.run([str(python), '-m', 'pytest', f'{test_path.as_posix()}::{selector}',
                         '-n', '0', '-o', 'addopts=', '-q', '--durations=2'], cwd=project, env=env)
raise SystemExit(result.returncode)
