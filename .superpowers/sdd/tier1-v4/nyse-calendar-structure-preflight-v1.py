"""Bounded structural/factual calendar metadata for original-format fixtures."""
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
base = Path(__file__).resolve().parent
output = base / 'nyse-calendar-structure-preflight-v1.json'
if output.exists():
    raise RuntimeError('Preserve prior metadata')
intake = json.loads((base / 'nyse-calendar-source-intake-v1.json').read_text())


class Structure(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tables = []
        self.table = None
        self.row = None
        self.cell = None
        self.cell_tag = None
        self.paragraph = None
        self.closure = []
        self.text = []

    def handle_starttag(self, tag, attrs):
        if tag == 'table':
            if self.table is not None:
                raise RuntimeError('Nested table requires separate parser')
            self.table = {'start_tag': self.get_starttag_text(), 'rows': []}
        elif tag == 'tr' and self.table is not None:
            self.row = []
        elif tag in ('td','th') and self.row is not None:
            self.cell = []
            self.cell_tag = self.get_starttag_text()
        elif tag == 'p':
            self.paragraph = []

    def handle_endtag(self, tag):
        if tag in ('td','th') and self.cell is not None:
            self.row.append({'start_tag': self.cell_tag, 'text': ' '.join(''.join(self.cell).split())})
            self.cell = None
        elif tag == 'tr' and self.row is not None:
            self.table['rows'].append(self.row)
            self.row = None
        elif tag == 'table' and self.table is not None:
            self.tables.append(self.table)
            self.table = None
        elif tag == 'p' and self.paragraph is not None:
            value = ' '.join(''.join(self.paragraph).split())
            match = re.search(r'will be closed on (Wednesday, December 5, 2018)', value)
            if match:
                self.closure.append({'tag': 'p', 'matched_clause': match.group(0),
                                     'closure_date': '2018-12-05',
                                     'full_paragraph_sha256': hashlib.sha256(value.encode()).hexdigest(),
                                     'full_paragraph_omitted': True})
            self.paragraph = None

    def handle_data(self, text):
        self.text.append(text)
        if self.cell is not None:
            self.cell.append(text)
        if self.paragraph is not None:
            self.paragraph.append(text)


records = []
for row in intake['records']:
    path = Path(row['path'])
    raw = path.read_bytes()
    if len(raw) != row['bytes'] or hashlib.sha256(raw).hexdigest() != row['sha256']:
        raise RuntimeError('Original capture changed')
    parser = Structure()
    parser.feed(raw.decode('utf-8'))
    tables = [table for table in parser.tables if any('HOLIDAY' in cell['text'] for cells in table['rows'] for cell in cells)]
    if len(tables) > 1:
        raise RuntimeError('Ambiguous original holiday table')
    visible = ' '.join(' '.join(parser.text).split())
    # These fixed source words are leads only, never affirmative authority.
    weekday_matches = re.findall(r'Monday (?:through|to) Friday', visible, flags=re.I)
    records.append({'path': str(path), 'source_sha256': row['sha256'],
                    'publisher_titles': [value['content'] for value in row['publisher_meta'] if value['key'] == 'og:title'],
                    'holiday_tables': tables, 'closure_structure': parser.closure,
                    'weekday_phrase_matches': weekday_matches,
                    'weekday_authority_established': False, 'clock_qualified': False})
    if hashlib.sha256(path.read_bytes()).hexdigest() != row['sha256']:
        raise RuntimeError('Source capture changed during parsing')
result = {'status': 'ORIGINAL_CALENDAR_TABLE_STRUCTURE_LEADS_ONLY', 'records': records,
          'gate_promotion': False, 'network_attempts': 0,
          'qualification': 'Actual original HTML table tags and factual date cells for source-shaped parser design. No whole article is reproduced. Only a short closure clause and full-paragraph digest are emitted. Normal-weekday authority, complete amendment inventory and original publication UTC remain unproved; no generated proof, backdated clock or calendar admission.'}
with output.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'status': result['status'], 'records': len(records),
                  'table_counts': [len(row['holiday_tables']) for row in records],
                  'closure_counts': [len(row['closure_structure']) for row in records],
                  'gate_promotion': False}), flush=True)
