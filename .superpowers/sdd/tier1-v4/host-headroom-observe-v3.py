"""Five-minute bounded host observation after fresh 5.0086GiB headroom recovery and serialized fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
from datetime import datetime, timezone

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'logs/host-headroom-window-v3.json'
if out.exists():
    raise RuntimeError('Preserve previous host observation')
guard_module = workspace.parent / 'tier1-parity/run_memory_guarded.py'
spec = importlib.util.spec_from_file_location('observed_memory_guard', guard_module)
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
    'status': 'BOUNDED_HOST_OBSERVATION_COMPLETE', 'samples': samples,
    'duration_seconds': 300, 'minimum_commit_free_gib': minimum_commit,
    'minimum_physical_free_gib': minimum_physical,
    'reschedule_condition': {'minimum_commit_free_gib': 3.0, 'minimum_physical_free_gib': 1.0},
    'reschedule_condition_met': minimum_commit >= 3.0 and minimum_physical >= 1.0,
    'qualification': 'Scheduling evidence only. Five-minute observation does not guarantee future headroom; every native build still requires its unchanged external memory/disk guards. No source, financial or release gate is measured.'
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: value for key, value in result.items() if key != 'samples'}), flush=True)
