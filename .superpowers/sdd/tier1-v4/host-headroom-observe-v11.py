"""Fresh original host window after reviewed orphan-spill cleanup and queued source fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
from datetime import datetime, timezone

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
out = base / 'logs/host-headroom-window-v11.json'
if out.exists():
    raise RuntimeError('Preserve previous host observation')
spec = importlib.util.spec_from_file_location('observed_memory_guard', base.parent / 'tier1-parity/run_memory_guarded.py')
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)
samples = []
for number in range(31):
    samples.append({'observed_at_utc': datetime.now(timezone.utc).isoformat(), **guard.host_memory()})
    if number % 6 == 0:
        print(json.dumps({'samples': len(samples), 'latest': samples[-1]}), flush=True)
    if number < 30:
        time.sleep(10)
minimum_commit = min(row['commit_free_gb'] for row in samples)
minimum_physical = min(row['physical_free_gb'] for row in samples)
result = {
    'status': 'BOUNDED_HOST_OBSERVATION_COMPLETE', 'samples': samples, 'duration_seconds': 300,
    'minimum_commit_free_gib': minimum_commit, 'minimum_physical_free_gib': minimum_physical,
    'reschedule_condition': {'minimum_commit_free_gib': 3.0, 'minimum_physical_free_gib': 1.0},
    'reschedule_condition_met': minimum_commit >= 3.0 and minimum_physical >= 1.0,
    'qualification': 'Scheduling evidence only; disk free observed above40GiB again and latest bounded recovery audit minimumcommit3.645GiB. Observerv10 failed the original minimum3GiB condition and is preserved. Original31sample/300s condition is unchanged; every later data job still requires original resource/source guards including40GiB disk floor. No financial or release gate measured.'}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({k: v for k, v in result.items() if k != 'samples'}), flush=True)
