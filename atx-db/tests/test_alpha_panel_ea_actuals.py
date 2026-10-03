import datetime as dt
import math

import pytest

from atx_db.alpha_panel import ea_actuals as EA


def release(extra="", unit="USD in millions, except per-share amounts", duration="Three months ended"):
    return f'''<html><p>{unit}</p><table><tr><th>{duration}</th></tr>
    <tr><td></td><td>March 31, 2020</td><td>March 31, 2019</td></tr>
    <tr><td>Net revenues</td><td>120</td><td>100</td></tr>
    <tr><td>Net income (loss)</td><td>(12)</td><td>10</td></tr>
    <tr><td>Diluted earnings per share</td><td>(0.50)</td><td>0.40</td></tr>{extra}</table></html>'''.encode()


def parse(blob):
    return EA.parse_release(blob, dt.date(2020, 5, 1))


def test_quarter_current_comparative_units_and_loss_sign():
    r = parse(release())
    assert r["period_end"] == dt.date(2020, 3, 31)
    assert (r["saleq"], r["niq"], r["epsfiq"]) == (120e6, -12e6, -.5)
    assert r["saleq_seasonal_comparative"] == 100e6
    assert r["epsfiq_seasonal_comparative"] == .4
    assert r["eps_split_comparable"]
    assert r["period_days"] is None


def test_aar_original_shaped_split_header_quarter_ytd_and_unknown_currency():
    # Geometry transcribed from 0001104659-18-018959 table 0. Explicit USD
    # is a synthetic positive control; the retained original only says '$'.
    blob='''<html><p>{currency} in millions except per share data</p><table>
    <tr><td></td><td></td><td colspan="5">Three Months Ended February 28,</td><td></td><td colspan="5">Nine Months Ended February 28,</td><td></td></tr>
    <tr><td></td><td></td><td colspan="2">2018</td><td></td><td colspan="2">2017</td><td></td><td colspan="2">2018</td><td></td><td colspan="2">2017</td><td></td></tr>
    <tr><td>Sales</td><td></td><td>$</td><td>456.3</td><td></td><td>$</td><td>407.2</td><td></td><td>$</td><td>1,274.8</td><td></td><td>$</td><td>1,140.3</td><td></td></tr>
    </table></html>'''
    r=EA.parse_release(blob.format(currency='USD').encode(),dt.date(2018,3,20))
    assert r['saleq']==456.3e6 and r['saleq_seasonal_comparative']==407.2e6
    assert r['period_end']==dt.date(2018,2,28)
    assert r['fiscal_quarter'] is None  # no historical fiscal calendar supplied
    original=EA.parse_release(blob.format(currency='').encode(),dt.date(2018,3,20))
    assert original['saleq'] is None
    assert any('currency_missing_or_ambiguous' in s for s in original['extraction_reasons'])
    # A year outside its explicit duration span cannot be rescued by order.
    bad=blob.replace('colspan="5">Three Months','colspan="1">Three Months')
    assert EA.parse_release(bad.format(currency='USD').encode(),dt.date(2018,3,20))['saleq'] is None
    two_amounts=blob.replace('<td>$</td><td>456.3','<td>1</td><td>456.3')
    assert EA.parse_release(two_amounts.format(currency='USD').encode(),dt.date(2018,3,20))['saleq'] is None


def test_abbott_original_shaped_quarter_abbreviations_exclude_percent_columns():
    # 0001104659-15-052323 table 11: source caption binds year tokens.
    blob='''<html><p>Second Quarter Ended June 30, 2015 and 2014</p>
    <p>{currency} in millions, except per share data</p><p>unaudited</p><table>
    <tr><td></td><td></td><td colspan="2">2Q15</td><td></td><td colspan="2">2Q14</td><td></td><td>% Change</td><td></td></tr>
    <tr><td>Net Sales</td><td></td><td>$</td><td>5,170</td><td></td><td>$</td><td>5,057</td><td></td><td>2.2</td><td></td></tr>
    </table></html>'''
    r=EA.parse_release(blob.format(currency='USD').encode(),dt.date(2015,7,22))
    assert r['saleq']==5170e6 and r['saleq_seasonal_comparative']==5057e6
    assert r['period_end']==dt.date(2015,6,30)
    assert all('2.2' not in e for e in r['evidence'])
    assert EA.parse_release(blob.format(currency='').encode(),dt.date(2015,7,22))['saleq'] is None
    for changed in (blob.replace('2Q14','3Q14'),blob.replace('2015 and 2014','2015 and 2013')):
        r=EA.parse_release(changed.format(currency='USD').encode(),dt.date(2015,7,22))
        assert r.get('saleq_seasonal_comparative') is None


def test_quarter_column_conflict_overlap_and_rowspan_fail_closed():
    assert EA.quarter_columns([[(0,3,'Three Months Ended March 31, 2020')],
                              [(1,2,'March 31, 2019')]],'')=={}
    assert EA.quarter_columns([[(1,2,'March 31, 2020')],[(1,2,'March 31, 2019')]],'Three months ended')=={}
    r=parse(release().replace(b'<td>March 31, 2020',b'<td rowspan="2">March 31, 2020'))
    assert r['saleq'] is None and any('unsupported_header_geometry' in s for s in r['extraction_reasons'])


def test_overlapping_quarter_ytd_duration_spans_reject_monetary_cells():
    # Review I1: quarter [1,3) overlaps YTD [2,4), year [2,3).
    blob=b'''<html><p>USD in millions</p><table>
    <tr><td></td><td colspan="2">Three Months Ended March 31,</td></tr>
    <tr><td></td><td></td><td colspan="2">Nine Months Ended March 31,</td></tr>
    <tr><td></td><td></td><td>2020</td><td>2020</td></tr>
    <tr><td>Sales</td><td></td><td>900</td><td>800</td></tr></table></html>'''
    r=EA.parse_release(blob,dt.date(2020,4,20))
    assert all(r[f] is None for f in EA.FIELDS)
    assert any('quarter_column_geometry_unqualified' in s for s in r['extraction_reasons'])
    # Conflict of the parent span rejects even an otherwise nonintersecting
    # child date at [1,2); duration evidence must be internally consistent.
    assert EA.quarter_columns([[(1,3,'Three Months Ended March 31,')],
        [(2,4,'Nine Months Ended March 31,')],[(1,2,'2020')]],'')=={}


@pytest.mark.parametrize('label',['% Change','Percentage change','Growth','Percent'])
def test_date_bearing_percent_change_headers_never_supply_monetary_actuals(label):
    blob=f'''<html><p>Three months ended USD in millions</p><table>
    <tr><td></td><td>March 31, 2020 {label}</td><td>March 31, 2019</td></tr>
    <tr><td>Sales</td><td>2.2</td><td>100</td></tr></table></html>'''.encode()
    r=EA.parse_release(blob,dt.date(2020,4,20))
    assert all(r[f] is None for f in EA.FIELDS)
    assert r.get('saleq_seasonal_comparative') is None


def test_multilevel_percent_spans_exclude_dates_and_abbreviated_quarter_tokens():
    dates_blob=b'''<html><p>Three months ended USD in millions</p><table>
    <tr><td></td><td>% Change</td><td></td></tr>
    <tr><td></td><td>March 31, 2020</td><td>March 31, 2019</td></tr>
    <tr><td>Sales</td><td>2.2</td><td>100</td></tr></table></html>'''
    assert EA.parse_release(dates_blob,dt.date(2020,4,20))['saleq'] is None
    abbrev=b'''<html><p>Second Quarter Ended June 30, 2015 and 2014</p>
    <p>USD in millions</p><table>
    <tr><td></td><td colspan="2">Percentage Change</td></tr>
    <tr><td></td><td>2Q15</td><td>2Q14</td></tr>
    <tr><td>Sales</td><td>2.2</td><td>100</td></tr></table></html>'''
    assert EA.parse_release(abbrev,dt.date(2015,7,22))['saleq'] is None
    # The percentage span excludes only its interval; independent monetary
    # columns retain current/comparative positive controls.
    assert EA.quarter_columns([[(3,5,'Percentage Change')],
        [(1,2,'March 31, 2020'),(2,3,'March 31, 2019'),(3,4,'March 31, 2020'),(4,5,'March 31, 2019')]],
        'Three months ended')=={(1,2):dt.date(2020,3,31),(2,3):dt.date(2019,3,31)}


@pytest.mark.parametrize("unit", ["in millions", "USD CAD in millions", "USD", "USD in millions and in thousands"])
def test_unknown_or_conflicting_currency_and_scale_is_missing(unit):
    assert parse(release(unit=unit))["saleq"] is None


@pytest.mark.parametrize("duration", ["Six months ended", "Year ended", "Three months and nine months ended"])
def test_duration_ambiguity_never_recasts_ytd_as_quarter(duration):
    assert parse(release(duration=duration))["saleq"] is None


def test_conflicting_gaap_rows_not_chosen_by_rank():
    r = parse(release('<tr><td>Net revenues</td><td>121</td><td>100</td></tr>'))
    assert r["saleq"] is None and "saleq:conflicting_rows" in r["reason"]


def test_non_gaap_table_excluded():
    assert parse(release(duration="Adjusted three months ended"))["saleq"] is None


def test_dash_is_missing_and_eps_scale_requires_declared_exception():
    assert EA.number("—") is None
    assert EA.number("12%") is None
    assert EA.number("(1,000)") == -1000
    r = parse(release(unit="USD in millions"))
    assert r["saleq"] == 120e6 and r["epsfiq"] is None


def test_ambiguous_and_missing_acceptance_clock():
    assert EA.qualified_clock({"available_at": dt.datetime(2020, 1, 1)}) is None
    assert EA.qualified_clock({"acceptance_clock": "unresolved_conservative", "available_at": dt.datetime(2020, 1, 1)}) is None


def test_prior_eight_surprises_and_cutoff_invariance():
    rows = [{"accession": str(i), "period_end": dt.date(2015+i//4, 3*(i%4+1), 28),
             "available_at": dt.datetime(2015+i//4, 3*(i%4+1), 29), "currency": "USD",
             "fiscal_year_label":2015+i//4,"fiscal_quarter":i%4+1,"fiscal_calendar_id":"12-31",
             "saleq": 100+i*i, "saleq_seasonal_comparative": 100,
             "epsfiq": None} for i in range(12)]
    result = EA.sue_history(rows)
    assert result[7]["sue_rev_ann"] is None
    assert result[8]["sue_rev_ann"] == pytest.approx(64/math.sqrt(sum((i*i-17.5)**2 for i in range(8))/7))
    assert EA.sue_history(rows[:9]) == result[:9]


def test_mixed_metric_table_currencies_fail_closed():
    import re
    first=re.sub(r'<tr><td>(?:Net income \(loss\)|Diluted earnings per share)</td>.*?</tr>','',release().decode())
    first=first.replace('<html>','<html><div>').replace('</html>','</div>')
    second=re.sub(r'<tr><td>Net revenues</td>.*?</tr>','',release(unit='CAD in millions, except per-share amounts').decode())
    second=second.replace('<html>','<div>').replace('</html>','</div></html>')
    r=parse((first+second).encode())
    assert r['currency'] is None and all(r[f] is None for f in EA.FIELDS)
    assert r['reason']=='mixed_currencies_across_metrics'


def test_comparative_conflicts_merge_independent_of_document_order():
    a=parse(release()); b=parse(release().replace(b'<td>100</td>',b'<td>99</td>'))
    merged=EA.merge_extracts([a,b])
    assert merged==EA.merge_extracts([b,a])
    assert merged['saleq']==120e6 and merged['saleq_seasonal_comparative'] is None
    assert 'saleq_seasonal_comparative:conflicting_documents' in merged['reason']
    b={**a,'period_basis':'unsupported_other_basis'}
    assert all(EA.merge_extracts([a,b])[f] is None for f in EA.FIELDS)


def test_source_quarter_requires_explicit_calendar_and_matching_period():
    text='First quarter 2020 financial results. Fiscal year ends December 31.'
    assert EA.fiscal_identity(text,dt.date(2020,3,31))['fiscal_quarter']==1
    assert EA.fiscal_identity('First quarter 2020 financial results',dt.date(2020,3,31))['fiscal_quarter'] is None
    assert EA.fiscal_identity(text,dt.date(2020,9,30))['fiscal_quarter'] is None


def test_annual_only_missing_quarter_and_calendar_transition_fail_sue():
    annual=[{'accession':str(i),'period_end':dt.date(2010+i,3,31),'available_at':dt.datetime(2010+i,5,1),
             'currency':'USD','saleq':100+i*i,'saleq_seasonal_comparative':100,
             'fiscal_year_label':2010+i,'fiscal_quarter':1,'fiscal_calendar_id':'12-31'} for i in range(9)]
    assert EA.sue_history(annual)[-1]['sue_rev_ann'] is None
    quarterly=[{**r,'accession':str(i),'period_end':dt.date(2010+i//4,3*(i%4+1),28),
                'available_at':dt.datetime(2010+i//4,3*(i%4+1),29),
                'fiscal_year_label':2010+i//4,'fiscal_quarter':i%4+1} for i,r in enumerate(annual)]
    assert EA.sue_history(quarterly)[-1]['sue_rev_ann'] is not None
    assert EA.sue_history(quarterly[:4]+quarterly[5:])[-1]['sue_rev_ann'] is None
    quarterly[-1]['fiscal_calendar_id']='09-30'
    last=EA.sue_history(quarterly)[-1]
    assert last['sue_rev_ann'] is None and 'calendar_transition' in last['sue_rev_ann_reason']


def test_same_clock_and_future_restated_vintages_not_visible_to_sue():
    rows = [{"accession": str(i), "period_end": dt.date(2010+i//4, 3*(i%4+1), 28),
             "available_at": dt.datetime(2020, 1, 1), "currency": "USD", "saleq": i,
             "fiscal_year_label":2010+i//4,"fiscal_quarter":i%4+1,"fiscal_calendar_id":"12-31",
             "saleq_seasonal_comparative": 0} for i in range(10)]
    assert all(r["sue_rev_ann"] is None for r in EA.sue_history(rows))
    assert EA.sue_history([{**r,'available_at':dt.datetime.combine(r['period_end'],dt.time())+dt.timedelta(days=1)} for r in rows])[-1]['sue_rev_ann'] is not None


def test_reaction_window_boundary_and_completion_clock():
    clock = dt.datetime(2022, 12, 30, 22)
    dates = [dt.date(2022, 12, d) for d in (28, 29, 30)]
    value, at, reason = EA.reaction_return(dates, [.1, -.1, .02], clock)
    assert value == pytest.approx(.0098) and at == clock and reason == "qualified"
    dates[-1] = dt.date(2023, 1, 3)
    assert EA.reaction_return(dates, [object(), object(), object()], clock)[2].startswith("holdout")
    assert EA.reaction_return(dates[:2], [0, 0], clock)[2] == "incomplete_window"


def test_resumable_units_reuse_exact_input_and_reject_changes(tmp_path,monkeypatch):
    row={'cik':1,'accession':'a','filing_date':dt.date(2020,5,1),'source_available_at':dt.datetime(2020,5,1,18),'acceptance_clock':'file_utc'}
    doc={'source_doc':'ex99.htm','doc_sha256':'abc'}
    from collections import defaultdict
    counts=defaultdict(int)
    first=EA.cached_extract(release(),row,doc,tmp_path,'code-input-v1',counts,1)
    assert counts['new_units']==1
    monkeypatch.setattr(EA,'parse_release',lambda *args:pytest.fail('complete unit was reparsed'))
    again=EA.cached_extract(release(),row,doc,tmp_path,'code-input-v1',counts,1)
    assert first==again and counts['cache_hits']==1
    changed={**row,'filing_date':dt.date(2020,5,2)}
    with pytest.raises(EA.ParseUnitLimit):
        EA.cached_extract(release(),changed,doc,tmp_path,'code-input-v1',counts,1)
    with pytest.raises(EA.ParseUnitLimit):
        EA.cached_extract(release(),row,doc,tmp_path,'code-input-v2',counts,1)
    # Interrupted writes carry no complete status and are never admitted.
    for path in tmp_path.rglob('*.json'):
        path.write_text('{"status":"incomplete"}',encoding='utf-8')
    with pytest.raises(EA.ParseUnitLimit):
        EA.cached_extract(release(),row,doc,tmp_path,'code-input-v1',counts,1)
