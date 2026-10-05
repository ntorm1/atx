"""Pure B1 filing parsing and exact full-primary-document comparisons."""
from __future__ import annotations

import datetime as dt
import html
import re
from collections import Counter
from html.parser import HTMLParser
from urllib.parse import urljoin
from zoneinfo import ZoneInfo

from . import filing_text as FT
from . import text_features as TF

ORIGINALS = frozenset({'10-K', '10-KT', '10-K405', '10-KSB', '10-KSB40',
                       '10-KT405', '10-Q', '10-QT'})
RULE = 'text-native-full-primary-tf-v2'
CLOCK_RULE = 'sec-original-index-accepted-utc-v1'
SCHEMA = 'atx.alpha-panel.text-native/v2'


def family(form):
    return '10-Q' if form in {'10-Q', '10-QT'} else ('10-K' if form in ORIGINALS else None)


def index_url(record):
    accession = record['accession']
    return f"{FT.ARCHIVES}/{record['cik']}/{accession.replace('-', '')}/{accession}-index.htm"


class VisibleText(HTMLParser):
    """All visible primary-document text, including every table; no numeric-table heuristic."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.chunks, self.suppressed = [], []

    def handle_starttag(self, tag, attrs):
        if tag in {'script', 'style', 'ix:header'} or self.suppressed:
            self.suppressed.append(tag)
        elif tag in {'p', 'div', 'tr', 'br', 'h1', 'h2', 'h3', 'td', 'th'}:
            self.chunks.append('\n' if tag != 'td' else ' ')

    def handle_endtag(self, tag):
        if self.suppressed:
            if tag == self.suppressed[-1]:
                self.suppressed.pop()
        elif tag in {'p', 'div', 'tr', 'h1', 'h2', 'h3'}:
            self.chunks.append('\n')

    def handle_data(self, value):
        if not self.suppressed:
            self.chunks.append(value)


def full_text(raw):
    decoded = FT.decode(raw)
    if '<html' not in decoded.lower() and '<body' not in decoded.lower():
        # Plain SEC text and fragments still pass through the tag parser.
        decoded = decoded.replace('<PAGE>', '\n')
    parser = VisibleText()
    parser.feed(decoded)
    return '\n'.join(re.sub(r'\s+', ' ', line).strip() for line in ''.join(parser.chunks).splitlines()
                     if line.strip())


def quarterly_sections(text):
    """Part-scoped Item headings; longest candidate avoids a short table of contents."""
    headings = list(re.finditer(r'(?im)^\s*(?:part\s+(II|I)\b[^\n]*|item\s+(\d+[A-Z]?)\s*[.:-]?[^\n]*)', text))
    part, found = None, {}
    for i, heading in enumerate(headings):
        if heading.group(1):
            part = heading.group(1).upper()
            continue
        item = heading.group(2).upper()
        key = 'mdna' if (part, item) == ('I', '2') else ('risk' if (part, item) == ('II', '1A') else None)
        if key:
            end = headings[i + 1].start() if i + 1 < len(headings) else len(text)
            value = text[heading.end():end].strip()
            if len(value) > len(found.get(key, '')):
                found[key] = value
    return found


def fiscal_metadata(raw):
    """Use explicit original DEI facts only, never infer quarters from calendar months."""
    decoded = FT.decode(raw)
    result = {}
    for name, key in [('DocumentFiscalPeriodFocus', 'fiscal_period'),
                      ('DocumentFiscalYearFocus', 'fiscal_year')]:
        pattern = (r'<(?:ix:nonNumeric|ix:nonFraction)\b[^>]*\bname\s*=\s*[\"\']'
                   r'(?:\w+:)?' + name + r'[\"\'][^>]*>(.*?)</(?:ix:nonNumeric|ix:nonFraction)>')
        values = {html.unescape(re.sub('<[^>]*>', '', x)).strip()
                  for x in re.findall(pattern, decoded, flags=re.I | re.S)}
        if len(values) == 1:
            value = values.pop()
            if key == 'fiscal_year' and value.isdigit():
                result[key] = int(value)
            elif key == 'fiscal_period' and value in {'FY', 'Q1', 'Q2', 'Q3', 'Q4'}:
                result[key] = value
    return result


def accepted_utc(value):
    """Authenticated SEC index wall time; reject nonexistent or ambiguous ET instants."""
    try:
        wall = dt.datetime.strptime(value, '%Y-%m-%d %H:%M:%S')
    except (ValueError, TypeError):
        return None
    zone = ZoneInfo('America/New_York')
    possibilities = {wall.replace(tzinfo=zone, fold=fold).astimezone(dt.UTC)
                     for fold in (0, 1)
                     if wall.replace(tzinfo=zone, fold=fold).astimezone(dt.UTC).astimezone(zone).replace(tzinfo=None) == wall}
    return next(iter(possibilities)).isoformat() if len(possibilities) == 1 else None


class IndexPage(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows, self.cells, self.cell, self.links = [], [], None, []

    def handle_starttag(self, tag, attrs):
        if tag == 'tr':
            self.cells, self.links = [], []
        elif tag == 'td':
            self.cell = []
        elif tag == 'a' and self.cell is not None:
            self.links.extend(v for k, v in attrs if k == 'href')

    def handle_data(self, value):
        if self.cell is not None:
            self.cell.append(value)

    def handle_endtag(self, tag):
        if tag == 'td' and self.cell is not None:
            self.cells.append(''.join(self.cell).strip())
            self.cell = None
        elif tag == 'tr' and self.cells:
            self.rows.append((self.cells, self.links))


def index_metadata(raw, record, url):
    decoded = FT.decode(raw)
    text = full_text(raw)
    accession = record['accession']
    if accession not in decoded or not re.search(r'CIK=(?:0*)' + str(record['cik']) + r'(?:\D|$)', decoded, re.I):
        return {'available_at': None, 'reason': 'index_accession_or_issuer_unproven'}
    acceptance = re.search(r'Accepted\s*\n?\s*(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})', text)
    report = re.search(r'Period of Report\s*\n?\s*(\d{4}-\d{2}-\d{2})', text)
    parser = IndexPage()
    parser.feed(decoded)
    links = []
    for cells, hrefs in parser.rows:
        if len(cells) >= 4 and cells[3] == record['form']:
            links.extend(hrefs)
    urls = {urljoin(url, link.replace('/ix?doc=', '')) for link in links
            if '/Archives/edgar/data/' in link and not link.lower().endswith('.xml')}
    clock = accepted_utc(acceptance.group(1)) if acceptance else None
    expected = '/Archives/edgar/data/' + str(record['cik']) + '/' + accession.replace('-', '') + '/'
    primary = next(iter(urls)) if len(urls) == 1 else None
    if primary and not primary.startswith('https://www.sec.gov' + expected):
        primary = None
    return {'available_at': clock, 'report_date': report.group(1) if report else None,
            'primary_url': primary, 'reason': None if clock else 'authenticated_acceptance_missing_or_ambiguous'}


def select_prior(current, history):
    if current['form'] not in ORIGINALS or not current.get('available_at'):
        return None
    if not all(current.get(k) is not None for k in ('fiscal_period', 'fiscal_year', 'report_date')):
        return None
    candidates = []
    for prior in history:
        if prior['form'] not in ORIGINALS or prior['cik'] != current['cik'] or family(prior['form']) != family(current['form']):
            continue
        if (prior.get('fiscal_period') != current['fiscal_period'] or
                prior.get('fiscal_year') != current['fiscal_year'] - 1 or not prior.get('available_at') or not prior.get('report_date')):
            continue
        gap = (dt.datetime.fromisoformat(current['available_at']) - dt.datetime.fromisoformat(prior['available_at'])).total_seconds() / 86400
        report_gap = (dt.date.fromisoformat(current['report_date']) - dt.date.fromisoformat(prior['report_date'])).days
        filing_gap = (dt.date.fromisoformat(current['filing_date']) - dt.date.fromisoformat(prior['filing_date'])).days
        # Different fiscal year ends/transition periods are not automatically comparable.
        if 180 <= gap <= 550 and 180 <= filing_gap <= 550 and 180 <= report_gap <= 550 and abs(report_gap - 365) <= 45:
            candidates.append(prior)
    return candidates[0] if len(candidates) == 1 else None


def profile_document(raw, form):
    text = full_text(raw)
    tf, n_words = Counter(), 0
    for match in TF.WORD_RE.finditer(text.lower()):
        n_words += 1
        if n_words > 500_000:
            raise ValueError('document_profile_word_bound_exceeded')
        tf[match.group()] += 1
    sections = quarterly_sections(text) if family(form) == '10-Q' else {k: s.text for k, s in FT.extract_sections(text, form)[0].items()}
    profile = {'tf': tf, 'sections': {k: TF.section_profile(v) for k, v in sections.items()}}
    profile['doc'] = TF.readability(text)
    return profile


def feature_row(record, profile, prior, prior_profile):
    out = dict(record)
    out.update(prior_accession=prior['accession'] if prior else None,
               prior_form=prior['form'] if prior else None,
               prior_report_date=prior.get('report_date') if prior else None,
               prior_available_at=prior.get('available_at') if prior else None,
               prior_gap_days=(dt.datetime.fromisoformat(record['available_at']) - dt.datetime.fromisoformat(prior['available_at'])).total_seconds() / 86400 if prior else None,
               doc_sim_cosine=TF.cosine_tf(profile['tf'], prior_profile['tf']) if profile and prior_profile else None,
               comparison_reason=None if prior and prior_profile and profile else 'unproven_or_ambiguous_prior_period',
               doc_words=profile['doc']['words'] if profile else None,
               doc_fog=profile['doc']['fog'] if profile else None)
    for section in TF.SECTIONS:
        current = profile['sections'].get(section) if profile else None
        previous = prior_profile['sections'].get(section) if prior_profile else None
        for key in ('chars', 'words', 'sentences', 'fog', 'pct_complex'):
            out[section + '_' + key] = current.get(key) if current else None
        out.update({section + '_' + k: v for k, v in TF.compare(current, previous).items()})
        if (current and len(current['sents']) > TF.MAX_EDIT_SENTENCES) or (previous and len(previous['sents']) > TF.MAX_EDIT_SENTENCES):
            out[section + '_sim_minedit'] = None
        out[section + '_reason'] = None if current else 'section_missing_or_empty'
    return out


def year_part(row):
    return str(dt.datetime.fromisoformat(row['available_at']).astimezone(dt.UTC).year) if row.get('available_at') else 'null'
