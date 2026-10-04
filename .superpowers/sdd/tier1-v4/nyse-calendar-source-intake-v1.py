"""Five bounded non-SEC publisher-page captures; no calendar qualification."""
from datetime import datetime, timezone
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import time
import requests

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'nyse-calendar-source-intake-v1.json'
landing = Path('C:/atx/atx-db/data/raw/source_intake/nyse_calendar_v1')
if output.exists() or landing.exists():
    raise RuntimeError('Preserve prior source intake')
current = landing
while current != current.parent:
    if current.is_symlink() or current.is_junction():
        raise RuntimeError('Reparse source landing path')
    current = current.parent
pages = (
    ('calendar-2017', '2017/NYSE-Group-Announces-2018-2019-and-2020-Holiday-and-Early-Closings-Calendar'),
    ('calendar-2019', '2019/NYSE-Group-Announces-2020-2021-and-2022-Holiday-and-Early-Closings-Calendar'),
    ('calendar-2020', '2020/NYSE-Group-Announces-2021-2022-and-2023-Holiday-and-Early-Closings-Calendar'),
    ('calendar-2021', '2021/NYSE-Group-Announces-2022-2023-and-2024-Holiday-and-Early-Closings-Calendar'),
    ('mourning-2018', '2018/New-York-Stock-Exchange-to-Honor-President-George-H-W-Bush'),
)


class Metadata(HTMLParser):
    def __init__(self):
        super().__init__()
        self.records = []

    def handle_starttag(self, tag, attributes):
        values = dict(attributes)
        if tag.lower() != 'meta':
            return
        name = values.get('name') or values.get('property') or values.get('itemprop') or ''
        if any(key in name.lower() for key in ('date', 'time', 'publish', 'title')):
            self.records.append({'key': name[:128], 'content': (values.get('content') or '')[:512]})


landing.mkdir(parents=True)
attempts = landing / 'attempts.jsonl'
records = []
last_request = 0.0
with requests.Session() as session, attempts.open('x', encoding='utf-8') as ledger:
    session.trust_env = False
    session.headers.update({'User-Agent': 'atx-db calendar-source-research/1.0', 'Accept': 'text/html'})
    session.mount('https://', requests.adapters.HTTPAdapter(max_retries=0))
    for number, (name, suffix) in enumerate(pages, 1):
        url = 'https://ir.theice.com/press/news-details/' + suffix + '/default.aspx'
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
                                     ('Date','Last-Modified','ETag','Content-Type','Location') if key in response.headers}
                body_bytes = 0
                digest = hashlib.sha256()
                with partial.open('xb') as stream:
                    for chunk in response.iter_content(65536):
                        if time.monotonic() - last_request > 25:
                            raise RuntimeError('Per-response elapsed bound exceeded')
                        body_bytes += len(chunk)
                        if body_bytes > 2 * 1024 * 1024:
                            raise RuntimeError('Publisher response exceeds2MiB bound')
                        digest.update(chunk)
                        stream.write(chunk)
                destination = landing / (name + '.html')
                if destination.exists():
                    raise RuntimeError('Refuse overwrite')
                partial.rename(destination)
                parser = Metadata()
                parser.feed(destination.read_text(encoding='utf-8', errors='replace'))
                record.update(path=str(destination), bytes=body_bytes, sha256=digest.hexdigest(),
                              publisher_meta=parser.records, completed=True)
        except Exception as error:
            record.update(completed=False, error=str(error), partial_path=str(partial),
                          partial_exists=partial.exists())
        record['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
        records.append(record)
        ledger.write(json.dumps({'attempt': number, 'outcome': record}) + '\n')
        ledger.flush()
        os.fsync(ledger.fileno())
for record in records:
    if record.get('completed'):
        payload = Path(record['path']).read_bytes()
        if len(payload) != record['bytes'] or hashlib.sha256(payload).hexdigest() != record['sha256']:
            raise RuntimeError('Captured original response changed')
result = {'status': 'FIVE_NYSE_PUBLISHER_CAPTURES_NOT_SOURCE_ADMISSION',
          'non_sec_attempts': len(records), 'sec_attempts': 0, 'landing': str(landing),
          'records': records, 'gate_promotion': False,
          'qualification': 'Five exact public publisher URLs only, no retries or redirects, durable attempt reservations, <=1request/second and <=2MiB per response. Current captured bytes and HTTP/header/body metadata are source leads. Publication UTC, complete historical weekday/holiday/revision coverage, source vintage and product admission must be independently authenticated; current retrieval time or date-only publication text must not be backdated into a release clock.'}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'non_sec_attempts': len(records),
                  'responses': [{key: row.get(key) for key in ('attempt','status_code','bytes','completed','error')}
                                for row in records], 'gate_promotion': False}), flush=True)
