"""Recover named loop execution records without granting a request budget."""
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
source = Path('C:/atx/atx-db/data/raw/sec_text/_logs/fetch-loop.log')
script = source.with_name('fetch_loop.sh')
output = base / 'sec-text-loop-metadata-audit-v1.json'
if output.exists():
    raise RuntimeError('Preserve prior evidence')


def safe_file(path):
    current = path
    while current != current.parent:
        if current.is_symlink() or current.is_junction():
            raise RuntimeError('Reparse metadata path')
        current = current.parent
    if not path.is_file():
        raise RuntimeError('Exact regular metadata file required')


safe_file(script)
script_bytes = script.read_bytes()
script_pin = {'bytes': len(script_bytes), 'sha256': hashlib.sha256(script_bytes).hexdigest()}
if script_pin != {'bytes': 893, 'sha256': 'a8f7b0565fd473ec97b4ed1a145dcddd946ebc8070aee4f3e1ba46e1a9a14f49'}:
    raise RuntimeError('Previously identified wrapper changed')
safe_file(source)
before = source.stat()
if before.st_size > 64 * 1024 * 1024:
    raise RuntimeError('Named execution log exceeds64MiB bound')
guards = []
loops = []
progress_count = 0
unclassified = 0
line_count = 0
digest = hashlib.sha256()
with source.open('rb') as stream:
    while True:
        raw = stream.readline(1024 * 1024 + 1)
        if not raw:
            break
        if len(raw) > 1024 * 1024:
            raise RuntimeError('Execution log line exceeds1MiB bound')
        line_count += 1
        digest.update(raw)
        try:
            value = json.loads(raw)
        except (ValueError, UnicodeError):
            unclassified += 1
            continue
        if not isinstance(value, dict):
            unclassified += 1
            continue
        command = value.get('command')
        if (isinstance(command, list) and all(isinstance(item, str) for item in command)
                and 'atx_db.alpha_panel.filing_text' in command):
            selected = {key: value.get(key) for key in (
                'guard_version', 'command', 'status', 'returncode', 'child_pid',
                'guard_pid', 'run_seconds', 'stop_reason', 'job_limit_gb', 'receipt',
                'admission', 'native_peak_job_memory_bytes', 'cap_hit',
            )}
            guards.append({'line': line_count, **selected})
        elif {'attempt', 'phase', 'rc', 'at'}.issubset(value):
            loops.append({'line': line_count, **{key: value[key] for key in ('attempt', 'phase', 'rc', 'at')}})
        elif {'batch', 'ok', 'failed'}.issubset(value):
            progress_count += 1
        else:
            unclassified += 1
        if len(guards) + len(loops) > 4096:
            raise RuntimeError('Execution record count bound exceeded')
after = source.stat()
if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
    raise RuntimeError('Log changed during read')
second = hashlib.sha256()
with source.open('rb') as stream:
    for block in iter(lambda: stream.read(256 * 1024), b''):
        second.update(block)
final = source.stat()
if (before.st_size, before.st_mtime_ns) != (final.st_size, final.st_mtime_ns) or second.digest() != digest.digest():
    raise RuntimeError('Log bytes changed on independent pass')
if script.read_bytes() != script_bytes:
    raise RuntimeError('Wrapper changed during inspection')
result = {
    'status': 'NAMED_TEXT_LOOP_EXECUTION_LOG_INSPECTED_NOT_BUDGET_CERTIFICATION',
    'source': {'path': str(source), 'bytes': before.st_size, 'sha256': digest.hexdigest()},
    'wrapper': {'path': str(script), **script_pin},
    'lines': line_count, 'guard_records': guards, 'loop_exit_records': loops,
    'fetch_progress_line_count': progress_count, 'unclassified_line_count': unclassified,
    'complete_invocation_inventory': False, 'all_attempt_count': None,
    'budget_upper_bound': None, 'remaining_budget_granted': 0,
    'network_attempts': 0, 'source_body_files_read': 0, 'gate_promotion': False,
    'qualification': 'Only the exact previously identified wrapper and its named stdout log were read and checked twice. Structured execution records are retained as leads; unknown/non-JSON contents are not copied. Wrapper exit records do not authenticate a complete child inventory, rate-lock coverage, original budget interval, durable attempt counts or absence of killed/duplicate producers. No request capacity or source qualification follows.',
}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'source': result['source'],
                  'guard_records': len(guards), 'loop_exit_records': len(loops),
                  'fetch_progress_lines': progress_count, 'unclassified_lines': unclassified,
                  'remaining_budget_granted': 0}), flush=True)
