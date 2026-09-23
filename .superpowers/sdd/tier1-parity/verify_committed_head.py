"""Controller check: verify committed source independently of shared-tree WIP."""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path('C:/atx')
PYTHON = ROOT / 'atx-db/.venv/Scripts/python.exe'
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
archive = subprocess.check_output(['git', 'archive', '--format=zip', sha, 'atx-db'], cwd=ROOT)
target = Path(tempfile.mkdtemp(prefix='atx-tier1-head-'))
with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
    for name in bundle.namelist():
        if not (target / name).resolve().is_relative_to(target.resolve()):
            raise RuntimeError(f'Unsafe archive member: {name}')
    bundle.extractall(target)
project = target / 'atx-db'
env = os.environ.copy()
env['PYTHONPATH'] = str(project / 'src')
env['PYTHONUTF8'] = '1'
print(json.dumps({'sha': sha, 'export': str(project)}), flush=True)
subprocess.run(
    [str(PYTHON), '-c', 'import atx_db; print(atx_db.__file__)'],
    cwd=project, env=env, check=True,
)
# Reuse only an exact content-fingerprinted, completed schema template.
key = subprocess.check_output(
    [str(PYTHON), '-c', "import runpy; print(runpy.run_path('tests/conftest.py')['_schema_fingerprint']())"],
    cwd=project, env=env, text=True,
).strip()
origin_cache = ROOT / 'atx-db/.pytest_cache/db_schema_templates' / key
dest_cache = project / '.pytest_cache/db_schema_templates' / key
cache_files = ('warehouse_template.duckdb', 'warehouse_template.duckdb.ready')
if all((origin_cache / name).is_file() for name in cache_files):
    dest_cache.mkdir(parents=True, exist_ok=True)
    for name in cache_files:
        shutil.copy2(origin_cache / name, dest_cache / name)
    print(json.dumps({'reused_exact_schema_template': key}), flush=True)
test_paths = ['tests/test_module_boundaries.py']
if '--module-only' not in sys.argv[1:]:
    test_paths.append('tests/test_schema_contract_v2.py')
if '--include-parity-regressions' in sys.argv[1:]:
    test_paths.extend([
        'tests/test_derived_metrics.py', 'tests/test_derived_annual.py',
        'tests/test_derived_pit_revisions.py', 'tests/test_forward_return_publication.py',
    ])
result = subprocess.run(
    [str(PYTHON), '-m', 'pytest', *test_paths, '-n', '0', '-q'],
    cwd=project, env=env,
)
print(json.dumps({'sha': sha, 'exit_code': result.returncode, 'export': str(project)}), flush=True)
raise SystemExit(result.returncode)
