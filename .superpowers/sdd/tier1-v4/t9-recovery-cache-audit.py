"""Bounded cross-store FPI receipt/cache audit; never issues HTTP."""
import collections
import json
from pathlib import Path
from urllib.parse import urlparse
from atx_db.alpha_panel import common as C

base = Path('C:/atx/atx-db/data')
urls = (base/'alpha_panel/v1/_tmp/events/fetch_fpi.txt').read_text(encoding='utf-8').splitlines()
accessions = {url.split('/')[-2] for url in urls}
matches = []
inventories = {}
for source in sorted((base/'raw').iterdir()):
    if not source.is_dir() or not source.name.startswith('sec'):
        continue
    paths = [p for p in source.rglob('*.jsonl') if any(s in p.name for s in ('ledger','receipt','attempt','units-done'))]
    counters = collections.Counter()
    for path in paths:
        for line in path.open('rb'):
            try:
                row = json.loads(line)
            except ValueError:
                counters['malformed'] += 1
                continue
            counters[path.name] += 1
            url = str(row.get('url') or '')
            acc = str(row.get('accession') or '').replace('-','')
            if not acc and '/Archives/edgar/data/' in url:
                parts = urlparse(url).path.split('/')
                acc = next((p for p in parts if len(p)==18 and p.isdigit()), '')
            if acc not in accessions:
                continue
            sha = row.get('sha256') or row.get('document_sha256')
            objects = [source/'objects'/str(sha)[:2]/(str(sha)+'.gz'),source/'objects'/str(sha)[:2]/str(sha)] if sha else []
            matches.append({'store':source.name,'receipt':str(path),'accession':acc,'url':url,
                            'object_present':any(p.is_file() for p in objects),
                            'body_paths':[str(p) for p in objects if p.is_file()],
                            'status':row.get('status',row.get('http_status',row.get('outcome')))})
    inventories[source.name] = dict(counters)
# sec_text retains only 10-K/20-F sections and diagnostics, not primary bodies;
# retain partial-source hits explicitly, without treating them as EX-99 bytes.
out = {'fpi_unique_urls':len(set(urls)),'fpi_accessions':len(accessions),
       'cross_store_receipt_matches':matches,'inventories':inventories,
       'notes':['sec_text stores 10-K/20-F sections; no full documents retained (filing_text source contract).',
                'submission/header metadata cannot replace an EX-99 release document.',
                'historical all-attempt counts remain uninstrumented; ledger lines are not retries/redirect counts.'],
       'network_attempts':0}
path = Path('C:/atx/.superpowers/sdd/tier1-v4/t9-recovery-cache-audit.json')
path.write_text(json.dumps(out,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'output':str(path),'matches':len(matches),'bodies':sum(r['object_present'] for r in matches)}))
