"""Source-bound distributed release clocks and complete table comparisons."""
from datetime import datetime, timezone
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
from zoneinfo import ZoneInfo

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'nyse-distributed-clock-preflight-v2.json'
if output.exists():
    raise RuntimeError('Preserve prior evidence')


def clean(value):
    return ' '.join(value.split())


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tables = []
        self.table = self.row = self.cell = self.paragraph = None
        self.paragraphs = []
        self.links = []
        self.titles = []
        self.heading = None

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == 'table':
            if self.table is not None:
                raise RuntimeError('Nested table')
            self.table = []
        elif tag == 'tr' and self.table is not None:
            self.row = []
        elif tag in ('td','th') and self.row is not None:
            self.cell = []
        elif tag == 'p':
            self.paragraph = []
        elif tag == 'h2':
            self.heading = []
        elif tag == 'a':
            url = attributes.get('href', '')
            if re.fullmatch(r'https?://(?:www\.)?businesswire\.com/news/home/[0-9]+/en/?', url):
                self.links.append(url.replace('http:', 'https:').replace('https://businesswire.', 'https://www.businesswire.').rstrip('/'))

    def handle_data(self, value):
        for collection in (self.cell, self.paragraph, self.heading):
            if collection is not None:
                collection.append(value)

    def handle_endtag(self, tag):
        if tag in ('td','th') and self.cell is not None:
            self.row.append(clean(''.join(self.cell)))
            self.cell = None
        elif tag == 'tr' and self.row is not None:
            if any(self.row):
                self.table.append(self.row)
            self.row = None
        elif tag == 'table' and self.table is not None:
            if any('HOLIDAY' in cell for row in self.table for cell in row):
                self.tables.append(self.table)
            self.table = None
        elif tag == 'p' and self.paragraph is not None:
            self.paragraphs.append(clean(''.join(self.paragraph)))
            self.paragraph = None
        elif tag == 'h2' and self.heading is not None:
            self.titles.append(clean(''.join(self.heading)))
            self.heading = None


bindings = []


def read(row):
    path = Path(row['path'])
    payload = path.read_bytes()
    if len(payload) != row['bytes'] or hashlib.sha256(payload).hexdigest() != row['sha256']:
        raise RuntimeError('Captured source binding changed')
    parser = Page()
    parser.feed(payload.decode('utf-8'))
    if len(parser.tables) != 1:
        raise RuntimeError('Missing or ambiguous holiday table')
    bindings.append(row)
    return parser


ice = json.loads((base / 'nyse-calendar-source-intake-v1.json').read_text())
distributed = json.loads((base / 'p9-original-source-intake-v2.json').read_text())
records = []
for year in (2020, 2021):
    original = next(row for row in ice['records'] if Path(row['path']).name == f'calendar-{year}.html')
    copied = next(row for row in distributed['records'] if Path(row.get('path','')).name == f'nasdaq-calendar-{year}.html')
    if original['status_code'] != 200 or not copied['successful_response']:
        raise RuntimeError('Not successful original capture')
    left, right = read(original), read(copied)
    labels = [right.paragraphs[index+1] for index, value in enumerate(right.paragraphs[:-1]) if value == 'Published']
    if len(labels) != 1:
        raise RuntimeError('Missing or ambiguous published clock')
    label = labels[0]
    match = re.fullmatch(r'([A-Z][a-z]{2} \d{1,2}, \d{4} \d{1,2}:\d{2}[ap]m) (EST|EDT)', label)
    if not match:
        raise RuntimeError('Unrecognized explicit publisher timezone')
    naive = datetime.strptime(match.group(1), '%b %d, %Y %I:%M%p')
    local = naive.replace(tzinfo=ZoneInfo('America/New_York'))
    if local.tzname() != match.group(2) or local.astimezone(timezone.utc).astimezone(local.tzinfo).replace(tzinfo=None) != naive:
        raise RuntimeError('Publisher timezone/date mismatch')
    source_links_equal = len(set(left.links)) == 1 and set(left.links) == set(right.links)
    table_equal = left.tables == right.tables
    # This second inventory diagnoses the v1 refusal without waiving admission.
    # A false value remains an unresolved source-content/provenance finding.
    early_left = [value for value in left.paragraphs if 'market will close early at' in value]
    early_right = [value for value in right.paragraphs if 'market will close early at' in value]
    early_equal = early_left == early_right
    records.append({
        'year': year, 'original_ice': {key: original[key] for key in ('url','path','bytes','sha256')},
        'distributed_original': {key: copied[key] for key in ('url','path','bytes','sha256')},
        'published_label': label, 'parsed_distributed_publication_utc': local.astimezone(timezone.utc).isoformat(),
        'source_version_urls': sorted(set(right.links)), 'same_explicit_source_version': source_links_equal,
        'ice_source_version_urls': sorted(set(left.links)),
        'factual_tables_when_unequal': None if table_equal else {'ice': left.tables, 'distributed': right.tables},
        'complete_factual_holiday_table_equal': table_equal,
        'table_rows': len(right.tables[0]),
        'table_canonical_sha256': hashlib.sha256(json.dumps(right.tables[0], ensure_ascii=False, separators=(',',':')).encode()).hexdigest(),
        'early_close_paragraph_counts': [len(early_left), len(early_right)],
        'complete_early_close_paragraphs_equal': early_equal,
        'early_close_paragraph_hashes': [[hashlib.sha256(value.encode()).hexdigest() for value in values] for values in (early_left,early_right)],
        'ice_publication_utc': None, 'calendar_admitted': False,
    })
for row in bindings:
    payload = Path(row['path']).read_bytes()
    if len(payload) != row['bytes'] or hashlib.sha256(payload).hexdigest() != row['sha256']:
        raise RuntimeError('Source changed during inspection')
result = {'status': 'DISTRIBUTED_CLOCK_AND_CONTENT_COMPARISON_DIAGNOSTIC_NOT_ADMISSION',
          'preserved_failed_preflight': 'nyse-distributed-clock-preflight-v1.py and logs/nyse-distributed-clock-preflight-v1.*',
          'records': records, 'network_attempts': 0, 'gate_promotion': False,
          'qualification': 'Actual distributed-original publication labels parsed with explicit EST/EDT and New York timezone agreement. Equality is measured, not required to emit this diagnostic. FALSE provenance/table/early-close equality remains unresolved and confers no source admission. The first stricter inspection refusal and original code are preserved. This is bounded metadata evidence, not a complete original-format product parser or independent admission: full schedule/amendment corpus, original weekday authority, source rights, reviewed consumer contract and complete native replay remain required. ICE date-only clocks remain NULL; current fetch timestamps never backdate a source.'}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'records': records, 'gate_promotion': False}), flush=True)
