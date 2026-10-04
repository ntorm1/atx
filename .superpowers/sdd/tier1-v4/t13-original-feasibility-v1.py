"""Bounded original-byte feasibility check; never acquires or certifies a source."""
import datetime as dt
import gzip
import hashlib
import json
import os
import re
from collections import Counter
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C, events_sources as E

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 'task13-original-feasibility-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous evidence')
limit = 2 * 1024 * 1024
catalog = E.catalog_path()
expected = 'a76d19d7458481fcd67de4f7397617234f34b98843b2449c291628564690bf64'
if C.sha256_file(catalog) != expected:
    raise RuntimeError('Catalog no longer matches preflight')
counts, selected = Counter(), {}
scanned = 0
for batch in pq.ParquetFile(catalog).iter_batches(batch_size=256):
    for row in batch.to_pylist():
        scanned += 1
        if scanned > 60000:
            raise RuntimeError('Catalog row bound exceeded')
        date, form = row.get('filing_date'), row.get('form')
        if date is None or date < dt.date(2020, 1, 1) or form not in ('8-K', '8-K/A', '10-Q', '10-Q/A'):
            continue
        source = row.get('doc_source')
        counts[(date.year, form, str(source))] += 1
        base = form.removesuffix('/A')
        strata = base + ':' + str(source)
        if strata not in ('8-K:landed_v2', '8-K:sec_events', '10-Q:sec_events'):
            continue
        key = (str(date), int(row['cik']), str(row['accession']), str(row.get('source_doc') or ''), str(row.get('doc_sha256') or ''))
        if strata not in selected or key < selected[strata][0]:
            selected[strata] = (key, row)

def ledger_matches(path, pred):
    if not path.exists():
        return {'missing': True}, []
    if path.stat().st_size > 64 * 1024 * 1024:
        raise RuntimeError('Ledger byte bound exceeded')
    pin = {'path': str(path), 'bytes': path.stat().st_size, 'sha256': C.sha256_file(path)}
    matches = []
    with path.open('rb') as stream:
        for line_number in range(1, 200001):
            raw = stream.readline(1024 * 1024 + 1)
            if not raw:
                break
            if len(raw) > 1024 * 1024:
                raise RuntimeError('Ledger line bound exceeded')
            record = json.loads(raw)
            if pred(record):
                matches.append({'line': line_number, 'line_sha256': hashlib.sha256(raw).hexdigest(), 'record': record})
                if len(matches) > 32:
                    raise RuntimeError('Ledger match bound exceeded')
        else:
            raise RuntimeError('Ledger record bound exceeded')
    if C.sha256_file(path) != pin['sha256']:
        raise RuntimeError('Ledger changed during scan')
    return pin, matches

results, total = [], 0
for strata in ('8-K:landed_v2', '8-K:sec_events', '10-Q:sec_events'):
    if strata not in selected:
        results.append({'stratum': strata, 'status': 'NO_ELIGIBLE_CATALOG_ROW'})
        continue
    row = selected[strata][1]
    record = {'stratum': strata, 'catalog_row': row, 'status': 'UNQUALIFIED'}
    source = row['doc_source']
    root = E.EARNINGS_RELEASE_DIR if source == 'landed_v2' else E.RAW_DIR
    if source == 'landed_v2':
        ledger, matches = ledger_matches(root / 'units-done.jsonl', lambda x: x.get('accession') == row['accession'] and x.get('document_sha256') == row.get('doc_sha256'))
    else:
        ledger, matches = ledger_matches(root / 'fetch-ledger.jsonl', lambda x: x.get('url') == row.get('url'))
    record.update(ledger_identity=ledger, matching_receipts=matches)
    sha = row.get('doc_sha256')
    if not sha and source == 'sec_events' and matches:
        sha = matches[-1]['record'].get('sha256')
    if not isinstance(sha, str) or not re.fullmatch('[0-9a-f]{64}', sha):
        record['status'] = 'NO_UNAMBIGUOUS_ORIGINAL_SHA'
        results.append(record)
        continue
    objects = root / 'objects' / sha[:2]
    candidates = [objects / (sha + '.gz'), objects / sha]
    path = next((p for p in candidates if p.exists()), None)
    if path is None:
        record['status'] = 'MISSING_RETAINED_OBJECT'
        results.append(record)
        continue
    size = path.stat().st_size
    record.update(object_path=str(path), stored_bytes=size)
    if size > limit:
        record['status'] = 'STORED_OBJECT_EXCEEDS_2MIB'
        results.append(record)
        continue
    stored_pin = C.sha256_file(path)
    opener = gzip.open if path.suffix == '.gz' else open
    with opener(path, 'rb') as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        record['status'] = 'DECODED_OBJECT_EXCEEDS_2MIB'
        results.append(record)
        continue
    total += size + len(raw)
    if total > 12 * 1024 * 1024:
        raise RuntimeError('Aggregate original-byte bound exceeded')
    raw_sha = hashlib.sha256(raw).hexdigest()
    if raw_sha != sha or C.sha256_file(path) != stored_pin:
        raise RuntimeError('Original byte identity mismatch')
    text = E.document_text(raw, str(row.get('source_doc') or ''))
    fragments = []
    for match in re.finditer(r'(?i)dividend|repurchas|buyback|spin.off', text):
        fragments.append(text[max(0, match.start() - 100):match.end() + 380])
        if len(fragments) == 10:
            break
    record.update(status='ORIGINAL_BYTES_MATCH_ONLY', stored_sha256=stored_pin,
                  original_sha256=raw_sha, original_bytes=len(raw), storage_encoding='gzip' if path.suffix == '.gz' else 'plain',
                  normalized_utf8_sha256=hashlib.sha256(text.encode('utf-8')).hexdigest(),
                  normalized_characters=len(text), header=text[:400], keyword_snippets=fragments,
                  clock_class_currency_cadence='UNQUALIFIED')
    results.append(record)
if C.sha256_file(catalog) != expected:
    raise RuntimeError('Catalog changed after use')
result = {'status': 'BOUNDED_RETAINED_ORIGINAL_FEASIBILITY_ONLY', 'catalog_sha256': expected,
          'catalog_rows_scanned': scanned, 'eligible_counts': [{'year': y, 'form': f, 'doc_source': s, 'rows': count} for (y, f, s), count in sorted(counts.items())],
          'selected': results, 'stored_plus_decoded_bytes': total, 'http_attempts': 0,
          'qualification': 'Original byte/source-layout feasibility only; no declaration population, source-clock/class certification, handcheck or gate PASS.'}
encoded = json.dumps(result, indent=2, default=str).encode('utf-8')
if len(encoded) > 128 * 1024:
    raise RuntimeError('Audit output bound exceeded')
out.write_bytes(encoded + b'\n')
print(json.dumps({'status': result['status'], 'catalog_rows_scanned': scanned, 'eligible_rows': sum(counts.values()),
                  'selected': [{'stratum': x['stratum'], 'status': x['status']} for x in results],
                  'stored_plus_decoded_bytes': total, 'http_attempts': 0}), flush=True)
