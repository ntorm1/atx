"""Read-only, bounded chronology audit of the previously inventoried SEC ledgers."""
import collections
import datetime as dt
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
workspace = Path(__file__).resolve().parent
base = Path('C:/atx/atx-db').resolve()
source_inventory = workspace / 't9-source-inventory.json'
output = workspace / 'sec-budget-chronology-audit-v1.json'
if output.exists():
    raise RuntimeError('Preserve prior audit evidence')

def identity(path):
    hasher = hashlib.sha256()
    size = 0
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            hasher.update(chunk)
            size += len(chunk)
    return {'bytes': size, 'sha256': hasher.hexdigest()}

inventory_pin = identity(source_inventory)
inventory = json.loads(source_inventory.read_text(encoding='utf-8'))
paths = {}
counter_paths = {}
for store, specification in inventory['sources'].items():
    for relative in specification['inventories']:
        path = (base / relative).resolve(strict=True)
        if not path.is_relative_to(base / 'data/raw'):
            raise RuntimeError('Ledger escaped known raw directory')
        if path.stat().st_size > 128 * 1024 * 1024:
            raise RuntimeError('Unexpected ledger size')
        paths[path] = store
    for relative in specification['counters']:
        path = (base / relative).resolve(strict=True)
        if not path.is_relative_to(base / 'data/raw') or path.stat().st_size > 16384:
            raise RuntimeError('Unexpected counter scope or size')
        counter_paths[path] = store
if sum(p.stat().st_size for p in paths) > 256 * 1024 * 1024:
    raise RuntimeError('Audit byte ceiling exceeded')

results = []
pins = {}
for path, store in sorted(paths.items()):
    first_stat = path.stat()
    hasher = hashlib.sha256()
    counts = collections.Counter()
    dates = collections.Counter()
    keys = collections.Counter()
    timestamp_fields = collections.Counter()
    first = last = None
    with path.open('rb') as stream:
        while True:
            raw = stream.readline(1024 * 1024 + 1)
            if not raw:
                break
            if len(raw) > 1024 * 1024:
                raise RuntimeError('Oversized ledger row')
            hasher.update(raw)
            counts['bytes'] += len(raw)
            counts['lines'] += 1
            try:
                row = json.loads(raw)
            except (ValueError, UnicodeDecodeError):
                counts['malformed_rows'] += 1
                continue
            if not isinstance(row, dict):
                counts['non_object_rows'] += 1
                continue
            keys.update(row.keys())
            field = next((k for k in ('fetched_at', 'at', 'attempted_at', 'ts', 'timestamp') if row.get(k) is not None), None)
            if field is None:
                counts['no_timestamp_rows'] += 1
                continue
            timestamp_fields[field] += 1
            try:
                value = row[field]
                stamp = (dt.datetime.fromtimestamp(value, dt.timezone.utc)
                         if isinstance(value, (int, float)) and not isinstance(value, bool)
                         else dt.datetime.fromisoformat(str(value).replace('Z', '+00:00')))
                if stamp.tzinfo is None:
                    counts['naive_timestamp_rows'] += 1
                    continue
                stamp = stamp.astimezone(dt.timezone.utc).isoformat()
            except (ValueError, TypeError, OverflowError, OSError):
                counts['invalid_timestamp_rows'] += 1
                continue
            counts['aware_timestamp_rows'] += 1
            dates[stamp[:10]] += 1
            first = min(first or stamp, stamp)
            last = max(last or stamp, stamp)
    pin = {'bytes': counts['bytes'], 'sha256': hasher.hexdigest()}
    now_stat = path.stat()
    if first_stat.st_size != now_stat.st_size or first_stat.st_mtime_ns != now_stat.st_mtime_ns or pin['bytes'] != first_stat.st_size:
        raise RuntimeError('Ledger changed during audit')
    pins[str(path)] = pin
    results.append({'store': store, 'path': str(path), 'identity': pin,
                    'counts': dict(counts), 'utc_dates': dict(sorted(dates.items())),
                    'timestamp_fields': dict(timestamp_fields), 'first_utc': first,
                    'last_utc': last, 'field_presence_counts': dict(sorted(keys.items()))})

counters = []
for path, store in counter_paths.items():
    pin = identity(path)
    value = json.loads(path.read_text(encoding='utf-8'))
    pins[str(path)] = pin
    counters.append({'store': store, 'path': str(path), 'identity': pin, 'counter': value})
for path, pin in pins.items():
    if identity(Path(path)) != pin:
        raise RuntimeError('Known receipt/counter bytes changed across audit')
if identity(source_inventory) != inventory_pin:
    raise RuntimeError('Prior inventory changed')
record = {
    'status': 'KNOWN_SEC_LEDGER_CHRONOLOGY_VERIFIED_NOT_BUDGET_CERTIFICATION',
    'observed_at_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
    'input_inventory': {'path': str(source_inventory), 'identity': inventory_pin},
    'ledger_count': len(results), 'ledger_bytes': sum(row['identity']['bytes'] for row in results),
    'ledgers': results, 'counters': counters,
    'qualification': 'Two-pass current identities and receipt chronology for the exact previously inventoried paths only. No unlisted stores, hidden retries/redirects, pre-receipt interruptions, allocation interval or complete all-attempt accounting is certified. Ledger rows are not interchangeable with request attempts; no sum is granted as remaining budget.',
    'network_attempts': 0, 'source_bodies_read': 0, 'return_rows_read': 0,
    'http_hold_remains': True
}
with output.open('x', encoding='utf-8') as stream:
    json.dump(record, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: value for key, value in record.items() if key not in ('ledgers', 'counters')}))
