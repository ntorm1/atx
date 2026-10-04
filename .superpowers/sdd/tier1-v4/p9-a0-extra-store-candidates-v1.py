"""Bounded metadata-only discovery of additional native original-body stores."""
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import stat
import time

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
BASE = Path(__file__).resolve().parent
ROOT = Path('C:/atx/atx-db/data')
RAW = ROOT / 'raw'
BUILD = ROOT / 'research/identity_rehearsal/session8-phased-r4/build.json'
OUT = BASE / 'p9-a0-extra-store-candidates-v1.json'
deadline = time.monotonic() + 45
if OUT.exists():
    raise RuntimeError('Preserve existing evidence')
observations = {}


def state(path):
    if not path.is_relative_to(ROOT):
        raise RuntimeError('Outside named data metadata scope')
    for item in (path, *path.parents):
        if item.is_symlink() or item.is_junction():
            raise RuntimeError('Reparse path refused')
    try:
        s = path.stat()
    except FileNotFoundError:
        return {'present': False}
    return {'present': True, 'bytes': s.st_size, 'mtime_ns': s.st_mtime_ns,
            'inode': s.st_ino, 'kind': 'file' if stat.S_ISREG(s.st_mode) else 'directory'}


def observe(path):
    current = state(path)
    observations[path] = current
    return current


before = observe(BUILD)
if before['bytes'] > 1024 * 1024:
    raise RuntimeError('Original metadata exceeds bound')
raw = BUILD.read_bytes()
if hashlib.sha256(raw).hexdigest() != '1b4c1ae22de46cf21d5eaadf52c50fbe81d2be1c79b3b29a6f0bc48c9e3eee30':
    raise RuntimeError('Original build changed')
docs = json.loads(raw)['pin']['document_review']['documents']
if len(docs) != 367:
    raise RuntimeError('Full original document set differs')
queue = deque([(RAW, 0)])
visited = []
stores = []
truncated = []
while queue:
    if len(visited) >= 256 or time.monotonic() >= deadline:
        raise RuntimeError('Bounded discovery stopped before a result')
    directory, depth = queue.popleft()
    if observe(directory).get('kind') != 'directory':
        raise RuntimeError('Expected original raw directory')
    ledger = directory / 'fetch-ledger.jsonl'
    if observe(ledger).get('kind') == 'file':
        stores.append(directory)
    visited.append(str(directory))
    with os.scandir(directory) as entries:
        count = 0
        for entry in entries:
            count += 1
            if count > 4096:
                raise RuntimeError('Directory metadata count bound')
            if entry.name == 'objects' or not entry.is_dir(follow_symlinks=False):
                continue
            child = Path(entry.path)
            if depth < 3:
                queue.append((child, depth + 1))
            else:
                truncated.append(str(child))
stores.sort()
old_stores = {RAW / p for p in ('sec_events', 'sec-earnings-release',
    'sec_issuer_history/round2', 'sec_universe_hand_check/round2')}
additional = [p for p in stores if p not in old_stores]
rows = []
for original, digest in sorted(docs.items()):
    if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
        raise RuntimeError('Invalid original digest')
    candidates = []
    for store in additional:
        for name in (digest, digest + '.gz'):
            path = store / 'objects' / digest[:2] / name
            current = observe(path)
            if current['present']:
                candidates.append({'path': str(path), 'stat_only': current})
        if time.monotonic() >= deadline:
            raise RuntimeError('Lookup time bound')
    rows.append({'original_path': original, 'sha256': digest, 'candidates': candidates})
for path, current in observations.items():
    if state(path) != current:
        raise RuntimeError('Named metadata state changed')
if BUILD.read_bytes() != raw:
    raise RuntimeError('Original build changed before publication')
summary = {'directories_visited': len(visited), 'native_stores_found': len(stores),
    'additional_stores_checked': len(additional), 'depth_limited_directories_not_searched': len(truncated),
    'original_documents': len(rows), 'documents_with_candidates': sum(bool(r['candidates']) for r in rows)}
result = {'status': 'BOUNDED_ADDITIONAL_NATIVE_CACHE_METADATA_ONLY', 'summary': summary,
    'native_stores': [str(p) for p in stores], 'additional_stores': [str(p) for p in additional],
    'directories_visited': visited, 'depth_limited_directories_not_searched': truncated,
    'document_candidates': rows, 'payload_bytes_read': 0, 'network_attempts': 0,
    'restored_files': 0, 'gate_promotion': False,
    'qualification': 'Native ledger parent directories within raw depth0..3 only. Objects subtrees are pruned during discovery and only exact original SHA plain/gzip names are stat-checked. No complete search of arbitrary backups, deeper stores, different storage layouts or body contents is claimed. Negative results do not establish global absence or source completeness.'}
with OUT.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], **summary}), flush=True)
