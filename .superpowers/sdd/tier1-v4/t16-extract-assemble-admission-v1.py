"""Assemble all85 independently audited receipts; never fill missing successes."""
import hashlib
import json
import os
from pathlib import Path

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'task16-extract-admission-r19-v1.json'
summary_path = workspace / 'task16-extract-admission-r19-v1.audit.json'
if out.exists() or summary_path.exists():
    raise RuntimeError('Preserve existing independent admission')

def read(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise RuntimeError('Duplicate evidence JSON key')
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding='utf-8-sig'), object_pairs_hook=pairs)

def pin(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'sha256': digest.hexdigest(), 'bytes': path.stat().st_size}

directory = Path('C:/atx/atx-db/data/alpha_panel/v1/fund_extract_validation_v1/r19_20261004_001')
receipts, evidence, rows, members = {}, {}, 0, 0
common_source = None
before = {}
for batch in range(85):
    name = f'{batch:04d}'
    admission_path = workspace / f'logs/t16-extract-r19-b{name}-admission-v1.json'
    before[str(admission_path)] = pin(admission_path)
    admitted = read(admission_path)
    if admitted['status'] != 'ONE_GUARDED_SOURCE_REPLAY_INDEPENDENTLY_PINNED' or admitted['batch'] != batch:
        raise RuntimeError('Missing independent successful batch admission')
    native_path = directory / f'batch-{name}.json'
    expected = admitted['native_receipt']
    if Path(expected['path']) != native_path or pin(native_path) != expected['identity']:
        raise RuntimeError('Native receipt changed since independent admission')
    before[str(native_path)] = expected['identity']
    guard_path = Path(admitted['guard_receipt']['path'])
    if pin(guard_path) != admitted['guard_receipt']['identity']:
        raise RuntimeError('Guard evidence changed since independent admission')
    guard = read(guard_path)
    if guard['status'] != 'completed' or guard['returncode'] != 0 or guard['cap_hit']:
        raise RuntimeError('Native guard did not finish successfully')
    before[str(guard_path)] = admitted['guard_receipt']['identity']
    audit_guard_path = workspace / f'logs/t16-extract-r19-b{name}-audit-v1.receipt.json'
    audit_guard = read(audit_guard_path)
    if audit_guard['status'] != 'completed' or audit_guard['returncode'] != 0 or audit_guard['cap_hit']:
        raise RuntimeError('Independent guard did not finish successfully')
    before[str(audit_guard_path)] = pin(audit_guard_path)
    native = read(native_path)
    if native['version'] != 'fund-extract-validation-v1' or type(native['batch']) is not int or native['batch'] != batch:
        raise RuntimeError('Native receipt identity/schema mismatch')
    source = {key: value for key, value in native['source'].items() if key != 'files'}
    if common_source is None:
        common_source = source
    elif source != common_source:
        raise RuntimeError('Mixed archive, extraction plan, code or contract across batches')
    if native['validation']['rows'] != admitted['rows'] or native['validation']['members'] != admitted['members']:
        raise RuntimeError('Admitted diagnostic population changed')
    receipts[str(batch)] = {'path': str(native_path), 'identity': expected['identity']}
    evidence[str(batch)] = {'independent_admission': {'path': str(admission_path), 'identity': before[str(admission_path)]},
                           'native_guard': admitted['guard_receipt'], 'independent_audit_guard': before[str(audit_guard_path)]}
    rows += admitted['rows']
    members += admitted['members']
for path, expected in before.items():
    if pin(path) != expected:
        raise RuntimeError('Evidence changed during complete inventory assembly')
for path, expected in common_source['code'].items():
    if pin(path) != expected:
        raise RuntimeError('Frozen source-validation code changed')
doc = {'version': 'fund-extract-validation-v1', 'receipts': receipts}
payload = (json.dumps(doc, indent=2, sort_keys=True) + '\n').encode()
expected_sha = hashlib.sha256(payload).hexdigest()
summary = {'status': 'ALL85_SUCCESSFUL_REPLAY_RECEIPTS_INDEPENDENTLY_PINNED', 'receipt_count': 85,
           'rows': rows, 'members': members, 'admission': {'path': str(out), 'sha256': expected_sha, 'bytes': len(payload)},
           'execution_evidence': evidence,
           'qualification': 'Complete successful source-validation receipt inventory only. Native freeze must still verify all current actual source/preparation/reader bytes and cached receipt semantics; all financial/economic/production gates remain open.'}
with summary_path.open('x', encoding='utf-8') as stream:
    json.dump(summary, stream, indent=2)
    stream.write('\n')
with out.open('xb') as stream:
    stream.write(payload)
if pin(out) != {'sha256': expected_sha, 'bytes': len(payload)}:
    raise RuntimeError('Written admission differs from independently fixed bytes')
print(json.dumps({key: summary[key] for key in ('status', 'receipt_count', 'rows', 'members', 'admission', 'qualification')}), flush=True)
