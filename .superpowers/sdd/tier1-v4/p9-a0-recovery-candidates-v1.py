"""Locate exact named cache candidates; hash one retained vendor candidate read-only."""
import hashlib
import json
import os
from pathlib import Path
import stat
import time

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
ROOT = Path('C:/atx')
BASE = Path(__file__).resolve().parent
OUT = BASE / 'p9-a0-recovery-candidates-v1.json'
BUILD = ROOT / 'atx-db/data/research/identity_rehearsal/session8-phased-r4/build.json'
VENDOR = Path('C:/Users/natha/Downloads/TickerHistory3.parquet')
STORES = [ROOT / ('atx-db/data/raw/' + relative) for relative in (
    'sec_events', 'sec-earnings-release', 'sec_issuer_history/round2',
    'sec_universe_hand_check/round2',
)]
deadline = time.monotonic() + 50
if OUT.exists():
    raise RuntimeError('Preserve previous evidence')


def state(path):
    if not (path.is_relative_to(ROOT) or path == VENDOR):
        raise RuntimeError('Outside named source scope')
    for part in (path, *path.parents):
        if part.is_symlink() or part.is_junction():
            raise RuntimeError(f'Reparse path: {part}')
    try:
        s = path.stat()
    except FileNotFoundError:
        return {'present': False}
    if not stat.S_ISREG(s.st_mode):
        raise RuntimeError(f'Regular file required: {path}')
    return {'present': True, 'bytes': s.st_size, 'mtime_ns': s.st_mtime_ns,
            'inode': s.st_ino, 'device': s.st_dev}


def check_time():
    if time.monotonic() >= deadline:
        raise TimeoutError('Bounded source observation exceeded 50 seconds')


build_before = state(BUILD)
if build_before['bytes'] > 1024 * 1024:
    raise RuntimeError('Unexpected build metadata size')
raw = BUILD.read_bytes()
if hashlib.sha256(raw).hexdigest() != '1b4c1ae22de46cf21d5eaadf52c50fbe81d2be1c79b3b29a6f0bc48c9e3eee30':
    raise RuntimeError('Published original build differs')
pin = json.loads(raw)['pin']
docs = pin['document_review']['documents']
if len(docs) != 367:
    raise RuntimeError('Original complete document population differs')
observed = {}
found = []
for original_path, digest in docs.items():
    if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
        raise RuntimeError('Bad pinned digest')
    candidates = []
    for store in STORES:
        for name in (digest + '.gz', digest):
            path = store / 'objects' / digest[:2] / name
            observed[path] = state(path)
            if observed[path]['present']:
                candidates.append({'path': str(path), **observed[path]})
    found.append({'original_path': original_path, 'original_sha256': digest,
                  'candidates_stat_only': candidates})
    check_time()
vendor_before = state(VENDOR)
vendor_result = {'path': str(VENDOR), **vendor_before,
                 'expected_sha256': pin['inputs']['ticker_history']['sha256'],
                 'observed_sha256': None, 'matches_original_bytes': None}
if vendor_before['present']:
    if vendor_before['bytes'] > 4 * 1024**3:
        raise RuntimeError('Vendor candidate exceeds declared bound')
    digest = hashlib.sha256()
    count = 0
    with VENDOR.open('rb') as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns) != (
                vendor_before['device'], vendor_before['inode'], vendor_before['bytes'], vendor_before['mtime_ns']):
            raise RuntimeError('Vendor open identity differs')
        while block := stream.read(1024 * 1024):
            digest.update(block)
            count += len(block)
            check_time()
        after = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns) != (
                after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise RuntimeError('Vendor candidate changed during observation')
    if state(VENDOR) != vendor_before or count != vendor_before['bytes']:
        raise RuntimeError('Vendor candidate identity/length changed')
    vendor_result.update(observed_sha256=digest.hexdigest(),
        matches_original_bytes=digest.hexdigest() == vendor_result['expected_sha256'])
for path, prior in observed.items():
    if state(path) != prior:
        raise RuntimeError('Named cache candidate state changed')
if state(BUILD) != build_before or BUILD.read_bytes() != raw:
    raise RuntimeError('Build changed across observation')
summary = {'documents_in_complete_original_pin': len(found),
    'documents_with_named_cache_candidate': sum(bool(r['candidates_stat_only']) for r in found),
    'candidate_objects_total': sum(len(r['candidates_stat_only']) for r in found),
    'vendor_candidate_matches_original_bytes': vendor_result['matches_original_bytes']}
result = {'status': 'A0_CACHE_RECOVERY_CANDIDATES_INSPECTED_NO_RESTORE',
    'summary': summary, 'stores_checked': [str(p) for p in STORES],
    'document_candidates': found, 'vendor_candidate': vendor_result,
    'native_object_naming_reference': 'atx-db/src/atx_db/sec_http.py FetchLedgerStore',
    'original_build_sha256': hashlib.sha256(raw).hexdigest(),
    'elapsed_seconds': round(50 - (deadline - time.monotonic()), 3),
    'html_or_gzip_payloads_read': 0, 'network_attempts': 0, 'files_restored': 0,
    'gate_promotion': False,
    'qualification': 'Complete 367 pinned document SHA names checked only in four explicitly named retained native stores. Candidate pathname/stat is not raw-body hash or ledger authority. No complete search of all possible stores or backup media is claimed. One named vendor file is streamed once with before/open/final identity checks; its digest is a recovery lead, not historical source-quality admission or global quiescence. No source data or producer pins were changed.'}
with OUT.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], **summary}), flush=True)
