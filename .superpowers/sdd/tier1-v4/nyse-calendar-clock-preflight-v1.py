"""Inspect pinned small publisher captures for explicit publication clock leads."""
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'nyse-calendar-clock-preflight-v1.json'
if output.exists():
    raise RuntimeError('Preserve prior clock inspection')
source = base / 'nyse-calendar-source-intake-v1.json'
raw = source.read_bytes()
source_pin = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
intake = json.loads(raw)
if intake['non_sec_attempts'] != 5 or intake['sec_attempts'] != 0:
    raise RuntimeError('Unexpected source capture plan')


class ClockParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.times = []
        self.date_text = []
        self.date_depth = 0
        self.depth = 0
        self.ld_depth = 0
        self.ld = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        self.depth += 1
        if tag.lower() == 'time' or 'datetime' in values:
            self.times.append({key: value for key, value in values.items()
                               if key.lower() in ('datetime','itemprop','class')})
        if any('date' in (values.get(key) or '').lower() for key in ('id','class','itemprop')):
            self.date_depth = self.depth
        if tag.lower() == 'script' and values.get('type') == 'application/ld+json':
            self.ld_depth = self.depth

    def handle_endtag(self, tag):
        if self.depth == self.date_depth:
            self.date_depth = 0
        if self.depth == self.ld_depth:
            self.ld_depth = 0
        self.depth = max(0, self.depth - 1)

    def handle_data(self, text):
        if self.date_depth and text.strip() and len(text) <= 512:
            self.date_text.append(text.strip())
        if self.ld_depth:
            self.ld.append(text)


rows = []
for record in intake['records']:
    if not record['completed'] or record['status_code'] != 200 or record['bytes'] > 2 * 1024 * 1024:
        raise RuntimeError('Complete bounded publisher response required')
    path = Path(record['path'])
    body = path.read_bytes()
    if len(body) != record['bytes'] or hashlib.sha256(body).hexdigest() != record['sha256']:
        raise RuntimeError('Publisher capture bytes changed')
    text = body.decode('utf-8', errors='strict')
    parser = ClockParser()
    parser.feed(text)
    iso = sorted(set(re.findall(r'\b(?:19|20)\d\d-\d\d-\d\dT\d\d:\d\d(?::\d\d(?:\.\d+)?)?(?:Z|[+-]\d\d:\d\d)?', text)))
    structured = []
    for block in parser.ld:
        try:
            value = json.loads(block)
        except ValueError:
            structured.append({'parseable': False})
            continue
        candidates = value if isinstance(value, list) else [value]
        structured.extend({key: item[key] for key in ('@type','headline','datePublished','dateModified') if key in item}
                          for item in candidates if isinstance(item, dict))
    rows.append({'path': str(path), 'source_sha256': record['sha256'], 'url': record['url'],
                 'date_element_text': parser.date_text[:100], 'time_attributes': parser.times[:100],
                 'json_ld_publication_fields': structured[:100], 'iso_datetime_strings': iso[:100],
                 'available_at_utc': None, 'clock_qualified': False})
    if hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
        raise RuntimeError('Capture changed during inspection')
if hashlib.sha256(source.read_bytes()).hexdigest() != source_pin['sha256']:
    raise RuntimeError('Intake evidence changed')
result = {'status': 'PUBLISHER_CLOCK_FIELDS_INSPECTED_NOT_ADMITTED', 'intake': source_pin,
          'records': rows, 'gate_promotion': False, 'network_attempts': 0,
          'qualification': 'HTML date/time/structured fields and ISO-shaped strings are bounded leads only. Contextless datetimes, current HTTP Date and date-only labels are not publication UTC proof. No full calendar or source-clock admission occurred.'}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'records': rows, 'gate_promotion': False}), flush=True)
