import datetime as dt
import json
from collections import Counter

import pytest

from atx_db.alpha_panel import text_native_v2 as V
from atx_db.alpha_panel import text_native_v2_job as J


def record(accession='now', year=2021, **changes):
    result = {'cik': 1, 'accession': accession, 'form': '10-Q', 'fiscal_period': 'Q1',
              'fiscal_year': year, 'report_date': f'{year}-03-31', 'filing_date': f'{year}-05-01',
              'available_at': f'{year}-05-01T20:00:00+00:00', 'reason': None}
    result.update(changes)
    return result


def test_quarter_sections_and_absence():
    text = 'PART I\nITEM 1. Statements\nfinancial table\nITEM 2. Management\nlong management discussion\nITEM 3. Market\nend\nPART II\nITEM 1. Legal\nlegal\nITEM 1A. Risk Factors\nquarterly risk\nITEM 2. Other\nend'
    assert V.quarterly_sections(text) == {'mdna': 'long management discussion', 'risk': 'quarterly risk'}
    assert 'risk' not in V.quarterly_sections(text.split('ITEM 1A.')[0])


def test_exact_cosine_and_whole_document_tables():
    assert V.TF.cosine_tf(Counter(a=2, b=1), Counter(a=2, b=1)) == pytest.approx(1)
    assert V.TF.cosine_tf(Counter(a=1), Counter(b=1)) == 0
    text = V.full_text(b'<html><body><table><tr><td>assets</td><td>123</td></tr></table><p>alpha</p><script>secret</script></body></html>')
    assert 'assets' in text and '123' in text and 'secret' not in text


def test_fiscal_metadata_requires_explicit_original_facts():
    raw = b'<ix:nonNumeric name="dei:DocumentFiscalPeriodFocus">Q2</ix:nonNumeric><ix:nonFraction name="dei:DocumentFiscalYearFocus">2020</ix:nonFraction>'
    assert V.fiscal_metadata(raw) == {'fiscal_period': 'Q2', 'fiscal_year': 2020}
    assert V.fiscal_metadata(b'quarter ended June 30, 2020') == {}


@pytest.mark.parametrize('gap,expected', [(179, False), (180, True), (550, True), (551, False)])
def test_comparator_acceptance_gap(gap, expected):
    prior = record('prior', 2020)
    clock = dt.datetime.fromisoformat(prior['available_at']) + dt.timedelta(days=gap)
    current = record(available_at=clock.isoformat())
    assert bool(V.select_prior(current, [prior])) == expected


def test_period_year_change_transition_amendments_and_null():
    prior, current = record('prior', 2020), record()
    assert V.select_prior(current, [prior]) == prior
    assert V.select_prior(current, [record('other', 2020, fiscal_period='Q2')]) is None
    assert V.select_prior(current, [record('amended', 2020, form='10-Q/A')]) is None
    assert V.select_prior(record(form='10-Q/A'), [prior]) is None
    assert V.select_prior(record(available_at=None), [prior]) is None
    assert V.select_prior(current, [record('prior', 2020, available_at=None)]) is None
    assert V.select_prior(current, [record('prior', 2020, report_date='2020-09-30')]) is None
    transition = record(form='10-KT', fiscal_period='FY')
    annual = record('annual', 2020, form='10-K', fiscal_period='FY')
    assert V.select_prior(transition, [annual]) == annual
    assert V.select_prior(transition, [annual, {**annual, 'accession': 'duplicate'}]) is None


def test_dst_and_year_routing():
    assert V.accepted_utc('2020-12-31 20:30:00') == '2021-01-01T01:30:00+00:00'
    assert V.accepted_utc('2021-11-07 01:30:00') is None
    assert V.accepted_utc('2021-03-14 02:30:00') is None
    assert V.year_part({'available_at': '2020-12-31T23:00:00-05:00'}) == '2021'
    assert V.year_part({'available_at': None}) == 'null'


def test_index_identity_clock_primary_and_report():
    row = record('0000000001-21-000001')
    raw = b'<html><body>Accession 0000000001-21-000001<a href="?CIK=1">issuer</a><div>Accepted</div><div>2021-05-01 16:00:00</div><div>Period of Report</div><div>2021-03-31</div><table><tr><td>1</td><td>Report</td><td><a href="/Archives/edgar/data/1/000000000121000001/report.htm">report.htm</a></td><td>10-Q</td><td>1234</td></tr></table></body></html>'
    meta = V.index_metadata(raw, row, 'https://www.sec.gov/Archives/edgar/data/1/a-index.html')
    assert meta['available_at'] == '2021-05-01T20:00:00+00:00'
    assert meta['report_date'] == '2021-03-31'
    assert meta['primary_url'].endswith('/report.htm')
    assert V.index_metadata(raw, {**row, 'cik': 2}, 'https://www.sec.gov')['available_at'] is None


def test_canonical_index_url_includes_accession_directory():
    assert V.index_url(record('0001564590-18-020158', cik=1673358)) == (
        'https://www.sec.gov/Archives/edgar/data/1673358/000156459018020158/0001564590-18-020158-index.htm')


def test_master_originals_and_whole_issuer_cap():
    raw = b'1|Co|10-Q|2020-05-01|edgar/data/1/0000000001-20-000001.txt\n1|Co|10-Q/A|2020-05-02|edgar/data/1/0000000001-20-000002.txt\n'
    assert len(list(J.parse_master(raw))) == 1
    rows = [record('q', 2020), record('k', 2020, form='10-K')]
    assert J.select_issuers(rows, 2, 1) == [1]
    with pytest.raises(ValueError, match='whole issuer'):
        J.select_issuers(rows, 1, 1)


def test_resume_after_interrupted_issuer_has_no_duplicate_or_drop(tmp_path, monkeypatch):
    class Transport:
        attempts, transferred = 0, 0
        def get(self, url, maximum):
            self.attempts += 1
            return b'alpha beta gamma', {'url': url, 'bytes': 16, 'sha256': 'synthetic'}
    monkeypatch.setattr(V, 'index_metadata', lambda raw, rec, url: {
        'available_at': rec['available_at'], 'report_date': rec['report_date'], 'primary_url': url + '/body', 'reason': None})
    monkeypatch.setattr(V, 'fiscal_metadata', lambda raw: {'fiscal_period': 'Q1', 'fiscal_year': 2020})
    rows = [record('first', 2020, index_filename='first.txt'), record('second', 2021, index_filename='second.txt')]
    transport = Transport()
    with pytest.raises(InterruptedError):
        J.issuer_unit(tmp_path, 1, rows, transport, interrupt_after=1)
    assert transport.attempts == 2
    result = J.issuer_unit(tmp_path, 1, rows, transport)
    assert [r['accession'] for r in result] == ['first', 'second']
    assert transport.attempts == 4
    assert J.issuer_unit(tmp_path, 1, rows, transport) == result
    assert transport.attempts == 4
    assert not (tmp_path / 'working' / '1.json').exists()


def test_year_parts_null_clock_kept_and_c5_check(tmp_path):
    raw = b'<html><body>alpha</body></html>'
    rows = [V.feature_row(record(), V.profile_document(raw, '10-Q'), None, None),
            V.feature_row(record('missing', available_at=None, reason='clock_unproven'), None, None, None)]
    files = J.write_parts(tmp_path, rows)
    plan = {'scope': 'pilot', 'census_by_filing_year': {'2021': 2}}
    J.atomic_json(tmp_path / 'manifest.json', {'status': 'complete', 'rule': V.RULE,
        'clock_rule': V.CLOCK_RULE, 'schema': V.SCHEMA, 'staleness_days': 400, 'scope': 'pilot', 'files': files})
    checks = J.acceptance(tmp_path, rows, plan)
    assert checks['i_manifest_and_year_parts']['status'] == 'PASS'
    assert 'parts/year=null/features.parquet' in files
