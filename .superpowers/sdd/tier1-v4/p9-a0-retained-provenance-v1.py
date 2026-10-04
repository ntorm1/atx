"""Bounded, read-only A0 retained code and preparation-chain inspection."""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
ROOT = Path('C:/atx')
BASE = Path(__file__).resolve().parent
R4 = ROOT / 'atx-db/data/research/identity_rehearsal/session8-phased-r4'
OPS = ROOT / 'atx-db/data/research/identity_inputs/session5/manifest.json'
REVIEW = ROOT / '.superpowers/sdd/tier1-v2/receipts/3.3/s8-combined-reviewed-r3-manifest.json'
PREP = ROOT / 'atx-db/data/research/identity_rehearsal/session5/preparation'
OUT = BASE / 'p9-a0-retained-provenance-v1.json'
NAMES = (
    '_fundamental_clock.py', '_submissions_archive.py', 'calendar.py',
    'historical_identity.py', 'historical_identity_sources.py', 'identity_links.py',
    'identity_reconstruction.py', 'market_daily.py', 'market_owner_bridge.py',
    'ticker_history.py', 'universe_us_listed.py', 'warehouse.py',
)
if OUT.exists():
    raise RuntimeError('Preserve existing result')
held = {}
observed = {}


def safe_path(path):
    if not path.is_absolute() or not path.is_relative_to(ROOT):
        raise RuntimeError('Path outside fixed workspace')
    for item in (path, *path.parents):
        if item.is_symlink() or item.is_junction():
            raise RuntimeError(f'Reparse path: {item}')


def file_state(path):
    safe_path(path)
    try:
        info = path.stat()
    except FileNotFoundError:
        return {'present': False}
    return {'present': True, 'bytes': info.st_size, 'mtime_ns': info.st_mtime_ns,
            'device': info.st_dev, 'inode': info.st_ino,
            'kind': 'file' if stat.S_ISREG(info.st_mode) else 'directory' if stat.S_ISDIR(info.st_mode) else 'other'}


def read_small(path):
    before = file_state(path)
    if before.get('kind') != 'file' or before['bytes'] > 2 * 1024 * 1024:
        raise RuntimeError(f'Small regular file required: {path}')
    raw = path.read_bytes()
    if file_state(path) != before or len(raw) != before['bytes']:
        raise RuntimeError(f'File changed while reading: {path}')
    if path in held and held[path] != raw:
        raise RuntimeError(f'File changed: {path}')
    held[path] = raw
    if sum(map(len, held.values())) > 8 * 1024 * 1024:
        raise RuntimeError('Total held bytes exceeded')
    return raw


def pin(raw):
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def metadata(path, expected):
    raw = read_small(path)
    if pin(raw)['sha256'] != expected:
        raise RuntimeError(f'Original metadata pin differs: {path}')
    return json.loads(raw), {'path': str(path), **pin(raw), 'expected_sha256': expected, 'matches': True}


def observe(path):
    state = file_state(path)
    observed[path] = state
    return {'path': str(path), **state, 'payload_hashed': False}


controller_raw = read_small(Path(__file__).resolve())
request_raw = read_small(BASE / 'p9-al-data-request-v2.md')
if pin(request_raw) != {'bytes': 59567, 'sha256': 'd2182547eaafda33d33c51502c07a4622f36d38fb2ca219fbef4eea95019ab02'}:
    raise RuntimeError('Frozen owner request differs')
build, build_info = metadata(R4 / 'build.json', '1b4c1ae22de46cf21d5eaadf52c50fbe81d2be1c79b3b29a6f0bc48c9e3eee30')
manifest, manifest_info = metadata(R4 / 'manifest.json', 'ac9bcda74b01badf507351a53b8b078c733ce00a7cc01a0dcf33d68935a6bc28')
native_pin = build['pin']
if set(native_pin['code_files']) != set(NAMES) or manifest['code_files'] != native_pin['code_files']:
    raise RuntimeError('Complete twelve-code declaration set differs')
snapshot_dir = R4 / 'code_snapshot'
safe_path(snapshot_dir)
directory_names = sorted(p.name for p in snapshot_dir.iterdir())
if directory_names != sorted(NAMES):
    raise RuntimeError('Snapshot directory differs from complete fixed code set')
code = []
for name in NAMES:
    retained = read_small(snapshot_dir / name)
    if pin(retained)['sha256'] != native_pin['code_files'][name]:
        raise RuntimeError(f'Retained original code hash differs: {name}')
    relative = 'atx-db/src/atx_db/' + name
    current = read_small(ROOT / relative)
    historical = subprocess.run(['git', '-C', str(ROOT), 'show', f'dec03454:{relative}'],
                                check=True, capture_output=True, timeout=15).stdout
    if len(historical) > 1024 * 1024:
        raise RuntimeError('Historical code too large')
    normalize = lambda raw: raw.replace(b'\r\n', b'\n')
    code.append({'name': name, 'declared_raw_sha256': native_pin['code_files'][name],
        'retained_raw': pin(retained), 'retained_lf': pin(normalize(retained)),
        'retained_crlf_count': retained.count(b'\r\n'),
        'retained_bare_lf_count': retained.count(b'\n') - retained.count(b'\r\n'),
        'current_raw': pin(current), 'current_lf': pin(normalize(current)),
        'dec03454_raw': pin(historical), 'dec03454_lf': pin(normalize(historical)),
        'retained_matches_current_raw': retained == current,
        'retained_matches_current_lf': normalize(retained) == normalize(current),
        'retained_matches_dec03454_raw': retained == historical,
        'retained_matches_dec03454_lf': normalize(retained) == normalize(historical)})
ops, ops_info = metadata(OPS, native_pin['ops_manifest_sha256'])
if Path(native_pin['document_review']['path']) != REVIEW:
    raise RuntimeError('Original review path differs')
review, review_info = metadata(REVIEW, native_pin['document_review']['sha256'])
prep = [observe(PREP), observe(PREP / 'preparation.json'), observe(PREP / 'retained.duckdb')]
fixed_sources = {
    'company_tickers': ROOT / 'atx-db/data/cache/company_tickers.json',
    'companyfacts': ROOT / 'atx-db/data/cache/companyfacts.zip',
    'submissions': ROOT / 'atx-db/data/cache/submissions.zip',
    'ticker_history': ROOT / 'atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet',
}
if set(native_pin['inputs']) != set(fixed_sources):
    raise RuntimeError('Original source roles differ')
sources = []
for name, path in fixed_sources.items():
    declaration = native_pin['inputs'][name]
    if Path(declaration['path']) != path:
        raise RuntimeError('Original source path differs')
    sources.append({'role': name, 'declared': declaration, 'observed': observe(path)})
exports = []
if not 0 < len(ops['exports']) <= 32:
    raise RuntimeError('OPS export count out of bound')
for row in ops['exports']:
    name = row['file']
    if Path(name).name != name or not name.endswith('.parquet'):
        raise RuntimeError('OPS export must be a simple Parquet basename')
    exports.append({'file': name, 'table': row['table'], 'declared_bytes': row['bytes'],
                    'declared_sha256': row['sha256'], 'observed': observe(OPS.parent / name)})
documents = native_pin['document_review']['documents']
if not 0 < len(documents) <= 2000:
    raise RuntimeError('Document count out of bound')
document_states = []
document_root = ROOT / 'atx-db/data/cache/identity-oracle-session8'
for path_text, sha in documents.items():
    path = Path(path_text)
    if not path.is_relative_to(document_root):
        raise RuntimeError('Document outside fixed original review cache')
    document_states.append({'declared_sha256': sha, **observe(path)})
for path, raw in list(held.items()):
    if read_small(path) != raw:
        raise RuntimeError('Independent final byte check failed')
for path, state in observed.items():
    if file_state(path) != state:
        raise RuntimeError(f'Stat-only input changed: {path}')
if sorted(p.name for p in snapshot_dir.iterdir()) != directory_names:
    raise RuntimeError('Code snapshot directory changed')
summary = {'retained_raw_code_pins_verified': len(code),
    'retained_matches_dec03454_lf': sum(r['retained_matches_dec03454_lf'] for r in code),
    'retained_matches_current_lf': sum(r['retained_matches_current_lf'] for r in code),
    'original_preparation_present': prep[0]['present'],
    'ops_metadata_pin_matches': True, 'review_metadata_pin_matches': True,
    'original_source_paths_present': sum(r['observed']['present'] for r in sources),
    'ops_export_paths_present': sum(r['observed']['present'] for r in exports),
    'ops_export_paths_total': len(exports),
    'document_paths_present': sum(r['present'] for r in document_states),
    'document_paths_total': len(document_states)}
result = {'status': 'A0_RETAINED_PROVENANCE_INSPECTED_NO_REPRODUCTION',
    'controller': {'path': str(Path(__file__).resolve()), **pin(controller_raw)},
    'request': pin(request_raw), 'original_build': build_info, 'original_manifest': manifest_info,
    'code': code, 'original_ops': ops_info, 'original_review': review_info,
    'preparation_paths': prep,
    'declared_preparation_sha256': native_pin['preparation_manifest_sha256'],
    'declared_retained_database_sha256': native_pin['retained_database_sha256'],
    'original_inputs': sources, 'ops_export_stat_only': exports,
    'reviewed_document_stat_only': document_states, 'summary': summary,
    'd0_reproduction_measured': False, 'arms_run': [], 'supported_single_build_commit': None,
    'parquet_database_zip_html_bodies_read': 0, 'network_attempts': 0,
    'source_quality_certified': False, 'gate_promotion': False,
    'qualification': 'Exact retained code and small manifest bytes checked before/finally. LF comparison diagnoses code-byte continuity only and never replaces native raw hash guards. Source, OPS export and reviewed document paths are stat-only; presence does not verify their bytes, coverage or quality. Missing preparation requires fresh pinned preparation. No data build, D0 measurement or producer-commit attribution occurred.'}
with OUT.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], **summary}), flush=True)
