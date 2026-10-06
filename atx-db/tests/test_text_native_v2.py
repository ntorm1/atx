import datetime as dt
import json
from collections import Counter

import pytest

from atx_db.alpha_panel import text_native_v2 as V
from atx_db.alpha_panel import text_native_v2_job as J
from atx_db.alpha_panel import text_native_v2_acceptance as A


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
    assert V.select_prior(current, [record('prior', 2020, report_date='2020-10-04')]) is None
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
    viewer = raw.replace(b'/Archives/edgar/data/1/000000000121000001/report.htm',
                         b'/ixviewer/doc/action?doc=/Archives/edgar/data/1/000000000121000001/report.htm')
    assert V.index_metadata(viewer, row, 'https://www.sec.gov/Archives/edgar/data/1/a-index.html')['primary_url'] == meta['primary_url']
    secondary = b'<tr><td>13</td><td>10-Q PDF</td><td><a href="/Archives/edgar/data/1/000000000121000001/report.pdf">report.pdf</a></td><td>10-Q</td><td>123</td></tr>'
    with_pdf = raw.replace(b'</table>', secondary + b'</table>')
    assert V.index_metadata(with_pdf, row, 'https://www.sec.gov/Archives/edgar/data/1/a-index.html')['primary_url'] == meta['primary_url']


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


def test_bounded_issuer_parts_replay_and_mutation_detection(tmp_path):
    raw = b'<html><body>alpha beta</body></html>'
    profile = V.profile_document(raw, '10-Q')
    previous = record('prior', 2020)
    current = record()
    rows = [V.feature_row(previous, profile, None, None),
            V.feature_row(current, profile, previous, profile)]
    for row in rows:
        row['body_receipt'] = {'bytes': 12}
    files = J.write_parts(tmp_path, rows, issuer=1)
    J.atomic_json(tmp_path / 'units' / '1.json', rows)
    files['units/1.json'] = J.digest(tmp_path / 'units' / '1.json')
    plan = {'scope': 'pilot', 'issuers': [1], 'records': rows, 'census_originals': 100,
            'census_by_filing_year': {'2020': 50, '2021': 50}}
    J.atomic_json(tmp_path / 'manifest.json', {'status': 'complete', 'rule': V.RULE,
        'clock_rule': V.CLOCK_RULE, 'schema': V.SCHEMA, 'staleness_days': 400, 'scope': 'pilot', 'files': files})
    checks = A.verify_stage(tmp_path, plan, J.digest, emit=False)
    assert checks['i_manifest_and_year_parts']['status'] == 'PASS'
    assert checks['iii_comparator_share']['2021']['share'] == 1
    assert checks['iv_originals_strict_prior_clock']['status'] == 'PASS'
    assert checks['ii_landed_over_census']['2021']['status'] == 'FAIL'
    (tmp_path / 'units' / '1.json').write_text(json.dumps(rows) + '\n', encoding='utf-8')
    assert A.verify_stage(tmp_path, plan, J.digest, emit=False)['i_manifest_and_year_parts']['status'] == 'FAIL'


def test_unknown_earlier_period_does_not_remove_gate_denominator():
    prior = record('prior', 2020, fiscal_period=None, fiscal_year=None)
    current = record(body_receipt={'bytes': 12}, doc_sim_cosine=None)
    stats = A.issuer_counts([prior, current])
    assert stats['eligible']['2021'] == 1
    assert stats['unknown']['2021'] == 1
    assert stats['finite']['2021'] == 0


def test_profile_scratch_pins_and_cleanup(tmp_path):
    profile = V.profile_document(b'<html><body>alpha beta</body></html>', '10-Q')
    reference = J.cache_profile(tmp_path, 1, 'accession', profile)
    assert J.cached_profile(tmp_path, reference)['tf'] == Counter(alpha=1, beta=1)
    path = tmp_path / reference['profile_file']
    saved = path.read_bytes()
    path.write_bytes(saved + b'mutation')
    with pytest.raises(ValueError, match='pin mismatch'):
        J.cached_profile(tmp_path, reference)
    with pytest.raises(ValueError, match='pin mismatch'):
        J.cleanup_owned_profile(tmp_path, reference)
    path.write_bytes(saved)
    J.cleanup_owned_profile(tmp_path, reference)
    assert not path.exists()


def test_transport_resume_reserves_unclosed_attempt(tmp_path, monkeypatch):
    monkeypatch.setattr(J.sec_http, 'sec_session', lambda **kwargs: object())
    ledger = tmp_path / 'receipts.jsonl'
    J.append_json(ledger, {'event': 'start', 'attempt': 1, 'maximum': 100, 'fetched_at': '2020-01-01T00:00:00+00:00'})
    J.append_json(ledger, {'event': 'end', 'attempt': 1, 'bytes': 40})
    J.append_json(ledger, {'event': 'start', 'attempt': 2, 'maximum': 100, 'fetched_at': '2020-01-01T00:00:01+00:00'})
    transport = J.Transport(ledger, 500)
    assert transport.attempts == 2 and transport.transferred == 140
    with pytest.raises(RuntimeError, match='transfer budget'):
        transport.get('https://www.sec.gov/example', 400)


def test_atomic_checkpoint_same_target_sharing_retry(tmp_path, monkeypatch):
    target = tmp_path / 'owned.json'
    original_replace = J.os.replace
    calls = []
    def replace(source, destination):
        calls.append((source, destination))
        if len(calls) < 3:
            raise PermissionError('synthetic transient sharing refusal')
        return original_replace(source, destination)
    monkeypatch.setattr(J.os, 'replace', replace)
    monkeypatch.setattr(J.sec_http.time, 'sleep', lambda seconds: None)
    J.atomic_json(target, {'checkpoint': 1})
    assert json.loads(target.read_text()) == {'checkpoint': 1}
    assert len(calls) == 3 and all(destination == target for _, destination in calls)


def test_fsds_fiscal_facts_require_original_identity_and_clock():
    current = record(fiscal_period=None, fiscal_year=None, reason='original_fiscal_period_unproven')
    row = {'adsh': 'now', 'cik': 1, 'form': '10-Q', 'period': '20210331', 'filed': '20210501',
           'fp': 'Q1', 'fy': 2021, 'accepted_utc': '2021-05-01 20:00:00', 'filename': 'pinned-source.parquet'}
    result = V.apply_fsds_fiscal(current, [row])
    assert result['fiscal_period'] == 'Q1' and result['fiscal_year'] == 2021 and result['reason'] is None
    assert result['fiscal_metadata_source_files'] == ['pinned-source.parquet']
    for changes in ({'cik': 2}, {'adsh': 'other'}, {'accepted_utc': '2021-05-01 16:00:00'},
                    {'period': '20210630'}, {'form': '10-Q/A'}, {'filed': '20210502'}):
        assert V.apply_fsds_fiscal(current, [{**row, **changes}])['fiscal_period'] is None
    assert V.apply_fsds_fiscal(current, [row, {**row, 'fp': 'Q2'}])['fiscal_period'] is None
    assert V.apply_fsds_fiscal({**current, 'available_at': None}, [row])['fiscal_period'] is None


def test_partial_checkpoint_recovery_preserves_prefix(tmp_path):
    output = tmp_path
    records = [record('one', 2020), record('two', 2021)]
    path = output / 'working' / '1.json'
    J.atomic_json(path, {'rows': records[:1], 'history': [], 'profile_files': {}})
    partial = path.with_suffix('.json.partial')
    partial.write_text(json.dumps({'rows': records, 'history': [], 'profile_files': {}}))
    J.recover_partial_checkpoint(output, path, records)
    assert json.loads(path.read_text())['rows'] == records and not partial.exists()
    partial.write_text(json.dumps({'rows': [records[1]], 'history': [], 'profile_files': {}}))
    with pytest.raises(ValueError, match='extension'):
        J.recover_partial_checkpoint(output, path, records)


def test_year_part_first_unproven_row_does_not_drop_later_provenance(tmp_path):
    import pyarrow.parquet as pq
    earlier = V.feature_row(record('unknown', fiscal_period=None, fiscal_year=None,
                           fiscal_metadata_basis=None), None, None, None)
    later = V.feature_row(record('proved', fiscal_metadata_basis='original_primary_dei'), None, None, None)
    files = J.write_parts(tmp_path, [earlier, later], issuer=1)
    path = next(tmp_path / rel for rel in files)
    rows = pq.ParquetFile(path).read().to_pylist()
    assert rows[0]['fiscal_metadata_basis'] is None
    assert rows[1]['fiscal_metadata_basis'] == 'original_primary_dei'
    assert rows[1]['fiscal_period'] == 'Q1'
    assert pq.ParquetFile(path).schema_arrow.field('doc_sim_cosine').type == J.pa.float64()

def test_rematerialization_refuses_existing_target_and_wrong_pin(tmp_path):
    from atx_db.alpha_panel import text_native_v2_job as J
    import pytest
    source = tmp_path / 'source'
    source.mkdir()
    J.atomic_json(source / 'manifest.json', {'files': {}})
    target = tmp_path / 'existing'
    target.mkdir()
    with pytest.raises(ValueError, match='must be absent'):
        J.rematerialize(source, target, 'bad')
    with pytest.raises(ValueError, match='pin mismatch'):
        J.rematerialize(source, tmp_path / 'new', 'bad')
    assert not (tmp_path / 'new').exists()


def test_transition_original_report_gap_300_is_valid():
    prior = record('annual', 2020, form='10-K', fiscal_period='FY', report_date='2020-01-01')
    current = record('transition', 2021, form='10-KT', fiscal_period='FY', report_date='2020-10-27')
    assert (dt.date.fromisoformat(current['report_date']) - dt.date.fromisoformat(prior['report_date'])).days == 300
    assert V.select_prior(current, [prior]) == prior


def test_complete_arrow_schema_unknown_and_populated_parts_identical(tmp_path):
    import pyarrow.parquet as pq
    unknown = V.feature_row(record('unknown', fiscal_year=None, fiscal_period=None), None, None, None)
    prior = record('prior', 2020)
    profile = V.profile_document(b'<html><body>PART I ITEM 2 Management discussion alpha beta. PART II ITEM 1A Risk Factors risk gamma.</body></html>', '10-Q')
    populated = V.feature_row(record('populated'), profile, prior, profile)
    unknown_files = J.write_parts(tmp_path / 'unknown', [unknown], issuer=1)
    populated_files = J.write_parts(tmp_path / 'populated', [populated], issuer=1)
    schemas = [pq.ParquetFile(root / next(iter(files))).schema_arrow for root, files in
               ((tmp_path / 'unknown', unknown_files), (tmp_path / 'populated', populated_files))]
    assert schemas[0] == schemas[1] == J.PUBLIC_SCHEMA
    assert schemas[0].field('prior_form').type == J.pa.string()
    assert schemas[0].field('mdna_sim_cosine').type == J.pa.float64()
    assert schemas[0].field('risk_words').type == J.pa.int64()
    assert set(unknown) == set(J.PUBLIC_SCHEMA.names)

def test_retained_comparator_audit_detects_new_transition_candidate(tmp_path):
    prior = V.feature_row(record('annual', 2020, form='10-K', fiscal_period='FY', report_date='2020-01-01'), None, None, None)
    current = V.feature_row(record('transition', 2021, form='10-KT', fiscal_period='FY', report_date='2020-10-27'), None, None, None)
    source = tmp_path / 'source'
    J.atomic_json(source / 'plan.json', {'issuers': [1]})
    J.atomic_json(source / 'units/1.json', [prior, current])
    files = {rel: J.digest(source / rel) for rel in ('plan.json', 'units/1.json')}
    J.atomic_json(source / 'manifest.json', {'files': files})
    audit = J.audit_comparators(source)
    assert audit['changed'] == [{'accession': 'transition', 'stored': None, 'selected': 'annual'},
                                {'accession': 'transition', 'reason': 'prior_clock_changed'}]
    target = tmp_path / 'r3'
    with pytest.raises(ValueError, match='profile recomputation'):
        J.rematerialize(source, target, J.digest(source / 'manifest.json')['sha256'])
    assert not target.exists()
