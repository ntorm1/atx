"""Inspect two additional retained text execution logs; no requests or source bodies."""
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
prior = base / 'sec-text-loop-metadata-audit-v1.json'
prior_sha = 'e5d871643368caa78868b50f2c9e7ddf01b0ce7caac5f11268d1676866ce1274'
out = base / 'sec-text-additional-logs-audit-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous evidence')
sources = (
    (Path('C:/atx/atx-db/data/raw/sec_text/_logs/fetch-1.log'), 174920),
    (Path('C:/atx/atx-db/data/raw/sec_text/_logs/fetch-2.log'), 6401),
)


def identity(path):
    current = path
    while current != current.parent:
        if current.is_symlink() or current.is_junction():
            raise RuntimeError('Reparse execution metadata path')
        current = current.parent
    before = path.stat()
    if not path.is_file() or before.st_size > 4 * 1024 * 1024:
        raise RuntimeError('Exact small regular execution metadata required')
    digest = hashlib.sha256()
    size = 0
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(256 * 1024), b''):
            size += len(chunk)
            digest.update(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('Metadata changed during identity pass')
    if size != before.st_size:
        raise RuntimeError('Metadata length changed')
    return {'bytes': size, 'sha256': digest.hexdigest()}


prior_pin = identity(prior)
if prior_pin['sha256'] != prior_sha:
    raise RuntimeError('Previous retained audit changed')
previous = json.loads(prior.read_text(encoding='utf-8'))
guard_keys = (
    'guard_version', 'command', 'status', 'returncode', 'child_pid',
    'guard_pid', 'run_seconds', 'stop_reason', 'job_limit_gb', 'receipt',
    'admission', 'native_peak_job_memory_bytes', 'cap_hit',
)
results = []
all_records = [{'source': str(prior), **row} for row in previous['guard_records']]
for path, expected_size in sources:
    pin = identity(path)
    if pin['bytes'] != expected_size:
        raise RuntimeError('Discovered execution log size changed')
    counts = {'physical_lines': 0, 'fetch_progress': 0, 'unclassified': 0}
    guards = []
    loops = []
    with path.open('rb') as stream:
        while True:
            raw = stream.readline(1024 * 1024 + 1)
            if not raw:
                break
            if len(raw) > 1024 * 1024:
                raise RuntimeError('Oversized execution metadata line')
            counts['physical_lines'] += 1
            try:
                row = json.loads(raw)
            except (ValueError, UnicodeError):
                counts['unclassified'] += 1
                continue
            if not isinstance(row, dict):
                counts['unclassified'] += 1
                continue
            command = row.get('command')
            if (isinstance(command, list) and all(isinstance(x, str) for x in command)
                    and 'atx_db.alpha_panel.filing_text' in command):
                item = {'line': counts['physical_lines'],
                        **{key: row.get(key) for key in guard_keys}}
                guards.append(item)
                all_records.append({'source': str(path), **item})
            elif {'attempt', 'phase', 'rc', 'at'}.issubset(row):
                loops.append({'line': counts['physical_lines'],
                              **{key: row[key] for key in ('attempt', 'phase', 'rc', 'at')}})
            elif {'batch', 'ok', 'failed'}.issubset(row):
                counts['fetch_progress'] += 1
            else:
                counts['unclassified'] += 1
            if len(all_records) + len(loops) > 8192:
                raise RuntimeError('Execution metadata record bound')
    if identity(path) != pin:
        raise RuntimeError('Log changed across independent passes')
    results.append({'path': str(path), 'identity': pin, 'counts': counts,
                    'guard_records': guards, 'wrapper_exit_records': loops})

# A PID alone is not an invocation identity. Preserve admitted time and command.
# Never sum state snapshots, merge conflicting records, or infer missing exits.
invocations = {}
unkeyed = 0
for row in all_records:
    admission = row.get('admission')
    admitted = admission.get('admitted_at_utc') if isinstance(admission, dict) else None
    if type(row.get('guard_pid')) is not int or not admitted:
        unkeyed += 1
        continue
    key = json.dumps([row['guard_pid'], admitted, row['command']], sort_keys=True)
    invocation = invocations.setdefault(key, {
        'guard_pid': row['guard_pid'], 'admitted_at_utc': admitted,
        'command': row['command'], 'locations': [], 'terminal_records': [],
    })
    invocation['locations'].append({'source': row['source'], 'line': row['line']})
    if row.get('returncode') is not None:
        terminal = {key: row.get(key) for key in guard_keys if key != 'admission'}
        if terminal not in invocation['terminal_records']:
            invocation['terminal_records'].append(terminal)
if identity(prior) != prior_pin:
    raise RuntimeError('Prior audit changed')
for row in results:
    if identity(Path(row['path'])) != row['identity']:
        raise RuntimeError('Additional log changed before final evidence')
result = {
    'status': 'ADDITIONAL_NAMED_TEXT_EXECUTION_LOGS_INSPECTED_NOT_BUDGET_CERTIFICATION',
    'prior': {'path': str(prior), **prior_pin}, 'additional_logs': results,
    'recovered_invocations': list(invocations.values()), 'unkeyed_guard_records': unkeyed,
    'complete_invocation_inventory': False, 'all_attempt_count': None,
    'budget_upper_bound': None, 'remaining_budget_granted': 0,
    'network_attempts': 0, 'source_body_files_read': 0, 'gate_promotion': False,
    'qualification': 'Only two newly identified exact execution logs and the prior named-loop audit were inspected. Multiple snapshots and shared-log records are grouped by guard PID, admission time and complete command without inferring absent terminals. Rate-lock identity, all invocations, pre-receipt interruptions and original baseline interval remain unproved. No request capacity or source qualification follows.',
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'additional_logs': [
    {'path': row['path'], 'identity': row['identity'], 'counts': row['counts'],
     'guard_records': len(row['guard_records'])} for row in results],
    'invocations': len(invocations), 'unkeyed_guard_records': unkeyed,
    'remaining_budget_granted': 0}), flush=True)
