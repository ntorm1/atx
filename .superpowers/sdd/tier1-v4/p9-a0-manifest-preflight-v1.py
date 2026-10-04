"""A0 D0 provenance leads from exact named metadata; never D0 row reproduction."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
workspace = Path('C:/atx')
base = Path(__file__).resolve().parent
output = base / 'p9-a0-manifest-preflight-v1.json'
if output.exists():
    raise RuntimeError('Preserve previous preflight')
request = base / 'p9-al-data-request-v2.md'
request_sha = 'd2182547eaafda33d33c51502c07a4622f36d38fb2ca219fbef4eea95019ab02'
root = workspace / 'atx-db/data/alpha_panel/v1'
rehearsal = workspace / 'atx-db/data/research/identity_rehearsal/session8-phased-r4'
sources = [
    ('published_pit_bridge', root / 'export/identity-bridge-v2-pit/manifest.json',
     ['schema', 'rule', 'status', 'code', 'source', 'files']),
    ('published_link_table', root / 'identity/link_table_manifest.json',
     ['schema', 'rule', 'status', 'code', 'inputs', 'files']),
    ('published_stage_i', root / 'identity/manifest.json',
     ['stage', 'code_version', 'sources', 'outputs', 'validation']),
    ('published_backfill', root / 'identity/backfill_manifest.json',
     ['rule', 'backfill_rows', 'name_tier', 'code', 'code_version']),
    ('published_name', root / 'identity/name_manifest.json',
     ['rule', 'code', 'code_version']),
    ('published_r4_manifest', rehearsal / 'manifest.json',
     ['code_sha256', 'code_files', 'preparation', 'pin', 'work', 'out']),
    ('published_r4_build', rehearsal / 'build.json',
     ['code_sha256', 'code_files', 'preparation', 'pin', 'work', 'out']),
]
code_paths = [
    'atx-db/src/atx_db/alpha_panel/identity_table.py',
    'atx-db/src/atx_db/alpha_panel/identity_links.py',
    'atx-db/src/atx_db/alpha_panel/identity_backfill.py',
    'atx-db/src/atx_db/alpha_panel/identity_names.py',
    'atx-db/src/atx_db/alpha_panel/common.py',
    'atx-db/src/atx_db/identity_links.py',
    'atx-db/src/atx_db/identity_reconstruction.py',
    'atx-db/src/atx_db/market_owner_bridge.py',
]


def read_small(path):
    current = path
    while current != current.parent:
        if current.is_symlink() or current.is_junction():
            raise RuntimeError('Reparse metadata/code path')
        current = current.parent
    before = path.stat()
    if not path.is_file() or before.st_size > 2 * 1024 * 1024:
        raise RuntimeError('Small regular metadata/code file required')
    raw = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('File changed while reading')
    if len(raw) != before.st_size:
        raise RuntimeError('Read length changed')
    return raw


def pin(raw):
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


request_raw = read_small(request)
if pin(request_raw) != {'bytes': 59567, 'sha256': request_sha}:
    raise RuntimeError('Frozen request changed')
held = {request: request_raw}
metadata = []
for label, path, fields in sources:
    raw = read_small(path)
    held[path] = raw
    data = json.loads(raw)
    if not isinstance(data, dict):
        raise RuntimeError('Metadata root must be an object')
    metadata.append({'role': label, 'path': str(path), **pin(raw),
                     'top_level_keys': sorted(data),
                     'declared_fields_only': {key: data.get(key) for key in fields},
                     'fields_absent': [key for key in fields if key not in data]})
if sum(len(raw) for raw in held.values()) > 4 * 1024 * 1024:
    raise RuntimeError('Total metadata bound')
codes = []
for relative in code_paths:
    historical = subprocess.run(['git', '-C', str(workspace), 'show', f'dec03454:{relative}'],
                                check=True, capture_output=True).stdout
    if len(historical) > 1024 * 1024:
        raise RuntimeError('Unexpected historical code length')
    path = workspace / relative
    current = read_small(path)
    held[path] = current
    codes.append({'path': relative, 'historical_revision': 'dec03454',
                  'historical_raw': pin(historical),
                  'historical_lf': pin(historical.replace(b'\r\n', b'\n')),
                  'current_raw': pin(current),
                  'current_lf': pin(current.replace(b'\r\n', b'\n')),
                  'current_matches_historical_raw': current == historical,
                  'current_matches_historical_lf': current.replace(b'\r\n', b'\n') == historical.replace(b'\r\n', b'\n')})
for path, raw in held.items():
    if read_small(path) != raw:
        raise RuntimeError('Metadata/code changed across independent final read')
result = {
    'status': 'A0_NAMED_PROVENANCE_METADATA_INSPECTED_NO_REPRODUCTION',
    'request': {'path': str(request), **pin(request_raw)},
    'expected_published_bridge_manifest_sha256': '09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01',
    'published_bridge_matches_request_pin': metadata[0]['sha256'] == '09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01',
    'metadata': metadata, 'historical_and_current_code': codes,
    'supported_single_build_commit': None,
    'd0_reproduction_measured': False, 'arm_rebuilds_run': [],
    'parquet_bodies_read': 0, 'network_attempts': 0,
    'source_quality_certified': False, 'gate_promotion': False,
    'qualification': 'Seven exact named JSON manifests and eight source files were checked before/finally. Code bytes at dec03454 are read-only Git objects. Reported manifest declarations and code comparisons are provenance leads, not proof of original payloads, complete source coverage, a single producing commit or a rebuilt D0 comparison. Missing fields remain absent; no declared path is followed to arbitrary payloads. Published data and other worktree remain unchanged.',
}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'],
                  'published_bridge_matches_request_pin': result['published_bridge_matches_request_pin'],
                  'named_manifests': len(metadata), 'code_files': len(codes),
                  'metadata_bytes': sum(row['bytes'] for row in metadata),
                  'd0_reproduction_measured': False, 'gate_promotion': False}), flush=True)
