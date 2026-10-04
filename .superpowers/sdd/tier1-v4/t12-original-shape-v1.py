"""Bounded source terms/layout snippets from the already selected Avon original."""
import gzip
import hashlib
import json
import os
import re
from pathlib import Path
from atx_db.alpha_panel import events_sources as E

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'task12-original-shape-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous source audit')
prior_path = workspace / 'task13-original-feasibility-v1.json'
prior_bytes = prior_path.read_bytes()
prior = json.loads(prior_bytes)
record = next(row for row in prior['selected'] if row['stratum'] == '8-K:sec_events')
if record['catalog_row']['accession'] != '0000950157-20-000022' or record['status'] != 'ORIGINAL_BYTES_MATCH_ONLY':
    raise RuntimeError('Unexpected source selection')
path = Path(record['object_path'])
limit = 2 * 1024 * 1024
if path.stat().st_size > limit:
    raise RuntimeError('Stored bound exceeded')
stored = path.read_bytes()
if hashlib.sha256(stored).hexdigest() != record['stored_sha256']:
    raise RuntimeError('Stored original changed')
with gzip.open(path, 'rb') as stream:
    raw = stream.read(limit + 1)
if len(raw) > limit or hashlib.sha256(raw).hexdigest() != record['original_sha256']:
    raise RuntimeError('Original-byte bound/hash mismatch')
text = E.document_text(raw, record['catalog_row']['source_doc'])
snippets = []
last_end = -1
for match in re.finditer(r'(?i)consummat|completed|effective time|converted into|each share|merger consideration|exchange ratio|class [ab]|ordinary shares', text):
    start, end = max(0, match.start() - 180), min(len(text), match.end() + 950)
    if start <= last_end:
        continue
    snippets.append({'start': start, 'end': end, 'text': text[start:end]})
    last_end = end
    if len(snippets) == 16:
        break
native = raw.decode('utf-8', errors='replace')
result = {'status': 'NATIVE_SOURCE_SHAPE_ONLY', 'prior_audit_sha256': hashlib.sha256(prior_bytes).hexdigest(),
          'catalog_row_unqualified': record['catalog_row'], 'object_path': str(path),
          'stored_sha256': record['stored_sha256'], 'original_sha256': record['original_sha256'],
          'original_bytes': len(raw), 'normalized_sha256': hashlib.sha256(text.encode()).hexdigest(),
          'native_acceptance_header_present': bool(re.search(r'<ACCEPTANCE-DATETIME>', native, re.I)),
          'native_submission_cik_header_present': bool(re.search(r'CENTRAL INDEX KEY:', native, re.I)),
          'header': text[:1200], 'terms_snippets': snippets,
          'returns_computed': False, 'http_attempts': 0,
          'qualification': 'Source-layout/terms leads only. No native clock, class mapping, settlement-price basis, exactDLRET or gate certification.'}
if hashlib.sha256(path.read_bytes()).hexdigest() != record['stored_sha256'] or prior_path.read_bytes() != prior_bytes:
    raise RuntimeError('Source changed during audit')
encoded = json.dumps(result, indent=2, default=str)
if len(encoded.encode()) > 64 * 1024:
    raise RuntimeError('Output bound exceeded')
out.write_text(encoded + '\n', encoding='utf-8')
print(json.dumps({'status': result['status'], 'original_bytes': len(raw), 'snippets': len(snippets),
                  'native_acceptance_header_present': result['native_acceptance_header_present'], 'http_attempts': 0}), flush=True)
