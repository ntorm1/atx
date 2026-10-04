"""Four bounded non-SEC source captures for genuine publication-clock work."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time
import requests

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'p9-original-source-intake-v2.json'
landing = Path('C:/atx/atx-db/data/raw/source_intake/p9_original_v2')
if output.exists() or landing.exists():
    raise RuntimeError('Preserve prior attempt')
for component in (landing, *landing.parents):
    if component.is_symlink() or component.is_junction():
        raise RuntimeError('Reparse source landing ancestor')
pages = (
    ('nasdaq-calendar-2020', 'https://www.nasdaq.com/press-release/nyse-group-announces-2021-2022-and-2023-holiday-and-early-closings-calendar-2020-12'),
    ('nasdaq-calendar-2021', 'https://www.nasdaq.com/press-release/nyse-group-announces-2022-2023-and-2024-holiday-and-early-closings-calendar-2021-12'),
    ('bls-cpi-archive-index', 'https://www.bls.gov/bls/news-release/cpi.htm'),
    ('bls-cpi-20100115', 'https://www.bls.gov/news.release/archives/cpi_01152010.htm'),
)
landing.mkdir(parents=True)
records = []
last_request = 0.0
with requests.Session() as session, (landing / 'attempts.jsonl').open('x', encoding='utf-8') as ledger:
    session.trust_env = False
    session.headers.update({'User-Agent': 'atx-db public-source-research/1.0', 'Accept': 'text/html'})
    session.mount('https://', requests.adapters.HTTPAdapter(max_retries=0))
    for number, (name, url) in enumerate(pages, 1):
        time.sleep(max(0, 1.05 - (time.monotonic() - last_request)))
        started_utc = datetime.now(timezone.utc).isoformat()
        ledger.write(json.dumps({'attempt': number, 'url': url, 'reserved_at_utc': started_utc}) + '\n')
        ledger.flush()
        os.fsync(ledger.fileno())
        last_request = time.monotonic()
        record = {'attempt': number, 'url': url, 'started_at_utc': started_utc,
                  'redirects_allowed': False, 'retries': 0, 'qualified': False}
        partial = landing / (name + '.partial')
        try:
            with session.get(url, timeout=(5, 15), stream=True, allow_redirects=False) as response:
                record['status_code'] = response.status_code
                record['headers'] = {key: response.headers[key] for key in
                                     ('Date', 'Last-Modified', 'ETag', 'Content-Type', 'Location') if key in response.headers}
                count = 0
                digest = hashlib.sha256()
                with partial.open('xb') as stream:
                    for chunk in response.iter_content(65536):
                        if time.monotonic() - last_request > 25:
                            raise RuntimeError('Response elapsed bound exceeded')
                        count += len(chunk)
                        if count > 2 * 1024 * 1024:
                            raise RuntimeError('Response exceeds2MiB')
                        digest.update(chunk)
                        stream.write(chunk)
                destination = landing / (name + '.html')
                if destination.exists():
                    raise RuntimeError('Refuse overwrite')
                partial.rename(destination)
                record.update(path=str(destination), bytes=count, sha256=digest.hexdigest(),
                              capture_complete=True, successful_response=response.status_code == 200)
        except Exception as error:
            record.update(capture_complete=False, successful_response=False, error=str(error),
                          partial_path=str(partial), partial_exists=partial.exists())
        record['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
        records.append(record)
        ledger.write(json.dumps({'attempt': number, 'outcome': record}) + '\n')
        ledger.flush()
        os.fsync(ledger.fileno())
for record in records:
    if record.get('capture_complete'):
        payload = Path(record['path']).read_bytes()
        if len(payload) != record['bytes'] or hashlib.sha256(payload).hexdigest() != record['sha256']:
            raise RuntimeError('Capture changed')
result = {'status': 'FOUR_ORIGINAL_SOURCE_CAPTURES_NOT_ADMISSION',
          'non_sec_attempts': len(records), 'sec_attempts': 0,
          'landing': str(landing), 'records': records, 'gate_promotion': False,
          'qualification': 'Exact public URLs, no redirect/retry, <=1request/second and <=2MiB per response. Retrieval clocks never qualify historical availability. A distributed original release must authenticate its own publication clock and complete corresponding content; it cannot backdate the ICE capture. CPI needs original NSA values/series identity, real release clocks, immutable correction vintages and complete monthly coverage. No source or consumer gate is promoted by capture.'}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'non_sec_attempts': len(records),
                  'responses': [{key: row.get(key) for key in ('attempt','status_code','bytes','successful_response','error')}
                                for row in records], 'gate_promotion': False}), flush=True)
